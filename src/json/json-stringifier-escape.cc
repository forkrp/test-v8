// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-stringifier-escape.h"

#include "hwy/highway.h"
#if defined(__ARM_NEON) && !defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace v8 {
namespace internal {
namespace {

// The same unsigned SIMD predicates as the upstream Latin1 fast stringifier,
// extended to UTF-16. Surrogates are left to the shared scalar encoder.
template <typename Char>
size_t FindEscape(const Char* chars, size_t length) {
#if defined(__ARM_NEON) && !defined(__aarch64__)
  // Highway can select EMU128 on ARMv7 NEON builds lacking VFPv4/FMA.
  // This scanner only needs integer NEON instructions.
  constexpr size_t stride = 16 / sizeof(Char);
  size_t i = 0;
  for (; length - i >= stride; i += stride) {
    uint8x16_t mask;
    if constexpr (sizeof(Char) == 1) {
      auto input = vld1q_u8(chars + i);
      mask = vorrq_u8(vcltq_u8(input, vdupq_n_u8(0x20)),
                      vorrq_u8(vceqq_u8(input, vdupq_n_u8(0x22)),
                               vceqq_u8(input, vdupq_n_u8(0x5c))));
    } else {
      auto input = vld1q_u16(chars + i);
      auto special = vorrq_u16(vcltq_u16(input, vdupq_n_u16(0x20)),
                               vorrq_u16(vceqq_u16(input, vdupq_n_u16(0x22)),
                                         vceqq_u16(input, vdupq_n_u16(0x5c))));
      special =
          vorrq_u16(special, vceqq_u16(vandq_u16(input, vdupq_n_u16(0xf800)),
                                       vdupq_n_u16(0xd800)));
      mask = vreinterpretq_u8_u16(special);
    }
    auto merged = vorr_u8(vget_low_u8(mask), vget_high_u8(mask));
    if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) break;
  }
  for (; i < length; ++i)
    if (!JsonStringDoNotEscape(chars[i])) break;
  return i;
#else
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
#endif
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
