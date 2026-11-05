// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-simd.h"

#if !defined(V8_JSON_ARM32_RUNTIME_DISPATCH) || !defined(__ARM_NEON)
#error "Build this file only as the optional NEON kernel"
#endif

#include <arm_neon.h>

#include "src/base/bits.h"
#include "src/base/memory.h"
#include "src/json/json-scanner.h"
#include "src/json/json-stringifier-escape.h"

namespace v8 {
namespace internal {
namespace {

template <typename Char>
V8_NOINLINE const Char* ScanBackgroundString(const Char* begin, const Char* end,
                                             uint32_t* bits) {
#if defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && !defined(__AARCH64EB__)
  using Word = uintptr_t;
  constexpr size_t kWordChars = sizeof(Word) / sizeof(Char);
  constexpr Word kLow = sizeof(Char) == 1 ? Word(-1) / 0xff : Word(-1) / 0xffff;
  constexpr Word kHigh = kLow * (sizeof(Char) == 1 ? 0x80 : 0x8000);
  if (static_cast<size_t>(end - begin) >= kWordChars) {
    Word input =
        base::ReadUnalignedValue<Word>(reinterpret_cast<base::Address>(begin));
    Word quote = input ^ (kLow * 0x22);
    Word slash = input ^ (kLow * 0x5c);
    Word special = ((input - kLow * 0x20) & ~input & kHigh) |
                   ((quote - kLow) & ~quote & kHigh) |
                   ((slash - kLow) & ~slash & kHigh);
    if (special) {
      // Borrow propagation can flag later lanes; the first flagged lane is
      // exact. Only characters before it may contribute to UTF-16 width.
      size_t count =
          base::bits::CountTrailingZerosNonZero(special) / (8 * sizeof(Char));
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
}

// The synchronous frontend has already scanned its short-string prefix.
// Check four vectors together, then reuse the worker scanner to locate the
// first special character. Only completely accepted blocks affect width.
template <typename Char>
V8_NOINLINE const Char* ScanLongJsonString(const Char* begin, const Char* end,
                                           uint32_t* bits) {
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
}

template <typename Char>
size_t FindEscape(const Char* chars, size_t length) {
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
}

template <typename SinkChar, typename Char>
size_t CopyStringNeon(SinkChar* sink, const Char* source, size_t length) {
  constexpr size_t stride = 16 / sizeof(Char);
  size_t i = 0;
  for (; length - i >= stride; i += stride) {
    auto input = [&]() {
      if constexpr (sizeof(Char) == 1)
        return vld1q_u8(source + i);
      else
        return vld1q_u16(source + i);
    }();
    uint8x16_t mask;
    if constexpr (sizeof(Char) == 1) {
      mask = vceqq_u8(input, vdupq_n_u8('\\'));
    } else {
      mask = vreinterpretq_u8_u16(vceqq_u16(input, vdupq_n_u16('\\')));
    }
    auto merged = vorr_u8(vget_low_u8(mask), vget_high_u8(mask));
    if (vget_lane_u64(vreinterpret_u64_u8(merged), 0)) break;
    if constexpr (sizeof(Char) == 1 && sizeof(SinkChar) == 1) {
      vst1q_u8(sink + i, input);
    } else if constexpr (sizeof(Char) == 2 && sizeof(SinkChar) == 2) {
      vst1q_u16(sink + i, input);
    } else if constexpr (sizeof(Char) == 1) {
      vst1q_u16(sink + i, vmovl_u8(vget_low_u8(input)));
      vst1q_u16(sink + i + 8, vmovl_u8(vget_high_u8(input)));
    } else {
      vst1_u8(sink + i, vmovn_u16(input));
    }
  }
  return i;
}

}  // namespace

const JsonSimdFunctions kJsonNeonFunctions = {
    ScanBackgroundString<uint8_t>,
    ScanBackgroundString<uint16_t>,
    ScanLongJsonString<uint8_t>,
    ScanLongJsonString<uint16_t>,
    FindEscape<uint8_t>,
    FindEscape<uint16_t>,
    CopyStringNeon<uint8_t, uint8_t>,
    CopyStringNeon<uint8_t, uint16_t>,
    CopyStringNeon<uint16_t, uint8_t>,
    CopyStringNeon<uint16_t, uint16_t>,
    true};

}  // namespace internal
}  // namespace v8
