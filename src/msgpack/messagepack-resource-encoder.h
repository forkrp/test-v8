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
  explicit ResourceEncoder(MessagePackBuffer* output) : output_(output) {}
  void Encode(const Node& root) {
    Count(root);
    for (auto& shape : shapes_) {
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
      size_t id = selected_strings_.size();
      size_t ref = ExtensionSize(UIntSize(id));
      size_t literal = StringSize(str.value.size());
#if !defined(MSGPACK_RESOURCE_NO_STRINGS)
      if (literal > ref && str.count > literal / (literal - ref)) {
        str.id = static_cast<int>(id);
        selected_strings_.push_back(&str);
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
  }

 private:
  struct Shape {
    std::vector<std::string> keys;
    size_t count = 0;
    int id = -1;
  };
  struct Text {
    std::string value;
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
  static std::string TextValue(const Node& n) {
    return std::string(n.via.str.ptr, n.via.str.size);
  }
  static void String(msgpack::packer<ResourceByteWriter>& p,
                     const std::string& s) {
    p.pack_str(static_cast<uint32_t>(s.size()));
    p.pack_str_body(s.data(), static_cast<uint32_t>(s.size()));
  }
  static std::string Signature(const Node& n) {
    std::string signature;
    for (uint32_t i = 0; i < n.via.map.size; ++i) {
      const Node& key = n.via.map.ptr[i].key;
      char length[4];
      messagepack_resources::Store(reinterpret_cast<uint8_t*>(length),
                                   key.via.str.size, 4);
      signature.append(length, 4);
      signature.append(key.via.str.ptr, key.via.str.size);
    }
    return signature;
  }
  void Count(const Node& n) {
    if (n.type == msgpack::type::MAP) {
      std::string sig = Signature(n);
      auto inserted = shape_index_.emplace(sig, shapes_.size());
      if (inserted.second) {
        Shape s;
        for (uint32_t i = 0; i < n.via.map.size; ++i)
          s.keys.push_back(TextValue(n.via.map.ptr[i].key));
        shapes_.push_back(std::move(s));
      }
      ++shapes_[inserted.first->second].count;
      for (uint32_t i = 0; i < n.via.map.size; ++i) Count(n.via.map.ptr[i].val);
    } else if (n.type == msgpack::type::ARRAY) {
      for (uint32_t i = 0; i < n.via.array.size; ++i) Count(n.via.array.ptr[i]);
    } else if (n.type == msgpack::type::STR) {
      std::string value = TextValue(n);
      auto inserted = string_index_.emplace(value, strings_.size());
      if (inserted.second) strings_.push_back({std::move(value), 0, -1});
      ++strings_[inserted.first->second].count;
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
      if (messagepack_resources::kWidth[kind] == 1) break;
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
  static bool Numbers(const Node& n, MessagePackBuffer* body) {
#if defined(MSGPACK_RESOURCE_NO_NUMBERS)
    return false;
#endif
    if (n.via.array.size < 16) return false;
    // Reject a late mixed/BigInt value before building any scaled blocks.
    for (uint32_t i = 0; i < n.via.array.size; ++i) {
      double value;
      if (!Numeric(n.via.array.ptr[i], &value)) return false;
    }

    std::vector<Block> blocks;
    for (uint32_t start = 0; start < n.via.array.size; start += 256) {
      uint32_t count = std::min<uint32_t>(256, n.via.array.size - start);
      double numbers[256];
      for (uint32_t i = 0; i < count; ++i)
        if (!Numeric(n.via.array.ptr[start + i], &numbers[i])) return false;
      blocks.push_back(PackBlock(numbers, count));
    }
    ResourceByteWriter w(body);
    msgpack::packer<ResourceByteWriter> p(w);
    p.pack_array(2);
    p.pack_uint32(n.via.array.size);
    p.pack_array(static_cast<uint32_t>(blocks.size()));
    for (const auto& b : blocks) {
      p.pack_array(3);
      p.pack_uint32(b.kind);
      p.pack_uint32(b.scale);
      p.pack_bin(static_cast<uint32_t>(b.bytes.size()));
      p.pack_bin_body(reinterpret_cast<const char*>(b.bytes.data()),
                      static_cast<uint32_t>(b.bytes.size()));
    }
    return true;
  }
  void Write(const Node& n, MessagePackBuffer* dest) {
    ResourceByteWriter w(dest);
    msgpack::packer<ResourceByteWriter> p(w);
    if (n.type == msgpack::type::STR) {
      const auto& text = strings_[string_index_.at(TextValue(n))];
      if (text.id >= 0) {
        MessagePackBuffer ref;
        ResourceByteWriter rw(&ref);
        msgpack::packer<ResourceByteWriter> rp(rw);
        rp.pack_uint32(text.id);
        Extension(dest, messagepack_resources::kString, ref);
      } else
        p.pack(n);
    } else if (n.type == msgpack::type::MAP) {
      const Shape& shape = shapes_[shape_index_.at(Signature(n))];
      if (shape.id < 0) {
        p.pack_map(n.via.map.size);
        for (uint32_t i = 0; i < n.via.map.size; ++i) {
          p.pack(n.via.map.ptr[i].key);
          Write(n.via.map.ptr[i].val, dest);
        }
        return;
      }
      MessagePackBuffer values;
      ResourceByteWriter vw(&values);
      msgpack::packer<ResourceByteWriter> vp(vw);
      vp.pack_array(n.via.map.size);
      std::vector<std::pair<size_t, size_t>> spans;
      for (uint32_t i = 0; i < n.via.map.size; ++i) {
        size_t start = values.size();
        Write(n.via.map.ptr[i].val, &values);
        spans.emplace_back(start, values.size() - start);
      }
      MessagePackBuffer record;
      ResourceByteWriter rw(&record);
      msgpack::packer<ResourceByteWriter> rp(rw);
      rp.pack_array(2);
      rp.pack_uint32(shape.id);
      rw.write(reinterpret_cast<const char*>(values.data()), values.size());
      size_t keys = 0;
      for (const auto& key : shape.keys) keys += StringSize(key.size());
      if (ExtensionSize(record.size()) < values.size() + keys) {
        Extension(dest, messagepack_resources::kShape, record);
      } else {
        p.pack_map(n.via.map.size);
        for (uint32_t i = 0; i < n.via.map.size; ++i) {
          p.pack(n.via.map.ptr[i].key);
          w.write(reinterpret_cast<const char*>(values.data() + spans[i].first),
                  spans[i].second);
        }
      }
    } else if (n.type == msgpack::type::ARRAY) {
      MessagePackBuffer ordinary;
      ResourceByteWriter ow(&ordinary);
      msgpack::packer<ResourceByteWriter> op(ow);
      op.pack_array(n.via.array.size);
      for (uint32_t i = 0; i < n.via.array.size; ++i)
        Write(n.via.array.ptr[i], &ordinary);
      MessagePackBuffer packed;
      if (Numbers(n, &packed) && ExtensionSize(packed.size()) < ordinary.size())
        Extension(dest, messagepack_resources::kNumbers, packed);
      else
        w.write(reinterpret_cast<const char*>(ordinary.data()),
                ordinary.size());
    } else
      p.pack(n);
  }
  MessagePackBuffer* output_;
  std::vector<Shape> shapes_;
  std::vector<Text> strings_;
  std::vector<Shape*> selected_shapes_;
  std::vector<Text*> selected_strings_;
  std::unordered_map<std::string, size_t> shape_index_, string_index_;
};
