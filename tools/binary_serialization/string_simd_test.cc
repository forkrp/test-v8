// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Standalone differential test of NEON paths against the retained scalar
// validator/converter. ASan/UBSan check exact spans and unaligned inputs.
#include "src/msgpack/messagepack-string.h"
#undef V8_MSGPACK_MESSAGEPACK_STRING_H_
#undef V8_MSGPACK_NEON
#define MSGPACK_DISABLE_SIMD
#define messagepack_strings scalar_strings
#include "src/msgpack/messagepack-string.h"
#undef messagepack_strings

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace fast = v8::internal::messagepack_strings;
namespace slow = v8::internal::scalar_strings;
void Check(bool ok) {
  if (!ok) std::abort();
}
uint32_t state = 20261005;
uint32_t Next() {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
size_t cases = 0;
void Verify(const std::vector<uint8_t>& bytes) {
  // Place the end of the span at the allocation end, varying input alignment.
  size_t offset = cases++ % 16;
  auto storage = std::make_unique<uint8_t[]>(offset + bytes.size() + 1);
  uint8_t* input = storage.get() + offset + 1;
  if (!bytes.empty()) std::memcpy(input, bytes.data(), bytes.size());
  fast::Utf8Info a;
  slow::Utf8Info b;
  bool valid = fast::ScanUtf8(input, bytes.size(), &a);
  Check(valid == slow::ScanUtf8(input, bytes.size(), &b));
  if (!valid) return;
  Check(a.length == b.length && a.ascii == b.ascii &&
        a.one_byte == b.one_byte && a.ascii_prefix == b.ascii_prefix);
  auto decoded = std::make_unique<uint16_t[]>(a.length ? a.length : 1);
  auto expected = std::make_unique<uint16_t[]>(a.length ? a.length : 1);
  fast::DecodeUtf8(input, bytes.size(), a.ascii_prefix, decoded.get());
  slow::DecodeUtf8(input, bytes.size(), b.ascii_prefix, expected.get());
  Check(!a.length ||
        std::memcmp(decoded.get(), expected.get(), a.length * 2) == 0);
  size_t length;
  bool ascii;
  Check(fast::Utf8Length(decoded.get(), a.length, &length, &ascii));
  Check(length == bytes.size() && ascii == a.ascii);
  auto encoded = std::make_unique<uint8_t[]>(length ? length : 1);
  fast::EncodeUtf8(decoded.get(), a.length, encoded.get());
  Check(!length || std::memcmp(encoded.get(), input, length) == 0);
  if (a.one_byte) {
    auto latin = std::make_unique<uint8_t[]>(a.length ? a.length : 1);
    fast::DecodeUtf8(input, bytes.size(), a.ascii_prefix, latin.get());
    for (int i = 0; i < a.length; ++i) Check(latin[i] == expected[i]);
    Check(fast::Utf8Length(latin.get(), a.length, &length, &ascii));
    Check(length == bytes.size());
    fast::EncodeUtf8(latin.get(), a.length, encoded.get());
    Check(!length || std::memcmp(encoded.get(), input, length) == 0);
  }
}
int main() {
  // All transitions among ASCII, Latin1, wider two-byte, three-byte, and
  // supplementary code points; sizes straddle every SIMD and scalar boundary.
  const uint32_t points[] = {0,     0x7f,   0x80,   0xff,   0x100,   0x7ff,
                             0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff};
  for (uint32_t x : points)
    for (uint32_t y : points) {
      for (int length = 0; length < 100; ++length) {
        std::vector<uint16_t> text;
        for (int i = 0; i < length; ++i) {
          uint32_t cp = i % 3 ? x : y;
          if (cp < 0x10000)
            text.push_back(cp);
          else {
            text.push_back(0xd800 + ((cp - 0x10000) >> 10));
            text.push_back(0xdc00 + ((cp - 0x10000) & 1023));
          }
        }
        size_t size;
        bool ascii;
        Check(slow::Utf8Length(text.data(), text.size(), &size, &ascii));
        std::vector<uint8_t> bytes(size);
        slow::EncodeUtf8(text.data(), text.size(), bytes.data());
        Verify(bytes);
        if (length == 99) {
          // Mutate every byte position; catches bad continuation classes and
          // overlong/surrogate/out-of-range values at block boundaries.
          for (size_t j = 0; j < bytes.size(); ++j) {
            auto changed = bytes;
            changed[j] = static_cast<uint8_t>(Next());
            Verify(changed);
          }
          for (size_t j = 0; j < bytes.size(); ++j) {
            Verify(std::vector<uint8_t>(bytes.begin(), bytes.begin() + j));
          }
        }
      }
    }
  for (int i = 0; i < 20000; ++i) {
    std::vector<uint8_t> bytes(Next() % 512);
    for (auto& b : bytes) b = static_cast<uint8_t>(Next());
    Verify(bytes);
  }
  const std::vector<std::vector<uint8_t>> bad_utf8 = {{0x80},
                                                      {0xff},
                                                      {0xc0, 0x80},
                                                      {0xc2, 0x20},
                                                      {0xe0, 0x80, 0x80},
                                                      {0xed, 0xa0, 0x80},
                                                      {0xf0, 0x80, 0x80, 0x80},
                                                      {0xf4, 0x90, 0x80, 0x80},
                                                      {0xf5, 0x80, 0x80, 0x80}};
  for (size_t offset = 0; offset < 80; ++offset) {
    for (const auto& bad : bad_utf8) {
      std::vector<uint8_t> bytes(offset, 'a');
      bytes.insert(bytes.end(), bad.begin(), bad.end());
      bytes.insert(bytes.end(), 80, 'a');
      fast::Utf8Info info;
      Check(!fast::ScanUtf8(bytes.data(), bytes.size(), &info));
      Verify(bytes);
    }
  }
  for (int position = 0; position < 40; ++position) {
    for (uint16_t bad : {uint16_t(0xd800), uint16_t(0xdbff), uint16_t(0xdc00),
                         uint16_t(0xdfff)}) {
      std::vector<uint16_t> chars(40, 'a');
      chars[position] = bad;
      size_t length;
      bool ascii;
      Check(!fast::Utf8Length(chars.data(), chars.size(), &length, &ascii));
    }
  }
  std::cout << "{\"stringDifferential\":\"PASS\",\"cases\":" << cases
            << ",\"randomByteInputs\":20000,\"unpairedSurrogates\":160}\n";
}
