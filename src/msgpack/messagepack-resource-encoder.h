// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Included after ResourceByteWriter. Analysis uses native data only: the
// ordinary encoder first validates the JS profile and captures an owned wire
// value. Keep the offline packer's template instantiations separate. Sharing
// the production writer changes its inlining decisions and register allocation.
class ResourceByteWriter : public ByteWriter {
 public:
  using ByteWriter::ByteWriter;
};
class ResourceEncoder {
 public:
  using Node = msgpack::object;
  explicit ResourceEncoder(MessagePackBuffer* output,
                           const std::atomic<bool>* cancelled = nullptr,
                           bool inplace_extensions = true)
      : output_(output), cancelled_(cancelled),
        inplace_extensions_(inplace_extensions) {}
  void Encode(const Node& root) {
#ifdef MSGPACK_RESOURCE_TIMINGS
    auto started = base::TimeTicks::Now();
#endif
    Count(root);
#ifdef MSGPACK_RESOURCE_TIMINGS
    auto counted = base::TimeTicks::Now();
#endif
    for (auto& shape : shapes_) {
      CheckCancelled();
      size_t keys = 0;
      for (const auto& key : shape.keys) keys += StringSize(key.size());
        // Charge the complete definition and a conservative reference header.
#if !defined(MSGPACK_RESOURCE_NO_SHAPES)
      if (shape.count > 1 && keys > 12 &&
          shape.count * (keys - 12) > keys + 5) {
        shape.id = static_cast<int>(selected_shapes_.size());
        selected_shapes_.push_back(&shape);
      }
#endif
    }
    for (auto& str : strings_) {
      CheckCancelled();
      size_t id = selected_strings_.size();
      size_t ref = ExtensionSize(UIntSize(id));
      size_t literal = StringSize(str.value.size());
#if !defined(MSGPACK_RESOURCE_NO_STRINGS)
      if (literal > ref && str.count > literal / (literal - ref)) {
        str.id = static_cast<int>(id);
        selected_strings_.push_back(&str);
        selected_string_index_.emplace(str.value, str.id);
        minimum_string_ = std::min(minimum_string_, str.value.size());
        maximum_string_ = std::max(maximum_string_, str.value.size());
      }
#endif
    }
    uint64_t metadata = selected_shapes_.size() * uint64_t{48} +
                        selected_strings_.size() * uint64_t{16};
    for (const auto* shape : selected_shapes_)
      metadata += shape->keys.size() * uint64_t{16};
    if (metadata > kMaxOutputBytes)
      throw std::length_error("MessagePack resource metadata limit exceeded");
    ResourceByteWriter w(output_);
    msgpack::packer<ResourceByteWriter> p(w);
    w.write(reinterpret_cast<const char*>(messagepack_resources::kMagic), 5);
    p.pack_array(3);
    p.pack_array(static_cast<uint32_t>(selected_shapes_.size()));
    for (const auto* shape : selected_shapes_) {
      p.pack_array(static_cast<uint32_t>(shape->keys.size()));
      for (const auto& key : shape->keys) String(p, key);
    }
    p.pack_array(static_cast<uint32_t>(selected_strings_.size()));
    for (const auto* str : selected_strings_) String(p, str->value);
    Write(root, output_);
#ifdef MSGPACK_RESOURCE_TIMINGS
    auto finished = base::TimeTicks::Now();
    std::fprintf(stderr, "resource count=%.3f write=%.3f ms\n",
                 (counted - started).InMillisecondsF(),
                 (finished - counted).InMillisecondsF());
#endif
  }
  void EncodeNumbers(const uint8_t* bytes, uint32_t count, bool integers,
                     size_t standard_size) {
#if defined(MSGPACK_RESOURCE_NO_NUMBERS)
    return;
#endif
    MessagePackBuffer body;
    PackNumbers(count, [&](uint32_t i) {
      if (integers) {
        int32_t value;
        std::memcpy(&value, bytes + size_t{i} * 4, 4);
        return static_cast<double>(value);
      }
      double value;
      std::memcpy(&value, bytes + size_t{i} * 8, 8);
      return value;
    }, &body);
    // The empty shape/string envelope occupies eight bytes. This is the same
    // complete candidate and strict size decision as the ordinary native tree.
    if (8 + ExtensionSize(body.size()) >= standard_size) return;
    ResourceByteWriter w(output_);
    msgpack::packer<ResourceByteWriter> p(w);
    w.write(reinterpret_cast<const char*>(messagepack_resources::kMagic), 5);
    p.pack_array(3);
    p.pack_array(0);
    p.pack_array(0);
    Extension(output_, messagepack_resources::kNumbers, body);
  }

 private:
  struct Shape {
    std::vector<std::string_view> keys;
    size_t count = 0;
    int id = -1;
    const msgpack::object_kv* fields = nullptr;
  };
  struct Text {
    std::string_view value;
    size_t count = 0;
    int id = -1;
  };
  struct Block {
    unsigned kind = 9, scale = 0;
    std::vector<uint8_t> bytes;
  };
  static size_t UIntSize(uint64_t n) {
    return n < 128           ? 1
           : n <= 255        ? 2
           : n <= 65535      ? 3
           : n <= UINT32_MAX ? 5
                             : 9;
  }
  static size_t StringSize(size_t n) {
    return n + (n < 32 ? 1 : n < 256 ? 2 : n < 65536 ? 3 : 5);
  }
  static size_t ExtensionSize(size_t n) {
    return n + ((n == 1 || n == 2 || n == 4 || n == 8 || n == 16) ? 2
                : n < 256                                         ? 3
                : n < 65536                                       ? 4
                                                                  : 6);
  }
  static std::string_view TextValue(const Node& n) {
    return std::string_view(n.via.str.ptr, n.via.str.size);
  }
  static void String(msgpack::packer<ResourceByteWriter>& p,
                     std::string_view s) {
    p.pack_str(static_cast<uint32_t>(s.size()));
    p.pack_str_body(s.data(), static_cast<uint32_t>(s.size()));
  }
  struct ShapeKey {
    const msgpack::object_kv* fields;
    uint32_t count;
  };
  struct ShapeHash {
    size_t operator()(ShapeKey shape) const {
      size_t hash = shape.count;
      for (uint32_t i = 0; i < shape.count; ++i)
        hash = (hash ^ std::hash<std::string_view>{}(
                           TextValue(shape.fields[i].key))) * 16777619u;
      return hash;
    }
  };
  struct ShapeEqual {
    bool operator()(ShapeKey a, ShapeKey b) const {
      if (a.count != b.count) return false;
      for (uint32_t i = 0; i < a.count; ++i)
        if (TextValue(a.fields[i].key) != TextValue(b.fields[i].key))
          return false;
      return true;
    }
  };
  static ShapeKey Signature(const Node& n) {
    return {n.via.map.ptr, n.via.map.size};
  }
  size_t FindShape(const Node& n, bool create) {
    ShapeKey key = Signature(n);
    uint32_t slot = key.count * 31;
    if (key.count) {
      auto first = TextValue(key.fields[0].key);
      slot += static_cast<uint32_t>(first.size()) * 17;
      if (!first.empty()) slot += static_cast<uint8_t>(first.front());
    }
    uint32_t& cached = shape_cache_[slot & 63];
    if (cached) {
      const Shape& shape = shapes_[cached - 1];
      if (ShapeEqual{}(key, {shape.fields,
                            static_cast<uint32_t>(shape.keys.size())}))
        return cached - 1;
    }
    auto found = shape_index_.find(key);
    size_t index;
    if (found != shape_index_.end()) {
      index = found->second;
    } else {
      DCHECK(create);
      index = shapes_.size();
      Shape shape;
      shape.fields = key.fields;
      shape.keys.reserve(key.count);
      for (uint32_t i = 0; i < key.count; ++i)
        shape.keys.push_back(TextValue(key.fields[i].key));
      shapes_.push_back(std::move(shape));
      shape_index_.emplace(key, index);
    }
    cached = static_cast<uint32_t>(index + 1);
    return index;
  }
  void Count(const Node& n) {
    CheckCancelled();
    if (n.type == msgpack::type::MAP) {
      ++shapes_[FindShape(n, true)].count;
      for (uint32_t i = 0; i < n.via.map.size; ++i) Count(n.via.map.ptr[i].val);
    } else if (n.type == msgpack::type::ARRAY) {
      for (uint32_t i = 0; i < n.via.array.size; ++i) Count(n.via.array.ptr[i]);
    } else if (n.type == msgpack::type::STR) {
      std::string_view value = TextValue(n);
      uint32_t slot = static_cast<uint32_t>(value.size()) * 31;
      if (!value.empty())
        slot += static_cast<uint8_t>(value.front()) * 17 +
                static_cast<uint8_t>(value.back());
      uint32_t& cached = text_cache_[slot & 127];
      if (cached && strings_[cached - 1].value == value) {
        ++strings_[cached - 1].count;
        return;
      }
      auto inserted = string_index_.emplace(value, strings_.size());
      if (inserted.second) strings_.push_back({value, 0, -1});
      ++strings_[inserted.first->second].count;
      cached = static_cast<uint32_t>(inserted.first->second + 1);
    }
  }
  static void Extension(MessagePackBuffer* dest, int8_t type,
                        const MessagePackBuffer& body) {
    ResourceByteWriter w(dest);
    msgpack::packer<ResourceByteWriter> p(w);
    p.pack_ext(static_cast<uint32_t>(body.size()), type);
    p.pack_ext_body(reinterpret_cast<const char*>(body.data()),
                    static_cast<uint32_t>(body.size()));
  }
  // Reserve the largest header, write the body once, then close the gap for
  // its canonical header. Offsets survive buffer growth; no pointer does.
  static void FinishExtension(MessagePackBuffer* dest, size_t start,
                              int8_t type) {
    size_t size = dest->size() - start - 6;
    size_t header = ExtensionSize(size) - size;
    uint8_t* bytes = const_cast<uint8_t*>(dest->data()) + start;
    if (header != 6) {
      std::memmove(bytes + header, bytes + 6, size);
      dest->Rewind(dest->size() - (6 - header));
    }
    if (size == 1 || size == 2 || size == 4 || size == 8 || size == 16) {
      bytes[0] = static_cast<uint8_t>(0xd4 + base::bits::CountTrailingZeros(
                                                    static_cast<uint32_t>(size)));
      bytes[1] = static_cast<uint8_t>(type);
    } else {
      unsigned width = size < 256 ? 1 : size < 65536 ? 2 : 4;
      bytes[0] = width == 1 ? 0xc7 : width == 2 ? 0xc8 : 0xc9;
      messagepack_resources::Store(bytes + 1, size, width);
      bytes[width + 1] = static_cast<uint8_t>(type);
    }
  }
  static bool Numeric(const Node& n, double* value) {
    if (n.type == msgpack::type::POSITIVE_INTEGER &&
        n.via.u64 <= kMaxSafeInteger)
      *value = static_cast<double>(n.via.u64);
    else if (n.type == msgpack::type::NEGATIVE_INTEGER &&
             n.via.i64 >= -static_cast<int64_t>(kMaxSafeInteger))
      *value = static_cast<double>(n.via.i64);
    else if (n.type == msgpack::type::FLOAT32 ||
             n.type == msgpack::type::FLOAT64)
      *value = n.via.f64;
    else
      return false;
    return true;
  }
  static Block PackBlock(const double* numbers, uint32_t count) {
    Block b;
    // Integer and scaled-integer candidates must survive binary64 bit equality.
    for (unsigned scale = 0; scale < 10; ++scale) {
      int64_t ints[256];
      bool valid = true;
      int64_t minimum = INT64_MAX, maximum = INT64_MIN;
      for (uint32_t i = 0; i < count; ++i) {
        double x = numbers[i] * messagepack_resources::kScale[scale];
        if (!std::isfinite(x) ||
            std::fabs(x) > static_cast<double>(kMaxSafeInteger)) {
          valid = false;
          break;
        }
        int64_t n = static_cast<int64_t>(std::round(x));
        double restored =
            static_cast<double>(n) / messagepack_resources::kScale[scale];
        if (base::bit_cast<uint64_t>(restored) !=
            base::bit_cast<uint64_t>(numbers[i])) {
          valid = false;
          break;
        }
        ints[i] = n;
        minimum = std::min(minimum, n);
        maximum = std::max(maximum, n);
      }
      if (!valid) continue;
      unsigned kind = 7;
      if (minimum >= 0) {
        kind = maximum <= UINT8_MAX    ? 0
               : maximum <= UINT16_MAX ? 2
               : maximum <= UINT32_MAX ? 4
                                       : 6;
      } else {
        kind = minimum >= INT8_MIN && maximum <= INT8_MAX     ? 1
               : minimum >= INT16_MIN && maximum <= INT16_MAX ? 3
               : minimum >= INT32_MIN && maximum <= INT32_MAX ? 5
                                                              : 7;
      }
      if (messagepack_resources::kWidth[kind] >=
              messagepack_resources::kWidth[b.kind] &&
          b.kind != 9)
        continue;
      b.kind = kind;
      b.scale = scale;
      b.bytes.resize(count * messagepack_resources::kWidth[kind]);
      for (uint32_t i = 0; i < count; ++i)
        messagepack_resources::Store(
            b.bytes.data() + i * messagepack_resources::kWidth[kind],
            static_cast<uint64_t>(ints[i]),
            messagepack_resources::kWidth[kind]);
      // Round(abs(x) * scale) is monotone in the positive scale. The first
      // exact scale therefore has the narrowest integer range; later scales
      // cannot improve its width. Equal-width choices already keep this scale.
      break;
    }
    if (messagepack_resources::kWidth[b.kind] > 4) {
      bool exact = true;
      for (uint32_t i = 0; i < count; ++i) {
        float n = static_cast<float>(numbers[i]);
        if (base::bit_cast<uint64_t>(static_cast<double>(n)) !=
                base::bit_cast<uint64_t>(numbers[i])) {
          exact = false;
          break;
        }
      }
      if (exact) {
        b.kind = 8;
        b.scale = 0;
      }
    }
    if (b.kind >= 8) {
      unsigned width = messagepack_resources::kWidth[b.kind];
      b.bytes.resize(count * width);
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t bits =
            b.kind == 8
                ? base::bit_cast<uint32_t>(static_cast<float>(numbers[i]))
                : base::bit_cast<uint64_t>(numbers[i]);
        messagepack_resources::Store(b.bytes.data() + i * width, bits, width);
      }
    }
    return b;
  }
  bool Numbers(const Node& n, MessagePackBuffer* body) {
#if defined(MSGPACK_RESOURCE_NO_NUMBERS)
    return false;
#endif
    if (n.via.array.size < 16) return false;
    // Reject a late mixed/BigInt value before building any scaled blocks.
    for (uint32_t i = 0; i < n.via.array.size; ++i) {
      double value;
      if (!Numeric(n.via.array.ptr[i], &value)) return false;
    }

    PackNumbers(n.via.array.size, [&](uint32_t i) {
      double number;
      CHECK(Numeric(n.via.array.ptr[i], &number));
      return number;
    }, body);
    return true;
  }
  template <typename Reader>
  void PackNumbers(uint32_t length, Reader read, MessagePackBuffer* body) {
    std::vector<Block> blocks;
    blocks.reserve((length + 255) / 256);
    for (uint32_t start = 0; start < length; start += 256) {
      CheckCancelled();
      uint32_t count = std::min<uint32_t>(256, length - start);
      double numbers[256];
      for (uint32_t i = 0; i < count; ++i)
        numbers[i] = read(start + i);
      blocks.push_back(PackBlock(numbers, count));
    }
    ResourceByteWriter w(body);
    msgpack::packer<ResourceByteWriter> p(w);
    p.pack_array(2);
    p.pack_uint32(length);
    p.pack_array(static_cast<uint32_t>(blocks.size()));
    for (const auto& b : blocks) {
      p.pack_array(3);
      p.pack_uint32(b.kind);
      p.pack_uint32(b.scale);
      p.pack_bin(static_cast<uint32_t>(b.bytes.size()));
      p.pack_bin_body(reinterpret_cast<const char*>(b.bytes.data()),
                      static_cast<uint32_t>(b.bytes.size()));
    }
  }
  void Write(const Node& n, MessagePackBuffer* dest) {
    CheckCancelled();
    ResourceByteWriter w(dest);
    msgpack::packer<ResourceByteWriter> p(w);
    if (n.type == msgpack::type::STR) {
      auto text = TextValue(n);
      auto selected = selected_string_index_.end();
      if (text.size() >= minimum_string_ && text.size() <= maximum_string_)
        selected = selected_string_index_.find(text);
      if (selected != selected_string_index_.end()) {
        p.pack_ext(static_cast<uint32_t>(UIntSize(selected->second)),
                   messagepack_resources::kString);
        p.pack_uint32(selected->second);
      } else
        p.pack(n);
    } else if (n.type == msgpack::type::MAP) {
      const Shape& shape = shapes_[FindShape(n, false)];
      if (shape.id < 0) {
        p.pack_map(n.via.map.size);
        for (uint32_t i = 0; i < n.via.map.size; ++i) {
          p.pack(n.via.map.ptr[i].key);
          Write(n.via.map.ptr[i].val, dest);
        }
        return;
      }
      // Selected shapes have more than 12 key bytes. A record adds at most
      // array(2) + uint32(id) + ext32 = 12 bytes to its value array, so the
      // existing per-record profitability check always succeeds for them.
      if (!inplace_extensions_) {
        // A near-limit standard value leaves no room for temporary header
        // padding. Preserve the canonical candidate with bounded body storage.
        MessagePackBuffer body;
        ResourceByteWriter body_writer(&body);
        msgpack::packer<ResourceByteWriter> body_packer(body_writer);
        body_packer.pack_array(2);
        body_packer.pack_uint32(shape.id);
        body_packer.pack_array(n.via.map.size);
        for (uint32_t i = 0; i < n.via.map.size; ++i)
          Write(n.via.map.ptr[i].val, &body);
        Extension(dest, messagepack_resources::kShape, body);
        return;
      }
      size_t start = dest->size();
      dest->Append(6);
      p.pack_array(2);
      p.pack_uint32(shape.id);
      p.pack_array(n.via.map.size);
      for (uint32_t i = 0; i < n.via.map.size; ++i)
        Write(n.via.map.ptr[i].val, dest);
      FinishExtension(dest, start, messagepack_resources::kShape);
    } else if (n.type == msgpack::type::ARRAY) {
      MessagePackBuffer packed;
      if (Numbers(n, &packed)) {
        struct Measure {
          size_t size = 0;
          void write(const char*, size_t n) { size += n; }
        } measure;
        msgpack::packer<Measure> plain(measure);
        plain.pack_array(n.via.array.size);
        for (uint32_t i = 0; i < n.via.array.size; ++i)
          plain.pack(n.via.array.ptr[i]);
        if (ExtensionSize(packed.size()) < measure.size) {
          Extension(dest, messagepack_resources::kNumbers, packed);
          return;
        }
      }
      p.pack_array(n.via.array.size);
      for (uint32_t i = 0; i < n.via.array.size; ++i)
        Write(n.via.array.ptr[i], dest);
    } else
      p.pack(n);
  }
  MessagePackBuffer* output_;
  void CheckCancelled() const {
    // Bound cancellation polling while avoiding an atomic load for every
    // primitive field. Numeric blocks each contain at most 256 values.
    if (cancelled_ && !(++cancel_check_ & 63) &&
        cancelled_->load(std::memory_order_relaxed))
      throw std::runtime_error("MessagePack resource operation cancelled");
  }
  const std::atomic<bool>* cancelled_;
  const bool inplace_extensions_;
  mutable uint32_t cancel_check_ = 0;
  std::vector<Shape> shapes_;
  std::vector<Text> strings_;
  std::vector<Shape*> selected_shapes_;
  std::vector<Text*> selected_strings_;
  std::unordered_map<ShapeKey, size_t, ShapeHash, ShapeEqual> shape_index_;
  std::unordered_map<std::string_view, size_t> string_index_;
  std::unordered_map<std::string_view, uint32_t> selected_string_index_;
  uint32_t shape_cache_[64]{};
  uint32_t text_cache_[128]{};
  size_t minimum_string_ = SIZE_MAX, maximum_string_ = 0;
};
