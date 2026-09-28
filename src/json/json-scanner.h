// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_SCANNER_H_
#define V8_JSON_JSON_SCANNER_H_

#include <algorithm>
#include <array>
#include <cstdint>

#include "src/base/bit-field.h"
#include "src/base/macros.h"

namespace v8 {
namespace internal {

// These primitives inspect immutable character spans only: no handles, heap
// accesses, or exception construction. Both JSON paths share the same rules.
constexpr bool IsJsonDecimalDigit(uint32_t c) { return c - '0' <= 9; }

enum class EscapeKind : uint8_t {
  kIllegal,
  kSelf,
  kBackspace,
  kTab,
  kNewLine,
  kFormFeed,
  kCarriageReturn,
  kUnicode
};

using EscapeKindField = base::BitField8<EscapeKind, 0, 3>;
using MayTerminateStringField = EscapeKindField::Next<bool, 1>;
using NumberPartField = MayTerminateStringField::Next<bool, 1>;

constexpr bool MayTerminateJsonString(uint8_t flags) {
  return MayTerminateStringField::decode(flags);
}

constexpr EscapeKind GetEscapeKind(uint8_t flags) {
  return EscapeKindField::decode(flags);
}

constexpr bool IsNumberPart(uint8_t flags) {
  return NumberPartField::decode(flags);
}

constexpr uint8_t GetJsonScanFlags(uint8_t c) {
  // clang-format off
  return (c == 'b' ? EscapeKindField::encode(EscapeKind::kBackspace)
          : c == 't' ? EscapeKindField::encode(EscapeKind::kTab)
          : c == 'n' ? EscapeKindField::encode(EscapeKind::kNewLine)
          : c == 'f' ? EscapeKindField::encode(EscapeKind::kFormFeed)
          : c == 'r' ? EscapeKindField::encode(EscapeKind::kCarriageReturn)
          : c == 'u' ? EscapeKindField::encode(EscapeKind::kUnicode)
          : c == '"' ? EscapeKindField::encode(EscapeKind::kSelf)
          : c == '\\' ? EscapeKindField::encode(EscapeKind::kSelf)
          : c == '/' ? EscapeKindField::encode(EscapeKind::kSelf)
          : EscapeKindField::encode(EscapeKind::kIllegal)) |
         (c < 0x20 ? MayTerminateStringField::encode(true)
          : c == '"' ? MayTerminateStringField::encode(true)
          : c == '\\' ? MayTerminateStringField::encode(true)
          : MayTerminateStringField::encode(false)) |
         NumberPartField::encode(c == '.' ||
                                 c == 'e' ||
                                 c == 'E' ||
                                 IsJsonDecimalDigit(c) ||
                                 c == '-' ||
                                 c == '+');
  // clang-format on
}

inline constexpr std::array<uint8_t, 256> character_json_scan_flags = [] {
  std::array<uint8_t, 256> flags{};
  for (int i = 0; i < 256; ++i)
    flags[i] = GetJsonScanFlags(static_cast<uint8_t>(i));
  return flags;
}();

template <typename Char>
const Char* ScanJsonStringCharacters(const Char* begin, const Char* end,
                                     uint32_t* bits) {
  return std::find_if(begin, end, [bits](Char c) {
    if (sizeof(Char) == 2 && V8_UNLIKELY(c > 0xff)) {
      *bits |= c;
      return false;
    }
    return MayTerminateJsonString(character_json_scan_flags[c]);
  });
}

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_SCANNER_H_
