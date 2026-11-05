// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_SIMD_H_
#define V8_JSON_JSON_SIMD_H_

#include <stddef.h>
#include <stdint.h>

#include "src/base/macros.h"

namespace v8 {
namespace internal {

#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH)
// Only native-memory kernels cross this boundary. The ordinary V8 translation
// units and dispatcher must be built without NEON; json-simd-neon.cc alone
// enables it. Neither table has a dynamic initializer.
struct JsonSimdFunctions {
  const uint8_t* (*scan8)(const uint8_t*, const uint8_t*, uint32_t*);
  const uint16_t* (*scan16)(const uint16_t*, const uint16_t*, uint32_t*);
  const uint8_t* (*scan_long8)(const uint8_t*, const uint8_t*, uint32_t*);
  const uint16_t* (*scan_long16)(const uint16_t*, const uint16_t*, uint32_t*);
  size_t (*escape8)(const uint8_t*, size_t);
  size_t (*escape16)(const uint16_t*, size_t);
  size_t (*copy8_8)(uint8_t*, const uint8_t*, size_t);
  size_t (*copy8_16)(uint8_t*, const uint16_t*, size_t);
  size_t (*copy16_8)(uint16_t*, const uint8_t*, size_t);
  size_t (*copy16_16)(uint16_t*, const uint16_t*, size_t);
  bool uses_neon;
};

extern const JsonSimdFunctions kJsonScalarFunctions;
extern const JsonSimdFunctions kJsonNeonFunctions;

// Called by V8's once-per-process initialization after CPU probing, before
// isolates can submit JSON worker tasks. The selected table is then immutable.
void InitializeJsonSimd();
const JsonSimdFunctions& GetJsonSimdFunctions();

template <typename Char>
V8_INLINE const Char* ScanJsonStringDispatched(const Char* begin,
                                               const Char* end, uint32_t* bits,
                                               bool long_string = false) {
  const auto& f = GetJsonSimdFunctions();
  if constexpr (sizeof(Char) == 1) {
    return (long_string ? f.scan_long8 : f.scan8)(begin, end, bits);
  } else {
    return (long_string ? f.scan_long16 : f.scan16)(begin, end, bits);
  }
}

template <typename SinkChar, typename Char>
V8_INLINE size_t CopyJsonStringDispatched(SinkChar* sink, const Char* source,
                                          size_t length) {
  const auto& f = GetJsonSimdFunctions();
  if constexpr (sizeof(SinkChar) == 1 && sizeof(Char) == 1) {
    return f.copy8_8(sink, source, length);
  } else if constexpr (sizeof(SinkChar) == 1) {
    return f.copy8_16(sink, source, length);
  } else if constexpr (sizeof(Char) == 1) {
    return f.copy16_8(sink, source, length);
  } else {
    return f.copy16_16(sink, source, length);
  }
}
#endif  // V8_JSON_ARM32_RUNTIME_DISPATCH

}  // namespace internal
}  // namespace v8
#endif  // V8_JSON_JSON_SIMD_H_
