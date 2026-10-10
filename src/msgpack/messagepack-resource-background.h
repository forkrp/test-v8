// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Validate compact input once and retain resource references in the native
// tape. Numeric blocks remain owned wire spans, rather than one token per value.
class ResourceTapeDecoder {
 public:
  explicit ResourceTapeDecoder(MessagePackAsyncData* data)
      : data_(data), end_(data->input.size()) {}
  void Decode() {
    if (end_ < 5 || data_->input.data()[4] != 1)
      Fail("Unsupported MessagePack resource version");
    pos_ = 5;
    if (ArrayCount() != 3) Fail("Malformed MessagePack resource envelope");
    uint32_t count = ArrayCount();
    if (count > end_ - pos_) Fail("Malformed MessagePack resource shape count");
    metadata_ = uint64_t{count} * 48;
    CheckBudget(uint64_t{count} * sizeof(Shape));
    shapes_.resize(count);
    data_->resource_shape_used.resize(count);
    for (auto& shape : shapes_) {
      CheckCancelled();
      shape.count = ArrayCount();
      if (shape.count > end_ - pos_)
        Fail("Malformed MessagePack resource field count");
      metadata_ += uint64_t{shape.count} * 16;
      CheckBudget(uint64_t{shape.count} * 96);
      std::unordered_set<std::string_view> seen;
      for (uint32_t i = 0; i < shape.count; ++i) {
        Span key = String(true, true);
        if (!seen.insert(key.text).second)
          Fail("Duplicate MessagePack resource shape key");
        shape.bytes += key.length;
      }
    }
    count = ArrayCount();
    if (count > end_ - pos_)
      Fail("Malformed MessagePack resource string count");
    metadata_ += uint64_t{count} * 16;
    CheckBudget(uint64_t{count} * sizeof(Span));
    strings_.reserve(count);
    data_->resource_string_used.resize(count);
    for (uint32_t i = 0; i < count; ++i)
      strings_.push_back(String(false, true));
    data_->compact = true;
    data_->resource_payload = static_cast<uint32_t>(pos_);
    Value(0, false);
    if (pos_ != end_) Fail("Trailing bytes after MessagePack value");
    data_->resource_metadata = metadata_;
  }

 private:
  struct Shape {
    uint32_t count = 0;
    uint64_t bytes = 0;
  };
  struct Span {
    size_t offset, length;
    std::string_view text;
    uint32_t table_index;
  };
  [[noreturn]] static void Fail(const char* message) {
    throw std::runtime_error(message);
  }
  void CheckCancelled() const {
    if (!(++cancel_check_ & 63) && Cancelled(data_))
      Fail("MessagePack resource operation cancelled");
  }
  void CheckBudget(uint64_t temporary = 0) const {
    if (metadata_ > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack resource metadata limit exceeded");
    uint64_t tables = shapes_.capacity() * sizeof(Shape) +
                      strings_.capacity() * sizeof(Span);
    if (data_->MemoryUsage() + metadata_ + tables + temporary >
        MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
  }
  void Wire(uint64_t bytes) {
    wire_bytes_ += bytes;
    // Keep the existing logical expansion bound without allocating expanded
    // bytes. Packed numbers use a conservative maximum of nine bytes/value.
    if (wire_bytes_ > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
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
    return static_cast<uint32_t>(Read(
        tag == 0xd9 ? 1
        : (tag == 0xda || tag == 0xdc || tag == 0xde) ? 2 : 4));
  }
  static size_t ContainerHeader(uint32_t count) {
    return count < 16 ? 1 : count < 65536 ? 3 : 5;
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
  void Transcode(MessagePackToken* token, const Span& span) {
    auto info = token->StringInfo();
    if (info.ascii || span.text.size() <= 64 || token->DecodedString()) return;
    size_t width = info.one_byte ? 1 : 2;
    size_t count = span.text.size();
    const auto* bytes = reinterpret_cast<const uint8_t*>(span.text.data());
    size_t needed = static_cast<size_t>(info.length) * width;
    CheckBudget(needed);
    if (width == 2 && (data_->decoded_strings.size() & 1))
      data_->decoded_strings.Append(1)[0] = 0;
    size_t offset = data_->decoded_strings.size();
    uint8_t* output = data_->decoded_strings.Append(needed);
    CheckBudget();
    size_t produced = 0;
    for (size_t i = 0; i < count;) {
      CheckCancelled();
      size_t n = std::min<size_t>(4096, count - i);
      if (i + n < count)
        while ((bytes[i + n] & 0xc0) == 0x80) --n;
      if (width == 1) {
        auto* end = messagepack_strings::DecodeUtf8(bytes + i, n, 0,
                                                   output + produced);
        produced = static_cast<size_t>(end - output);
      } else {
        auto* start = reinterpret_cast<uint16_t*>(output);
        auto* end = messagepack_strings::DecodeUtf8(bytes + i, n, 0,
                                                   start + produced / 2);
        produced = static_cast<size_t>(end - start) * 2;
      }
      i += n;
    }
    DCHECK_EQ(produced, needed);
    info.ascii_prefix = static_cast<int>(offset);
    token->SetString(info, true);
  }
  Span String(bool key, bool table, MessagePackToken* token = nullptr) {
    CheckCancelled();
    size_t start = pos_;
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (!IsString(tag)) Fail("MessagePack resource entries must be strings");
    uint32_t count = Length(tag);
    if (count > end_ - pos_) Fail("Truncated MessagePack input");
    if (count > static_cast<uint32_t>(String::kMaxLength))
      Fail("Invalid MessagePack UTF-8 string");
    const uint8_t* bytes = data_->input.data() + pos_;
    messagepack_strings::Utf8Info info;
    if (!key && !table && count > 64 && count <= 4096) {
      MessagePackToken value;
      value.offset = static_cast<uint32_t>(start);
      if (!DecodeBoundedString(data_, bytes, count, &info, &value))
        Fail("Invalid MessagePack UTF-8 string");
      CheckBudget();
      pos_ += count;
      *token = value;
      return {start, pos_ - start,
              std::string_view(reinterpret_cast<const char*>(bytes), count), 0};
    }
    for (size_t i = 0; i < count;) {
      CheckCancelled();
      size_t n = std::min<size_t>(4096, count - i);
      if (i + n < count) {
        while (n && (bytes[i + n] & 0xc0) == 0x80) --n;
        if (!n) Fail("Invalid MessagePack UTF-8 string");
      }
      messagepack_strings::Utf8Info chunk;
      if (!messagepack_strings::ScanUtf8(bytes + i, n, &chunk))
        Fail("Invalid MessagePack UTF-8 string");
      if (info.ascii)
        info.ascii_prefix += chunk.ascii ? static_cast<int>(n)
                                        : chunk.ascii_prefix;
      info.length += chunk.length;
      info.ascii &= chunk.ascii;
      info.one_byte &= chunk.one_byte;
      i += n;
    }
    if (info.length > String::kMaxLength)
      throw std::length_error("MessagePack string limit exceeded");
    pos_ += count;
    MessagePackToken value;
    value.offset = static_cast<uint32_t>(start);
    value.SetString(info, false);
    Span span{start, pos_ - start,
              std::string_view(reinterpret_cast<const char*>(bytes), count),
              static_cast<uint32_t>(data_->resource_tables.size())};
    if (table) {
      data_->resource_tables.push_back(value);
      CheckBudget();
    } else {
      if (!key) Transcode(&value, span);
      *token = value;
    }
    return span;
  }
  uint32_t Append(const MessagePackToken& token) {
    if (!(data_->tokens.size() % MessagePackTape::kBlock))
      CheckBudget(MessagePackTape::kBlock * sizeof(MessagePackToken));
    uint32_t index = static_cast<uint32_t>(data_->tokens.size());
    data_->tokens.push_back(token);
    size_t capacity = data_->resource_work.capacity();
    data_->resource_work.push_back(1);
    if (data_->resource_work.capacity() != capacity) CheckBudget();
    return index;
  }
  void Numbers(size_t depth, MessagePackToken* token) {
    if (depth >= 256)
      throw std::length_error("MessagePack nesting limit exceeded");
    if (ArrayCount() != 2) Fail("Malformed MessagePack resource numeric array");
    uint32_t count = UInt(), blocks = ArrayCount();
    if (!count || !blocks || blocks > count || count > end_ - pos_ ||
        count > static_cast<uint32_t>(FixedArray::kMaxLength) ||
        blocks > (end_ - pos_) / 4)
      Fail("Malformed MessagePack resource numeric count");
    token->kind = MessagePackToken::kResourceNumbers;
    token->container = {};
    token->container.count = count;
    token->container.auxiliary =
        static_cast<uint32_t>(data_->resource_numbers.size());
    token->container.all_numbers = token->container.all_smis = true;
    uint32_t total = 0;
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
      if (kind < 8 && width == 8) {
        for (uint32_t j = 0; j < bytes / width; ++j) {
          uint64_t bits = messagepack_resources::Load(
              data_->input.data() + start + j * width, width);
          if (std::fabs(messagepack_resources::Number(bits, kind, 0)) >
              kSafeInteger)
            Fail("MessagePack resource packed integer exceeds safe range");
        }
      }
      data_->resource_numbers.push_back(
          {static_cast<uint32_t>(start), static_cast<uint16_t>(bytes / width),
           static_cast<uint8_t>(kind), static_cast<uint8_t>(scale)});
      CheckBudget();
      total += bytes / width;
      token->container.all_smis &= kind <= 3 && scale == 0;
    }
    if (total != count) Fail("MessagePack resource numeric count mismatch");
    if (!token->container.all_smis &&
        count > static_cast<uint32_t>(FixedDoubleArray::kMaxLength))
      throw std::length_error("MessagePack resource container limit exceeded");
    Wire(ContainerHeader(count) + uint64_t{count} * 9);
  }
  bool ScanAscii(size_t depth, bool key, uint32_t* work) {
    CheckCancelled();
    if (*work == 256 || depth > 16) return false;
    ++*work;
    size_t start = pos_;
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (key && !IsString(tag)) Fail("MessagePack object keys must be strings");
    if (IsString(tag)) {
      uint32_t size = Length(tag);
      if (size > end_ - pos_) Fail("Truncated MessagePack input");
      if (size > static_cast<uint32_t>(String::kMaxLength))
        Fail("Invalid MessagePack UTF-8 string");
      if (!messagepack_strings::IsAscii(data_->input.data() + pos_, size))
        return false;
      Skip(size);
      Wire(pos_ - start);
      return true;
    }
    bool map = (tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf;
    bool array = (tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd;
    if (map || array) {
      uint32_t count = Length(tag);
      if (count > (end_ - pos_) / (map ? 2 : 1))
        Fail("Malformed MessagePack container count");
      if (count > static_cast<uint32_t>(FixedArray::kMaxLength))
        throw std::length_error("MessagePack container limit exceeded");
      if (count > (256 - *work) / (map ? 2 : 1)) return false;
      Wire(pos_ - start);
      for (uint32_t i = 0; i < count; ++i) {
        if (map && !ScanAscii(depth + 1, true, work)) return false;
        if (!ScanAscii(depth + 1, false, work)) return false;
      }
      return true;
    }
    if ((tag >= 0xc7 && tag <= 0xc9) || (tag >= 0xd4 && tag <= 0xd8)) {
      uint32_t size =
          tag >= 0xd4 && tag <= 0xd8
              ? uint32_t{1} << (tag - 0xd4)
              : static_cast<uint32_t>(Read(size_t{1} << (tag - 0xc7)));
      uint8_t type = static_cast<uint8_t>(Read(1));
      if (size > end_ - pos_) Fail("Truncated MessagePack resource extension");
      struct EndScope {
        size_t& end;
        size_t saved;
        ~EndScope() { end = saved; }
      } scope{end_, end_};
      end_ = pos_ + size;
      if (type == messagepack_resources::kString) {
        uint32_t index = UInt();
        if (index >= strings_.size())
          Fail("Invalid MessagePack resource string reference");
        data_->resource_string_used[index] = 1;
        Transcode(&data_->resource_tables[strings_[index].table_index],
                  strings_[index]);
        Wire(strings_[index].length);
      } else if (type == messagepack_resources::kShape) {
        if (ArrayCount() != 2) Fail("Malformed MessagePack resource record");
        uint32_t index = UInt();
        if (index >= shapes_.size())
          Fail("Invalid MessagePack resource shape reference");
        uint32_t count = ArrayCount(false);
        if (count != shapes_[index].count)
          Fail("MessagePack resource field count mismatch");
        if (count > 256 - *work) return false;
        *work += count;
        data_->resource_shape_used[index] = 1;
        Wire(ContainerHeader(count) + shapes_[index].bytes);
        for (uint32_t i = 0; i < count; ++i)
          if (!ScanAscii(depth + 1, false, work)) return false;
      } else {
        // Packed blocks and non-ASCII literals keep their existing tape path.
        return false;
      }
      if (pos_ != end_) Fail("Trailing MessagePack resource extension bytes");
      return true;
    }
    if (tag >= 0xca && tag <= 0xd3)
      Skip(tag == 0xca ? 4 : tag == 0xcb ? 8 : size_t{1} << ((tag - 0xcc) & 3));
    else if (!(tag <= 0x7f || tag >= 0xe0 || tag == 0xc0 || tag == 0xc2 ||
               tag == 0xc3))
      Fail("Unsupported MessagePack value");
    Wire(pos_ - start);
    return true;
  }
  bool AsciiRecord(uint32_t count, size_t depth, uint32_t* work) {
    if (count >= 256 || depth > 16) return false;
    size_t start = pos_, limit = end_;
    uint64_t wire = wire_bytes_;
    uint32_t total = count + 1;
    for (uint32_t i = 0; i < count; ++i) {
      if (!ScanAscii(depth + 1, false, &total)) {
        pos_ = start;
        end_ = limit;
        wire_bytes_ = wire;
        return false;
      }
    }
    *work = total;
    return true;
  }
  uint32_t Extension(uint8_t tag, size_t depth, MessagePackToken* token) {
    uint32_t size = tag >= 0xd4 && tag <= 0xd8
                        ? uint32_t{1} << (tag - 0xd4)
                        : static_cast<uint32_t>(Read(size_t{1} << (tag - 0xc7)));
    uint8_t type = static_cast<uint8_t>(Read(1));
    if (size > end_ - pos_) Fail("Truncated MessagePack resource extension");
    size_t saved_end = end_;
    end_ = pos_ + size;
    uint32_t work = 1;
    if (type == messagepack_resources::kString) {
      uint32_t index = UInt();
      if (index >= strings_.size())
        Fail("Invalid MessagePack resource string reference");
      data_->resource_string_used[index] = 1;
      Transcode(&data_->resource_tables[strings_[index].table_index],
                strings_[index]);
      token->kind = MessagePackToken::kResourceString;
      token->string = {index, 0};
      Wire(strings_[index].length);
    } else if (type == messagepack_resources::kShape) {
      if (ArrayCount() != 2) Fail("Malformed MessagePack resource record");
      uint32_t index = UInt();
      if (index >= shapes_.size())
        Fail("Invalid MessagePack resource shape reference");
      uint32_t count = ArrayCount(false);
      if (depth >= 256)
        throw std::length_error("MessagePack nesting limit exceeded");
      if (count != shapes_[index].count)
        Fail("MessagePack resource field count mismatch");
      if (count > end_ - pos_) Fail("Malformed MessagePack container count");
      data_->resource_shape_used[index] = 1;
      token->kind = MessagePackToken::kResourceMap;
      token->container = {};
      token->container.count = count;
      token->container.auxiliary = index;
      Wire(ContainerHeader(count) + shapes_[index].bytes);
      work = std::min<uint32_t>(65535, count + 1);
      uint32_t body = static_cast<uint32_t>(pos_);
      if (AsciiRecord(count, depth, &work)) {
        // Validation emits one record descriptor, without constructing and
        // immediately discarding scalar tokens or converting unused numbers.
        token->kind = MessagePackToken::kResourceRecord;
        token->container.auxiliary =
            static_cast<uint32_t>(data_->resource_records.size());
        size_t capacity = data_->resource_records.capacity();
        data_->resource_records.push_back({body, index});
        if (data_->resource_records.capacity() != capacity) CheckBudget();
      } else {
        for (uint32_t i = 0; i < count; ++i)
          work = std::min<uint32_t>(65535, work + Value(depth + 1, false));
      }
    } else if (type == messagepack_resources::kNumbers) {
      Numbers(depth, token);
      work = std::min<uint32_t>(65535, token->container.count + 1);
    } else {
      Fail("Unknown MessagePack resource extension");
    }
    if (pos_ != end_) Fail("Trailing MessagePack resource extension bytes");
    end_ = saved_end;
    return work;
  }
  void Number(uint8_t tag, MessagePackToken* token) {
    token->kind = MessagePackToken::kNumber;
    if (tag <= 0x7f || tag >= 0xe0) {
      token->number = tag <= 0x7f ? tag : static_cast<int8_t>(tag);
      return;
    }
    size_t n = tag == 0xca ? 4 : tag == 0xcb ? 8
                                           : size_t{1} << ((tag - 0xcc) & 3);
    uint64_t bits = Read(n);
    if (tag == 0xca) {
      uint32_t word = static_cast<uint32_t>(bits);
      float value;
      std::memcpy(&value, &word, 4);
      token->number = value;
    } else if (tag == 0xcb) {
      std::memcpy(&token->number, &bits, 8);
    } else if (tag >= 0xd0 && (bits & (uint64_t{1} << (n * 8 - 1)))) {
      if (n < 8) bits |= ~uint64_t{0} << (n * 8);
      int64_t value;
      std::memcpy(&value, &bits, 8);
      if (value < -static_cast<int64_t>(kSafeInteger))
        token->kind = MessagePackToken::kOther;
      else
        token->number = static_cast<double>(value);
    } else if (bits > kSafeInteger) {
      token->kind = MessagePackToken::kOther;
    } else {
      token->number = static_cast<double>(bits);
    }
  }
  uint32_t Value(size_t depth, bool key) {
    CheckCancelled();
    size_t start = pos_;
    uint8_t tag = static_cast<uint8_t>(Read(1));
    if (key && !IsString(tag)) Fail("MessagePack object keys must be strings");
    MessagePackToken token;
    token.offset = static_cast<uint32_t>(start);
    uint32_t index = Append(token), work = 1;
    if (IsString(tag)) {
      pos_ = start;
      Wire(String(key, false, &token).length);
    } else if ((tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf ||
               (tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd) {
      bool map = (tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf;
      uint32_t count = Length(tag);
      if (depth >= 256)
        throw std::length_error("MessagePack nesting limit exceeded");
      if (count > (end_ - pos_) / (map ? 2 : 1))
        Fail("Malformed MessagePack container count");
      if (count > static_cast<uint32_t>(FixedArray::kMaxLength))
        throw std::length_error("MessagePack container limit exceeded");
      Wire(pos_ - start);
      token.kind = map ? MessagePackToken::kMap : MessagePackToken::kArray;
      token.container = {};
      token.container.count = count;
      token.container.all_numbers = token.container.all_smis = !map;
      for (uint32_t i = 0; i < count; ++i) {
        if (map)
          work = std::min<uint32_t>(65535, work + Value(depth + 1, true));
        size_t child = data_->tokens.size();
        work = std::min<uint32_t>(65535, work + Value(depth + 1, false));
        const auto& value = data_->tokens[child];
        if (!map) {
          token.container.all_numbers &= value.kind == MessagePackToken::kNumber;
          token.container.all_smis &= value.kind == MessagePackToken::kNumber &&
              value.number >= Smi::kMinValue && value.number <= Smi::kMaxValue &&
              std::trunc(value.number) == value.number &&
              !(value.number == 0 && std::signbit(value.number));
        }
      }
    } else if ((tag >= 0xc7 && tag <= 0xc9) || (tag >= 0xd4 && tag <= 0xd8)) {
      work = Extension(tag, depth, &token);
    } else {
      if (tag <= 0x7f || tag >= 0xe0 || (tag >= 0xca && tag <= 0xd3))
        Number(tag, &token);
      else if (!(tag == 0xc0 || tag == 0xc2 || tag == 0xc3))
        Fail("Unsupported MessagePack value");
      Wire(pos_ - start);
    }
    if (token.kind == MessagePackToken::kResourceMap && work <= 256 &&
        depth <= 16) {
      bool ascii = true;
      for (size_t child = index + 1; child < data_->tokens.size(); ++child) {
        const auto& value = data_->tokens[child];
        ascii &= !value.IsString() ||
                 value.kind == MessagePackToken::kStringAscii;
      }
      if (ascii) {
        // This bounded subtree can use the direct builder. Scalar tape entries
        // add no validation or slicing benefit; retain only its record span.
        data_->tokens.Rewind(index + 1);
        data_->resource_work.resize(index + 1);
        token.container.all_numbers = true;  // Direct ASCII record marker.
      }
    }
    token.next = static_cast<uint32_t>(data_->tokens.size());
    data_->tokens[index] = token;
    data_->resource_work[index] = static_cast<uint16_t>(work);
    return work;
  }
  MessagePackAsyncData* data_;
  size_t pos_ = 0, end_;
  uint64_t metadata_ = 0, wire_bytes_ = 0;
  mutable uint32_t cancel_check_ = 0;
  std::vector<Shape> shapes_;
  std::vector<Span> strings_;
};
