// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#define MSGPACK_NO_BOOST
#include <cmath>
#include <cstring>
#include <limits>
#include <msgpack.hpp>
#include <stdexcept>

#include "src/base/bits.h"
#include "src/msgpack/messagepack-async.h"
#include "src/objects/smi.h"
#include "src/objects/string.h"

namespace v8::internal {
namespace {
constexpr uint64_t kSafeInteger = 9007199254740991ULL;
class Writer {
 public:
  explicit Writer(MessagePackBuffer* output) : output_(output) {}
  void write(const char* bytes, size_t n) {
    if (n) std::memcpy(output_->Append(n), bytes, n);
  }

 private:
  MessagePackBuffer* output_;
};
void Number(msgpack::packer<Writer>* packer, double n) {
  if (std::isfinite(n) && !(n == 0 && std::signbit(n)) && std::trunc(n) == n &&
      n >= -static_cast<double>(kSafeInteger) && n <= kSafeInteger) {
    if (n >= 0)
      packer->pack_uint64(static_cast<uint64_t>(n));
    else
      packer->pack_int64(static_cast<int64_t>(n));
  } else if (std::isfinite(n) &&
             std::fabs(n) <= std::numeric_limits<float>::max() &&
             static_cast<double>(static_cast<float>(n)) == n) {
    packer->pack_float(static_cast<float>(n));
  } else {
    packer->pack_double(n);
  }
}
bool Cancelled(MessagePackAsyncData* data) {
  return data->cancelled.load(std::memory_order_relaxed);
}
template <typename Char>
void String(MessagePackAsyncData* data, msgpack::packer<Writer>* packer,
            const uint8_t* bytes, size_t length) {
  constexpr size_t kChunk = 4096;
  Char scratch[kChunk];
  size_t count = length / sizeof(Char);
  if (count <= kChunk) {
    if (Cancelled(data)) return;
    if constexpr (sizeof(Char) == 1) {
      // A bounded width probe avoids an extra full ASCII scan on clearly
      // non-ASCII Latin-1 values. IsAscii still validates the entire span.
      if ((!count || ((bytes[0] | bytes[count / 2] | bytes[count - 1]) < 0x80)) &&
          messagepack_strings::IsAscii(bytes, count)) {
        packer->pack_str(static_cast<uint32_t>(count));
        if (count) std::memcpy(data->output.Append(count), bytes, count);
        return;
      }
    }
    const Char* chars;
    if constexpr (sizeof(Char) == 1) {
      chars = bytes;
    } else {
      // Captured code units follow variable-length wire prefixes and may be
      // unaligned. The bounded scratch span is aligned for UTF-16 routines.
      std::memcpy(scratch, bytes, length);
      chars = scratch;
    }
    size_t utf8;
    bool ascii;
    bool simd = messagepack_strings::UseSimdForEncoding(chars, count);
    bool ok = simd ? messagepack_strings::Utf8Length<Char, true>(
                         chars, count, &utf8, &ascii)
                   : messagepack_strings::Utf8Length<Char, false>(
                         chars, count, &utf8, &ascii);
    if (!ok)
      throw std::runtime_error(
          "Unpaired UTF-16 surrogate is outside the UTF-8 profile");
    packer->pack_str(static_cast<uint32_t>(utf8));
    uint8_t* output = data->output.Append(utf8);
    if constexpr (sizeof(Char) == 1) {
      if (ascii) {
        std::memcpy(output, chars, count);
        return;
      }
    }
    if (simd)
      messagepack_strings::EncodeUtf8<Char, true>(chars, count, output);
    else
      messagepack_strings::EncodeUtf8<Char, false>(chars, count, output);
    return;
  }
  size_t total = 0;
  std::vector<size_t> chunks;
  for (int pass = 0; pass < 2; ++pass) {
    size_t chunk = 0;
    for (size_t i = 0; i < count; ++chunk) {
      if (Cancelled(data)) return;
      size_t n = std::min(kChunk, count - i);
      const Char* chars;
      if constexpr (sizeof(Char) == 1) {
        chars = bytes + i;
      } else {
        std::memcpy(scratch, bytes + i * sizeof(Char), n * sizeof(Char));
        if (i + n < count && scratch[n - 1] >= 0xd800 &&
            scratch[n - 1] <= 0xdbff)
          --n;
        chars = scratch;
      }
      if (!pass) {
        size_t utf8;
        bool ascii;
        bool simd = messagepack_strings::UseSimdForEncoding(chars, n);
        bool ok = simd ? messagepack_strings::Utf8Length<Char, true>(
                             chars, n, &utf8, &ascii)
                       : messagepack_strings::Utf8Length<Char, false>(
                             chars, n, &utf8, &ascii);
        if (!ok)
          throw std::runtime_error(
              "Unpaired UTF-16 surrogate is outside the UTF-8 profile");
        total += utf8;
        if (total > MessagePackAsyncData::kNativeLimit)
          throw std::length_error("MessagePack output limit exceeded");
        chunks.push_back((utf8 << 2) | (simd ? 2 : 0) | (ascii ? 1 : 0));
      } else {
        size_t info = chunks[chunk];
        uint8_t* output = data->output.Append(info >> 2);
        if constexpr (sizeof(Char) == 1) {
          if (info & 1) {
            std::memcpy(output, chars, n);
            i += n;
            continue;
          }
        }
        if (info & 2)
          messagepack_strings::EncodeUtf8<Char, true>(chars, n, output);
        else
          messagepack_strings::EncodeUtf8<Char, false>(chars, n, output);
      }
      i += n;
    }
    if (!pass) packer->pack_str(static_cast<uint32_t>(total));
  }
}
void Encode(MessagePackAsyncData* data) {
  // A capture with no deferred parts is already complete wire bytes.
  // Foreground completion transfers that owned buffer directly.
  if (data->parts.empty()) return;
  Writer writer(&data->output);
  msgpack::packer<Writer> packer(writer);
  size_t cursor = 0;
  for (const auto& part : data->parts) {
    if (Cancelled(data)) return;
    writer.write(reinterpret_cast<const char*>(data->input.data() + cursor),
                 part.offset - cursor);
    const uint8_t* bytes = data->input.data() + part.offset;
    if (part.kind == MessagePackEncodePart::kString8) {
      String<uint8_t>(data, &packer, bytes, part.length);
    } else if (part.kind == MessagePackEncodePart::kString16) {
      String<uint16_t>(data, &packer, bytes, part.length);
    } else {
      size_t size = part.kind == MessagePackEncodePart::kIntegers ? 4 : 8;
      for (size_t i = 0; i < part.length; i += size) {
        if (!(i & 4095) && Cancelled(data)) return;
        if (size == 4) {
          int32_t n;
          std::memcpy(&n, bytes + i, 4);
          if (n >= 0)
            packer.pack_uint32(static_cast<uint32_t>(n));
          else
            packer.pack_int32(n);
        } else {
          double n;
          std::memcpy(&n, bytes + i, 8);
          Number(&packer, n);
        }
      }
    }
    cursor = part.offset + part.length;
  }
  writer.write(reinterpret_cast<const char*>(data->input.data() + cursor),
               data->input.size() - cursor);
}

class Parser {
 public:
  explicit Parser(MessagePackAsyncData* data) : data_(data) {}
  void Parse() {
    Value(0, false);
    if (!Cancelled(data_) && pos_ != data_->input.size())
      throw std::runtime_error("Trailing bytes after MessagePack value");
  }

 private:
  uint64_t Read(size_t n) {
    if (n > data_->input.size() - pos_)
      throw std::runtime_error("Truncated MessagePack input");
    const uint8_t* bytes = data_->input.data() + pos_;
    pos_ += n;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    switch (n) {
      case 1:
        return *bytes;
      case 2: {
        uint16_t word;
        std::memcpy(&word, bytes, 2);
        return base::bits::ReverseBytes(word);
      }
      case 4: {
        uint32_t word;
        std::memcpy(&word, bytes, 4);
        return base::bits::ReverseBytes(word);
      }
      case 8: {
        uint64_t word;
        std::memcpy(&word, bytes, 8);
        return base::bits::ReverseBytes(word);
      }
    }
#endif
    uint64_t bits = 0;
    for (size_t i = 0; i < n; ++i) bits = (bits << 8) | bytes[i];
    return bits;
  }
  uint32_t Length(uint8_t tag) {
    if ((tag & 0xe0) == 0xa0) return tag & 31;
    if ((tag & 0xf0) == 0x80 || (tag & 0xf0) == 0x90) return tag & 15;
    return static_cast<uint32_t>(
        Read(tag == 0xd9                                   ? 1
             : (tag == 0xda || tag == 0xdc || tag == 0xde) ? 2
                                                           : 4));
  }
  void Number(uint8_t tag, MessagePackToken* result) {
    auto& token = *result;
    if (tag <= 0x7f || tag >= 0xe0) {
      token.kind = MessagePackToken::kNumber;
      token.number = tag <= 0x7f ? tag : static_cast<int8_t>(tag);
      return;
    }
    size_t n = tag == 0xca   ? 4
               : tag == 0xcb ? 8
                             : size_t{1} << ((tag - 0xcc) & 3);
    uint64_t bits = Read(n);
    token.kind = MessagePackToken::kNumber;
    if (tag == 0xca) {
      uint32_t word = static_cast<uint32_t>(bits);
      float value;
      std::memcpy(&value, &word, 4);
      token.number = value;
    } else if (tag == 0xcb) {
      std::memcpy(&token.number, &bits, 8);
    } else if (tag >= 0xd0 && (bits & (uint64_t{1} << (n * 8 - 1)))) {
      if (n < 8) bits |= ~uint64_t{0} << (n * 8);
      int64_t value;
      std::memcpy(&value, &bits, 8);
      if (value < -static_cast<int64_t>(kSafeInteger))
        token.kind = MessagePackToken::kOther;
      else
        token.number = static_cast<double>(value);
    } else if (bits > kSafeInteger) {
      token.kind = MessagePackToken::kOther;
    } else {
      token.number = static_cast<double>(bits);
    }
  }
  void AppendNumber(const MessagePackToken& token) {
    // Workers append before any foreground tape discard. Check the complete
    // budget when a fixed block grows, rather than for every numeric scalar.
    if (!(data_->tokens.size() % MessagePackTape::kBlock) &&
        data_->MemoryUsage() > MessagePackAsyncData::kNativeLimit -
            MessagePackTape::kBlock * sizeof(MessagePackToken))
      throw std::length_error("MessagePack async native memory limit exceeded");
    data_->tokens.push_back(token);
  }
  void Value(size_t depth, bool key) {
    if (Cancelled(data_)) return;
    size_t index = data_->tokens.size();
    if ((index + 1) * sizeof(MessagePackToken) + data_->input.capacity() +
            data_->decoded_strings.capacity() >
        MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
    MessagePackToken token;
    token.offset = static_cast<uint32_t>(pos_);
    uint8_t tag = static_cast<uint8_t>(Read(1));
    bool string = (tag & 0xe0) == 0xa0 || (tag >= 0xd9 && tag <= 0xdb);
    if (key && !string)
      throw std::runtime_error("MessagePack object keys must be strings");
    if (data_->tokens.size() == data_->tokens.capacity()) {
      size_t limit =
          (MessagePackAsyncData::kNativeLimit - data_->input.capacity() -
           data_->decoded_strings.capacity()) /
          sizeof(MessagePackToken);
      data_->tokens.reserve(std::min(
          limit, std::max<size_t>(1024, data_->tokens.capacity() * 2)));
    }
    data_->tokens.push_back(token);
    if (data_->MemoryUsage() > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
    if (tag <= 0x7f || tag >= 0xe0) {
      token.kind = MessagePackToken::kNumber;
      token.number = tag <= 0x7f ? tag : static_cast<int8_t>(tag);
    } else if (string) {
      messagepack_strings::Utf8Info info;
      bool decoded_string = false;
      uint32_t count = Length(tag);
      if (count > data_->input.size() - pos_)
        throw std::runtime_error("Truncated MessagePack input");
      const uint8_t* bytes = data_->input.data() + pos_;
      StringMemo* memo = nullptr;
      if (!key && count > 64 && count <= 4096) {
        uint32_t hash = count * 16777619u;
        for (size_t i : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{7},
                         size_t{15}, size_t{31}})
          hash = (hash ^ bytes[i]) * 16777619u;
        hash ^= bytes[count - 1];
        memo = &strings_[hash & 127];
      }
      if (memo && memo->bytes && memo->length == count &&
          std::memcmp(memo->bytes, bytes, count) == 0) {
        info = memo->info;
        decoded_string = memo->decoded;
      } else {
        // Split at UTF-8 codepoint boundaries and retain aggregate width info.
        for (size_t i = 0; i < count;) {
          if (Cancelled(data_)) return;
          size_t n = std::min<size_t>(4096, count - i);
          if (i + n < count) {
            while (n && (bytes[i + n] & 0xc0) == 0x80) --n;
            if (!n)
              throw std::runtime_error("Invalid MessagePack UTF-8 string");
          }
          messagepack_strings::Utf8Info chunk_info;
          if (!messagepack_strings::ScanUtf8(bytes + i, n, &chunk_info))
            throw std::runtime_error("Invalid MessagePack UTF-8 string");
          if (info.ascii)
            info.ascii_prefix += chunk_info.ascii ? static_cast<int>(n)
                                                  : chunk_info.ascii_prefix;
          info.length += chunk_info.length;
          info.ascii &= chunk_info.ascii;
          info.one_byte &= chunk_info.one_byte;
          i += n;
        }
        if (info.length > String::kMaxLength)
          throw std::length_error("MessagePack string limit exceeded");
        // Transcode long value strings on the worker. Keys and small strings
        // retain the tuned foreground path; all staged code units are owned.
        if (!key && !info.ascii && count > 64) {
          size_t width = info.one_byte ? 1 : 2;
          size_t bytes_needed = static_cast<size_t>(info.length) * width;
          if (width == 2 && (data_->decoded_strings.size() & 1))
            data_->decoded_strings.Append(1)[0] = 0;
          size_t offset = data_->decoded_strings.size();
          uint8_t* decoded = data_->decoded_strings.Append(bytes_needed);
          if (data_->MemoryUsage() > MessagePackAsyncData::kNativeLimit)
            throw std::length_error(
                "MessagePack async native memory limit exceeded");
          size_t produced = 0;
          for (size_t i = 0; i < count;) {
            if (Cancelled(data_)) return;
            size_t n = std::min<size_t>(4096, count - i);
            if (i + n < count)
              while ((bytes[i + n] & 0xc0) == 0x80) --n;
            if (width == 1) {
              uint8_t* end = messagepack_strings::DecodeUtf8(
                  bytes + i, n, 0, decoded + produced);
              produced = static_cast<size_t>(end - decoded);
            } else {
              uint16_t* start = reinterpret_cast<uint16_t*>(decoded);
              uint16_t* end = messagepack_strings::DecodeUtf8(
                  bytes + i, n, 0, start + produced / 2);
              produced = static_cast<size_t>(end - start) * 2;
            }
            i += n;
          }
          DCHECK_EQ(produced, bytes_needed);
          // This field is a code-unit buffer offset for staged strings, and
          // remains an ASCII prefix length for the ordinary UTF-8 path.
          info.ascii_prefix = static_cast<int>(offset);
          decoded_string = true;
        }
        if (memo)
          *memo = {bytes, count, info, static_cast<bool>(decoded_string)};
      }
      token.SetString(info, decoded_string);
      pos_ += count;
    } else if ((tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf ||
               (tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd) {
      bool map = (tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf;
      token.kind = map ? MessagePackToken::kMap : MessagePackToken::kArray;
      token.container = {};
      token.container.all_numbers = token.container.all_smis = true;
      uint32_t count = Length(tag);
      if (depth >= 256)
        throw std::length_error("MessagePack nesting limit exceeded");
      if (count > (data_->input.size() - pos_) / (map ? 2 : 1))
        throw std::runtime_error("Malformed MessagePack container count");
      // The wire count is 32 bits. Validate it before narrowing to the tape's
      // 30-bit field; admitted input is at most 256 MiB, so a valid count fits.
      token.container.count = count;
      for (uint32_t i = 0; i < token.container.count; ++i) {
        if (Cancelled(data_)) return;
        if (map) Value(depth + 1, true);
        MessagePackToken number;
        const MessagePackToken* child;
        uint8_t next = pos_ < data_->input.size() ? data_->input.data()[pos_] : 0xc1;
        if (!map && (next <= 0x7f || next >= 0xe0 ||
                     (next >= 0xca && next <= 0xd3))) {
          number.offset = static_cast<uint32_t>(pos_++);
          Number(next, &number);
          number.next = static_cast<uint32_t>(data_->tokens.size() + 1);
          AppendNumber(number);
          child = &number;
        } else {
          size_t index = data_->tokens.size();
          Value(depth + 1, false);
          if (Cancelled(data_)) return;
          child = &data_->tokens[index];
        }
        const auto& value = *child;
        if (!map)
          token.container.all_numbers &=
              value.kind == MessagePackToken::kNumber;
        if (!map && token.container.all_smis)
          token.container.all_smis =
              value.kind == MessagePackToken::kNumber &&
              value.number >= Smi::kMinValue &&
              value.number <= Smi::kMaxValue &&
              std::trunc(value.number) == value.number &&
              !(value.number == 0 && std::signbit(value.number));
      }
    } else if (tag == 0xc0 || tag == 0xc2 || tag == 0xc3) {
      // These tags carry no body.
    } else if (tag >= 0xca && tag <= 0xd3) {
      Number(tag, &token);
    } else {
      throw std::runtime_error("Unsupported MessagePack value");
    }
    token.next = static_cast<uint32_t>(data_->tokens.size());
    data_->tokens[index] = token;
  }
  struct StringMemo {
    const uint8_t* bytes = nullptr;
    uint32_t length = 0;
    messagepack_strings::Utf8Info info;
    bool decoded = false;
  };
  StringMemo strings_[128]{};
  MessagePackAsyncData* data_;
  size_t pos_ = 0;
};
}  // namespace
void RunMessagePackWorker(MessagePackAsyncData* data) {
  try {
    if (data->encode)
      Encode(data);
    else
      Parser(data).Parse();
  } catch (const std::bad_alloc&) {
    data->error = "MessagePack native allocation failed";
  } catch (const std::exception& e) {
    data->error = e.what();
  } catch (...) {
    data->error = "Native MessagePack exception";
  }
}
}  // namespace v8::internal
