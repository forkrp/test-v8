// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
#ifndef V8_MSGPACK_MESSAGEPACK_RESOURCE_FORMAT_H_
#define V8_MSGPACK_MESSAGEPACK_RESOURCE_FORMAT_H_
namespace messagepack_resources {
constexpr uint8_t kMagic[] = {'V', '8', 'M', 'R', 1};
constexpr int8_t kShape = 0x50, kString = 0x51, kNumbers = 0x52;
constexpr double kScale[] = {1,      10,      100,      1000,      10000,
                             100000, 1000000, 10000000, 100000000, 1000000000};
// Even integer kinds are unsigned, odd ones signed. IEEE kinds are 8 and 9.
constexpr unsigned kWidth[] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
inline uint64_t Load(const uint8_t* p, unsigned width) {
  uint64_t bits = 0;
  for (unsigned i = 0; i < width; ++i) bits = (bits << 8) | p[i];
  return bits;
}
inline void Store(uint8_t* p, uint64_t bits, unsigned width) {
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
