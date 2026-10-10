// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef V8_MSGPACK_MESSAGEPACK_RESOURCE_FORMAT_H_
#define V8_MSGPACK_MESSAGEPACK_RESOURCE_FORMAT_H_
#include <cstdint>
#include <cstring>

#include "src/base/bits.h"
namespace messagepack_resources {
constexpr uint8_t kMagic[] = {'V', '8', 'M', 'R', 1};
constexpr int8_t kShape = 0x50, kString = 0x51, kNumbers = 0x52;
constexpr double kScale[] = {1,      10,      100,      1000,      10000,
                             100000, 1000000, 10000000, 100000000, 1000000000};
// Even integer kinds are unsigned, odd ones signed. IEEE kinds are 8 and 9.
constexpr unsigned kWidth[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
inline uint64_t Load(const uint8_t* p, unsigned width) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  // Bounded memcpy permits unaligned wire spans without aliasing or alignment
  // assumptions. ARM lowers these fixed-width swaps to rev/rev16 instructions.
  switch (width) {
    case 1: return *p;
    case 2: {
      uint16_t n;
      std::memcpy(&n, p, 2);
      return ::v8::base::bits::ReverseBytes(n);
    }
    case 4: {
      uint32_t n;
      std::memcpy(&n, p, 4);
      return ::v8::base::bits::ReverseBytes(n);
    }
    case 8: {
      uint64_t n;
      std::memcpy(&n, p, 8);
      return ::v8::base::bits::ReverseBytes(n);
    }
  }
#endif
  uint64_t bits = 0;
  for (unsigned i = 0; i < width; ++i) bits = (bits << 8) | p[i];
  return bits;
}
inline void Store(uint8_t* p, uint64_t bits, unsigned width) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  switch (width) {
    case 1: *p = static_cast<uint8_t>(bits); return;
    case 2: {
      uint16_t n = ::v8::base::bits::ReverseBytes(static_cast<uint16_t>(bits));
      std::memcpy(p, &n, 2);
      return;
    }
    case 4: {
      uint32_t n = ::v8::base::bits::ReverseBytes(static_cast<uint32_t>(bits));
      std::memcpy(p, &n, 4);
      return;
    }
    case 8: {
      uint64_t n = ::v8::base::bits::ReverseBytes(bits);
      std::memcpy(p, &n, 8);
      return;
    }
  }
#endif
  for (unsigned i = width; i; --i) {
    p[i - 1] = static_cast<uint8_t>(bits);
    bits >>= 8;
  }
}
inline double Number(uint64_t bits, unsigned kind, unsigned scale) {
  if (kind == 8) {
    uint32_t word = static_cast<uint32_t>(bits);
    float n;
    std::memcpy(&n, &word, 4);
    return n;
  }
  if (kind == 9) {
    double n;
    std::memcpy(&n, &bits, 8);
    return n;
  }
  if (kind & 1) {
    unsigned width = kWidth[kind];
    if (width < 8 && (bits & (uint64_t{1} << (width * 8 - 1))))
      bits |= ~uint64_t{0} << (width * 8);
    int64_t n;
    std::memcpy(&n, &bits, 8);
    double value = static_cast<double>(n);
    return scale ? value / kScale[scale] : value;
  }
  double value = static_cast<double>(bits);
  return scale ? value / kScale[scale] : value;
}
}  // namespace messagepack_resources
#endif
