// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-number-parser.h"

#include <type_traits>

#include "src/base/logging.h"
#include "src/base/macros.h"
#include "third_party/fast_float/include/fast_float/fast_float.h"

namespace v8 {
namespace internal {
namespace {

template <typename Char>
V8_INLINE const Char* ScanAndConvertJsonNumber(const Char* first,
                                               const Char* last,
                                               double* value) {
  DCHECK_LT(first, last);
  // Match fast_float's character types, as in upstream V8's conversions.
  using UC = std::conditional_t<std::is_same_v<Char, uint8_t>, char, char16_t>;
  static_assert(sizeof(UC) == sizeof(Char));
  // Keep the format constant inside this outlined specialization. Clang can
  // otherwise retain runtime format checks in the general from_chars wrapper.
  constexpr fast_float::parse_options_t<UC> options(
      fast_float::chars_format::json);
  auto result = fast_float::from_chars_advanced(
      reinterpret_cast<const UC*>(first), reinterpret_cast<const UC*>(last),
      *value, options);
  // Out-of-range values are deliberately accepted: JSON numbers overflow to
  // signed infinity and underflow to signed zero. fast_float sets both values.
  if (result.ec == std::errc() || result.ec == std::errc::result_out_of_range) {
    // from_chars can return the fixed-point prefix of an incomplete exponent
    // ("1e", "1e+"). Let V8's scanner report the missing exponent digits.
    if (result.ptr != reinterpret_cast<const UC*>(last) &&
        (*result.ptr == 'e' || *result.ptr == 'E')) {
      return nullptr;
    }
    return reinterpret_cast<const Char*>(result.ptr);
  }
  return nullptr;
}

}  // namespace

V8_NOINLINE const uint8_t* TryParseJsonNumber(const uint8_t* first,
                                              const uint8_t* last,
                                              double* value) {
  return ScanAndConvertJsonNumber(first, last, value);
}

V8_NOINLINE const uint16_t* TryParseJsonNumber(const uint16_t* first,
                                               const uint16_t* last,
                                               double* value) {
  return ScanAndConvertJsonNumber(first, last, value);
}

}  // namespace internal
}  // namespace v8
