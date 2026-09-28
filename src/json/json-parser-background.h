// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_PARSER_BACKGROUND_H_
#define V8_JSON_JSON_PARSER_BACKGROUND_H_

#include <atomic>
#include <cstring>
#include <deque>
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
    kSmi = 16,
    kAllNumbers = 32,
    kAllSmis = 64,
    kCachedString = 128,
    // Kind-specific: an object has at least one directly cached named key.
    kHasCachedKeys = kCachedString
  };
  union {
    // Store the bits with four-byte alignment; memcpy accessors avoid undefined
    // unaligned double loads on platforms with stricter alignment than x64.
    uint32_t number_bits[2];
    struct {
      uint32_t length, start;
    } string;
    struct {
      uint32_t end, count;
    } container;
  } data{};
  Kind kind = kNull;
  uint8_t flags = 0;
  // Container depth (saturated at 17), or a value/key string cache slot.
  uint16_t depth = 0;

  double Number() const {
    double value;
    std::memcpy(&value, data.number_bits, sizeof(value));
    return value;
  }
  void SetNumber(double value) {
    std::memcpy(data.number_bits, &value, sizeof(value));
  }

  JsonString AsString(int source_offset = 0, bool cache_key = false) const {
    if (flags & kIndex) return JsonString(data.string.start);
    return JsonString(source_offset + data.string.start + 1, data.string.length,
                      flags & kConvert, flags & kInternalize, flags & kEscape,
                      cache_key && kind == kKey && (flags & kCachedString)
                          ? depth
                          : JsonString::kNoCacheSlot);
  }
};
static_assert(sizeof(BackgroundJsonNode) == 12);

struct BackgroundJsonData {
  explicit BackgroundJsonData(bool track_source) : track_source(track_source) {}
  const bool track_source;
  std::atomic<bool> cancelled{false};
  base::OwnedVector<uint8_t> one_byte;
  base::OwnedVector<uint16_t> two_byte;
  bool is_one_byte = true;
  std::deque<BackgroundJsonNode> nodes;
  size_t node_base = 0;
  // Needed only by revivers, not by the common no-reviver path.
  std::vector<uint32_t> source_ends;
  std::vector<uint32_t> source_starts;
  bool failed = false;
  int error_position = 0;
  base::Optional<MessageTemplate> error_message;

  const BackgroundJsonNode& NodeAt(size_t index) const {
    DCHECK_GE(index, node_base);
    return nodes[index - node_base];
  }
  size_t NodeCount() const { return node_base + nodes.size(); }
  void DiscardBefore(size_t index) {
    DCHECK_GE(index, node_base);
    DCHECK_LE(index, NodeCount());
    nodes.erase(nodes.begin(), nodes.begin() + (index - node_base));
    node_base = index;
  }

  size_t MemoryUsage() const {
    // Estimate live payload; deque block slack and allocator bookkeeping are
    // deliberately not treated as an exact process-memory measurement.
    return one_byte.size() + two_byte.size() * sizeof(uint16_t) +
           nodes.size() * sizeof(BackgroundJsonNode) +
           (source_starts.capacity() + source_ends.capacity()) *
               sizeof(uint32_t);
  }
};

// Uses only native memory, immutable input, and V8's numeric conversion helper.
void ParseJsonInBackground(BackgroundJsonData* data);

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_PARSER_BACKGROUND_H_
