// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_MSGPACK_MESSAGEPACK_STRING_H_
#define V8_MSGPACK_MESSAGEPACK_STRING_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

// ARMv7 and AArch64 share these intrinsics. Other targets, and the explicit
// benchmark control, retain scalar paths. No load/store crosses a span's end.
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && \
    !defined(MSGPACK_DISABLE_SIMD)
#define V8_MSGPACK_NEON 1
#include <arm_neon.h>
#endif

namespace v8::internal::messagepack_strings {
struct Utf8Info {
  int length = 0;
  int ascii_prefix = 0;
  bool ascii = true;
  bool one_byte = true;
};

#ifdef V8_MSGPACK_NEON
inline bool Any(uint8x16_t bits) {
  uint64x2_t words = vreinterpretq_u64_u8(bits);
  return (vgetq_lane_u64(words, 0) | vgetq_lane_u64(words, 1)) != 0;
}
inline bool All(uint8x8_t bits) {
  return vget_lane_u64(vreinterpret_u64_u8(bits), 0) == UINT64_MAX;
}
inline unsigned Count(uint8x16_t mask) {
  auto sum =
      vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(vandq_u8(mask, vdupq_n_u8(1)))));
  return static_cast<unsigned>(vgetq_lane_u64(sum, 0) + vgetq_lane_u64(sum, 1));
}
inline unsigned Count(uint16x8_t mask) {
  auto sum = vpaddlq_u32(vpaddlq_u16(vandq_u16(mask, vdupq_n_u16(1))));
  return static_cast<unsigned>(vgetq_lane_u64(sum, 0) + vgetq_lane_u64(sum, 1));
}
#endif

inline bool IsAscii(const uint8_t* s, size_t n) {
  if (n <= 16) {
    if (n >= 8) {
      uint64_t first, last;
      std::memcpy(&first, s, 8);
      std::memcpy(&last, s + n - 8, 8);
      return ((first | last) & UINT64_C(0x8080808080808080)) == 0;
    }
    if (n >= 4) {
      uint32_t first, last;
      std::memcpy(&first, s, 4);
      std::memcpy(&last, s + n - 4, 4);
      return ((first | last) & UINT32_C(0x80808080)) == 0;
    }
    if (n >= 2) {
      uint16_t first, last;
      std::memcpy(&first, s, 2);
      std::memcpy(&last, s + n - 2, 2);
      return ((first | last) & 0x8080) == 0;
    }
    return !n || !(s[0] & 0x80);
  }
  size_t i = 0;
#ifdef V8_MSGPACK_NEON
  if (n <= 32) {
    auto bytes = vorrq_u8(vld1q_u8(s), vld1q_u8(s + n - 16));
    return !Any(vandq_u8(bytes, vdupq_n_u8(0x80)));
  }
  while (n - i >= 64) {
    auto bytes = vorrq_u8(vorrq_u8(vld1q_u8(s + i), vld1q_u8(s + i + 16)),
                          vorrq_u8(vld1q_u8(s + i + 32), vld1q_u8(s + i + 48)));
    if (Any(vandq_u8(bytes, vdupq_n_u8(0x80)))) return false;
    i += 64;
  }
  while (n - i >= 16) {
    if (Any(vandq_u8(vld1q_u8(s + i), vdupq_n_u8(0x80)))) return false;
    i += 16;
  }
#endif
  constexpr uintptr_t high_bits =
      (std::numeric_limits<uintptr_t>::max() / 255) * 128;
  for (; n - i >= sizeof(uintptr_t); i += sizeof(uintptr_t)) {
    uintptr_t word;
    std::memcpy(&word, s + i, sizeof(word));
    if (word & high_bits) return false;
  }
  for (; i < n; ++i)
    if (s[i] & 0x80) return false;
  return true;
}

inline bool ScanUtf8Body(const uint8_t* s, size_t n, Utf8Info* info) {
  size_t i = 0;
#ifdef V8_MSGPACK_NEON
  // For long strings, validate all UTF-8 byte classes and their relationships
  // in parallel. Carries describe the previous block, including a sequence
  // split across the boundary. Scalar code below completes the final sequence.
  if (n >= 64) {
    // Most configuration text is ASCII. Scan its prefix in 64-byte blocks
    // before setting up the UTF-8 relationship checks.
    while (n - i >= 64) {
      auto high =
          vorrq_u8(vorrq_u8(vld1q_u8(s + i), vld1q_u8(s + i + 16)),
                   vorrq_u8(vld1q_u8(s + i + 32), vld1q_u8(s + i + 48)));
      if (Any(vandq_u8(high, vdupq_n_u8(0x80)))) break;
      info->length += 64;
      i += 64;
    }
    auto previous = vdupq_n_u8(0);
    auto previous_leads = vdupq_n_u8(0);
    auto previous_long = vdupq_n_u8(0);
    auto previous_four = vdupq_n_u8(0);
    bool previous_ascii = true;
    for (; n - i >= 16; i += 16) {
      auto bytes = vld1q_u8(s + i);
      auto high = vcgeq_u8(bytes, vdupq_n_u8(0x80));
      // ASCII blocks with no pending continuation need only this test.
      bool block_ascii = !Any(high);
      if (block_ascii && previous_ascii) {
        info->length += 16;
        previous = bytes;
        previous_leads = previous_long = previous_four = vdupq_n_u8(0);
        continue;
      }
      previous_ascii = block_ascii;
      auto cont = vceqq_u8(vandq_u8(bytes, vdupq_n_u8(0xc0)), vdupq_n_u8(0x80));
      auto two = vandq_u8(vcgeq_u8(bytes, vdupq_n_u8(0xc2)),
                          vcleq_u8(bytes, vdupq_n_u8(0xdf)));
      auto three =
          vceqq_u8(vandq_u8(bytes, vdupq_n_u8(0xf0)), vdupq_n_u8(0xe0));
      auto four = vandq_u8(vcgeq_u8(bytes, vdupq_n_u8(0xf0)),
                           vcleq_u8(bytes, vdupq_n_u8(0xf4)));
      auto long_leads = vorrq_u8(three, four);
      auto leads = vorrq_u8(two, long_leads);
      auto expected = vorrq_u8(vextq_u8(previous_leads, leads, 15),
                               vextq_u8(previous_long, long_leads, 14));
      expected = vorrq_u8(expected, vextq_u8(previous_four, four, 13));
      auto bad = vorrq_u8(veorq_u8(cont, expected),
                          vbicq_u8(high, vorrq_u8(cont, leads)));
      auto prev = vextq_u8(previous, bytes, 15);
      bad = vorrq_u8(bad, vandq_u8(vceqq_u8(prev, vdupq_n_u8(0xe0)),
                                   vcltq_u8(bytes, vdupq_n_u8(0xa0))));
      bad = vorrq_u8(bad, vandq_u8(vceqq_u8(prev, vdupq_n_u8(0xed)),
                                   vcgeq_u8(bytes, vdupq_n_u8(0xa0))));
      bad = vorrq_u8(bad, vandq_u8(vceqq_u8(prev, vdupq_n_u8(0xf0)),
                                   vcltq_u8(bytes, vdupq_n_u8(0x90))));
      bad = vorrq_u8(bad, vandq_u8(vceqq_u8(prev, vdupq_n_u8(0xf4)),
                                   vcgeq_u8(bytes, vdupq_n_u8(0x90))));
      if (Any(bad)) return false;
      if (info->ascii && Any(high)) {
        size_t first = i;
        while (s[first] < 0x80) ++first;
        info->ascii_prefix = static_cast<int>(first);
        info->ascii = false;
      }
      info->length += 16 - Count(cont) + Count(four);
      info->one_byte &= !Any(vcgeq_u8(bytes, vdupq_n_u8(0xc4)));
      previous = bytes;
      previous_leads = leads;
      previous_long = long_leads;
      previous_four = four;
    }
    // A lead in the last three bytes may need scalar tail bytes. Rewind only
    // that incomplete code point and undo the length counted for its lead.
    if (i) {
      size_t start = i - 1;
      while (start && (s[start] & 0xc0) == 0x80 && i - start < 4) --start;
      uint8_t lead = s[start];
      size_t width = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
      if (i - start < width) {
        info->length -= width == 4 ? 2 : 1;
        i = start;
      }
    }
  }
#endif
  constexpr uintptr_t kHighBits =
      (std::numeric_limits<uintptr_t>::max() / 255) * 128;
  for (; i < n;) {
    if (n - i >= sizeof(uintptr_t)) {
      uintptr_t word;
      std::memcpy(&word, s + i, sizeof(word));
      if ((word & kHighBits) == 0) {
        info->length += sizeof(word);
        i += sizeof(word);
        continue;
      }
    }
    uint8_t c = s[i++];
    if (c < 0x80) {
      ++info->length;
      continue;
    }
    if (info->ascii) info->ascii_prefix = static_cast<int>(i - 1);
    info->ascii = false;
    unsigned follow;
    uint32_t cp;
    if (c >= 0xc2 && c <= 0xdf) {
      follow = 1;
      cp = c & 31;
    } else if (c >= 0xe0 && c <= 0xef) {
      follow = 2;
      cp = c & 15;
    } else if (c >= 0xf0 && c <= 0xf4) {
      follow = 3;
      cp = c & 7;
    } else
      return false;
    if (follow > n - i) return false;
    for (unsigned j = 0; j < follow; ++j) {
      c = s[i++];
      if ((c & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (c & 63);
    }
    if ((follow == 1 && cp < 0x80) || (follow == 2 && cp < 0x800) ||
        (follow == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff))
      return false;
    info->one_byte &= cp <= 0xff;
    info->length += cp > 0xffff ? 2 : 1;
  }
  return true;
}

inline bool ScanUtf8(const uint8_t* s, size_t n, Utf8Info* info) {
  if (n <= 32) {
    if (!IsAscii(s, n)) return ScanUtf8Body(s, n, info);
    info->length += static_cast<int>(n);
    return true;
  }
#ifdef V8_MSGPACK_NEON
  // Keep short ASCII keys/values in the caller. The vector validator is large
  // enough to be outlined; entering it for tiny ASCII strings costs more than
  // these word tests. Non-ASCII input still receives full strict validation.
  if (n < 64) {
    constexpr uintptr_t high_bits =
        (std::numeric_limits<uintptr_t>::max() / 255) * 128;
    size_t i = 0;
    for (; n - i >= sizeof(uintptr_t); i += sizeof(uintptr_t)) {
      uintptr_t word;
      std::memcpy(&word, s + i, sizeof(word));
      if (word & high_bits) return ScanUtf8Body(s, n, info);
    }
    for (; i < n; ++i) {
      if (s[i] >= 0x80) return ScanUtf8Body(s, n, info);
    }
    info->length += static_cast<int>(n);
    return true;
  }
#endif
  return ScanUtf8Body(s, n, info);
}

// The immutable input has passed ScanUtf8. SIMD handles ASCII and regular
// two/three-byte runs; mixed widths and supplementary characters use scalar
// code. Stores write only the exact number of decoded code units.
template <typename Char>
Char* DecodeUtf8(const uint8_t* s, size_t n, int ascii_prefix, Char* out) {
  if constexpr (sizeof(Char) == 1) {
    if (ascii_prefix) std::memcpy(out, s, ascii_prefix);
  } else {
    for (int i = 0; i < ascii_prefix; ++i) out[i] = s[i];
  }
  out += ascii_prefix;
  for (size_t i = ascii_prefix; i < n;) {
#ifdef V8_MSGPACK_NEON
    if (n - i >= 16 && s[i] < 0x80) {
      auto bytes = vld1q_u8(s + i);
      if (!Any(vcgeq_u8(bytes, vdupq_n_u8(0x80)))) {
        if constexpr (sizeof(Char) == 1)
          vst1q_u8(out, bytes);
        else {
          vst1q_u16(out, vmovl_u8(vget_low_u8(bytes)));
          vst1q_u16(out + 8, vmovl_u8(vget_high_u8(bytes)));
        }
        out += 16;
        i += 16;
        continue;
      }
    }
    if (n - i >= 16 && s[i] >= 0xc2 && s[i] < 0xe0) {
      auto bytes = vld2_u8(s + i);
      auto regular = vand_u8(
          vceq_u8(vand_u8(bytes.val[0], vdup_n_u8(0xe0)), vdup_n_u8(0xc0)),
          vceq_u8(vand_u8(bytes.val[1], vdup_n_u8(0xc0)), vdup_n_u8(0x80)));
      size_t count =
          All(regular)                                                   ? 8
          : vget_lane_u32(vreinterpret_u32_u8(regular), 0) == UINT32_MAX ? 4
                                                                         : 0;
      if (count) {
        auto chars = vorrq_u16(
            vshlq_n_u16(vmovl_u8(vand_u8(bytes.val[0], vdup_n_u8(31))), 6),
            vmovl_u8(vand_u8(bytes.val[1], vdup_n_u8(63))));
        if constexpr (sizeof(Char) == 1) {
          if (count == 8)
            vst1_u8(out, vmovn_u16(chars));
          else {
            uint32_t packed =
                vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(chars)), 0);
            std::memcpy(out, &packed, sizeof(packed));
          }
        } else {
          if (count == 8)
            vst1q_u16(out, chars);
          else
            vst1_u16(out, vget_low_u16(chars));
        }
        out += count;
        i += count * 2;
        continue;
      }
    }
    if constexpr (sizeof(Char) == 2) {
      if (n - i >= 24 && s[i] >= 0xe0 && s[i] < 0xf0) {
        auto bytes = vld3_u8(s + i);
        auto regular = vand_u8(
            vceq_u8(vand_u8(bytes.val[0], vdup_n_u8(0xf0)), vdup_n_u8(0xe0)),
            vand_u8(vceq_u8(vand_u8(bytes.val[1], vdup_n_u8(0xc0)),
                            vdup_n_u8(0x80)),
                    vceq_u8(vand_u8(bytes.val[2], vdup_n_u8(0xc0)),
                            vdup_n_u8(0x80))));
        size_t count =
            All(regular)                                                   ? 8
            : vget_lane_u32(vreinterpret_u32_u8(regular), 0) == UINT32_MAX ? 4
                                                                           : 0;
        if (count) {
          auto chars = vorrq_u16(
              vshlq_n_u16(vmovl_u8(vand_u8(bytes.val[0], vdup_n_u8(15))), 12),
              vorrq_u16(vshlq_n_u16(
                            vmovl_u8(vand_u8(bytes.val[1], vdup_n_u8(63))), 6),
                        vmovl_u8(vand_u8(bytes.val[2], vdup_n_u8(63)))));
          if (count == 8)
            vst1q_u16(out, chars);
          else
            vst1_u16(out, vget_low_u16(chars));
          out += count;
          i += count * 3;
          continue;
        }
      }
    }
#endif
    uint32_t cp = s[i++];
    if (cp >= 0x80) {
      if (cp < 0xe0)
        cp = ((cp & 31) << 6) | (s[i++] & 63);
      else if (cp < 0xf0) {
        cp = ((cp & 15) << 12) | ((s[i] & 63) << 6) | (s[i + 1] & 63);
        i += 2;
      } else {
        cp = ((cp & 7) << 18) | ((s[i] & 63) << 12) | ((s[i + 1] & 63) << 6) |
             (s[i + 2] & 63);
        i += 3;
      }
    }
    if constexpr (sizeof(Char) == 2) {
      if (cp > 0xffff) {
        *out++ = static_cast<Char>(0xd800 + ((cp - 0x10000) >> 10));
        *out++ = static_cast<Char>(0xdc00 + ((cp - 0x10000) & 1023));
        continue;
      }
    }
    *out++ = static_cast<Char>(cp);
  }
  return out;
}

template <typename Char, bool use_simd = true>
bool Utf8Length(const Char* s, size_t n, size_t* length, bool* ascii) {
  *length = 0;
  *ascii = true;
#ifdef V8_MSGPACK_NEON
  size_t next_simd = 0;
#endif
  for (size_t i = 0; i < n;) {
#ifdef V8_MSGPACK_NEON
    if constexpr (use_simd) {
      if constexpr (sizeof(Char) == 1) {
        if (n - i >= 16) {
          auto high = vcgeq_u8(vld1q_u8(s + i), vdupq_n_u8(0x80));
          unsigned extra = Count(high);
          *length += 16 + extra;
          *ascii &= extra == 0;
          i += 16;
          continue;
        }
      } else {
        if (i >= next_simd && n - i >= 8 && (s[i] & 0xf800) != 0xd800) {
          next_simd = i + 8;
          auto chars = vld1q_u16(s + i);
          auto surrogate = vceqq_u16(vandq_u16(chars, vdupq_n_u16(0xf800)),
                                     vdupq_n_u16(0xd800));
          if (!Any(vreinterpretq_u8_u16(surrogate))) {
            unsigned extra = Count(vcgeq_u16(chars, vdupq_n_u16(0x80))) +
                             Count(vcgeq_u16(chars, vdupq_n_u16(0x800)));
            *length += 8 + extra;
            *ascii &= extra == 0;
            i += 8;
            continue;
          }
        }
      }
    }
#endif
    uint32_t cp = s[i++];
    *ascii &= cp < 0x80;
    if constexpr (sizeof(Char) == 2) {
      if (cp >= 0xd800 && cp <= 0xdfff) {
        if (cp > 0xdbff || i == n || s[i] < 0xdc00 || s[i] > 0xdfff)
          return false;
        ++i;
        *length += 4;
        continue;
      }
    }
    *length += 1 + (cp >= 0x80) + (cp >= 0x800);
  }
  return true;
}

// Utf8Length validated surrogate pairs and reserved the exact output span.
template <typename Char, bool use_simd = true>
void EncodeUtf8(const Char* s, size_t n, uint8_t* out) {
#ifdef V8_MSGPACK_NEON
  size_t next_simd = 0;
#endif
  for (size_t i = 0; i < n;) {
#ifdef V8_MSGPACK_NEON
    // A failed attempt falls back for that window rather than repeatedly
    // probing overlapping mixed-width or surrogate-heavy windows.
    if (use_simd && i >= next_simd && n - i >= 8 &&
        (sizeof(Char) == 1 || (s[i] & 0xf800) != 0xd800)) {
      next_simd = i + 8;
      uint16x8_t chars;
      if constexpr (sizeof(Char) == 1)
        chars = vmovl_u8(vld1_u8(s + i));
      else
        chars = vld1q_u16(s + i);
      if (!Any(vreinterpretq_u8_u16(vcgeq_u16(chars, vdupq_n_u16(0x80))))) {
        vst1_u8(out, vmovn_u16(chars));
        out += 8;
        i += 8;
        continue;
      }
      auto two = vandq_u16(vcgeq_u16(chars, vdupq_n_u16(0x80)),
                           vcltq_u16(chars, vdupq_n_u16(0x800)));
      if (!Any(vreinterpretq_u8_u16(vmvnq_u16(two)))) {
        uint8x8x2_t bytes;
        bytes.val[0] =
            vorr_u8(vmovn_u16(vshrq_n_u16(chars, 6)), vdup_n_u8(0xc0));
        bytes.val[1] =
            vorr_u8(vand_u8(vmovn_u16(chars), vdup_n_u8(63)), vdup_n_u8(0x80));
        vst2_u8(out, bytes);
        out += 16;
        i += 8;
        continue;
      }
      if constexpr (sizeof(Char) == 2) {
        auto three =
            vandq_u16(vcgeq_u16(chars, vdupq_n_u16(0x800)),
                      vmvnq_u16(vceqq_u16(vandq_u16(chars, vdupq_n_u16(0xf800)),
                                          vdupq_n_u16(0xd800))));
        if (!Any(vreinterpretq_u8_u16(vmvnq_u16(three)))) {
          uint8x8x3_t bytes;
          bytes.val[0] =
              vorr_u8(vmovn_u16(vshrq_n_u16(chars, 12)), vdup_n_u8(0xe0));
          bytes.val[1] =
              vorr_u8(vand_u8(vmovn_u16(vshrq_n_u16(chars, 6)), vdup_n_u8(63)),
                      vdup_n_u8(0x80));
          bytes.val[2] = vorr_u8(vand_u8(vmovn_u16(chars), vdup_n_u8(63)),
                                 vdup_n_u8(0x80));
          vst3_u8(out, bytes);
          out += 24;
          i += 8;
          continue;
        }
      }
    }
#endif
    uint32_t cp = s[i++];
    if constexpr (sizeof(Char) == 2) {
      if (cp >= 0xd800 && cp <= 0xdbff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (s[i++] - 0xdc00);
      }
    }
    if (cp < 0x80)
      *out++ = static_cast<uint8_t>(cp);
    else if (cp < 0x800) {
      *out++ = static_cast<uint8_t>(0xc0 | (cp >> 6));
      *out++ = static_cast<uint8_t>(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
      *out++ = static_cast<uint8_t>(0xe0 | (cp >> 12));
      *out++ = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63));
      *out++ = static_cast<uint8_t>(0x80 | (cp & 63));
    } else {
      *out++ = static_cast<uint8_t>(0xf0 | (cp >> 18));
      *out++ = static_cast<uint8_t>(0x80 | ((cp >> 12) & 63));
      *out++ = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63));
      *out++ = static_cast<uint8_t>(0x80 | (cp & 63));
    }
  }
}

// Uniform-width runs benefit from interleaved NEON stores. Repeated failed
// probes on mixed scripts or surrogate-heavy strings can outweigh that gain.
// Two bounded samples select the conversion strategy; they do not validate
// the string. Both strategies still preflight every code unit strictly.
template <typename Char>
bool UseSimdForEncoding(const Char* s, size_t n) {
#ifdef V8_MSGPACK_NEON
  if (n < 64) return false;
  for (size_t offset : {n / 3, 2 * n / 3}) {
    uint16x8_t chars;
    if constexpr (sizeof(Char) == 1)
      chars = vmovl_u8(vld1_u8(s + offset));
    else
      chars = vld1q_u16(s + offset);
    if (!Any(vreinterpretq_u8_u16(vcgeq_u16(chars, vdupq_n_u16(0x80)))))
      continue;
    auto two = vandq_u16(vcgeq_u16(chars, vdupq_n_u16(0x80)),
                         vcltq_u16(chars, vdupq_n_u16(0x800)));
    if (!Any(vreinterpretq_u8_u16(vmvnq_u16(two)))) continue;
    auto three =
        vandq_u16(vcgeq_u16(chars, vdupq_n_u16(0x800)),
                  vmvnq_u16(vceqq_u16(vandq_u16(chars, vdupq_n_u16(0xf800)),
                                      vdupq_n_u16(0xd800))));
    if (Any(vreinterpretq_u8_u16(vmvnq_u16(three)))) return false;
  }
  return true;
#else
  return false;
#endif
}
}  // namespace v8::internal::messagepack_strings
#endif  // V8_MSGPACK_MESSAGEPACK_STRING_H_
