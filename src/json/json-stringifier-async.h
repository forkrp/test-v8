// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_STRINGIFIER_ASYNC_H_
#define V8_JSON_JSON_STRINGIFIER_ASYNC_H_

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

#include "include/v8-primitive.h"
#include "src/objects/objects.h"
#include "src/objects/string.h"

namespace v8 {
namespace internal {

// Only native values cross to the worker. String offsets refer to owned copies,
// never V8 heap addresses. Memo slots are hints and require an exact span
// match.
struct JsonStringifyPart {
  enum Kind : uint8_t {
    kRaw8,
    kRaw16,
    kString8,
    kString16,
    kNumber,
    kNumbers,
    kIntegers,
    kInteger
  };
  union {
    double number;
    int32_t integer;
    struct {
      uint32_t offset, length;
    } span;
  } data{};
  uint8_t kind : 3 = kRaw8;
  uint8_t prefix_length : 3 = 0;
  uint8_t leading_comma : 1 = false;
  uint8_t memoize : 1 = false;
  uint8_t prefix[7]{};
};
static_assert(sizeof(JsonStringifyPart) == 16);

struct JsonStringifyData {
  std::atomic<bool> cancelled{false};
  std::deque<JsonStringifyPart> parts;
  std::vector<uint8_t> one_byte;
  std::vector<uint16_t> two_byte;
  std::vector<double> numbers;
  std::vector<int32_t> integers;
  std::unique_ptr<v8::String::ExternalOneByteStringResource> result8;
  std::unique_ptr<v8::String::ExternalStringResource> result16;
  size_t minimum_length = 0;
  size_t number_count = 0;
  bool wide_output = false;
  bool overflowed = false;

  bool AddLength(size_t length) {
    if (length > String::kMaxLength ||
        minimum_length > String::kMaxLength - length) {
      overflowed = true;
    }
    if (overflowed) return false;
    minimum_length += length;
    return true;
  }
  void Reset() {
    parts.clear();
    one_byte.clear();
    two_byte.clear();
    numbers.clear();
    integers.clear();
    minimum_length = number_count = 0;
    wide_output = overflowed = false;
  }
  size_t MemoryUsage() const {
    return parts.size() * sizeof(JsonStringifyPart) + one_byte.capacity() +
           two_byte.capacity() * sizeof(uint16_t) +
           numbers.capacity() * sizeof(double) +
           integers.capacity() * sizeof(int32_t);
  }
};

// true means captured output; undefined means stringify produces no text.
MaybeHandle<Object> CaptureJsonStringify(Isolate* isolate, Handle<Object> value,
                                         Handle<Object> replacer,
                                         Handle<Object> gap,
                                         JsonStringifyData* data);
void EncodeJsonStringify(JsonStringifyData* data);

}  // namespace internal
}  // namespace v8
#endif  // V8_JSON_JSON_STRINGIFIER_ASYNC_H_
