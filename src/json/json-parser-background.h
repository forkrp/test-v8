// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_PARSER_BACKGROUND_H_
#define V8_JSON_JSON_PARSER_BACKGROUND_H_

#include <atomic>
#include <vector>

#include "src/base/vector.h"
#include "src/common/message-template.h"
#include "src/json/json-parser.h"

namespace v8 {
namespace internal {

// A preorder tape. A container points past its subtree; keys and values occupy
// separate entries. No V8 handles or heap pointers ever enter the worker.
struct BackgroundJsonNode {
  enum Kind : uint8_t {
    kObject,
    kArray,
    kKey,
    kString,
    kNumber,
    kTrue,
    kFalse,
    kNull
  };
  enum Flag : uint8_t {
    kConvert = 1,
    kEscape = 2,
    kInternalize = 4,
    kIndex = 8,
    kAllNumbers = 32,
    kAllSmis = 64
  };
  union {
    double number;
    struct {
      uint32_t length, index;
    } string;
    struct {
      uint32_t end, count;
    } container;
  } data{};
  uint32_t start = 0;
  Kind kind = kNull;
  uint8_t flags = 0;
  // Saturates at 17: only small subtrees of depth <= 16 use local recursion.
  uint16_t depth = 0;

  JsonString AsString(int source_offset = 0) const {
    if (flags & kIndex) return JsonString(data.string.index);
    return JsonString(source_offset + start + 1, data.string.length,
                      flags & kConvert, flags & kInternalize, flags & kEscape);
  }
};
static_assert(sizeof(BackgroundJsonNode) == 16);

struct BackgroundJsonData {
  explicit BackgroundJsonData(bool track_source) : track_source(track_source) {}
  const bool track_source;
  std::atomic<bool> cancelled{false};
  base::OwnedVector<uint8_t> one_byte;
  base::OwnedVector<uint16_t> two_byte;
  bool is_one_byte = true;
  std::vector<BackgroundJsonNode> nodes;
  // Needed only by revivers, not by the common no-reviver path.
  std::vector<uint32_t> source_ends;
  bool failed = false;
  int error_position = 0;
  base::Optional<MessageTemplate> error_message;
};

// Uses only native memory, immutable input, and V8's numeric conversion helper.
void ParseJsonInBackground(BackgroundJsonData* data);

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_PARSER_BACKGROUND_H_
