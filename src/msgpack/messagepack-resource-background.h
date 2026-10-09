// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Included in the background codec's anonymous namespace. Expand compact
// resources into owned standard wire bytes before the existing tape parser and
// cooperative foreground builder run. No V8 handles or heap access belong here.
class ResourceWireDecoder {
 public:
  explicit ResourceWireDecoder(MessagePackAsyncData* data)
      : data_(data), end_(data->input.size()) {}
  void Decode() {
    if (end_ < 5 || data_->input.data()[4] != 1)
      Fail("Unsupported MessagePack resource version");
    pos_ = 5;
    if (ArrayCount() != 3) Fail("Malformed MessagePack resource envelope");
    uint32_t count = ArrayCount();
    if (count > end_ - pos_) Fail("Malformed MessagePack resource shape count");
    Charge(uint64_t{count} * 48, uint64_t{count} * sizeof(shapes_[0]));
    shapes_.resize(count);
    for (auto& shape : shapes_) {
      CheckCancelled();
      uint32_t fields = ArrayCount();
      if (fields > end_ - pos_)
        Fail("Malformed MessagePack resource field count");
      Charge(uint64_t{fields} * 16, uint64_t{fields} * sizeof(Span));
      shape.reserve(fields);
      // Views reference immutable owned input; no string body is copied here.
      std::unordered_set<std::string_view> seen;
      CheckBudget(uint64_t{fields} * 64);
      for (uint32_t i = 0; i < fields; ++i) {
        Span key = String();
        if (!seen.insert(key.text).second)
          Fail("Duplicate MessagePack resource shape key");
        shape.push_back(key);
      }
    }
    count = ArrayCount();
    if (count > end_ - pos_)
      Fail("Malformed MessagePack resource string count");
    Charge(uint64_t{count} * 16, uint64_t{count} * sizeof(Span));
    strings_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) strings_.push_back(String());
    Value(0, false);
    if (pos_ != end_) Fail("Trailing bytes after MessagePack value");
    // Release the compact input before allocating the native tape. Swapping
    // preserves malloc/page-mapping ownership and avoids another byte copy.
    data_->input.Swap(&data_->output);
    data_->output.Clear();
  }

 private:
  struct Span {
    size_t offset, length;
    std::string_view text;
  };
  [[noreturn]] static void Fail(const char* message) {
    throw std::runtime_error(message);
  }
  void CheckCancelled() const {
    if (Cancelled(data_)) Fail("MessagePack resource operation cancelled");
  }
  void CheckBudget(uint64_t temporary = 0) const {
    if (data_->MemoryUsage() + std::max(metadata_, table_bytes_) + temporary >
        MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
  }
  void Charge(uint64_t bytes, uint64_t native_bytes) {
    metadata_ += bytes;
    table_bytes_ += native_bytes;
    if (metadata_ > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack resource metadata limit exceeded");
    CheckBudget();
  }
  uint64_t Read(size_t n) {
    if (n > end_ - pos_) Fail("Truncated MessagePack input");
    uint64_t bits = messagepack_resources::Load(data_->input.data() + pos_,
                                                static_cast<unsigned>(n));
    pos_ += n;
    return bits;
  }
  void Skip(size_t n) {
    if (n > end_ - pos_) Fail("Truncated MessagePack input");
    pos_ += n;
  }
  static bool IsString(uint8_t tag) {
    return (tag & 0xe0) == 0xa0 || (tag >= 0xd9 && tag <= 0xdb);
  }
  uint32_t Length(uint8_t tag) {
    if ((tag & 0xe0) == 0xa0) return tag & 31;
    if ((tag & 0xf0) == 0x80 || (tag & 0xf0) == 0x90) return tag & 15;
    return static_cast<uint32_t>(
        Read(tag == 0xd9                                   ? 1
             : (tag == 0xda || tag == 0xdc || tag == 0xde) ? 2
                                                           : 4));
  }
  uint32_t ArrayCount(bool check_limit = true) {
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if ((tag & 0xf0) != 0x90 && tag != 0xdc && tag != 0xdd)
      Fail("Malformed MessagePack resource array");
    uint32_t count = Length(tag);
    if (check_limit && count > static_cast<uint32_t>(FixedArray::kMaxLength))
      throw std::length_error("MessagePack resource container limit exceeded");
    return count;
  }
  uint32_t UInt() {
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (tag <= 0x7f) return tag;
    if (tag < 0xcc || tag > 0xcf)
      Fail("Malformed MessagePack resource integer");
    uint64_t value = Read(size_t{1} << (tag - 0xcc));
    if (value > UINT32_MAX) Fail("Malformed MessagePack resource integer");
    return static_cast<uint32_t>(value);
  }
  Span String() {
    CheckCancelled();
    size_t start = pos_;
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (!IsString(tag)) Fail("MessagePack resource entries must be strings");
    uint32_t count = Length(tag);
    if (count > end_ - pos_) Fail("Truncated MessagePack input");
    if (count > static_cast<uint32_t>(String::kMaxLength))
      Fail("Invalid MessagePack UTF-8 string");
    const uint8_t* bytes = data_->input.data() + pos_;
    // Also validate unused table entries. Split only at UTF-8 boundaries so
    // cancellation stays bounded even for large dictionary values and keys.
    for (size_t i = 0; i < count;) {
      CheckCancelled();
      size_t n = std::min<size_t>(4096, count - i);
      if (i + n < count) {
        while (n && (bytes[i + n] & 0xc0) == 0x80) --n;
        if (!n) Fail("Invalid MessagePack UTF-8 string");
      }
      messagepack_strings::Utf8Info info;
      if (!messagepack_strings::ScanUtf8(bytes + i, n, &info))
        Fail("Invalid MessagePack UTF-8 string");
      i += n;
    }
    pos_ += count;
    return {start, pos_ - start,
            std::string_view(reinterpret_cast<const char*>(bytes), count)};
  }
  void Copy(size_t offset, size_t length) {
    for (size_t i = 0; i < length;) {
      CheckCancelled();
      size_t n = std::min<size_t>(4096, length - i);
      std::memcpy(data_->output.Append(n), data_->input.data() + offset + i, n);
      CheckBudget();
      i += n;
    }
  }
  void Copy(const Span& span) { Copy(span.offset, span.length); }
  void Numbers(size_t depth) {
    if (depth >= 256)
      throw std::length_error("MessagePack nesting limit exceeded");
    if (ArrayCount() != 2) Fail("Malformed MessagePack resource numeric array");
    uint32_t count = UInt(), blocks = ArrayCount();
    if (!count || !blocks || blocks > count || count > end_ - pos_ ||
        count > static_cast<uint32_t>(FixedArray::kMaxLength) ||
        blocks > (end_ - pos_) / 4)
      Fail("Malformed MessagePack resource numeric count");
    Writer writer(&data_->output);
    msgpack::packer<Writer> packer(writer);
    packer.pack_array(count);
    uint32_t total = 0;
    bool smi = true;
    for (uint32_t i = 0; i < blocks; ++i) {
      CheckCancelled();
      if (ArrayCount() != 3)
        Fail("Malformed MessagePack resource numeric block");
      uint32_t kind = UInt(), scale = UInt();
      if (kind > 9 || scale > 9 || (kind >= 8 && scale))
        Fail("Invalid MessagePack resource numeric representation");
      uint8_t tag = static_cast<uint8_t>(Read(1));
      if (tag < 0xc4 || tag > 0xc6)
        Fail("MessagePack resource numeric block requires binary bytes");
      uint32_t bytes = static_cast<uint32_t>(Read(size_t{1} << (tag - 0xc4)));
      unsigned width = messagepack_resources::kWidth[kind];
      if (!bytes || bytes % width || bytes / width > 256 ||
          bytes / width > count - total)
        Fail("Invalid MessagePack resource numeric block size");
      size_t start = pos_;
      Skip(bytes);
      for (uint32_t j = 0; j < bytes / width; ++j) {
        uint64_t bits = messagepack_resources::Load(
            data_->input.data() + start + j * width, width);
        if (kind < 8 && width == 8 &&
            std::fabs(messagepack_resources::Number(bits, kind, 0)) >
                kSafeInteger)
          Fail("MessagePack resource packed integer exceeds safe range");
        Number(&packer, messagepack_resources::Number(bits, kind, scale));
      }
      total += bytes / width;
      smi &= kind <= 3 && scale == 0;
      CheckBudget();
    }
    if (total != count) Fail("MessagePack resource numeric count mismatch");
    if (!smi && count > static_cast<uint32_t>(FixedDoubleArray::kMaxLength))
      throw std::length_error("MessagePack resource container limit exceeded");
  }
  void Extension(uint8_t tag, size_t depth) {
    uint32_t size =
        tag >= 0xd4 && tag <= 0xd8
            ? uint32_t{1} << (tag - 0xd4)
            : static_cast<uint32_t>(Read(size_t{1} << (tag - 0xc7)));
    uint8_t type = static_cast<uint8_t>(Read(1));
    if (size > end_ - pos_) Fail("Truncated MessagePack resource extension");
    size_t saved_end = end_;
    end_ = pos_ + size;
    if (type == messagepack_resources::kString) {
      uint32_t index = UInt();
      if (index >= strings_.size())
        Fail("Invalid MessagePack resource string reference");
      Copy(strings_[index]);
    } else if (type == messagepack_resources::kShape) {
      if (ArrayCount() != 2) Fail("Malformed MessagePack resource record");
      uint32_t index = UInt();
      if (index >= shapes_.size())
        Fail("Invalid MessagePack resource shape reference");
      uint32_t count = ArrayCount(false);
      if (depth >= 256)
        throw std::length_error("MessagePack nesting limit exceeded");
      if (count != shapes_[index].size())
        Fail("MessagePack resource field count mismatch");
      if (count > end_ - pos_) Fail("Malformed MessagePack container count");
      Writer writer(&data_->output);
      msgpack::packer<Writer> packer(writer);
      packer.pack_map(count);
      CheckBudget();
      for (const Span& key : shapes_[index]) {
        Copy(key);
        Value(depth + 1, false);
      }
    } else if (type == messagepack_resources::kNumbers) {
      Numbers(depth);
    } else {
      Fail("Unknown MessagePack resource extension");
    }
    if (pos_ != end_) Fail("Trailing MessagePack resource extension bytes");
    end_ = saved_end;
  }
  void Value(size_t depth, bool key) {
    CheckCancelled();
    size_t start = pos_;
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (key && !IsString(tag)) Fail("MessagePack object keys must be strings");
    if (IsString(tag)) {
      pos_ = start;
      Copy(String());
      return;
    }
    bool map = (tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf;
    bool array = (tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd;
    if (map || array) {
      uint32_t count = Length(tag);
      if (depth >= 256)
        throw std::length_error("MessagePack nesting limit exceeded");
      if (count > (end_ - pos_) / (map ? 2 : 1))
        Fail("Malformed MessagePack container count");
      if (count > static_cast<uint32_t>(FixedArray::kMaxLength))
        throw std::length_error("MessagePack container limit exceeded");
      Copy(start, pos_ - start);
      for (uint32_t i = 0; i < count; ++i) {
        if (map) Value(depth + 1, true);
        Value(depth + 1, false);
      }
      return;
    }
    if ((tag >= 0xc7 && tag <= 0xc9) || (tag >= 0xd4 && tag <= 0xd8)) {
      Extension(tag, depth);
      return;
    }
    if (tag >= 0xca && tag <= 0xd3)
      Skip(tag == 0xca ? 4 : tag == 0xcb ? 8 : size_t{1} << ((tag - 0xcc) & 3));
    else if (!(tag <= 0x7f || tag >= 0xe0 || tag == 0xc0 || tag == 0xc2 ||
               tag == 0xc3))
      Fail("Unsupported MessagePack value");
    Copy(start, pos_ - start);
  }
  MessagePackAsyncData* data_;
  size_t pos_ = 0, end_;
  uint64_t metadata_ = 0, table_bytes_ = 0;
  std::vector<std::vector<Span>> shapes_;
  std::vector<Span> strings_;
};
