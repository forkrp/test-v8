// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_MSGPACK_MESSAGEPACK_H_
#define V8_MSGPACK_MESSAGEPACK_H_
#include <cstdint>
#include <string>
#include <vector>

#include "src/base/vector.h"
#include "src/handles/handles.h"

namespace v8 {
namespace internal {
class Isolate;
class Object;
enum class MessagePackDecodeMode { kVisitor, kNativeTree, kMPackReader };

// Experimental engine-internal interface. Input is caller-owned immutable
// native memory, alive for the synchronous call. No third-party JS codec.
MaybeHandle<Object> DecodeMessagePack(
    Isolate* isolate, base::Vector<const uint8_t> input, std::string* error,
    MessagePackDecodeMode mode = MessagePackDecodeMode::kVisitor);
bool EncodeMessagePack(Isolate* isolate, Handle<Object> value,
                       std::vector<uint8_t>* output, std::string* error,
                       bool lossless_float32 = false);
}  // namespace internal
}  // namespace v8
#endif  // V8_MSGPACK_MESSAGEPACK_H_
