// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-stringifier-escape.h"

#include "hwy/highway.h"

namespace v8 {
namespace internal {
namespace {

// The same unsigned SIMD predicates as the upstream Latin1 fast stringifier,
// extended to UTF-16. Surrogates are left to the shared scalar encoder.
template <typename Char>
size_t FindEscape(const Char* chars, size_t length) {
  namespace hw = hwy::HWY_NAMESPACE;
  hw::FixedTag<Char, 16 / sizeof(Char)> tag;
  const size_t stride = hw::Lanes(tag);
  const auto space = hw::Set(tag, 0x20);
  const auto quote = hw::Set(tag, 0x22);
  const auto slash = hw::Set(tag, 0x5c);
  size_t i = 0;
  for (; length - i >= stride; i += stride) {
    const auto input = hw::LoadU(tag, chars + i);
    auto special = hw::Or(hw::Lt(input, space),
                          hw::Or(hw::Eq(input, quote), hw::Eq(input, slash)));
    if constexpr (sizeof(Char) == 2) {
      special = hw::Or(special, hw::Eq(hw::And(input, hw::Set(tag, 0xf800)),
                                       hw::Set(tag, 0xd800)));
    }
    if (!hw::AllFalse(tag, special)) {
      return i + hw::FindKnownFirstTrue(tag, special);
    }
  }
  for (; i < length; ++i) {
    if (!JsonStringDoNotEscape(chars[i])) break;
  }
  return i;
}

}  // namespace

size_t FindJsonEscape(const uint8_t* chars, size_t length) {
  return FindEscape(chars, length);
}

size_t FindJsonEscape(const uint16_t* chars, size_t length) {
  return FindEscape(chars, length);
}

}  // namespace internal
}  // namespace v8
