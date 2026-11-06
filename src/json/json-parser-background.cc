// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-parser-background.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include "include/v8-platform.h"
#include "src/base/bits.h"
#include "src/base/memory.h"
#include "src/base/small-vector.h"
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include "src/base/strings.h"
#include "src/json/json-array-scanner.h"
#include "src/json/json-number-parser.h"
#include "src/json/json-scanner.h"
#include "src/json/json-simd.h"
#include "src/numbers/conversions-inl.h"
#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
#include "src/base/page-allocator.h"
#include "src/utils/allocation.h"
#endif

namespace v8 {
namespace internal {
namespace {

template <typename Char>
V8_NOINLINE const Char* ScanBackgroundString(const Char* begin, const Char* end,
                                            uint32_t* bits) {
#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH)
  return ScanJsonStringDispatched(begin, end, bits);
#else
#if defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && !defined(__AARCH64EB__)
  using Word = uintptr_t;
  constexpr size_t kWordChars = sizeof(Word) / sizeof(Char);
  constexpr Word kLow = sizeof(Char) == 1 ? Word(-1) / 0xff
                                         : Word(-1) / 0xffff;
  constexpr Word kHigh = kLow * (sizeof(Char) == 1 ? 0x80 : 0x8000);
  if (static_cast<size_t>(end - begin) >= kWordChars) {
    Word input = base::ReadUnalignedValue<Word>(
        reinterpret_cast<base::Address>(begin));
    Word quote = input ^ (kLow * 0x22);
    Word slash = input ^ (kLow * 0x5c);
    Word special = ((input - kLow * 0x20) & ~input & kHigh) |
                   ((quote - kLow) & ~quote & kHigh) |
                   ((slash - kLow) & ~slash & kHigh);
    if (special) {
      // Borrow propagation can flag later lanes; the first flagged lane is
      // exact. Only characters before it may contribute to UTF-16 width.
      size_t count = base::bits::CountTrailingZerosNonZero(special) /
                     (8 * sizeof(Char));
      if constexpr (sizeof(Char) == 2) {
        for (size_t i = 0; i < count; ++i) *bits |= begin[i];
      }
      return begin + count;
    }
    if constexpr (sizeof(Char) == 2) {
      if (input & (kLow * 0xff00)) *bits |= 0x100;
    }
    begin += kWordChars;
  }
  constexpr size_t kVectorChars = 16 / sizeof(Char);
  while (static_cast<size_t>(end - begin) >= kVectorChars) {
    uint8x16_t mask;
    if constexpr (sizeof(Char) == 1) {
      auto input = vld1q_u8(begin);
      mask = vorrq_u8(vcltq_u8(input, vdupq_n_u8(0x20)),
                     vorrq_u8(vceqq_u8(input, vdupq_n_u8(0x22)),
                              vceqq_u8(input, vdupq_n_u8(0x5c))));
    } else {
      auto input = vld1q_u16(begin);
      auto special = vorrq_u16(vcltq_u16(input, vdupq_n_u16(0x20)),
                              vorrq_u16(vceqq_u16(input, vdupq_n_u16(0x22)),
                                        vceqq_u16(input, vdupq_n_u16(0x5c))));
      mask = vreinterpretq_u8_u16(special);
      auto merged = vorr_u8(vget_low_u8(mask), vget_high_u8(mask));
      if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) break;
      auto wide = vreinterpretq_u8_u16(vcgtq_u16(input, vdupq_n_u16(0xff)));
      merged = vorr_u8(vget_low_u8(wide), vget_high_u8(wide));
      if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) *bits |= 0x100;
      begin += kVectorChars;
      continue;
    }
    auto merged = vorr_u8(vget_low_u8(mask), vget_high_u8(mask));
    if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) break;
    begin += kVectorChars;
  }
#endif
  return ScanJsonStringCharacters(begin, end, bits);
#endif
}

// The synchronous frontend has already scanned its short-string prefix.
// Check four vectors together, then reuse the worker scanner to locate the
// first special character. Only completely accepted blocks affect width.
template <typename Char>
V8_NOINLINE const Char* ScanLongJsonString(const Char* begin, const Char* end,
                                           uint32_t* bits) {
#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH)
  return ScanJsonStringDispatched(begin, end, bits, true);
#else
#if defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && !defined(__AARCH64EB__)
  constexpr size_t kVectorChars = 16 / sizeof(Char);
  auto load = [](const Char* p) {
    if constexpr (sizeof(Char) == 1) {
      return vld1q_u8(p);
    } else {
      return vld1q_u16(p);
    }
  };
  auto special = [](auto input) -> uint8x16_t {
    if constexpr (sizeof(Char) == 1) {
      return vorrq_u8(vcltq_u8(input, vdupq_n_u8(0x20)),
                      vorrq_u8(vceqq_u8(input, vdupq_n_u8(0x22)),
                               vceqq_u8(input, vdupq_n_u8(0x5c))));
    } else {
      return vreinterpretq_u8_u16(
          vorrq_u16(vcltq_u16(input, vdupq_n_u16(0x20)),
                    vorrq_u16(vceqq_u16(input, vdupq_n_u16(0x22)),
                              vceqq_u16(input, vdupq_n_u16(0x5c)))));
    }
  };
  uint16x8_t width = vdupq_n_u16(0);
  while (static_cast<size_t>(end - begin) >= 4 * kVectorChars) {
    auto a = load(begin);
    auto b = load(begin + kVectorChars);
    auto c = load(begin + 2 * kVectorChars);
    auto d = load(begin + 3 * kVectorChars);
    auto mask = vorrq_u8(vorrq_u8(special(a), special(b)),
                         vorrq_u8(special(c), special(d)));
    auto merged = vorr_u8(vget_low_u8(mask), vget_high_u8(mask));
    if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) break;
    if constexpr (sizeof(Char) == 2) {
      width = vorrq_u16(width, vorrq_u16(vorrq_u16(a, b), vorrq_u16(c, d)));
    }
    begin += 4 * kVectorChars;
  }
  if constexpr (sizeof(Char) == 2) {
    auto wide = vreinterpretq_u8_u16(vcgtq_u16(width, vdupq_n_u16(0xff)));
    auto merged = vorr_u8(vget_low_u8(wide), vget_high_u8(wide));
    if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) *bits |= 0x100;
  }
#endif
  return ScanBackgroundString(begin, end, bits);
#endif
}

template <typename Char>
class BackgroundJsonParser {
 public:
  BackgroundJsonParser(BackgroundJsonData* output,
                       base::Vector<const Char> input,
                       const std::atomic<bool>& cancelled, uint32_t start = 0)
      : output_(output),
        input_(input),
        cancelled_(cancelled),
        position_(start) {}

  void ParseArrayPart(size_t start) {
    position_ = static_cast<uint32_t>(start);
    BackgroundJsonNode root;
    root.kind = BackgroundJsonNode::kArray;
    root.depth = 1;
    root.flags = BackgroundJsonNode::kAllNumbers |
                 BackgroundJsonNode::kAllSmis;
    stack_.push_back({Append(root, position_), 0, true});
    Parse<true>();
  }

  template <bool array_part = false>
  void Parse() {
    using Node = BackgroundJsonNode;
    // Grow by blocks, not by input bytes: a huge single string has one node.
    while (!Cancelled()) {
      SkipWhitespace();
      if (Cancelled()) return;
      uint32_t c = Current();
      if constexpr (array_part) {
        if (state_ == State::kArrayNext && stack_.size() == 1) {
          if (position_ == input_.size()) {
            output_->nodes.front().data.container.end =
                static_cast<uint32_t>(output_->nodes.size());
            return;
          }
          if (c == ']') {
            Fail();
            return;
          }
        }
      }
      switch (state_) {
        case State::kDone:
          if (position_ != input_.size()) {
            Fail(MessageTemplate::kJsonParseUnexpectedNonWhiteSpaceCharacter);
          }
          return;
        case State::kObjectFirstKey:
        case State::kObjectKey: {
          if (state_ == State::kObjectFirstKey && c == '}') {
            Close();
            break;
          }
          if (c != '"') {
            Fail(state_ == State::kObjectFirstKey
                     ? MessageTemplate::kJsonParseExpectedPropNameOrRBrace
                     : MessageTemplate::
                           kJsonParseExpectedDoubleQuotedPropertyName);
            return;
          }
          Node node;
          node.kind = Node::kKey;
          uint32_t start = position_;
          node.data.string.start = start;
          if (!ReadString<true>(&node)) return;
          Append(node, start);
          // A key must be followed by a colon and a value. Consume the colon
          // here instead of dispatching another state for every property.
          SkipWhitespace();
          if (Cancelled()) return;
          if (Current() != ':') {
            Fail(MessageTemplate::kJsonParseExpectedColonAfterPropertyName);
            return;
          }
          ++position_;
          SkipWhitespace();
          if (Cancelled()) return;
          c = Current();
          goto read_value;
        }
        case State::kObjectNext:
        case State::kArrayNext: {
          bool array = state_ == State::kArrayNext;
          if (c == ',') {
            ++position_;
            state_ = array ? State::kValue : State::kObjectKey;
          } else if (c == (array ? ']' : '}')) {
            Close();
          } else {
            Fail(array ? MessageTemplate::kJsonParseExpectedCommaOrRBrack
                       : MessageTemplate::kJsonParseExpectedCommaOrRBrace);
            return;
          }
          break;
        }
        case State::kArrayFirstValue:
          if (c == ']') {
            Close();
            break;
          }
          state_ = State::kValue;
          [[fallthrough]];
        case State::kValue: {
        read_value:
          Node node;
          uint32_t start = position_;
          switch (c) {
            case '{':
            case '[': {
              bool array = c == '[';
              node.kind = array ? Node::kArray : Node::kObject;
              node.depth = 1;
              if (array) node.flags |= Node::kAllNumbers | Node::kAllSmis;
              ++position_;
              uint32_t index = static_cast<uint32_t>(output_->nodes.size());
              if (Current() == (array ? ']' : '}')) {
                ++position_;
                node.data.container.end = index + 1;
                Append(node, start);
                CompleteContainer(node.depth);
                continue;
              }
              stack_.push_back({Append(node, start), index, array});
              state_ = array ? State::kArrayFirstValue : State::kObjectFirstKey;
              continue;
            }
            case '"':
              node.kind = Node::kString;
              node.data.string.start = start;
              if (!ReadString<false>(&node)) return;
              break;
            case 't':
              node.kind = Node::kTrue;
              if (!Literal("true")) return;
              break;
            case 'f':
              node.kind = Node::kFalse;
              if (!Literal("false")) return;
              break;
            case 'n':
              node.kind = Node::kNull;
              if (!Literal("null")) return;
              break;
            default:
              if (c != '-' && !Digit(c)) {
                Fail();
                return;
              }
              node.kind = Node::kNumber;
              double number;
              if (!ReadNumber(&number)) return;
              node.SetNumber(number);
              if (IsSmiDouble(number)) {
                node.flags |= Node::kSmi;
                consecutive_doubles_ = 0;
              } else if (consecutive_doubles_ < 2) {
                ++consecutive_doubles_;
              }
          }
          Append(node, start);
          CompleteValue(node.kind == Node::kNumber, node.flags & Node::kSmi);
          break;
        }
      }
    }
  }

 private:
  enum class State {
    kValue,
    kObjectFirstKey,
    kObjectKey,
    kObjectNext,
    kArrayFirstValue,
    kArrayNext,
    kDone
  };
  struct Frame {
    BackgroundJsonNode* node;
    uint32_t source_index;
    bool array;
    uint8_t key_count = 0;
    bool cached_keys = false;
  };

  bool Cancelled() const {
    return cancelled_.load(std::memory_order_relaxed);
  }
  uint32_t Current() const {
    return position_ < input_.size() ? input_[position_] : UINT32_MAX;
  }
  static bool Digit(uint32_t c) { return IsJsonDecimalDigit(c); }
  void Fail(base::Optional<MessageTemplate> message = base::nullopt) {
    output_->failed = true;
    output_->error_position = static_cast<int>(position_);
    output_->error_message = message;
  }
  void SkipWhitespace() {
    while (position_ < input_.size()) {
      uint32_t c = input_[position_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++position_;
      if ((position_ & 4095) == 0 && Cancelled()) return;
    }
  }
  V8_INLINE BackgroundJsonNode* Append(const BackgroundJsonNode& node,
                                        uint32_t start) {
    output_->nodes.push_back(node);
    if (output_->track_source) {
      output_->source_starts.push_back(start);
      output_->source_ends.push_back(position_);
    }
    // Appending to the tape preserves pointers to existing nodes.
    return &output_->nodes.back();
  }
  void CompleteValue(bool number = false, bool smi = false) {
    if (stack_.empty()) {
      state_ = State::kDone;
    } else {
      const auto& frame = stack_.back();
      auto& parent = *frame.node;
      ++parent.data.container.count;
      if (frame.array) {
        if (!number) parent.flags &= ~BackgroundJsonNode::kAllNumbers;
        if (!smi) parent.flags &= ~BackgroundJsonNode::kAllSmis;
      }
      // Compact JSON usually places a comma immediately after the value.
      // Whitespace, closing delimiters, and errors keep the normal state.
      if (Current() == ',') {
        ++position_;
        state_ = frame.array ? State::kValue : State::kObjectKey;
      } else {
        state_ = frame.array ? State::kArrayNext : State::kObjectNext;
      }
    }
  }
  void Close() {
    ++position_;
    auto* node = stack_.back().node;
    node->data.container.end =
        static_cast<uint32_t>(output_->nodes.size());
    if (stack_.back().cached_keys) {
      node->flags |= BackgroundJsonNode::kHasCachedKeys;
    }
    if (output_->track_source)
      output_->source_ends[stack_.back().source_index] = position_;
    stack_.pop_back();
    CompleteContainer(node->depth);
  }
  void CompleteContainer(uint16_t depth) {
    if (!stack_.empty()) {
      auto& parent = *stack_.back().node;
      parent.depth = std::max<uint16_t>(
          parent.depth, std::min<uint16_t>(17, depth + 1));
    }
    CompleteValue();
  }
  bool Literal(const char* literal) {
    for (; *literal; ++literal, ++position_) {
      if (Current() != static_cast<uint32_t>(*literal)) {
        Fail();
        return false;
      }
    }
    return true;
  }

  bool TryReadDirectNumber(uint32_t start, double* number) {
    // Keep long literals on the scanner's cancellation checks. A prefix at
    // the window boundary is only complete at the actual end of the input.
    constexpr size_t kNumberWindow = 128;
    const Char* first = input_.begin() + start;
    const Char* limit = first + std::min(kNumberWindow, input_.size() - start);
    const Char* end = TryParseJsonNumber(first, limit, number);
    if (end == nullptr || (end == limit && limit != input_.end())) return false;
    position_ = static_cast<uint32_t>(end - input_.begin());
    return true;
  }

  bool ReadNumber(double* number) {
    // Two successive non-Smis predict a run of doubles without penalizing
    // alternating Smi/double input with a conversion attempt on every Smi.
    // Failed conversions leave the cursor unchanged for the regular scanner.
    if (consecutive_doubles_ >= 2 && TryReadDirectNumber(position_, number)) {
      return true;
    }
    uint32_t start = position_;
    bool negative = Current() == '-';
    if (negative) ++position_;
    uint32_t digits = 0;
    uint64_t significand = 0;
    if (Current() == '0') {
      ++position_;
      ++digits;
      if (Digit(Current())) {
        Fail();
        return false;
      }
    } else {
      if (!Digit(Current())) {
        Fail(MessageTemplate::kJsonParseNoNumberAfterMinusSign);
        return false;
      }
      while (Digit(Current())) {
        if (++digits <= 15) significand = significand * 10 + Current() - '0';
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
    }
    if (digits > 15 || Current() == '.' || Current() == 'e' ||
        Current() == 'E') {
      if (TryReadDirectNumber(start, number)) return true;
    }
    bool fast_decimal = digits <= 15;
    int exponent = 0;
    if (Current() == '.') {
      ++position_;
      if (!Digit(Current())) {
        Fail(MessageTemplate::kJsonParseUnterminatedFractionalNumber);
        return false;
      }
      while (Digit(Current())) {
        if (++digits <= 15) {
          significand = significand * 10 + Current() - '0';
          --exponent;
        } else {
          fast_decimal = false;
        }
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
    }
    if (Current() == 'e' || Current() == 'E') {
      ++position_;
      bool negative_exponent = Current() == '-';
      if (Current() == '+' || Current() == '-') ++position_;
      if (!Digit(Current())) {
        Fail(MessageTemplate::kJsonParseExponentPartMissingNumber);
        return false;
      }
      int parsed_exponent = 0;
      int exponent_digits = 0;
      while (Digit(Current())) {
        if (++exponent_digits <= 3) {
          parsed_exponent = parsed_exponent * 10 + Current() - '0';
        } else {
          fast_decimal = false;
        }
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
      exponent += negative_exponent ? -parsed_exponent : parsed_exponent;
    }
    if (!fast_decimal ||
        !TryJsonDecimalToDouble(significand, exponent, negative, number)) {
      *number = StringToDouble(input_.SubVector(start, position_),
                               NO_CONVERSION_FLAGS,
                               std::numeric_limits<double>::quiet_NaN());
    }
    return true;
  }

  template <bool is_key>
  bool ReadString(BackgroundJsonNode* node) {
    ++position_;
    uint32_t key_slot = JsonString::kNoCacheSlot;
    if constexpr (is_key) {
      auto& frame = stack_.back();
      DCHECK(!frame.array);
      if (!output_->track_source && frame.key_count < key_strings_.size()) {
        // Same-shaped records repeat keys at the same depth and field ordinal.
        // Exact matching replaces scanning on a hit, rather than rescanning
        // and hashing a key that was already validated in this document.
        // Wide objects use the generic foreground builder; do not repeatedly
        // probe the same full table for all of their remaining properties.
        key_slot = static_cast<uint32_t>(
            (stack_.size() * 8 + frame.key_count++) &
            (key_strings_.size() - 1));
        if (key_strings_[key_slot].kind == BackgroundJsonNode::kKey) {
          const auto& previous = key_strings_[key_slot];
          uint32_t length = previous.data.string.length;
          if (input_.size() - position_ > length &&
              input_[position_ + length] == '"' &&
              EqualKey(input_.begin() + previous.data.string.start + 1,
                       input_.begin() + position_, length * sizeof(Char))) {
            node->data.string.length = length;
            node->flags = previous.flags;
            node->depth = static_cast<uint16_t>(key_slot);
            frame.cached_keys = true;
            position_ += length + 1;
            return true;
          }
        }
      }
    }
    if constexpr (!is_key) {
      // Keep short ordinary values in the parser loop. Longer strings and
      // escapes use the outlined decoder without enlarging the numeric loop.
      const Char* begin = input_.begin() + position_;
      const Char* end = begin + std::min<size_t>(32, input_.size() - position_);
      uint32_t bits = 0;
      const Char* stop = ScanJsonStringCharacters(begin, end, &bits);
      if (stop != end && *stop == '"') {
        using Node = BackgroundJsonNode;
        uint32_t length = static_cast<uint32_t>(stop - begin);
        position_ += length + 1;
        node->data.string.length = length;
        bool convert = sizeof(Char) == 1 ? bits > 255 : bits <= 255;
        node->flags = (convert ? Node::kConvert : 0) |
                      ((sizeof(Char) == 1 && length < 10)
                           ? Node::kInternalize
                           : 0);
        if (length < 10) CacheShortString(node);
        return true;
      }
    }
    return ReadStringFallback<is_key>(node, key_slot);
  }

  template <bool is_key, bool use_vectors = true>
  V8_NOINLINE bool ReadStringFallback(
      BackgroundJsonNode* node, uint32_t key_slot, uint32_t length = 0,
      uint32_t bits = 0, uint32_t index = 0, bool is_index = is_key,
      bool leading_zero = false, bool escaped = false) {
    using Node = BackgroundJsonNode;
    // Value strings never carry property-index state, including a scalar
    // handoff after an escape. Keep index processing out of that specialization.
    if constexpr (!is_key) is_index = false;
    auto add_index_char = [&](uint32_t c, uint32_t at) {
      if (!is_index) return;
      if (!Digit(c) || (at != 0 && leading_zero) ||
          index > (UINT32_MAX - 1 - (c - '0')) / 10) {
        is_index = false;
      } else {
        index = index * 10 + c - '0';
        if (at == 0) leading_zero = c == '0';
      }
    };
    while (position_ < input_.size()) {
      if (Cancelled()) return false;
      const Char* begin = input_.begin() + position_;
      const Char* end =
          input_.begin() + std::min<size_t>(position_ + 4096, input_.size());
      const Char* stop;
#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH) ||            \
    (defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && \
     !defined(__AARCH64EB__))
      if constexpr (use_vectors) {
        const Char* prefix_end = begin + std::min<ptrdiff_t>(32, end - begin);
        stop = ScanJsonStringCharacters(begin, prefix_end, &bits);
        if (stop == prefix_end && prefix_end != end) {
          stop = ScanBackgroundString(stop, end, &bits);
        }
      } else {
        stop = ScanJsonStringCharacters(begin, end, &bits);
      }
#else
      stop = ScanJsonStringCharacters(begin, end, &bits);
#endif
      for (const Char* p = begin; p != stop && is_index; ++p)
        add_index_char(*p, length + static_cast<uint32_t>(p - begin));
      uint32_t count = static_cast<uint32_t>(stop - begin);
      position_ += count;
      length += count;
      if (stop == end) continue;
      uint32_t c = Current();
      if (c == '"') {
        ++position_;
        node->data.string.length = length;
        if (is_index && length != 0) node->data.string.start = index;
        bool convert = sizeof(Char) == 1 ? bits > 255 : bits <= 255;
        node->flags =
            (convert ? Node::kConvert : 0) | (escaped ? Node::kEscape : 0) |
            ((is_key || (sizeof(Char) == 1 && length < 10))
                 ? Node::kInternalize
                 : 0) |
            ((is_index && length != 0) ? Node::kIndex : 0);
        if (!escaped) {
          if constexpr (is_key) {
            if (key_slot != JsonString::kNoCacheSlot && !is_index &&
                length != 0 && length <= 32 &&
                key_strings_[key_slot].kind != Node::kKey) {
              // Never replace a slot already referenced by another tape key.
              node->flags |= Node::kCachedString;
              node->depth = static_cast<uint16_t>(key_slot);
              key_strings_[key_slot] = *node;
              stack_.back().cached_keys = true;
            }
          } else if (length < 10) {
            CacheShortString(node);
          }
        }
        return true;
      }
      if (c < 0x20) {
        Fail(MessageTemplate::kJsonParseBadControlCharacter);
        return false;
      }
      if (c == '\\') {
        escaped = true;
        ++position_;
        c = Current();
        if (c > 255) {
          Fail();
          return false;
        }
        switch (c) {
          case '"':
          case '/':
          case '\\':
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          case 'u': {
            c = 0;
            for (int i = 0; i < 4; ++i) {
              ++position_;
              int digit = base::HexValue(Current());
              if (digit < 0) {
                Fail(MessageTemplate::kJsonParseBadUnicodeEscape);
                return false;
              }
              c = c * 16 + digit;
            }
            break;
          }
          default:
            Fail(MessageTemplate::kJsonParseBadEscapedCharacter);
            return false;
        }
#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH) ||            \
    (defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && \
     !defined(__AARCH64EB__))
        if constexpr (use_vectors) {
          if (count < 32) {
            // A dense escape run switches once to a separately compiled scalar
            // loop. The loop itself has no vector-probing branch.
            bits |= c;
            add_index_char(c, length);
            ++length;
            ++position_;
            return ReadStringFallback<is_key, false>(
                node, key_slot, length, bits, index, is_index, leading_zero,
                escaped);
          }
        }
#endif
      }
      bits |= c;
      add_index_char(c, length);
      ++length;
      ++position_;
    }
    Fail(MessageTemplate::kJsonParseUnterminatedString);
    return false;
  }

  static V8_INLINE bool EqualKey(const Char* left, const Char* right,
                                 size_t bytes) {
    auto a = reinterpret_cast<base::Address>(left);
    auto b = reinterpret_cast<base::Address>(right);
    // Read exactly the validated key length, including at the input boundary.
    // Equality of same-sized words is independent of the host byte order.
    switch (bytes) {
      case 1:
        return base::ReadUnalignedValue<uint8_t>(a) ==
               base::ReadUnalignedValue<uint8_t>(b);
      case 2:
        return base::ReadUnalignedValue<uint16_t>(a) ==
               base::ReadUnalignedValue<uint16_t>(b);
      case 4:
        return base::ReadUnalignedValue<uint32_t>(a) ==
               base::ReadUnalignedValue<uint32_t>(b);
      case 8:
        return base::ReadUnalignedValue<uint64_t>(a) ==
               base::ReadUnalignedValue<uint64_t>(b);
      default:
        return std::memcmp(left, right, bytes) == 0;
    }
  }

  void CacheShortString(BackgroundJsonNode* node) {
    // Keys and values have separate bounded tables: a cached value need not
    // be internalized, whereas the object builder requires internalized keys.
    // Never replace a slot already referenced by the tape on collision.
    auto& slots = short_strings_;
    const Char* chars = input_.begin() + node->data.string.start + 1;
    uint32_t hash = node->data.string.length;
    for (uint32_t i = 0; i < node->data.string.length; ++i)
      hash = hash * 31 + chars[i];
    uint32_t slot = hash & (slots.size() - 1);
    if (slots[slot].kind == BackgroundJsonNode::kString) {
      const auto& previous = slots[slot];
      if (previous.data.string.length != node->data.string.length) return;
      const Char* previous_chars =
          input_.begin() + previous.data.string.start + 1;
      size_t bytes = node->data.string.length * sizeof(Char);
      if (std::memcmp(previous_chars, chars, bytes) != 0) return;
    }
    node->flags |= BackgroundJsonNode::kCachedString;
    node->depth = static_cast<uint16_t>(slot);
    if (slots[slot].kind != BackgroundJsonNode::kString) slots[slot] = *node;
  }

  BackgroundJsonData* const output_;
  base::Vector<const Char> input_;
  const std::atomic<bool>& cancelled_;
  uint32_t position_;
  State state_ = State::kValue;
  uint8_t consecutive_doubles_ = 0;
  // Parallel workers frequently update shallow container frames. Keep them on
  // each worker's stack: tiny heap buffers can share a cache line across jobs.
  base::SmallVector<Frame, 16> stack_;
  // Cache validated metadata directly: hits need no deque lookup. The tape's
  // cache-slot format and first-entry-on-collision rule stay the same.
  std::array<BackgroundJsonNode, 64> short_strings_{};
  std::array<BackgroundJsonNode, 64> key_strings_{};
};

#if (defined(V8_OS_MACOS) && defined(V8_HOST_ARCH_ARM64) && \
     defined(__ARM_NEON)) ||                              \
    (defined(V8_OS_ANDROID) &&                            \
     (defined(V8_HOST_ARCH_ARM) || defined(V8_HOST_ARCH_ARM64)))
struct ArrayPart {
  size_t begin = 0;
  size_t end = 0;
  std::unique_ptr<BackgroundJsonData> data;
};

constexpr size_t kMaxArrayParts = 8;

struct ArrayPartsWork {
  void Publish(size_t begin, size_t end) {
    const size_t index = ready.load(std::memory_order_relaxed);
    DCHECK_LT(index, parts.size());
    parts[index] = {begin, end, std::make_unique<BackgroundJsonData>(false)};
    ready.store(index + 1, std::memory_order_release);
  }

  std::array<ArrayPart, kMaxArrayParts> parts;
  std::atomic<size_t> ready{0};
  std::atomic<size_t> next{0};
};

template <typename Char>
class ParseArrayPartsJob final : public JobTask {
 public:
  ParseArrayPartsJob(base::Vector<const Char> input,
                     const std::atomic<bool>& cancelled, ArrayPartsWork& work)
      : input_(input), cancelled_(cancelled), work_(work) {}

  void Run(JobDelegate* delegate) override {
    while (!delegate->ShouldYield()) {
      size_t index = work_.next.load(std::memory_order_relaxed);
      if (index >= work_.ready.load(std::memory_order_acquire)) return;
      if (!work_.next.compare_exchange_weak(index, index + 1,
                                            std::memory_order_relaxed)) {
        continue;
      }
      const auto& part = work_.parts[index];
      BackgroundJsonParser<Char>(part.data.get(),
                                 input_.SubVector(0, part.end), cancelled_)
          .ParseArrayPart(part.begin);
    }
  }

  size_t GetMaxConcurrency(size_t worker_count) const override {
    const size_t ready = work_.ready.load(std::memory_order_acquire);
    const size_t next = work_.next.load(std::memory_order_relaxed);
    return worker_count + ready - std::min(ready, next);
  }

 private:
  base::Vector<const Char> input_;
  const std::atomic<bool>& cancelled_;
  ArrayPartsWork& work_;
};

template <typename Char>
bool TryParseArrayInParallel(BackgroundJsonData* data, Platform* platform,
                             base::Vector<const Char> input,
                             uint32_t* parse_start) {
#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
  // Large two-byte copies and parallel tapes exceeded the ARM32 peak-memory
  // budget on device. Keep these inputs on the serial path.
  if constexpr (sizeof(Char) == 2) return false;
#endif
  if (data->track_source || !data->allow_parallel ||
      input.size() < 4 * 1024 * 1024 ||
      platform->NumberOfWorkerThreads() < 2) {
    return false;
  }
  // Retain offsets into the original input for strings and error reporting.
  size_t array_begin = 0;
  while (array_begin < input.size() && IsJsonArraySpace(input[array_begin])) {
    ++array_begin;
    if ((array_begin & 4095) == 0 &&
        data->cancelled.load(std::memory_order_relaxed)) {
      return false;
    }
  }
  *parse_start = static_cast<uint32_t>(array_begin);
  if (array_begin == input.size() || input[array_begin] != '[') return false;
#if defined(V8_OS_ANDROID)
  {
    // Sparse arrays, such as one large string or payload object, can spend
    // more time finding cuts than parsing. Reuse the native grammar for a
    // bounded density probe; its partial result is never materialized.
    BackgroundJsonData probe(false);
    const size_t probe_end = std::min(input.size(), array_begin + 4096);
    BackgroundJsonParser<Char>(&probe, input.SubVector(0, probe_end),
                               data->cancelled, *parse_start)
        .Parse();
    if (probe.NodeCount() < 32) return false;
  }
#endif
  ArrayPartsWork work;
  std::unique_ptr<JobHandle> job;
  size_t begin = array_begin + 1;
  auto publish = [&](size_t end) {
    work.Publish(begin, end);
    begin = end + 1;
    if (!job) {
      job = platform->CreateJob(
          TaskPriority::kUserVisible,
          std::make_unique<ParseArrayPartsJob<Char>>(input, data->cancelled,
                                                    work));
    }
    job->NotifyConcurrencyIncrease();
  };
  // More ranges let workers start earlier and shorten the last range after
  // the scan. Reuse the same platform worker pool.
  size_t part_limit = input.size() >= 16 * 1024 * 1024 ? kMaxArrayParts : 4;
  auto publish_cut = [](void* context, size_t end) {
    (*static_cast<decltype(publish)*>(context))(end);
  };
#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH)
  auto scan = [] {
    if constexpr (sizeof(Char) == 1) {
      return GetJsonSimdFunctions().array_cuts8;
    } else {
      return GetJsonSimdFunctions().array_cuts16;
    }
  }();
#else
  auto scan = FindJsonArrayCuts<Char>;
#endif
  size_t end = scan(input.begin(), input.size(), array_begin, data->cancelled,
                    part_limit, &publish, publish_cut);
  if (end == 0 || !job) {
    if (job) job->Join();
    return false;
  }
  work.Publish(begin, end);
  job->Join();
  if (data->cancelled.load(std::memory_order_relaxed)) return true;
  const size_t count = work.ready.load(std::memory_order_relaxed);
  for (size_t i = 0; i < count; ++i) {
    const auto& part = work.parts[i].data;
    if (part->failed || part->nodes.empty() ||
        part->nodes.front().data.container.end != part->nodes.size()) {
      return false;
    }
  }
  data->nodes.swap(work.parts[0].data->nodes);
  auto& root = data->nodes.front();
  size_t total_nodes = data->nodes.size();
  for (size_t i = 1; i < count; ++i) {
    auto& nodes = work.parts[i].data->nodes;
    const auto& part_root = nodes.front();
    root.flags &= part_root.flags;
    root.depth = std::max(root.depth, part_root.depth);
    root.data.container.count += part_root.data.container.count;
    data->part_boundaries.push_back(total_nodes);
    nodes.pop_front();
    total_nodes += nodes.size();
    data->remaining_parts.push_back(std::move(nodes));
  }
  root.data.container.end = static_cast<uint32_t>(total_nodes);
  data->total_nodes = total_nodes;
  return true;
}
#endif  // Supported parallel-array platforms

template <typename Char>
void ParseBackgroundInput(BackgroundJsonData* data, Platform* platform,
                          base::Vector<const Char> input) {
  uint32_t start = 0;
#if (defined(V8_OS_MACOS) && defined(V8_HOST_ARCH_ARM64) && \
     defined(__ARM_NEON)) ||                              \
    (defined(V8_OS_ANDROID) &&                            \
     (defined(V8_HOST_ARCH_ARM) || defined(V8_HOST_ARCH_ARM64)))
  if (TryParseArrayInParallel(data, platform, input, &start)) return;
#endif
  // The split probe has already validated this prefix as JSON whitespace.
  BackgroundJsonParser<Char>(data, input, data->cancelled, start).Parse();
}

}  // namespace

void ParseJsonInBackground(BackgroundJsonData* data, Platform* platform) {
  if (data->is_one_byte) {
    ParseBackgroundInput<uint8_t>(data, platform, data->one_byte.as_vector());
  } else {
    ParseBackgroundInput<uint16_t>(data, platform, data->two_byte.as_vector());
  }
}

const uint8_t* ScanJsonStringCharactersLong(const uint8_t* begin,
                                            const uint8_t* end,
                                            uint32_t* bits) {
  return ScanLongJsonString(begin, end, bits);
}

const uint16_t* ScanJsonStringCharactersLong(const uint16_t* begin,
                                             const uint16_t* end,
                                             uint32_t* bits) {
  return ScanLongJsonString(begin, end, bits);
}

#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
BackgroundJsonTape::Block::Block(size_t capacity) : capacity_(capacity) {
  if (capacity == kInitialBlockNodes) {
    nodes_ = NewArray<BackgroundJsonNode>(capacity);
    return;
  }
  // Large blocks have independent mappings so consumed pages go straight
  // back to the OS instead of accumulating in the native allocator's cache.
  base::PageAllocator allocator;
  const size_t page_size = allocator.AllocatePageSize();
  mapped_size_ = RoundUp(capacity * sizeof(BackgroundJsonNode), page_size);
  void* storage = AllocatePages(&allocator, nullptr, mapped_size_, page_size,
                                PageAllocator::kReadWrite);
  if (storage == nullptr) {
    V8::FatalProcessOutOfMemory(nullptr, "BackgroundJsonTape");
  }
  nodes_ = new (storage) BackgroundJsonNode[capacity];
}

BackgroundJsonTape::Block::~Block() {
  static_assert(std::is_trivially_destructible_v<BackgroundJsonNode>);
  if (mapped_size_ != 0) {
    base::PageAllocator allocator;
    FreePages(&allocator, nodes_, mapped_size_);
  } else {
    DeleteArray(nodes_);
  }
}
#endif

}  // namespace internal
}  // namespace v8
