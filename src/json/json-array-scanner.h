// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_ARRAY_SCANNER_H_
#define V8_JSON_JSON_ARRAY_SCANNER_H_

#include <algorithm>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "src/base/macros.h"

namespace v8 {
namespace internal {

V8_INLINE bool IsJsonArraySpace(uint32_t c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// This scan only finds cut positions. Each part still undergoes the full
// grammar check, and any failure goes through the serial error reporter.
#if defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && !defined(__AARCH64EB__)
V8_INLINE uint64_t JsonArrayStructureLanes(uint8x16_t mask) {
  constexpr uint64_t kLane = 0x1111111111111111;
  return vget_lane_u64(
             vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(mask), 4)), 0) &
         kLane;
}
#endif

// Keep scalar and NEON instantiations local to their translation units. ARM32
// dispatch builds both variants and must never coalesce their implementations.
template <typename Char, bool find_end = false, bool collect_hints = false>
static size_t FindJsonArrayCuts(const Char* input, size_t length,
                                size_t array_begin,
                                const std::atomic<bool>& cancelled,
                                size_t part_limit, void* context,
                                void (*publish)(void*, size_t)) {
  static_assert(!collect_hints || find_end);
  // A coarse index stays small even at V8's maximum string length. Only
  // complete top-level element boundaries are recorded, never syntax tokens
  // inside a string or nested container.
  constexpr size_t kHintSpacing = 512 * 1024;
  const Char* start = input;
  const Char* end = input + length;
  while (!find_end && end != start && IsJsonArraySpace(end[-1])) {
    --end;
    if ((end - start) % 4096 == 0 &&
        cancelled.load(std::memory_order_relaxed)) {
      return 0;
    }
  }
  if (end == start || (!find_end && end[-1] != ']')) return 0;
  const Char* p = start + array_begin + 1;
  size_t next_cancel_check = 4096;
  size_t cuts_remaining = part_limit - 1;
  const size_t spacing = (length - array_begin) / part_limit;
  size_t target = collect_hints ? array_begin + kHintSpacing
                  : find_end    ? length
                                : array_begin + spacing;
  int depth = 1;
  bool in_string = false;
  bool escaped = false;
  while (p != end) {
    if (static_cast<size_t>(p - start) >= next_cancel_check) {
      if (cancelled.load(std::memory_order_relaxed)) return 0;
      next_cancel_check = p - start + 4096;
    }
    const Char* limit = p + std::min<ptrdiff_t>(16, end - p);
#if defined(__ARM_NEON) && !defined(__ARM_BIG_ENDIAN) && !defined(__AARCH64EB__)
    constexpr uint64_t kLane = 0x1111111111111111;
    if (limit - p == 16 && !escaped) {
      uint8x16_t block;
      if constexpr (sizeof(Char) == 1) {
        block = vld1q_u8(p);
      } else {
        // Saturation keeps non-ASCII code units from becoming syntax bytes.
        block = vcombine_u8(vqmovn_u16(vld1q_u16(p)),
                            vqmovn_u16(vld1q_u16(p + 8)));
      }
      if (!JsonArrayStructureLanes(vceqq_u8(block, vdupq_n_u8('\\')))) {
        uint64_t strings =
            JsonArrayStructureLanes(vceqq_u8(block, vdupq_n_u8('"')));
        strings ^= strings << 4;
        strings ^= strings << 8;
        strings ^= strings << 16;
        strings ^= strings << 32;
        if (in_string) strings ^= kLane;
        const auto outside = ~strings;
        const auto braces = vorrq_u8(block, vdupq_n_u8(0x20));
        const auto close = vceqq_u8(braces, vdupq_n_u8('}'));
        const int closes =
            __builtin_popcountll(JsonArrayStructureLanes(close) & outside);
        bool look_for_cut = static_cast<size_t>(limit - start) > target;
        if constexpr (collect_hints) {
          // Crossing a hint target inside a large string must not force a
          // byte-at-a-time scan until that string ends.
          look_for_cut =
              look_for_cut &&
              (JsonArrayStructureLanes(vceqq_u8(block, vdupq_n_u8(','))) &
               outside) != 0;
        }
        if (depth > closes && (!look_for_cut || depth > closes + 1)) {
          const auto open = vceqq_u8(braces, vdupq_n_u8('{'));
          depth +=
              __builtin_popcountll(JsonArrayStructureLanes(open) & outside) -
              closes;
          in_string = ((strings >> 60) & 1) != 0;
          p = limit;
          continue;
        }
      }
    }
#endif
    for (; p != limit; ++p) {
      const auto c = *p;
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      switch (c) {
        case '"': in_string = true; break;
        case '[': case '{': ++depth; break;
        case ']': case '}':
          if (--depth == 0) {
            return *p == ']' && (find_end || p == end - 1) ? p - start : 0;
          }
          break;
        case ',':
          if constexpr (collect_hints) {
            if (depth == 1 && static_cast<size_t>(p - start) >= target) {
              publish(context, p - start);
              target = p - start + kHintSpacing;
            }
          } else if (!find_end && depth == 1 &&
                     static_cast<size_t>(p - start) >= target) {
            publish(context, p - start);
            if (--cuts_remaining == 0) {
              // The part parser validates the final interval through its end.
              // No further cuts are needed in the last part.
              return end - start - 1;
            }
            target += spacing;
          }
          break;
      }
    }
  }
  return 0;
}

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_ARRAY_SCANNER_H_
