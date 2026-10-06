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
enum class MessagePackDecodeMode {
  kDirect,
  kVisitor,
  kNativeTree,
  kMPackReader
};

// Checked native bytes, with malloc ownership by default and optional page
// mappings for large allocations. Release reports the matching ownership kind;
// legacy recipients always receive memory that can be released with free().
class MessagePackBuffer {
 public:
  explicit MessagePackBuffer(bool use_pages = false) : use_pages_(use_pages) {}
  ~MessagePackBuffer();
  MessagePackBuffer(const MessagePackBuffer&) = delete;
  MessagePackBuffer& operator=(const MessagePackBuffer&) = delete;
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  // With a mapping_size recipient, ownership includes the native allocation
  // kind. Zero denotes malloc/free; nonzero denotes an OS mapping of that size.
  // Legacy recipients receive malloc-owned memory even if pages were enabled.
  uint8_t* Release(size_t* mapping_size = nullptr);
  void Clear();
  void Rewind(size_t size) {
    DCHECK_LE(size, size_);
    size_ = size;
  }
  uint8_t* Append(size_t count) {
    if (count <= capacity_ - size_) {
      uint8_t* result = data_ ? data_ + size_ : nullptr;
      size_ += count;
      return result;
    }
    return AppendSlow(count);
  }

 private:
  uint8_t* AppendSlow(size_t count);
  void FreeAllocation();
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  bool use_pages_ = false;
  bool mapped_ = false;
};

// Experimental engine-internal interface. Input is caller-owned immutable
// native memory, alive for the synchronous call. No third-party JS codec.
MaybeHandle<Object> DecodeMessagePack(
    Isolate* isolate, base::Vector<const uint8_t> input, std::string* error,
    MessagePackDecodeMode mode = MessagePackDecodeMode::kDirect,
    bool resource = false);
bool EncodeMessagePackResource(Isolate* isolate, Handle<Object> value,
                               MessagePackBuffer* output, std::string* error);
bool EncodeMessagePack(Isolate* isolate, Handle<Object> value,
                       std::vector<uint8_t>* output, std::string* error,
                       bool lossless_float32 = false);
bool EncodeMessagePack(Isolate* isolate, Handle<Object> value,
                       MessagePackBuffer* output, std::string* error,
                       bool lossless_float32 = false);
}  // namespace internal
}  // namespace v8
#endif  // V8_MSGPACK_MESSAGEPACK_H_
