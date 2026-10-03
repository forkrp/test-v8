// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>

#include "src/base/vector.h"
#include "src/json/json-stringifier-async.h"
#include "src/json/json-stringifier-escape.h"
#include "src/numbers/conversions.h"

namespace v8 {
namespace internal {
namespace {

V8_NOINLINE bool ValidateUnescapedSurrogateSuffix(const uint16_t* begin,
                                                  const uint16_t* end) {
  while (begin != end) {
    uint16_t c = *begin++;
    if (c >= 0xd800 && c <= 0xdbff) {
      if (begin == end || *begin < 0xdc00 || *begin > 0xdfff) return false;
      ++begin;
    } else if (!JsonStringDoNotEscape(c)) {
      return false;
    }
  }
  return true;
}

template <typename Char, typename Storage = base::OwnedVector<Char>>
class JsonOutputResource final
    : public std::conditional_t<sizeof(Char) == 1,
                                v8::String::ExternalOneByteStringResource,
                                v8::String::ExternalStringResource> {
 public:
  using ViewChar = std::conditional_t<sizeof(Char) == 1, char, uint16_t>;
  JsonOutputResource(Storage buffer, size_t length)
      : buffer_(std::move(buffer)), length_(length) {}
  const ViewChar* data() const override {
    return reinterpret_cast<const ViewChar*>(buffer_.data());
  }
  size_t length() const override { return length_; }

 private:
  const Storage buffer_;
  const size_t length_;
};

template <typename Char>
class JsonNativeOutput {
 public:
  explicit JsonNativeOutput(JsonStringifyData* data) : data_(data) {}

  void Encode() {
    if (!ok()) return;
    if (AdoptUnescapedRoot()) return;
    Ensure(std::min<size_t>(String::kMaxLength,
                            data_->minimum_length + data_->number_count * 7));
    for (const auto& part : data_->parts) {
      if (!ok()) return;
      if (!Ensure(part.prefix_length)) return;
      for (uint8_t i = 0; i < part.prefix_length; ++i) {
        buffer_[size_++] = part.prefix[i];
      }
      using Part = JsonStringifyPart;
      switch (part.kind) {
        case Part::kRaw8:
        case Part::kString8:
          Text(part, data_->one_byte);
          break;
        case Part::kRaw16:
        case Part::kString16:
          Text(part, data_->two_byte);
          break;
        case Part::kNumber:
          Number(part.data.number);
          break;
        case Part::kNumbers:
          for (uint32_t i = 0; i < part.data.span.length; ++i) {
            if ((i & 4095) == 0 && !ok()) return;
            if (i != 0 || part.leading_comma) Append(',');
            Number(data_->numbers[part.data.span.offset + i]);
          }
          break;
        case Part::kIntegers:
          for (uint32_t i = 0; i < part.data.span.length; ++i) {
            if ((i & 4095) == 0 && !ok()) return;
            if (i != 0 || part.leading_comma) Append(',');
            char text[100];
            AppendString(IntToStringView(
                data_->integers[part.data.span.offset + i],
                base::Vector<char>(text, 100)));
          }
          break;
      }
    }
    if (!ok()) return;
    if constexpr (sizeof(Char) == 1) {
      data_->result8 =
          std::make_unique<JsonOutputResource<Char>>(std::move(buffer_), size_);
    } else {
      data_->result16 =
          std::make_unique<JsonOutputResource<Char>>(std::move(buffer_), size_);
    }
  }

  template <typename InputChar>
  V8_INLINE void Append(InputChar value) {
    if (size_ == buffer_.size() && !Ensure(1)) return;
    buffer_[size_++] = static_cast<Char>(value);
  }
  void AppendCString(const char* text) {
    AppendString(text);
  }
  void AppendString(std::string_view text) {
    size_t length = text.size();
    if (!Ensure(length)) return;
    for (size_t i = 0; i < length; ++i) {
      buffer_[size_++] = static_cast<Char>(text[i]);
    }
  }

 private:
  bool AdoptUnescapedRoot() {
    if (data_->parts.size() != 1) return false;
    const auto& part = data_->parts.front();
    constexpr auto kind = sizeof(Char) == 1 ? JsonStringifyPart::kString8
                                            : JsonStringifyPart::kString16;
    if (part.kind != kind || part.prefix_length || part.data.span.offset != 1) {
      return false;
    }
    auto& source = [this]() -> std::vector<Char>& {
      if constexpr (sizeof(Char) == 1) {
        return data_->one_byte;
      } else {
        return data_->two_byte;
      }
    }();
    size_t length = source.size();
    if (length != static_cast<size_t>(part.data.span.length) + 2) return false;
    // Capture reserves the quotes but only the worker decides whether the
    // string needs escaping. Immutable native storage can then be adopted.
    for (size_t i = 1; i + 1 < length;) {
      if (!ok()) return true;
      size_t count = std::min<size_t>(4096, length - i - 1);
      if constexpr (sizeof(Char) == 2) {
        if (i + count < length - 1 && source[i + count - 1] >= 0xd800 &&
            source[i + count - 1] <= 0xdbff) {
          --count;  // Validate a boundary pair together in the next chunk.
        }
      }
      size_t prefix = FindJsonEscape(source.data() + i, count);
      if (prefix != count) {
        if constexpr (sizeof(Char) == 1) {
          return false;
        } else {
          if (!ValidateUnescapedSurrogateSuffix(source.data() + i + prefix,
                                                source.data() + i + count)) {
            return false;
          }
        }
      }
      i += count;
    }
    auto result = std::make_unique<JsonOutputResource<Char, std::vector<Char>>>(
        std::move(source), length);
    if constexpr (sizeof(Char) == 1) {
      data_->result8 = std::move(result);
    } else {
      data_->result16 = std::move(result);
    }
    return true;
  }
  bool ok() const {
    return !data_->overflowed &&
           !data_->cancelled.load(std::memory_order_relaxed);
  }
  bool Ensure(size_t extra) {
    if (extra > String::kMaxLength || size_ > String::kMaxLength - extra) {
      data_->overflowed = true;
      return false;
    }
    if (size_ + extra <= buffer_.size()) return true;
    size_t capacity = std::max<size_t>(
        size_ + extra, std::max<size_t>(2048, buffer_.size() * 2));
    capacity = std::min<size_t>(capacity, String::kMaxLength);
    auto replacement = base::OwnedVector<Char>::NewForOverwrite(capacity);
    if (size_)
      std::memcpy(replacement.begin(), buffer_.begin(), size_ * sizeof(Char));
    buffer_ = std::move(replacement);
    return true;
  }
  void Number(double value) {
    if (!std::isfinite(value)) {
      AppendCString("null");
    } else {
      char text[100];
      AppendString(DoubleToStringView(value, base::Vector<char>(text, 100)));
    }
  }

  struct Memo {
    uint8_t kind = JsonStringifyPart::kRaw8;
    uint32_t source_offset = 0, source_length = 0;
    size_t start = 0, length = 0;
    bool valid = false;
  };

  template <typename InputChar>
  void Text(const JsonStringifyPart& part,
            const std::vector<InputChar>& source) {
    bool quoted = part.kind == JsonStringifyPart::kString8 ||
                  part.kind == JsonStringifyPart::kString16;
    size_t slot = (part.data.span.offset ^ (part.data.span.length * 31) ^
                   (static_cast<uint32_t>(part.kind) << 5)) &
                  127;
    Memo* memo = part.memoize ? &memo_[slot] : nullptr;
    if (memo && memo->valid && memo->kind == part.kind &&
        memo->source_offset == part.data.span.offset &&
        memo->source_length == part.data.span.length) {
      if (!Ensure(memo->length)) return;
      std::memcpy(buffer_.begin() + size_, buffer_.begin() + memo->start,
                  memo->length * sizeof(Char));
      size_ += memo->length;
      return;
    }
    size_t start = size_;
    if (quoted) Append('"');
    size_t position = 0;
    size_t length = part.data.span.length;
    while (position < length && ok()) {
      size_t count = std::min<size_t>(4096, length - position);
      const InputChar* begin = source.data() + part.data.span.offset + position;
      if (sizeof(InputChar) == 2 && position + count < length &&
          begin[count - 1] >= 0xD800 && begin[count - 1] <= 0xDBFF) {
        --count;  // Never split a surrogate pair at a cancellation boundary.
      }
      size_t prefix = quoted && count >= 32 ? FindJsonEscape(begin, count) : 0;
      if (!quoted || (count >= 32 && prefix == count)) {
        if (!Ensure(count)) return;
        if constexpr (sizeof(Char) == sizeof(InputChar)) {
          std::memcpy(buffer_.begin() + size_, begin, count * sizeof(Char));
          size_ += count;
        } else {
          for (size_t i = 0; i < count; ++i) Append(begin[i]);
        }
      } else {
        auto span = base::Vector<const InputChar>(begin, count);
        if (count * 6 <= String::kMaxLength - size_) {
          if (!Ensure(count * 6)) return;
          JsonStringSpanWriter<Char> writer{buffer_.begin() + size_};
          if (prefix >= 32) {
            WriteSparseString(span, prefix, &writer);
          } else {
            WriteJsonString<false>(span, &writer);
          }
          size_ = writer.cursor - buffer_.begin();
        } else {
          // A conservative reservation must not reject an actually fitting
          // output near the maximum length. Bounds-check this rare tail.
          WriteJsonString<false>(span, this);
        }
      }
      position += count;
    }
    if (!ok()) return;
    if (quoted) Append('"');
    if (memo) {
      *memo = {part.kind, part.data.span.offset, part.data.span.length,
               start,     size_ - start,         true};
    }
  }

  template <typename InputChar>
  V8_NOINLINE void WriteSparseString(base::Vector<const InputChar> span,
                                     size_t prefix,
                                     JsonStringSpanWriter<Char>* writer) {
    const InputChar* chars = span.begin();
    size_t remaining = span.size();
    while (true) {
      if constexpr (sizeof(Char) == sizeof(InputChar)) {
        std::memcpy(writer->cursor, chars, prefix * sizeof(Char));
        writer->cursor += prefix;
      } else {
        for (size_t i = 0; i < prefix; ++i) writer->Append(chars[i]);
      }
      chars += prefix;
      remaining -= prefix;
      if (!remaining) return;
      size_t escaped = 1;
      if constexpr (sizeof(InputChar) == 2) {
        if (chars[0] >= 0xd800 && chars[0] <= 0xdbff && remaining > 1 &&
            chars[1] >= 0xdc00 && chars[1] <= 0xdfff) {
          escaped = 2;
        }
      }
      WriteJsonString<false>(base::Vector<const InputChar>(chars, escaped),
                             writer);
      chars += escaped;
      remaining -= escaped;
      if (!remaining) return;
      prefix = FindJsonEscape(chars, remaining);
      if (prefix < 32) {
        // Dense escapes and short suffixes keep the existing scalar encoder.
        WriteJsonString<false>(base::Vector<const InputChar>(chars, remaining),
                               writer);
        return;
      }
    }
  }

  JsonStringifyData* const data_;
  base::OwnedVector<Char> buffer_;
  size_t size_ = 0;
  std::array<Memo, 128> memo_{};
};

}  // namespace

void EncodeJsonStringify(JsonStringifyData* data) {
  if (data->wide_output) {
    JsonNativeOutput<uint16_t>(data).Encode();
  } else {
    JsonNativeOutput<uint8_t>(data).Encode();
  }
  // The result owns its storage. Drop the semantic snapshot before notifying
  // the foreground, including when cancellation or output overflow occurred.
  std::deque<JsonStringifyPart>().swap(data->parts);
  std::vector<uint8_t>().swap(data->one_byte);
  std::vector<uint16_t>().swap(data->two_byte);
  std::vector<double>().swap(data->numbers);
  std::vector<int32_t>().swap(data->integers);
}

}  // namespace internal
}  // namespace v8
