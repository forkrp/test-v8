// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_PARSER_BACKGROUND_H_
#define V8_JSON_JSON_PARSER_BACKGROUND_H_

#include <atomic>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "src/base/vector.h"
#include "src/common/message-template.h"
#include "src/json/json-parser.h"

namespace v8 {
class Platform;
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

#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
// Freeing many small deque blocks can make the Android 32-bit allocator
// repeatedly scan for pages to return to the OS. Reserve larger blocks once,
// keep their node addresses stable, and release each block as it is consumed.
class BackgroundJsonTape {
 private:
  static constexpr size_t kInitialBlockNodes = 256;
  static constexpr size_t kBlockNodes = 8192;

  class Block {
   public:
    explicit Block(size_t capacity);
    ~Block();
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    const BackgroundJsonNode* data() const { return nodes_; }
    size_t size() const { return size_; }
    bool full() const { return size_ == capacity_; }
    size_t AllocatedBytes() const {
      return mapped_size_ ? mapped_size_
                          : capacity_ * sizeof(BackgroundJsonNode);
    }
    BackgroundJsonNode& operator[](size_t index) { return nodes_[index]; }
    const BackgroundJsonNode& operator[](size_t index) const {
      return nodes_[index];
    }
    void push_back(const BackgroundJsonNode& node) { nodes_[size_++] = node; }

   private:
    const size_t capacity_;
    size_t mapped_size_ = 0;
    size_t size_ = 0;
    BackgroundJsonNode* nodes_;
  };

 public:
  class const_iterator {
   public:
    const_iterator() = default;
    const BackgroundJsonNode& operator*() const { return *node_; }
    const BackgroundJsonNode* operator->() const { return node_; }
    bool operator==(const const_iterator& other) const {
      return node_ == other.node_;
    }

    const_iterator& operator++() {
      if (++node_ == block_end_) {
        if (++block_index_ == tape_->blocks_.size()) {
          node_ = nullptr;
        } else {
          SetBlock(0);
        }
      }
      return *this;
    }
    const_iterator operator++(int) {
      auto previous = *this;
      ++*this;
      return previous;
    }

   private:
    friend class BackgroundJsonTape;
    const_iterator(const BackgroundJsonTape* tape, size_t offset)
        : tape_(tape) {
      SetBlock(offset);
    }
    void SetBlock(size_t offset) {
      const auto& block = tape_->blocks_[block_index_];
      node_ = block.data() + offset;
      block_end_ = block.data() + block.size();
    }

    const BackgroundJsonTape* tape_ = nullptr;
    size_t block_index_ = 0;
    const BackgroundJsonNode* node_ = nullptr;
    const BackgroundJsonNode* block_end_ = nullptr;
  };

  BackgroundJsonTape() = default;
  BackgroundJsonTape(BackgroundJsonTape&& other) noexcept {
    *this = std::move(other);
  }
  BackgroundJsonTape& operator=(BackgroundJsonTape&& other) noexcept {
    blocks_.swap(other.blocks_);
    std::swap(front_offset_, other.front_offset_);
    std::swap(size_, other.size_);
    std::swap(allocated_bytes_, other.allocated_bytes_);
    return *this;
  }

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  size_t AllocatedBytes() const { return allocated_bytes_; }
  BackgroundJsonNode& front() { return blocks_.front()[front_offset_]; }
  BackgroundJsonNode& back() {
    auto& block = blocks_.back();
    return block[block.size() - 1];
  }
  const_iterator begin() const { return cbegin(); }
  const_iterator end() const { return {}; }
  const_iterator cbegin() const {
    return empty() ? const_iterator{} : const_iterator(this, front_offset_);
  }
  const BackgroundJsonNode& operator[](size_t index) const {
    DCHECK_LT(index, size_);
    index += front_offset_;
    for (const auto& block : blocks_) {
      if (index < block.size()) return block[index];
      index -= block.size();
    }
    UNREACHABLE();
  }

  void push_back(const BackgroundJsonNode& node) {
    if (blocks_.empty() || blocks_.back().full()) {
      blocks_.emplace_back(blocks_.empty() ? kInitialBlockNodes : kBlockNodes);
      allocated_bytes_ += blocks_.back().AllocatedBytes();
    }
    blocks_.back().push_back(node);
    ++size_;
  }

  void DiscardPrefix(size_t count) {
    DCHECK_LE(count, size_);
    size_ -= count;
    front_offset_ += count;
    while (!blocks_.empty() && front_offset_ >= blocks_.front().size()) {
      front_offset_ -= blocks_.front().size();
      allocated_bytes_ -= blocks_.front().AllocatedBytes();
      blocks_.pop_front();
    }
  }

 private:
  std::deque<Block> blocks_;
  size_t front_offset_ = 0;
  size_t size_ = 0;
  size_t allocated_bytes_ = 0;
};
#else
using BackgroundJsonTape = std::deque<BackgroundJsonNode>;
#endif

struct BackgroundJsonData {
  explicit BackgroundJsonData(bool track_source) : track_source(track_source) {}
  const bool track_source;
  std::atomic<bool> cancelled{false};
  base::OwnedVector<uint8_t> one_byte;
  base::OwnedVector<uint16_t> two_byte;
  bool is_one_byte = true;
  BackgroundJsonTape nodes;
  size_t node_base = 0;
  // The foreground owns the tapes once the background job has joined.
  std::vector<BackgroundJsonTape> remaining_parts;
  std::vector<size_t> part_boundaries;
  size_t total_nodes = 0;
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
  size_t NodeCount() const {
    return total_nodes ? total_nodes : node_base + nodes.size();
  }
  void DiscardBefore(size_t index) {
    DCHECK_GE(index, node_base);
    DCHECK_LE(index, node_base + nodes.size());
#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
    nodes.DiscardPrefix(index - node_base);
#else
    nodes.erase(nodes.begin(), nodes.begin() + (index - node_base));
#endif
    node_base = index;
  }

  size_t MemoryUsage() const {
#if defined(V8_OS_ANDROID) && defined(V8_HOST_ARCH_ARM)
    // A partially consumed block stays allocated until its last node is used.
    size_t tape_bytes = nodes.AllocatedBytes();
    for (const auto& part : remaining_parts) tape_bytes += part.AllocatedBytes();
#else
    // Estimate live payload; deque block slack and allocator bookkeeping are
    // deliberately not treated as an exact process-memory measurement.
    size_t live_nodes = nodes.size();
    for (const auto& part : remaining_parts) live_nodes += part.size();
    size_t tape_bytes = live_nodes * sizeof(BackgroundJsonNode);
#endif
    return one_byte.size() + two_byte.size() * sizeof(uint16_t) +
           tape_bytes +
           (source_starts.capacity() + source_ends.capacity()) *
               sizeof(uint32_t);
  }
};

// Uses only native memory, immutable input, and V8's numeric conversion helper.
void ParseJsonInBackground(BackgroundJsonData* data, Platform* platform);

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_PARSER_BACKGROUND_H_
