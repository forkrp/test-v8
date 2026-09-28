// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-parser-background.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "src/base/strings.h"
#include "src/json/json-scanner.h"
#include "src/numbers/conversions-inl.h"

namespace v8 {
namespace internal {
namespace {

template <typename Char>
class BackgroundJsonParser {
 public:
  BackgroundJsonParser(BackgroundJsonData* output,
                       base::Vector<const Char> input)
      : output_(output), input_(input) {}

  void Parse() {
    using Node = BackgroundJsonNode;
    // Grow by blocks, not by input bytes: a huge single string has one node.
    while (!Cancelled()) {
      SkipWhitespace();
      if (Cancelled()) return;
      uint32_t c = Current();
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
          state_ = State::kValue;
          break;
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
          Node node;
          uint32_t start = position_;
          switch (c) {
            case '{':
            case '[':
              node.kind = c == '{' ? Node::kObject : Node::kArray;
              node.depth = 1;
              if (c == '[') node.flags |= Node::kAllNumbers | Node::kAllSmis;
              ++position_;
              stack_.push_back({Append(node, start), c == '['});
              state_ =
                  c == '[' ? State::kArrayFirstValue : State::kObjectFirstKey;
              continue;
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
              if (IsSmiDouble(number)) node.flags |= Node::kSmi;
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
    uint32_t node;
    bool array;
  };

  bool Cancelled() const {
    return output_->cancelled.load(std::memory_order_relaxed);
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
  V8_INLINE uint32_t Append(const BackgroundJsonNode& node, uint32_t start) {
    uint32_t index = static_cast<uint32_t>(output_->nodes.size());
    output_->nodes.push_back(node);
    if (output_->track_source) {
      output_->source_starts.push_back(start);
      output_->source_ends.push_back(position_);
    }
    return index;
  }
  void CompleteValue(bool number = false, bool smi = false) {
    if (stack_.empty()) {
      state_ = State::kDone;
    } else {
      auto& parent = output_->nodes[stack_.back().node];
      ++parent.data.container.count;
      if (!number) parent.flags &= ~BackgroundJsonNode::kAllNumbers;
      if (!smi) parent.flags &= ~BackgroundJsonNode::kAllSmis;
      state_ = stack_.back().array ? State::kArrayNext : State::kObjectNext;
    }
  }
  void Close() {
    ++position_;
    uint32_t node = stack_.back().node;
    output_->nodes[node].data.container.end =
        static_cast<uint32_t>(output_->nodes.size());
    if (output_->track_source) output_->source_ends[node] = position_;
    stack_.pop_back();
    if (!stack_.empty()) {
      auto& parent = output_->nodes[stack_.back().node];
      parent.depth = std::max<uint16_t>(
          parent.depth, std::min<uint16_t>(17, output_->nodes[node].depth + 1));
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

  bool ReadNumber(double* number) {
    uint32_t start = position_;
    bool negative = Current() == '-';
    if (negative) ++position_;
    uint32_t digits = 0;
    uint32_t integer = 0;
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
        if (++digits <= 9) integer = integer * 10 + Current() - '0';
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
    }
    bool fractional = false;
    if (Current() == '.') {
      fractional = true;
      ++position_;
      if (!Digit(Current())) {
        Fail(MessageTemplate::kJsonParseUnterminatedFractionalNumber);
        return false;
      }
      while (Digit(Current())) {
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
    }
    if (Current() == 'e' || Current() == 'E') {
      fractional = true;
      ++position_;
      if (Current() == '+' || Current() == '-') ++position_;
      if (!Digit(Current())) {
        Fail(MessageTemplate::kJsonParseExponentPartMissingNumber);
        return false;
      }
      while (Digit(Current())) {
        ++position_;
        if ((position_ & 4095) == 0 && Cancelled()) return false;
      }
    }
    if (!fractional && digits <= 9) {
      *number = negative ? -static_cast<double>(integer) : integer;
    } else {
      *number = StringToDouble(input_.SubVector(start, position_),
                               NO_CONVERSION_FLAGS,
                               std::numeric_limits<double>::quiet_NaN());
    }
    return true;
  }

  template <bool is_key>
  bool ReadString(BackgroundJsonNode* node) {
    using Node = BackgroundJsonNode;
    ++position_;
    uint32_t length = 0, bits = 0, index = 0;
    bool is_index = is_key;
    bool leading_zero = false;
    bool escaped = false;
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
      const Char* stop = ScanJsonStringCharacters(begin, end, &bits);
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
        if (!is_key && length < 10 && !escaped) {
          CacheShortString(node);
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
      }
      bits |= c;
      add_index_char(c, length);
      ++length;
      ++position_;
    }
    Fail(MessageTemplate::kJsonParseUnterminatedString);
    return false;
  }

  void CacheShortString(BackgroundJsonNode* node) {
    // Bound retained V8 strings to 64 slots. A collision falls back to the
    // existing string table; never replace a slot already referenced by tape.
    uint32_t hash = node->data.string.length;
    const Char* chars = input_.begin() + node->data.string.start + 1;
    for (uint32_t i = 0; i < node->data.string.length; ++i)
      hash = hash * 31 + chars[i];
    uint32_t slot = hash & (short_strings_.size() - 1);
    if (short_strings_[slot]) {
      const auto& previous = output_->nodes[short_strings_[slot] - 1];
      if (previous.data.string.length != node->data.string.length ||
          std::memcmp(input_.begin() + previous.data.string.start + 1, chars,
                      node->data.string.length * sizeof(Char)) != 0) {
        return;
      }
    } else {
      short_strings_[slot] = static_cast<uint32_t>(output_->nodes.size()) + 1;
    }
    node->flags |= BackgroundJsonNode::kCachedString;
    node->depth = static_cast<uint16_t>(slot);
  }

  BackgroundJsonData* const output_;
  base::Vector<const Char> input_;
  uint32_t position_ = 0;
  State state_ = State::kValue;
  std::vector<Frame> stack_;
  std::array<uint32_t, 64> short_strings_{};
};

}  // namespace

void ParseJsonInBackground(BackgroundJsonData* data) {
  if (data->is_one_byte) {
    BackgroundJsonParser<uint8_t>(data, data->one_byte.as_vector()).Parse();
  } else {
    BackgroundJsonParser<uint16_t>(data, data->two_byte.as_vector()).Parse();
  }
}

}  // namespace internal
}  // namespace v8
