// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_NUMBER_PARSER_H_
#define V8_JSON_JSON_NUMBER_PARSER_H_

#include <stdint.h>

namespace v8 {
namespace internal {

// Scan and convert a JSON number prefix without heap access or allocation.
// Return its end on success (including overflow/underflow), or nullptr on
// invalid syntax. The caller retains its scanner for precise error reporting.
const uint8_t* TryParseJsonNumber(const uint8_t* first, const uint8_t* last,
                                  double* value);
const uint16_t* TryParseJsonNumber(const uint16_t* first, const uint16_t* last,
                                   double* value);

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_NUMBER_PARSER_H_
