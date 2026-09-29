// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_STRINGIFIER_ESCAPE_H_
#define V8_JSON_JSON_STRINGIFIER_ESCAPE_H_

#include "src/base/vector.h"

namespace v8 {
namespace internal {

extern const char kJsonStringEscapeTable[];
extern const bool kJsonStringNoEscape[256];

// Native memory only; never reads past length. Returns length if no escaping
// is needed. Callers doing background work bound length for cancellation.
size_t FindJsonEscape(const uint8_t* chars, size_t length);
size_t FindJsonEscape(const uint16_t* chars, size_t length);

template <typename Char>
V8_INLINE bool JsonStringDoNotEscape(Char c) {
  if constexpr (sizeof(Char) == 1) {
    return kJsonStringNoEscape[static_cast<uint8_t>(c)];
  } else {
    return (c >= 0x20 && c <= 0x21) ||
           (c >= 0x23 && c != 0x5C && (c < 0xD800 || c > 0xDFFF));
  }
}

// Fast synchronous output and the worker reserve capacity before writing.
// Keep the cursor local, not in the owning segmented/growing buffer.
template <typename Char>
struct JsonStringSpanWriter {
  Char* cursor;
  template <typename InputChar>
  V8_INLINE void Append(InputChar value) {
    *cursor++ = static_cast<Char>(value);
  }
  V8_INLINE void AppendCString(const char* text) {
    while (*text) Append(static_cast<uint8_t>(*text++));
  }
};

// Shared with the synchronous stringifier. The caller supplies output capacity
// and, for background work, bounds each span to a cancellation-sized chunk.
template <bool raw_json, typename Char, typename Writer>
V8_INLINE bool WriteJsonString(base::Vector<const Char> src, Writer* dest) {
  bool escaped = false;
  for (int i = 0; i < src.length(); ++i) {
    Char c = src[i];
    if (raw_json || JsonStringDoNotEscape(c)) {
      dest->Append(c);
    } else if (sizeof(Char) != 1 && c >= 0xD800 && c <= 0xDFFF) {
      escaped = true;
      if (c <= 0xDBFF && i + 1 < src.length() && src[i + 1] >= 0xDC00 &&
          src[i + 1] <= 0xDFFF) {
        dest->Append(c);
        dest->Append(src[++i]);
      } else {
        constexpr char hex[] = "0123456789abcdef";
        char text[] = {'\\',
                       'u',
                       hex[(c >> 12) & 15],
                       hex[(c >> 8) & 15],
                       hex[(c >> 4) & 15],
                       hex[c & 15],
                       0};
        dest->AppendCString(text);
      }
    } else {
      escaped = true;
      dest->AppendCString(kJsonStringEscapeTable + c * 8);
    }
  }
  return escaped;
}

}  // namespace internal
}  // namespace v8
#endif  // V8_JSON_JSON_STRINGIFIER_ESCAPE_H_
