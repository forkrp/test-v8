// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Measures the public JS bindings. UTF-8 helpers provide an explicit native
// byte transport boundary for JSON; string-only JSON is a separate contract.
#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "include/libplatform/libplatform.h"
#include "include/v8.h"

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void Free(void* data, size_t, void*) { std::free(data); }
void Utf8Encode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope scope(isolate);
  Check(info[0]->IsString(), "UTF-8 encode expects a string");
  auto text = info[0].As<v8::String>();
  int length = text->Utf8Length(isolate);
  void* bytes = std::malloc(static_cast<size_t>(length));
  Check(bytes || !length, "UTF-8 allocation failed");
  int written = text->WriteUtf8(
      isolate, static_cast<char*>(bytes), length, nullptr,
      v8::String::NO_NULL_TERMINATION | v8::String::REPLACE_INVALID_UTF8);
  Check(written == length, "UTF-8 write length mismatch");
  auto backing = v8::ArrayBuffer::NewBackingStore(bytes, length, Free, nullptr);
  auto buffer = v8::ArrayBuffer::New(isolate, std::move(backing));
  info.GetReturnValue().Set(v8::Uint8Array::New(buffer, 0, length));
}
void Utf8Decode(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::Isolate* isolate = info.GetIsolate();
  v8::HandleScope scope(isolate);
  Check(info[0]->IsUint8Array(), "UTF-8 decode expects Uint8Array");
  auto view = info[0].As<v8::Uint8Array>();
  auto backing = view->Buffer()->GetBackingStore();
  auto text =
      v8::String::NewFromUtf8(
          isolate,
          static_cast<const char*>(backing->Data()) + view->ByteOffset(),
          v8::NewStringType::kNormal, static_cast<int>(view->ByteLength()))
          .ToLocalChecked();
  info.GetReturnValue().Set(text);
}
v8::Local<v8::String> Text(v8::Isolate* isolate, const std::string& value) {
  return v8::String::NewFromUtf8(isolate, value.data(),
                                 v8::NewStringType::kNormal,
                                 static_cast<int>(value.size()))
      .ToLocalChecked();
}
v8::Local<v8::Value> Eval(v8::Isolate* isolate, v8::Local<v8::Context> context,
                          const std::string& source) {
  v8::TryCatch caught(isolate);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> value;
  if (!v8::Script::Compile(context, Text(isolate, source)).ToLocal(&script) ||
      !script->Run(context).ToLocal(&value)) {
    v8::String::Utf8Value error(isolate, caught.Exception());
    std::cerr << "JS benchmark failed: " << *error << '\n';
    std::exit(1);
  }
  return value;
}
double CpuSeconds() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
         usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

void Now(const v8::FunctionCallbackInfo<v8::Value>& info) {
  static auto origin = Clock::now();
  info.GetReturnValue().Set(
      std::chrono::duration<double, std::milli>(Clock::now() - origin).count());
}
void GC(const v8::FunctionCallbackInfo<v8::Value>& info) {
  info.GetIsolate()->LowMemoryNotification();
}
void Print(const v8::FunctionCallbackInfo<v8::Value>& info) {
  v8::String::Utf8Value text(info.GetIsolate(), info[0]);
  std::cout << *text << std::endl;
}
void Quit(const v8::FunctionCallbackInfo<v8::Value>& info) { std::exit(1); }
std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "File missing");
  return std::string((std::istreambuf_iterator<char>(file)), {});
}
int AsyncMain(char** argv) {
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  auto isolate = v8::Isolate::New(params);
  {
    v8::Isolate::Scope scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    for (const auto& helper : {std::pair<const char*, v8::FunctionCallback>{
                                   "__utf8Encode", Utf8Encode},
                               {"__utf8Decode", Utf8Decode},
                               {"gc", GC},
                               {"print", Print},
                               {"quit", Quit}})
      context->Global()
          ->Set(context, Text(isolate, helper.first),
                v8::Function::New(context, helper.second).ToLocalChecked())
          .Check();
    auto performance = v8::Object::New(isolate);
    performance
        ->Set(context, Text(isolate, "now"),
              v8::Function::New(context, Now).ToLocalChecked())
        .Check();
    context->Global()
        ->Set(context, Text(isolate, "performance"), performance)
        .Check();
    auto arguments = v8::Array::New(isolate, 5);
    for (int i = 0; i < 5; ++i)
      arguments->Set(context, i, Text(isolate, argv[i + 3])).Check();
    context->Global()
        ->Set(context, Text(isolate, "arguments"), arguments)
        .Check();
    context->Global()
        ->Set(context, Text(isolate, "__source"), Text(isolate, Read(argv[3])))
        .Check();
    auto result = Eval(isolate, context, Read(argv[2])).As<v8::Promise>();
    isolate->PerformMicrotaskCheckpoint();
    while (result->State() == v8::Promise::kPending ||
           isolate->HasPendingBackgroundTasks()) {
      v8::platform::PumpMessageLoop(
          platform.get(), isolate,
          v8::platform::MessageLoopBehavior::kWaitForWork);
      isolate->PerformMicrotaskCheckpoint();
    }
    Check(result->State() == v8::Promise::kFulfilled,
          "Async benchmark rejected");
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 8 && std::string(argv[1]) == "--async") return AsyncMain(argv);
  bool prepare_only = argc == 5 && std::string(argv[1]) == "--prepare";
  bool cold = argc == 8 && std::string(argv[6]) == "--cold";
  Check(argc == 6 || prepare_only || cold,
        "Usage: encode|decode codec source.json iterations warmups [--cold "
        "input.bytes], or --prepare codec source.json output.bytes");
  std::string operation = prepare_only ? "encode" : argv[1], codec = argv[2];
  Check(operation == "encode" || operation == "decode", "Invalid operation");
  Check(codec == "msgpack" || codec == "json_bytes" || codec == "json_string",
        "Invalid codec");
  int iterations = prepare_only ? 1 : std::stoi(argv[4]);
  int warmups = prepare_only ? 0 : std::stoi(argv[5]);
  Check(iterations > 0 && warmups >= 0, "Invalid counts");
  Check(!cold || (iterations == 1 && warmups == 0),
        "Cold requires one call and zero warmups");
  std::ifstream file(argv[3], std::ios::binary);
  Check(file.good(), "Source file missing");
  std::string source((std::istreambuf_iterator<char>(file)), {});
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  auto allocator = std::unique_ptr<v8::ArrayBuffer::Allocator>(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    const std::pair<const char*, v8::FunctionCallback> helpers[] = {
        {"__utf8Encode", Utf8Encode}, {"__utf8Decode", Utf8Decode}};
    for (auto entry : helpers)
      context->Global()
          ->Set(context, Text(isolate, entry.first),
                v8::Function::New(context, entry.second).ToLocalChecked())
          .Check();
    std::string action =
        operation == "encode"
            ? (codec == "msgpack"      ? "MSGPACK.encode(input)"
               : codec == "json_bytes" ? "__utf8Encode(JSON.stringify(input))"
                                       : "JSON.stringify(input)")
            : (codec == "msgpack"      ? "MSGPACK.decode(input)"
               : codec == "json_bytes" ? "JSON.parse(__utf8Decode(input))"
                                       : "JSON.parse(input)");
    std::string prepare =
        operation == "encode" ? "const input = original;"
        : codec == "msgpack"  ? "const input = MSGPACK.encode(original);"
        : codec == "json_bytes"
            ? "const input = __utf8Encode(JSON.stringify(original));"
            : "const input = JSON.stringify(original);";
    // The preparation scope retains only the required input. Decode does not
    // retain the original graph, so GC cannot artificially keep its maps alive.
    std::string body = "(() => { const original = JSON.parse(";
    // Pass the source string through a temporary global rather than quoting it.
    context->Global()
        ->Set(context, Text(isolate, "__source"), Text(isolate, source))
        .Check();
    body +=
        "__source); " + prepare + "const once = () => " + action +
        "; const out = once(); "
        "const restored = " +
        (operation == "decode"   ? std::string("out")
         : codec == "msgpack"    ? std::string("MSGPACK.decode(out)")
         : codec == "json_bytes" ? std::string("JSON.parse(__utf8Decode(out))")
                                 : std::string("JSON.parse(out)")) +
        "; if (JSON.stringify(restored) !== JSON.stringify(original)) throw "
        "new Error('Value mismatch'); "
        "const payloadBytes = " +
        (operation == "decode"
             ? (codec == "json_string"
                    ? std::string("__utf8Encode(input).byteLength")
                    : std::string("input.byteLength"))
             : (codec == "json_string"
                    ? std::string("__utf8Encode(out).byteLength")
                    : std::string("out.byteLength"))) +
        "; return [payloadBytes, function(n) { let value, observed = 0; for "
        "(let i = 0; i < n; ++i) { value = once(); " +
        (operation == "encode"
             ? std::string("observed ^= value.length;")
             : std::string("observed ^= Array.isArray(value) ? value.length : "
                           "Object.keys(value).length;")) +
        " } return [observed, value]; }]; })()";
    if (cold) {
      if (operation == "decode") {
        std::ifstream wire_file(argv[7], std::ios::binary);
        Check(wire_file.good(), "Cold input file missing");
        std::string bytes((std::istreambuf_iterator<char>(wire_file)), {});
        v8::Local<v8::Value> input;
        if (codec == "json_string")
          input = Text(isolate, bytes);
        else {
          auto backing =
              v8::ArrayBuffer::NewBackingStore(isolate, bytes.size());
          std::memcpy(backing->Data(), bytes.data(), bytes.size());
          auto buffer = v8::ArrayBuffer::New(isolate, std::move(backing));
          input = v8::Uint8Array::New(buffer, 0, bytes.size());
        }
        context->Global()->Set(context, Text(isolate, "__wire"), input).Check();
      }
      // No codec call, decoded graph, or schema cache exists before the clock.
      // Validate against the original source only after the timed first call.
      body = "(() => { const input = " +
             (operation == "encode" ? std::string("JSON.parse(__source)")
                                    : std::string("__wire")) +
             "; const once = () => " + action +
             "; return [0, function(n) { const value = once(); return [0, "
             "value]; }]; })()";
    }
    auto prepared = Eval(isolate, context, body).As<v8::Array>();
    auto loop = prepared->Get(context, 1).ToLocalChecked().As<v8::Function>();
    double payload_bytes = prepared->Get(context, 0)
                               .ToLocalChecked()
                               ->NumberValue(context)
                               .FromJust();
    if (!cold)
      context->Global()->Delete(context, Text(isolate, "__source")).Check();
    auto run = [&](int count) {
      v8::Local<v8::Value> args[] = {v8::Integer::New(isolate, count)};
      v8::Local<v8::Value> result;
      Check(loop->Call(context, context->Global(), 1, args).ToLocal(&result),
            "JS loop failed");
      return result;
    };
    if (prepare_only) {
      auto output = run(1).As<v8::Array>()->Get(context, 1).ToLocalChecked();
      std::ofstream prepared_file(argv[4], std::ios::binary);
      Check(prepared_file.good(), "Prepared input output file failed");
      if (output->IsString()) {
        v8::String::Utf8Value bytes(isolate, output);
        prepared_file.write(*bytes, bytes.length());
      } else {
        auto view = output.As<v8::Uint8Array>();
        auto backing = view->Buffer()->GetBackingStore();
        prepared_file.write(
            static_cast<const char*>(backing->Data()) + view->ByteOffset(),
            view->ByteLength());
      }
      Check(prepared_file.good(), "Prepared input write failed");
    } else {
      if (warmups) {
        v8::HandleScope warm_scope(isolate);
        run(warmups);
      }
      isolate->LowMemoryNotification();
      v8::HeapStatistics heap_before;
      isolate->GetHeapStatistics(&heap_before);
      double cpu = CpuSeconds();
      auto start = Clock::now();
      auto result = run(iterations).As<v8::Array>();
      double milliseconds =
          std::chrono::duration<double, std::milli>(Clock::now() - start)
              .count();
      cpu = CpuSeconds() - cpu;
      int observed = result->Get(context, 0)
                         .ToLocalChecked()
                         ->Int32Value(context)
                         .FromJust();
      v8::HeapStatistics heap_peak;
      isolate->GetHeapStatistics(&heap_peak);
      isolate->LowMemoryNotification();
      v8::HeapStatistics heap_retained;
      isolate->GetHeapStatistics(&heap_retained);
      if (cold) {
        context->Global()
            ->Set(context, Text(isolate, "__out"),
                  result->Get(context, 1).ToLocalChecked())
            .Check();
        std::string restore = operation == "decode" ? "__out"
                              : codec == "msgpack"  ? "MSGPACK.decode(__out)"
                              : codec == "json_bytes"
                                  ? "JSON.parse(__utf8Decode(__out))"
                                  : "JSON.parse(__out)";
        Eval(isolate, context,
             "if (JSON.stringify(" + restore +
                 ") !== JSON.stringify(JSON.parse(__source))) throw new "
                 "Error('Cold value mismatch');");
        auto bytes = Eval(
            isolate, context,
            operation == "decode"
                ? (codec == "json_string" ? "__utf8Encode(__wire).byteLength"
                                          : "__wire.byteLength")
                : (codec == "json_string" ? "__utf8Encode(__out).byteLength"
                                          : "__out.byteLength"));
        payload_bytes = bytes->NumberValue(context).FromJust();
      }
      rusage usage{};
      getrusage(RUSAGE_SELF, &usage);
      std::cout << "{\"millisecondsPerOperation\":" << milliseconds / iterations
                << ",\"cpuSeconds\":" << cpu << ",\"iterations\":" << iterations
                << ",\"warmups\":" << warmups
                << ",\"payloadBytes\":" << payload_bytes
                << ",\"observed\":" << observed
                << ",\"peakRssRaw\":" << usage.ru_maxrss
                << ",\"firstApiCall\":" << (cold ? "true" : "false")
                << ",\"heapBeforeBytes\":" << heap_before.used_heap_size()
                << ",\"heapAfterTimingBytes\":" << heap_peak.used_heap_size()
                << ",\"heapRetainedBytes\":" << heap_retained.used_heap_size()
                << ",\"externalBeforeBytes\":" << heap_before.external_memory()
                << ",\"externalRetainedBytes\":"
                << heap_retained.external_memory() << "}\n";
    }
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
}
