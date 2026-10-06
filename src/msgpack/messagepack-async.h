// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_MSGPACK_MESSAGEPACK_ASYNC_H_
#define V8_MSGPACK_MESSAGEPACK_ASYNC_H_
#include <atomic>
#include <memory>
#include <vector>

#include "src/msgpack/messagepack-string.h"
#include "src/msgpack/messagepack.h"
#include "src/objects/objects.h"

namespace v8::internal {
class BackingStore;
class PersistentHandles;
class JSPromise;
class JSObject;
class String;

// This data is owned native storage. Workers never receive heap references,
// handles, the caller's ArrayBuffer, or realm caches.
struct MessagePackEncodePart {
  enum Kind : uint32_t { kNumber, kNumbers, kIntegers, kString8, kString16 };
  uint32_t offset, length;
  Kind kind;
};
struct MessagePackToken {
  enum Kind : uint8_t {
    kOther,
    kNumber,
    kArray,
    kMap,
    kStringAscii,
    kString8,
    kString16,
    kDecodedString8,
    kDecodedString16
  };
  // A tag offset is strictly below the 256 MiB input limit.
  uint32_t offset : 28 = 0;
  uint32_t kind : 4 = kOther;
  uint32_t next = 0;
  union {
    double number = 0;
    struct {
      uint32_t length, auxiliary;
    } string;
    struct {
      uint32_t count : 30;
      uint32_t all_numbers : 1;
      uint32_t all_smis : 1;
    } container;
  };
  bool IsString() const { return kind >= kStringAscii; }
  bool DecodedString() const { return kind >= kDecodedString8; }
  void SetString(messagepack_strings::Utf8Info info, bool decoded) {
    kind = decoded      ? (info.one_byte ? kDecodedString8 : kDecodedString16)
           : info.ascii ? kStringAscii
           : info.one_byte ? kString8
                           : kString16;
    string = {static_cast<uint32_t>(info.length),
              static_cast<uint32_t>(info.ascii_prefix)};
  }
  messagepack_strings::Utf8Info StringInfo() const {
    messagepack_strings::Utf8Info info;
    info.length = static_cast<int>(string.length);
    info.ascii_prefix = static_cast<int>(string.auxiliary);
    info.ascii = kind == kStringAscii;
    info.one_byte =
        kind == kStringAscii || kind == kString8 || kind == kDecodedString8;
    return info;
  }
};
static_assert(sizeof(MessagePackToken) == 16);
// Fixed native blocks permit consumed tape storage to be released between
// foreground slices without moving token indices or retaining token pointers.
class MessagePackTape {
 public:
  static constexpr size_t kBlock = 4096;
  size_t size() const { return count_; }
  size_t base() const { return base_; }
  size_t capacity() const { return live_blocks_ * kBlock; }
  void reserve(size_t) {}  // Blocks grow on demand, rather than geometrically.
  void push_back(const MessagePackToken& token) {
    if (!(count_ % kBlock)) {
      blocks_.push_back(std::make_unique<MessagePackToken[]>(kBlock));
      ++live_blocks_;
    }
    (*this)[count_++] = token;
  }
  MessagePackToken& operator[](size_t index) {
    return blocks_[index / kBlock][index % kBlock];
  }
  const MessagePackToken& operator[](size_t index) const {
    return blocks_[index / kBlock][index % kBlock];
  }
  void DiscardBefore(size_t index) {
    while (base_ + kBlock <= index) {
      blocks_[base_ / kBlock].reset();
      base_ += kBlock;
      --live_blocks_;
    }
  }

 private:
  std::vector<std::unique_ptr<MessagePackToken[]>> blocks_;
  size_t count_ = 0, base_ = 0, live_blocks_ = 0;
};
struct MessagePackAsyncData {
  static constexpr size_t kNativeLimit = 256 * 1024 * 1024;
  std::atomic<bool> cancelled{false};
#if defined(V8_TARGET_OS_ANDROID) && defined(V8_TARGET_ARCH_ARM)
  MessagePackBuffer input{true};
  MessagePackBuffer decoded_strings{true};
  MessagePackBuffer output{true};
#else
  MessagePackBuffer input;
  MessagePackBuffer decoded_strings;
  MessagePackBuffer output;
#endif
  std::vector<MessagePackEncodePart> parts;
  MessagePackTape tokens;
  std::string error;
  bool encode = false;
  size_t MemoryUsage() const {
    return input.capacity() + decoded_strings.capacity() + output.capacity() +
           parts.capacity() * sizeof(MessagePackEncodePart) +
           tokens.capacity() * sizeof(MessagePackToken);
  }
};
struct MessagePackBuildFrame {
  uint32_t end, index = 0;
  Handle<JSObject> object;
  Handle<String> key;
  bool map;
};

bool CaptureMessagePack(Isolate*, Handle<Object>, MessagePackAsyncData*);
void RunMessagePackWorker(MessagePackAsyncData*);
// Returns true when complete (including errors). All roots belong to the
// foreground state; native token references never survive a call.
bool BuildMessagePackSlice(Isolate*, MessagePackAsyncData*, PersistentHandles*,
                           Handle<Object> result, uint32_t* cursor,
                           std::vector<MessagePackBuildFrame>* frames);
MaybeHandle<JSPromise> MessagePackAsync(Isolate*, Handle<Object>, bool encode);
bool HasPendingMessagePackTasks(Isolate*);
void CancelMessagePackTasks(Isolate*);
bool GetMessagePackInput(Isolate*, Handle<Object>,
                         std::shared_ptr<BackingStore>*, size_t* offset,
                         size_t* length);
// Common public error and independently owned output construction.
void ThrowMessagePackError(Isolate*, const std::string&, bool encode);
Handle<Object> MessagePackOutput(Isolate*, MessagePackBuffer*);
}  // namespace v8::internal
#endif  // V8_MSGPACK_MESSAGEPACK_ASYNC_H_
