// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-simd.h"

#if defined(V8_JSON_ARM32_RUNTIME_DISPATCH)
#if defined(__ARM_NEON)
#error "The ARM32 JSON dispatcher must be compiled without NEON"
#endif

#include "src/base/cpu.h"
#include "src/codegen/cpu-features.h"
#include "src/flags/flags.h"
#include "src/json/json-array-scanner.h"
#include "src/json/json-scanner.h"
#include "src/json/json-stringifier-escape.h"

namespace v8 {
namespace internal {
namespace {

template <typename Char>
size_t FindEscapeScalar(const Char* chars, size_t length) {
  size_t i = 0;
  while (i < length && JsonStringDoNotEscape(chars[i])) ++i;
  return i;
}

template <typename SinkChar, typename Char>
size_t CopyStringScalar(SinkChar* sink, const Char* source, size_t length) {
  size_t i = 0;
  // Input and output width have already been validated by the JSON scanner.
  while (i < length && source[i] != '\\') {
    sink[i] = static_cast<SinkChar>(source[i]);
    ++i;
  }
  return i;
}

const JsonSimdFunctions* selected = &kJsonScalarFunctions;

}  // namespace

const JsonSimdFunctions kJsonScalarFunctions = {
    ScanJsonStringCharacters<uint8_t>,
    ScanJsonStringCharacters<uint16_t>,
    ScanJsonStringCharacters<uint8_t>,
    ScanJsonStringCharacters<uint16_t>,
    FindEscapeScalar<uint8_t>,
    FindEscapeScalar<uint16_t>,
    CopyStringScalar<uint8_t, uint8_t>,
    CopyStringScalar<uint8_t, uint16_t>,
    CopyStringScalar<uint16_t, uint8_t>,
    CopyStringScalar<uint16_t, uint16_t>,
    FindJsonArrayCuts<uint8_t>,
    FindJsonArrayCuts<uint16_t>,
    false};

void InitializeJsonSimd() {
  // Respect V8's feature restriction (also used by mksnapshot) and check the
  // actual CPU. Compiler-implied features must never authorize this kernel.
  selected = v8_flags.json_simd && CpuFeatures::IsSupported(NEON) &&
                     base::CPU().has_neon()
                 ? &kJsonNeonFunctions
                 : &kJsonScalarFunctions;
}

const JsonSimdFunctions& GetJsonSimdFunctions() { return *selected; }

}  // namespace internal
}  // namespace v8
#endif  // V8_JSON_ARM32_RUNTIME_DISPATCH
