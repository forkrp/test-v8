// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-parser-background.h"

#include <algorithm>
#include <limits>

#include "src/base/strings.h"
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
    // Do not reserve proportional to bytes: a huge single string has one node.
    output_->nodes.reserve(256);
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
          node.start = position_;
          if (!ReadString(&node)) return;
          Append(node);
          state_ = State::kColon;
          break;
        }
        case State::kColon:
          if (c != ':') {
            Fail(MessageTemplate::kJsonParseExpectedColonAfterPropertyName);
            return;
          }
          ++position_;
          state_ = State::kValue;
          break;
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
          node.start = position_;
          switch (c) {
            case '{':
            case '[':
              node.kind = c == '{' ? Node::kObject : Node::kArray;
              node.depth = 1;
              if (c == '[') node.flags |= Node::kAllNumbers | Node::kAllSmis;
              ++position_;
              stack_.push_back({Append(node), c == '['});
              state_ =
                  c == '[' ? State::kArrayFirstValue : State::kObjectFirstKey;
              continue;
            case '"':
              node.kind = Node::kString;
              if (!ReadString(&node)) return;
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
              if (!ReadNumber(&node.data.number)) return;
          }
          Append(node);
          CompleteValue(
              node.kind == Node::kNumber,
              node.kind == Node::kNumber && IsSmiDouble(node.data.number));
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
    kColon,
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
  static bool Digit(uint32_t c) { return c >= '0' && c <= '9'; }
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
  uint32_t Append(const BackgroundJsonNode& node) {
    uint32_t index = static_cast<uint32_t>(output_->nodes.size());
    output_->nodes.push_back(node);
    if (output_->track_source) output_->source_ends.push_back(position_);
    return index;
  }
  void CompleteValue(bool number = false, bool smi = false) {
    if (stack_.empty()) {
      state_ = State::kDone;
    } else {
      ++output_->nodes[stack_.back().node].data.container.count;
      auto& parent = output_->nodes[stack_.back().node];
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

  bool ReadString(BackgroundJsonNode* node) {
    using Node = BackgroundJsonNode;
    ++position_;
    uint32_t length = 0, bits = 0, index = 0;
    bool is_index = node->kind == Node::kKey;
    bool leading_zero = false;
    bool escaped = false;
    while (position_ < input_.size()) {
      if ((position_ & 4095) == 0 && Cancelled()) return false;
      uint32_t c = Current();
      if (c == '"') {
        ++position_;
        node->data.string.length = length;
        node->data.string.index = index;
        bool convert = sizeof(Char) == 1 ? bits > 255 : bits <= 255;
        node->flags =
            (convert ? Node::kConvert : 0) | (escaped ? Node::kEscape : 0) |
            ((node->kind == Node::kKey || (sizeof(Char) == 1 && length < 10))
                 ? Node::kInternalize
                 : 0) |
            ((is_index && length != 0) ? Node::kIndex : 0);
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
      if (is_index) {
        if (!Digit(c) || (length != 0 && leading_zero) ||
            index > (UINT32_MAX - 1 - (c - '0')) / 10) {
          is_index = false;
        } else {
          index = index * 10 + c - '0';
          if (length == 0) leading_zero = c == '0';
        }
      }
      ++length;
      ++position_;
    }
    Fail(MessageTemplate::kJsonParseUnterminatedString);
    return false;
  }

  BackgroundJsonData* const output_;
  base::Vector<const Char> input_;
  uint32_t position_ = 0;
  State state_ = State::kValue;
  std::vector<Frame> stack_;
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
