// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_STRINGIFIER_CAPTURE_H_
#define V8_JSON_JSON_STRINGIFIER_CAPTURE_H_

#include <array>

#include "src/execution/isolate-inl.h"
#include "src/heap/local-heap.h"
#include "src/json/json-stringifier-async.h"
#include "src/objects/elements-kind.h"
#include "src/objects/objects-inl.h"
#include "src/objects/string-inl.h"

namespace v8 {
namespace internal {

// This adapter lives only during the caller-thread semantic traversal. Its raw
// identity hints are cleared on GC and never escape to background work.
class JsonStringifyCapture {
 public:
  JsonStringifyCapture(Isolate* isolate, JsonStringifyData* data)
      : isolate_(isolate), data_(data) {
    isolate_->main_thread_local_heap()->AddGCEpilogueCallback(ClearCache, this);
  }
  ~JsonStringifyCapture() {
    isolate_->main_thread_local_heap()->RemoveGCEpilogueCallback(ClearCache,
                                                                 this);
  }
  void Reset() {
    data_->Reset();
    prefix_length_ = 0;
    ClearCache(this);
  }
  void Finish() {
    if (prefix_length_ && !data_->overflowed) Push(JsonStringifyPart{});
  }
  bool overflowed() const { return data_->overflowed; }

  template <typename Char>
  void Literal(base::Vector<const Char> text) {
    if (text.empty() || !data_->AddLength(text.size())) return;
    bool narrow = true;
    if (text.size() + prefix_length_ <= sizeof(prefix_)) {
      if constexpr (sizeof(Char) > 1) {
        for (Char c : text) narrow = narrow && c <= 255;
      }
      if (narrow) {
        for (Char c : text) prefix_[prefix_length_++] = static_cast<uint8_t>(c);
        return;
      }
    }
    JsonStringifyPart part;
    if constexpr (sizeof(Char) == 1) {
      part.kind = JsonStringifyPart::kRaw8;
      part.data.span.offset = static_cast<uint32_t>(data_->one_byte.size());
      data_->one_byte.insert(data_->one_byte.end(), text.begin(), text.end());
    } else {
      part.kind = JsonStringifyPart::kRaw16;
      part.data.span.offset = static_cast<uint32_t>(data_->two_byte.size());
      data_->two_byte.insert(data_->two_byte.end(), text.begin(), text.end());
      data_->wide_output = true;
    }
    part.data.span.length = static_cast<uint32_t>(text.size());
    if (prefix_length_ == 0 && !data_->parts.empty()) {
      auto& previous = data_->parts.back();
      if (previous.kind == part.kind &&
          previous.data.span.offset + previous.data.span.length ==
              part.data.span.offset) {
        previous.data.span.length += part.data.span.length;
        return;
      }
    }
    Push(part);
  }

  void Number(double value) {
    if (!data_->AddLength(1)) return;
    JsonStringifyPart part;
    part.kind = JsonStringifyPart::kNumber;
    part.data.number = value;
    Push(part);
    ++data_->number_count;
  }

  template <ElementsKind kind, typename Elements>
  void Numbers(Tagged<Elements> elements, uint32_t start, uint32_t end) {
    size_t length = end - start;
    if (!data_->AddLength(length * 2 - (start == 0 ? 1 : 0))) return;
    JsonStringifyPart part;
    part.data.span.length = static_cast<uint32_t>(length);
    part.leading_comma = start != 0;
    if constexpr (IsSmiElementsKind(kind)) {
      part.kind = JsonStringifyPart::kIntegers;
      part.data.span.offset = static_cast<uint32_t>(data_->integers.size());
      for (uint32_t i = start; i < end; ++i) {
        data_->integers.push_back(Smi::cast(elements->get(i)).value());
      }
    } else {
      part.kind = JsonStringifyPart::kNumbers;
      part.data.span.offset = static_cast<uint32_t>(data_->numbers.size());
      for (uint32_t i = start; i < end; ++i) {
        data_->numbers.push_back(elements->get_scalar(i));
      }
    }
    data_->number_count += length;
    Push(part);
  }

  void StringValue(Handle<String> value, bool raw, bool key = false) {
    if (!BeginString(*value, raw, key)) return;
    value = String::Flatten(isolate_, value);
    DisallowGarbageCollection no_gc;
    CopyString(*value, raw, key, no_gc);
  }

  // Fast traversal already holds a no-GC scope and only passes flat strings.
  void StringValue(Tagged<String> value, bool raw, bool key,
                   const DisallowGarbageCollection& no_gc) {
    if (!BeginString(value, raw, key)) return;
    CopyString(value, raw, key, no_gc);
  }

 private:
  bool BeginString(Tagged<String> value, bool raw, bool key) {
    if (!data_->AddLength(value->length() + (raw ? 0 : 2))) return false;
    size_t slot = ((value->ptr() >> 4) & 63) + (key ? 0 : 64);
    if (!raw && cache_[slot].identity == value->ptr()) {
      Push(cache_[slot].part);
      return false;
    }
    return true;
  }

  void CopyString(Tagged<String> value, bool raw, bool key,
                  const DisallowGarbageCollection& no_gc) {
    DCHECK(value->IsFlat());
    JsonStringifyPart part;
    part.data.span.length = value->length();
    bool root = !raw && prefix_length_ == 0 && data_->parts.empty();
    if (String::IsOneByteRepresentationUnderneath(value)) {
      auto chars = value->GetCharVector<uint8_t>(no_gc);
      part.kind = raw ? JsonStringifyPart::kRaw8 : JsonStringifyPart::kString8;
      if (root) {
        data_->one_byte.reserve(chars.size() + 2);
        data_->one_byte.push_back('"');
      }
      part.data.span.offset = static_cast<uint32_t>(data_->one_byte.size());
      data_->one_byte.insert(data_->one_byte.end(), chars.begin(), chars.end());
      if (root) data_->one_byte.push_back('"');
    } else {
      auto chars = value->GetCharVector<uint16_t>(no_gc);
      part.kind =
          raw ? JsonStringifyPart::kRaw16 : JsonStringifyPart::kString16;
      if (root) {
        data_->two_byte.reserve(chars.size() + 2);
        data_->two_byte.push_back('"');
      }
      part.data.span.offset = static_cast<uint32_t>(data_->two_byte.size());
      data_->two_byte.insert(data_->two_byte.end(), chars.begin(), chars.end());
      if (root) data_->two_byte.push_back('"');
      data_->wide_output = true;
    }
    if (!raw) {
      size_t slot = ((value->ptr() >> 4) & 63) + (key ? 0 : 64);
      part.memoize = true;
      cache_[slot] = {value->ptr(), part};
    }
    Push(part);
  }

  void Push(JsonStringifyPart part) {
    part.prefix_length = prefix_length_;
    memcpy(part.prefix, prefix_, sizeof(prefix_));
    prefix_length_ = 0;
    data_->parts.push_back(part);
  }
  static void ClearCache(void* self) {
    auto* capture = static_cast<JsonStringifyCapture*>(self);
    for (auto& entry : capture->cache_) entry.identity = 0;
  }
  struct Entry {
    Address identity = 0;
    JsonStringifyPart part;
  };
  Isolate* const isolate_;
  JsonStringifyData* const data_;
  std::array<Entry, 128> cache_{};
  uint8_t prefix_[7]{};
  uint8_t prefix_length_ = 0;
};

// A null handle without an exception means the side-effect-free attempt was
// unsupported. The caller resets the snapshot and performs the generic walk.
MaybeHandle<Object> TryCaptureJsonStringifyFast(Isolate* isolate,
                                                Handle<Object> value,
                                                JsonStringifyCapture* capture);

}  // namespace internal
}  // namespace v8
#endif  // V8_JSON_JSON_STRINGIFIER_CAPTURE_H_
