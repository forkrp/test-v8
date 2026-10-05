// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <sys/resource.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "include/libplatform/libplatform.h"
#include "include/v8.h"
#include "src/api/api-inl.h"
#include "src/base/page-allocator.h"
#include "src/msgpack/messagepack.h"

namespace i = v8::internal;
namespace base = v8::base;
using Clock = std::chrono::steady_clock;
[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}
void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}
std::vector<uint8_t> Read(const std::string& file) {
  std::ifstream in(file, std::ios::binary);
  Check(in.good(), "Cannot open " + file);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),
                             std::istreambuf_iterator<char>()};
  Check(bytes.size() <= static_cast<size_t>(std::numeric_limits<int>::max()),
        "Input too large");
  return bytes;
}
void Write(const std::string& file, const uint8_t* bytes, size_t size) {
  std::ofstream out(file, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes), size);
  Check(out.good(), "Cannot write " + file);
}
v8::Local<v8::Value> ParseJson(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               const std::vector<uint8_t>& bytes) {
  v8::Local<v8::String> text;
  v8::Local<v8::Value> value;
  Check(v8::String::NewFromUtf8(
            isolate, reinterpret_cast<const char*>(bytes.data()),
            v8::NewStringType::kNormal, static_cast<int>(bytes.size()))
            .ToLocal(&text),
        "UTF-8 allocation failed");
  Check(v8::JSON::Parse(context, text).ToLocal(&value), "JSON parse failed");
  return value;
}
std::string Json(v8::Isolate* isolate, v8::Local<v8::Context> context,
                 v8::Local<v8::Value> value) {
  v8::Local<v8::String> text;
  Check(v8::JSON::Stringify(context, value).ToLocal(&text),
        "JSON stringify failed");
  v8::String::Utf8Value bytes(isolate, text);
  return std::string(*bytes, bytes.length());
}
v8::Local<v8::Value> Decode(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            const std::string& codec,
                            const std::vector<uint8_t>& bytes,
                            v8::Local<v8::String> json_text = {}) {
  if (codec == "json") return ParseJson(isolate, context, bytes);
  if (codec == "json_string") {
    v8::Local<v8::Value> value;
    Check(v8::JSON::Parse(context, json_text).ToLocal(&value),
          "JSON parse failed");
    return value;
  }
  if (codec == "v8") {
    v8::ValueDeserializer decoder(isolate, bytes.data(), bytes.size());
    Check(decoder.ReadHeader(context).FromMaybe(false), "V8 header failed");
    v8::Local<v8::Value> value;
    Check(decoder.ReadValue(context).ToLocal(&value), "V8 decode failed");
    return value;
  }
  std::string error;
  i::Handle<i::Object> object;
  auto mode = codec == "msgpack_tree" ? i::MessagePackDecodeMode::kNativeTree
              : codec == "mpack"      ? i::MessagePackDecodeMode::kMPackReader
              : codec == "msgpack_visitor" ? i::MessagePackDecodeMode::kVisitor
                                           : i::MessagePackDecodeMode::kDirect;
  Check(
      i::DecodeMessagePack(
          reinterpret_cast<i::Isolate*>(isolate),
          base::Vector<const uint8_t>(bytes.data(), bytes.size()), &error, mode)
          .ToHandle(&object),
      error);
  return v8::Utils::ToLocal(object);
}
std::vector<uint8_t> Encode(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            const std::string& codec,
                            v8::Local<v8::Value> value) {
  if (codec == "json") {
    auto s = Json(isolate, context, value);
    return {s.begin(), s.end()};
  }
  if (codec == "v8") {
    v8::ValueSerializer encoder(isolate);
    encoder.WriteHeader();
    Check(encoder.WriteValue(context, value).FromMaybe(false),
          "V8 encode failed");
    auto buffer = encoder.Release();
    std::vector<uint8_t> result(buffer.first, buffer.first + buffer.second);
    std::free(buffer.first);
    return result;
  }
  std::vector<uint8_t> result;
  std::string error;
  Check(i::EncodeMessagePack(reinterpret_cast<i::Isolate*>(isolate),
                             v8::Utils::OpenHandle(*value), &result, &error,
                             codec == "msgpack32"),
        error);
  return result;
}
v8::Local<v8::Value> Evaluate(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              const std::string& source) {
  auto text = v8::String::NewFromUtf8(isolate, ("(" + source + ")").c_str())
                  .ToLocalChecked();
  return v8::Script::Compile(context, text)
      .ToLocalChecked()
      ->Run(context)
      .ToLocalChecked();
}
void BufferOwnershipSelfTest() {
  for (bool pages : {false, true}) {
    for (bool report_mapping : {false, true}) {
      for (size_t size : {size_t{17}, size_t{32767}, size_t{32768},
                          size_t{49315}, size_t{65537}, size_t{262149}}) {
        i::MessagePackBuffer buffer(pages);
        size_t first = size * 2 / 3;
        auto fill = [](uint8_t* output, size_t start, size_t count) {
          for (size_t j = 0; j < count; ++j)
            output[j] =
                static_cast<uint8_t>(((start + j) * 29) ^ ((start + j) >> 8));
        };
        fill(buffer.Append(first), 0, first);
        fill(buffer.Append(size - first), first, size - first);
        size_t mapping_size = 0;
        uint8_t* owned =
            buffer.Release(report_mapping ? &mapping_size : nullptr);
        Check(owned && buffer.size() == 0, "Buffer ownership transfer failed");
        for (size_t j = 0; j < size; ++j)
          Check(owned[j] == static_cast<uint8_t>((j * 29) ^ (j >> 8)),
                "Buffer growth/trim lost data");
        if (mapping_size) {
          base::PageAllocator allocator;
          Check(pages && report_mapping && mapping_size >= size &&
                    mapping_size % allocator.AllocatePageSize() == 0,
                "Invalid mapped buffer ownership");
          Check(allocator.FreePages(owned, mapping_size),
                "Mapped buffer release failed");
        } else {
          // The legacy interface must remain malloc/free compatible even when
          // the buffer used mappings while growing.
          std::free(owned);
        }
      }
    }
  }
}
void SelfTest(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  BufferOwnershipSelfTest();
  const std::vector<std::string> expressions = {
      "null",
      "true",
      "false",
      "0",
      "-0",
      "127",
      "128",
      "-32",
      "-33",
      "65536",
      "9007199254740991",
      "-9007199254740991",
      "1.25",
      "NaN",
      "Infinity",
      "-Infinity",
      "9007199254740992n",
      "18446744073709551615n",
      "-9223372036854775808n",
      "'中文🌏é\\u0000'",
      "[]",
      "{}",
      "({a:1,b:2.5,c:'native'})",
      "[1,2,3]",
      "[1,1.25,-0]",
      "[NaN,Infinity,-Infinity,-0]",
      "[{x:NaN},{x:1.25},{x:-0},{x:Infinity}]",
      "[1,'a',null,{x:1}]",
      "JSON.parse('{\"__proto__\":{\"x\":1},\"constructor\":2,\"0\":3,"
      "\"4294967294\":4}')",
      "Array.from({length:4000},(_,i)=>({id:i,name:'记录'+i,score:i/"
      "8,child:{value:i%2?1:'x'},tags:[i,null]}))",
      "[{x:1,y:2},{x:1.25,y:3},{x:'changed'},{z:4},{x:null,y:2}]",
      "''",
      "'\\u0000\\u007f\\u0080\\u00ff\\u0100\\u07ff\\u0800\\ud7ff\\ue000\\uffff"
      "'",
      "String.fromCodePoint(0x10000,0x10ffff,0x1f30f)",
      "Array.from({length:4096},(_,i)=>String.fromCodePoint(i<2048?i:"
      "0x10000+(i-2048)*511)).join('')",
      "Array.from({length:65536},(_,i)=>i>=0xd800&&i<=0xdfff?'':"
      "String.fromCodePoint(i)).join('')",
      "String.fromCodePoint(...Array.from({length:256},(_,i)=>i)).repeat(32)",
      "['a'.repeat(64), 'a'.repeat(10)+'b'+'a'.repeat(53)]",
      "Array.from({length:6000},(_,i)=>i/10).concat(9007199254740992n)",
      "({numbers:Array.from({length:6000},(_,i)=>i/10)})",
      "[{'标题':'中文文本🌏'.repeat(100)}, {'标题':'中文文本🌏'.repeat(100)}]",
      "Array.from({length:2000},(_,i)=>({left:{x:i,y:i/8},"
      "right:{name:'different',ok:i%2===0},tags:[i,null]}))",
      "[{a:[1.5,-0,NaN,Infinity]}, {a:[1.5,'late',-0,NaN]},"
      "{a:[1,2,9007199254740991,9007199254740992n]}]",
      "Array.from({length:40},(_,i)=>'a'.repeat(i)+'中文序列化'.repeat(40)+'🌏'"
      ")",
      "Array.from({length:40},(_,i)=>'a'.repeat(i)+'Ελληνικά'.repeat(40)+'é')",
      "Array.from({length:40},(_,i)=>'a'.repeat(i)+'éÿ'.repeat(40)+'\\u0000')",
      "Array.from({length:40},(_,i)=>'a'.repeat(i)+'🌏😀'.repeat(40)+'中文')",
      "'éÿ'.repeat(10000)"};
  auto equal =
      Evaluate(isolate, context,
               "(function eq(a,b){if(Object.is(a,b))return "
               "true;if(a===null||b===null||typeof "
               "a!=='object'||typeof b!=='object')return false;"
               "if(Array.isArray(a)!==Array.isArray(b))return "
               "false;if(!Array.isArray(b)&&Object.getPrototypeOf(b)!==Object."
               "prototype)return false;let ka=Object.keys(a),kb=Object.keys(b);"
               "if(ka.length!==kb.length||ka.some((k,i)=>k!==kb[i]))"
               "return false;return ka.every(k=>eq(a[k],b[k]));})")
          .As<v8::Function>();
  for (const auto& expression : expressions) {
    v8::HandleScope scope(isolate);
    auto original = Evaluate(isolate, context, expression);
    for (const auto& encoder : {"msgpack", "msgpack32"}) {
      auto bytes = Encode(isolate, context, encoder, original);
      for (const auto& codec :
           {"msgpack", "msgpack_visitor", "msgpack_tree", "mpack"}) {
        auto decoded = Decode(isolate, context, codec, bytes);
        if (decoded->IsObject() && !decoded->IsArray()) {
          auto object =
              i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*decoded));
          Check(object->HasFastProperties(),
                "Small decoded object uses dictionary properties");
        }
        v8::Local<v8::Value> args[] = {original, decoded};
        Check(equal->Call(context, context->Global(), 2, args)
                  .ToLocalChecked()
                  ->IsTrue(),
              "Mismatch: " + expression);
        isolate->LowMemoryNotification();
        Check(equal->Call(context, context->Global(), 2, args)
                  .ToLocalChecked()
                  ->IsTrue(),
              "GC mismatch: " + expression);
      }
      // Every incomplete prefix must fail. Bound large test cost.
      for (size_t n = 0; n < bytes.size() && n < 96; ++n) {
        for (auto mode : {i::MessagePackDecodeMode::kDirect,
                          i::MessagePackDecodeMode::kVisitor,
                          i::MessagePackDecodeMode::kNativeTree,
                          i::MessagePackDecodeMode::kMPackReader}) {
          v8::HandleScope prefix_scope(isolate);
          std::string error;
          auto result = i::DecodeMessagePack(
              reinterpret_cast<i::Isolate*>(isolate),
              base::Vector<const uint8_t>(bytes.data(), n), &error, mode);
          Check(result.is_null(), "Truncated prefix accepted");
        }
      }
    }
  }
  std::vector<std::vector<uint8_t>> malformed = {
      {0xc1},
      {0xa1, 0xff},
      {0xa3, 0xed, 0xa0, 0x80},
      {0xa2, 0xc0, 0xaf},
      {0xa2, 0xc2, 0x20},
      {0xa3, 0xe0, 0x80, 0x80},
      {0xa4, 0xf0, 0x80, 0x80, 0x80},
      {0xa4, 0xf4, 0x90, 0x80, 0x80},
      {0xa4, 0xf5, 0x80, 0x80, 0x80},
      {0xa1, 0xc2},
      {0x81, 0x01, 0xc0},
      {0xdd, 0xff, 0xff, 0xff, 0xff},
      {0xdf, 0xff, 0xff, 0xff, 0xff},
      {0xc0, 0xc0},
      {0xc6, 0xff, 0xff, 0xff, 0xff},
      {0xdb, 0xff, 0xff, 0xff, 0xff}};
  auto deep = std::vector<uint8_t>(257, 0x91);
  deep.push_back(0xc0);
  malformed.push_back(deep);
  // Both strings have the same cache fingerprint, but the second differs in
  // an unsampled byte and contains invalid UTF-8. A hash hit must still check
  // the complete input before reusing a validated string.
  std::vector<uint8_t> collision = {0x92, 0xd9, 64};
  collision.insert(collision.end(), 64, 'a');
  collision.push_back(0xd9);
  collision.push_back(64);
  collision.insert(collision.end(), 64, 'a');
  collision[3 + 64 + 2 + 10] = 0xff;
  malformed.push_back(collision);
  for (const auto& bytes : malformed) {
    for (auto mode :
         {i::MessagePackDecodeMode::kDirect, i::MessagePackDecodeMode::kVisitor,
          i::MessagePackDecodeMode::kNativeTree,
          i::MessagePackDecodeMode::kMPackReader}) {
      v8::HandleScope malformed_scope(isolate);
      std::string error;
      auto result = i::DecodeMessagePack(
          reinterpret_cast<i::Isolate*>(isolate),
          base::Vector<const uint8_t>(bytes.data(), bytes.size()), &error,
          mode);
      Check(result.is_null() && !error.empty(), "Malformed input accepted");
    }
  }
  auto duplicate =
      Decode(isolate, context, "msgpack", {0x82, 0xa1, 'x', 1, 0xa1, 'x', 2});
  Check(Json(isolate, context, duplicate) == "{\"x\":2}",
        "Duplicate key semantics failed");
  for (const auto& codec :
       {"msgpack", "msgpack_visitor", "msgpack_tree", "mpack"}) {
    auto large_signed =
        Decode(isolate, context, codec,
               {0xd3, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    bool lossless = false;
    Check(large_signed->IsBigInt() &&
              large_signed.As<v8::BigInt>()->Int64Value(&lossless) ==
                  std::numeric_limits<int64_t>::max() &&
              lossless,
          "Signed positive int64 lost precision");
    auto deepest = std::vector<uint8_t>(256, 0x91);
    deepest.push_back(0xc0);
    Check(Decode(isolate, context, codec, deepest)->IsArray(),
          "Valid boundary depth rejected");
  }
  uint32_t random = 20261004;
  for (int sample = 0; sample < 2000; ++sample) {
    v8::HandleScope fuzz_scope(isolate);
    auto next = [&]() {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      return random;
    };
    std::vector<uint8_t> bytes(next() % 64);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(next());
    bool accepted[3];
    int backend = 0;
    for (auto mode :
         {i::MessagePackDecodeMode::kDirect, i::MessagePackDecodeMode::kVisitor,
          i::MessagePackDecodeMode::kNativeTree,
          i::MessagePackDecodeMode::kMPackReader}) {
      std::string error;
      accepted[backend++] =
          !i::DecodeMessagePack(
               reinterpret_cast<i::Isolate*>(isolate),
               base::Vector<const uint8_t>(bytes.data(), bytes.size()), &error,
               mode)
               .is_null();
    }
    Check(accepted[0] == accepted[1] && accepted[1] == accepted[2],
          "Backend acceptance mismatch");
  }
  for (const auto& expression :
       {"(()=>{let x={};x.x=x;return x})()", "[,1]",
        "({get x(){throw Error('executed')}})", "'\\ud800'", "(()=>{})",
        "18446744073709551616n", "'a'.repeat(40)+'\\ud800'+'a'.repeat(40)",
        "'a'.repeat(40)+'\\udc00'+'a'.repeat(40)"}) {
    auto value = Evaluate(isolate, context, expression);
    std::vector<uint8_t> bytes;
    std::string error;
    Check(!i::EncodeMessagePack(reinterpret_cast<i::Isolate*>(isolate),
                                v8::Utils::OpenHandle(*value), &bytes, &error),
          "Unsupported encode accepted");
    Check(bytes.empty() && !error.empty(),
          "Encode failure left output or no error");
  }
  std::cout << "{\"selfTest\":\"PASS\",\"roundTrips\":" << expressions.size()
            << ",\"backends\":4,\"malformedCases\":" << malformed.size()
            << ",\"differentialByteInputs\":2000" << "}\n";
}
double CpuSeconds() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec +
         r.ru_stime.tv_usec / 1e6;
}
size_t PeakRss() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
#if defined(__APPLE__)
  return r.ru_maxrss;
#else
  return r.ru_maxrss * 1024;
#endif
}
int main(int argc, char** argv) {
  Check(argc >= 2,
        "Usage: benchmark --self-test | --prepare JSON PREFIX | --measure "
        "CODEC FILE ITERATIONS | --encode CODEC JSON ITERATIONS");
  if (argc > 2 && std::string(argv[2]) == "--stress")
    v8::V8::SetFlagsFromString("--random-gc-interval=100 --stress-compaction");
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  auto isolate = v8::Isolate::New(params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    std::string operation = argv[1];
    if (operation == "--self-test")
      SelfTest(isolate, context);
    else if (operation == "--prepare") {
      Check(argc == 4, "Prepare args");
      auto value = ParseJson(isolate, context, Read(argv[2]));
      std::string canonical = Json(isolate, context, value);
      // All encoders receive the exact graph representable by the JSON baseline
      // (JSON.stringify normalizes negative zero and non-finite numbers).
      value =
          ParseJson(isolate, context,
                    std::vector<uint8_t>(canonical.begin(), canonical.end()));
      for (const auto& codec : {"json", "msgpack", "msgpack32", "v8"}) {
        auto bytes = Encode(isolate, context, codec, value);
        Check(Json(isolate, context, Decode(isolate, context, codec, bytes)) ==
                  canonical,
              "Fixture mismatch");
        if (std::string(codec) == "msgpack" ||
            std::string(codec) == "msgpack32") {
          Check(Json(isolate, context,
                     Decode(isolate, context, "msgpack_tree", bytes)) ==
                    canonical,
                "Tree fixture mismatch");
          Check(Json(isolate, context,
                     Decode(isolate, context, "mpack", bytes)) == canonical,
                "MPack fixture mismatch");
        }
        Write(std::string(argv[3]) + '.' + codec, bytes.data(), bytes.size());
      }
      std::cout << "{\"prepared\":true}\n";
    } else {
      Check(argc == 5, "Measure args");
      std::string codec = argv[2];
      auto bytes = Read(argv[3]);
      int iterations = std::stoi(argv[4]);
      Check(iterations > 0, "Iterations must be positive");
      v8::Local<v8::String> text;
      if (codec == "json_string")
        text = v8::String::NewFromUtf8(
                   isolate, reinterpret_cast<const char*>(bytes.data()),
                   v8::NewStringType::kNormal, static_cast<int>(bytes.size()))
                   .ToLocalChecked();
      v8::Local<v8::Value> original;
      if (operation == "--encode")
        original = ParseJson(isolate, context, bytes);
      size_t observed = 0;
      for (int n = 0; n < 20; ++n) {
        v8::HandleScope scope(isolate);
        if (operation == "--encode")
          observed ^= Encode(isolate, context, codec, original).size();
        else {
          auto value = Decode(isolate, context, codec, bytes, text);
          observed ^= value->IsObject();
        }
      }
      isolate->LowMemoryNotification();
      v8::HeapStatistics before;
      isolate->GetHeapStatistics(&before);
      double cpu = CpuSeconds();
      auto start = Clock::now();
      for (int n = 0; n < iterations; ++n) {
        v8::HandleScope scope(isolate);
        if (operation == "--encode")
          observed ^= Encode(isolate, context, codec, original).size();
        else {
          auto value = Decode(isolate, context, codec, bytes, text);
          observed ^= value->IsObject();
        }
      }
      double elapsed =
          std::chrono::duration<double, std::milli>(Clock::now() - start)
              .count();
      cpu = CpuSeconds() - cpu;
      std::vector<v8::Global<v8::Value>> retained;
      if (operation != "--encode")
        for (int n = 0; n < 5; ++n) {
          v8::HandleScope scope(isolate);
          retained.emplace_back(isolate,
                                Decode(isolate, context, codec, bytes, text));
        }
      isolate->LowMemoryNotification();
      v8::HeapStatistics after;
      isolate->GetHeapStatistics(&after);
      double heap_delta = (static_cast<double>(after.used_heap_size()) -
                           before.used_heap_size()) /
                          5;
      std::cout << "{\"milliseconds\":" << elapsed
                << ",\"millisecondsPerOperation\":" << elapsed / iterations
                << ",\"cpuSeconds\":" << cpu << ",\"iterations\":" << iterations
                << ",\"retainedHeapBytesPerGraph\":" << heap_delta
                << ",\"peakRssBytes\":" << PeakRss()
                << ",\"observed\":" << observed << "}\n";
      for (auto& root : retained) root.Reset();
    }
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  return 0;
}
