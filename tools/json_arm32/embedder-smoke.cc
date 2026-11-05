// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Compile with the NDK's own C++ headers, not Chromium's libc++ headers. This
// exercises the public C++ API across the rebuilt runtime's ABI boundary.

#include <unistd.h>

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

#include "include/libplatform/libplatform.h"
#include "include/v8.h"

extern "C" __attribute__((visibility("default"))) int V8JsonArm32Smoke(
    const char* flags) {
  // Exercise the NDK-header caller's exception ABI and the linked unwinder.
  try {
    throw std::runtime_error("ARM32 unwind probe");
  } catch (const std::runtime_error& error) {
    if (std::string(error.what()) != "ARM32 unwind probe") return 6;
  }
  if (flags && *flags) v8::V8::SetFlagsFromString(flags);
  auto platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  if (!v8::V8::Initialize()) return 1;
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();
  v8::Isolate* isolate = v8::Isolate::New(params);
  int status = 2;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handles(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch caught(isolate);
    const char* source = R"JS(
      (async function() {
        const value = {text: 'a'.repeat(4096) + '\ud800',
                       wide: '\u6f22'.repeat(257), numbers: [1, 1.25, 5e-324]};
        const encoded = JSON.stringify(value);
        if (encoded !== await JSON.stringifyAsync(value)) throw 'stringify';
        const parsed = JSON.parse(encoded);
        const background = await JSON.parseAsync(encoded);
        if (parsed.text !== value.text || background.wide !== value.wide ||
            background.numbers[2] !== 5e-324) throw 'parse';
        if (!Object.is(await JSON.parseAsync('-0'), -0)) throw 'negative zero';
        let sum = 0;
        function add(a, b) { return a + b; }
        for (let i = 0; i < 50000; ++i) sum = add(sum, i);
        if (sum !== 1249975000) throw 'JIT';
        if (typeof WebAssembly !== 'undefined') {
          const bytes = new Uint8Array([0,97,115,109,1,0,0,0,
            1,5,1,96,0,1,127,3,2,1,0,7,5,1,1,102,0,0,
            10,6,1,4,0,65,42,11]);
          const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes));
          if (instance.exports.f() !== 42) throw 'Wasm';
        }
        return 42;
      })()
    )JS";
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (v8::Script::Compile(
            context, v8::String::NewFromUtf8(isolate, source).ToLocalChecked())
            .ToLocal(&script) &&
        script->Run(context).ToLocal(&result) && result->IsPromise()) {
      auto promise = result.As<v8::Promise>();
      for (int i = 0; i < 10000 && promise->State() == v8::Promise::kPending;
           ++i) {
        v8::platform::PumpMessageLoop(platform.get(), isolate);
        isolate->PerformMicrotaskCheckpoint();
        if (promise->State() == v8::Promise::kPending) usleep(1000);
      }
      if (promise->State() == v8::Promise::kFulfilled &&
          promise->Result()->Int32Value(context).FromMaybe(0) == 42) {
        status = 0;
      } else if (promise->State() == v8::Promise::kRejected) {
        v8::String::Utf8Value error(isolate, promise->Result());
        fprintf(stderr, "JSON embedder rejected: %s\n", *error);
      }
    }
    if (caught.HasCaught()) {
      v8::String::Utf8Value error(isolate, caught.Exception());
      fprintf(stderr, "JSON embedder exception: %s\n", *error);
    }
  }
  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  printf("JSON embedder smoke status=%d\n", status);
  return status;
}
