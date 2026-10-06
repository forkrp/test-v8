// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Included in DirectMessagePackDecoder. Only DecodeResource enables extensions.
struct ResourceShape {
  std::vector<Handle<String>> keys;
  bool indexed = false;
  Handle<Map> map;
  bool dense_layout = false;
};
struct ResourceBlock {
  unsigned kind, scale;
  uint32_t count;
  const uint8_t* bytes;
};
bool ArrayCount(uint32_t* count) {
  const uint8_t* tag;
  if (!Read(1, &tag)) return false;
  if ((*tag & 0xf0) != 0x90 && *tag != 0xdc && *tag != 0xdd) {
    Fail("Malformed MessagePack resource array");
    return false;
  }
  if (!Length(*tag, count)) return false;
  if (*count > static_cast<uint32_t>(FixedArray::kMaxLength)) {
    Fail("MessagePack resource container limit exceeded");
    return false;
  }
  return true;
}
bool ResourceUInt(uint32_t* n) {
  const uint8_t* tag;
  if (!Read(1, &tag)) return false;
  if (*tag <= 0x7f) {
    *n = *tag;
    return true;
  }
  if (*tag == 0xcc) {
    uint8_t v;
    if (!Unsigned(&v)) return false;
    *n = v;
    return true;
  }
  if (*tag == 0xcd) {
    uint16_t v;
    if (!Unsigned(&v)) return false;
    *n = v;
    return true;
  }
  if (*tag == 0xce) return Unsigned(n);
  if (*tag == 0xcf) {
    uint64_t v;
    if (!Unsigned(&v)) return false;
    if (v <= UINT32_MAX) {
      *n = static_cast<uint32_t>(v);
      return true;
    }
  }
  Fail("Malformed MessagePack resource integer");
  return false;
}
bool ResourceTables() {
  uint32_t count;
  if (!ArrayCount(&count) || count != 3) {
    Fail("Malformed MessagePack resource envelope");
    return false;
  }
  if (!ArrayCount(&count)) return false;
  if (count > static_cast<size_t>(end_ - cursor_)) {
    Fail("Malformed MessagePack resource shape count");
    return false;
  }
  resource_->metadata_bytes = uint64_t{count} * 48;
  if (resource_->metadata_bytes > kMaxOutputBytes) {
    Fail("MessagePack resource metadata limit exceeded");
    return false;
  }
  resource_->shapes.resize(count);
  for (auto& shape : resource_->shapes) {
    uint32_t fields;
    if (!ArrayCount(&fields)) return false;
    if (fields > static_cast<size_t>(end_ - cursor_)) {
      Fail("Malformed MessagePack resource field count");
      return false;
    }
    resource_->metadata_bytes += uint64_t{fields} * 16;
    if (resource_->metadata_bytes > kMaxOutputBytes) {
      Fail("MessagePack resource metadata limit exceeded");
      return false;
    }
    std::unordered_set<std::string> seen;
    shape.keys.reserve(fields);
    for (uint32_t i = 0; i < fields; ++i) {
      const uint8_t* tag;
      if (!Read(1, &tag)) return false;
      if ((*tag & 0xe0) != 0xa0 && (*tag < 0xd9 || *tag > 0xdb)) {
        Fail("MessagePack resource keys must be strings");
        return false;
      }
      const uint8_t* start = cursor_;
      uint32_t size;
      if (!Length(*tag, &size) || size > static_cast<size_t>(end_ - cursor_))
        return false;
      std::string text(reinterpret_cast<const char*>(cursor_), size);
      cursor_ = start;
      if (!seen.insert(std::move(text)).second) {
        Fail("Duplicate MessagePack resource shape key");
        return false;
      }
      Handle<String> key = StringValue(*tag, true);
      if (key.is_null()) return false;
      uint32_t index;
      shape.indexed |= key->AsArrayIndex(&index);
      shape.keys.push_back(key);
    }
  }
  if (!ArrayCount(&count)) return false;
  if (count > static_cast<size_t>(end_ - cursor_)) {
    Fail("Malformed MessagePack resource string count");
    return false;
  }
  resource_->metadata_bytes += uint64_t{count} * 16;
  if (resource_->metadata_bytes > kMaxOutputBytes) {
    Fail("MessagePack resource metadata limit exceeded");
    return false;
  }
  resource_->strings.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* tag;
    if (!Read(1, &tag)) return false;
    if ((*tag & 0xe0) != 0xa0 && (*tag < 0xd9 || *tag > 0xdb)) {
      Fail("MessagePack resource dictionary entries must be strings");
      return false;
    }
    Handle<String> value = StringValue(*tag, false);
    if (value.is_null()) return false;
    resource_->strings.push_back(value);
  }
  return true;
}
Handle<Object> ResourceNumbers(size_t depth) {
  if (depth >= kMaxDepth) return Fail("MessagePack nesting limit exceeded");
  uint32_t count, blocks, fields;
  if (!ArrayCount(&fields) || fields != 2 || !ResourceUInt(&count) ||
      !ArrayCount(&blocks))
    return Fail("Malformed MessagePack resource numeric array");
  if (!count || !blocks || blocks > count ||
      count > static_cast<size_t>(end_ - cursor_) ||
      count > static_cast<uint32_t>(FixedArray::kMaxLength) ||
      blocks > static_cast<size_t>(end_ - cursor_) / 4)
    return Fail("Malformed MessagePack resource numeric count");
  std::vector<ResourceBlock> spans;
  spans.reserve(blocks);
  uint32_t total = 0;
  bool smi = true;
  for (uint32_t i = 0; i < blocks; ++i) {
    uint32_t kind, scale, bytes;
    if (!ArrayCount(&fields) || fields != 3 || !ResourceUInt(&kind) ||
        !ResourceUInt(&scale))
      return Fail("Malformed MessagePack resource numeric block");
    if (kind > 9 || scale > 9 || (kind >= 8 && scale))
      return Fail("Invalid MessagePack resource numeric representation");
    const uint8_t* tag;
    if (!Read(1, &tag)) return {};
    if (*tag == 0xc4) {
      uint8_t n;
      if (!Unsigned(&n)) return {};
      bytes = n;
    } else if (*tag == 0xc5) {
      uint16_t n;
      if (!Unsigned(&n)) return {};
      bytes = n;
    } else if (*tag == 0xc6) {
      if (!Unsigned(&bytes)) return {};
    } else
      return Fail("MessagePack resource numeric block requires binary bytes");
    unsigned width = messagepack_resources::kWidth[kind];
    if (!bytes || bytes % width || bytes / width > 256 ||
        bytes / width > count - total)
      return Fail("Invalid MessagePack resource numeric block size");
    const uint8_t* body;
    if (!Read(bytes, &body)) return {};
    spans.push_back({kind, scale, bytes / width, body});
    total += bytes / width;
    smi &= kind <= 3 && scale == 0;
  }
  if (total != count)
    return Fail("MessagePack resource numeric count mismatch");
  if (!smi && count > static_cast<uint32_t>(FixedDoubleArray::kMaxLength))
    return Fail("MessagePack resource container limit exceeded");
  Handle<JSArray> array = isolate_->factory()->NewJSArray(
      smi ? PACKED_SMI_ELEMENTS : PACKED_DOUBLE_ELEMENTS, count, count);
  DisallowGarbageCollection no_gc;
  Tagged<FixedArrayBase> storage = array->elements();
  uint32_t position = 0;
  for (const auto& span : spans) {
    unsigned width = messagepack_resources::kWidth[span.kind];
    for (uint32_t i = 0; i < span.count; ++i) {
      uint64_t bits =
          messagepack_resources::Load(span.bytes + i * width, width);
      double n = messagepack_resources::Number(bits, span.kind, span.scale);
      if (span.kind < 8 && width == 8) {
        // Scaling cannot make an out-of-profile integer admissible.
        double integer = messagepack_resources::Number(bits, span.kind, 0);
        if (std::fabs(integer) > static_cast<double>(kMaxSafeInteger))
          return Fail("MessagePack resource packed integer exceeds safe range");
      }
      if (smi)
        FixedArray::cast(storage)->set(position++,
                                       Smi::FromInt(static_cast<int>(n)));
      else
        FixedDoubleArray::cast(storage)->set(position++, n);
    }
  }
  return array;
}
Handle<Object> ResourceExtension(uint8_t tag, size_t depth,
                                 Handle<String> role) {
  uint32_t size;
  if (tag >= 0xd4 && tag <= 0xd8)
    size = uint32_t{1} << (tag - 0xd4);
  else if (tag == 0xc7) {
    uint8_t n;
    if (!Unsigned(&n)) return {};
    size = n;
  } else if (tag == 0xc8) {
    uint16_t n;
    if (!Unsigned(&n)) return {};
    size = n;
  } else if (tag == 0xc9) {
    if (!Unsigned(&size)) return {};
  } else
    return Fail("Invalid MessagePack resource extension");
  const uint8_t* type;
  if (!Read(1, &type)) return {};
  if (size > static_cast<size_t>(end_ - cursor_))
    return Fail("Truncated MessagePack resource extension");
  const uint8_t* limit = cursor_ + size;
  struct EndScope {
    const uint8_t*& end;
    const uint8_t* saved;
    ~EndScope() { end = saved; }
  } scope{end_, end_};
  end_ = limit;
  Handle<Object> result;
  if (*type == messagepack_resources::kString) {
    uint32_t index;
    if (!ResourceUInt(&index)) return {};
    if (index >= resource_->strings.size())
      return Fail("Invalid MessagePack resource string reference");
    result = resource_->strings[index];
  } else if (*type == messagepack_resources::kShape) {
    uint32_t fields, index;
    if (!ArrayCount(&fields) || fields != 2 || !ResourceUInt(&index))
      return Fail("Malformed MessagePack resource record");
    if (index >= resource_->shapes.size())
      return Fail("Invalid MessagePack resource shape reference");
    const uint8_t* array;
    if (!Read(1, &array)) return {};
    if ((*array & 0xf0) != 0x90 && *array != 0xdc && *array != 0xdd)
      return Fail("Malformed MessagePack resource record values");
    result =
        Container<true>(*array, depth, role, true, &resource_->shapes[index]);
  } else if (*type == messagepack_resources::kNumbers) {
    result = ResourceNumbers(depth);
  } else
    return Fail("Unknown MessagePack resource extension");
  if (!result.is_null() && cursor_ != limit)
    return Fail("Trailing MessagePack resource extension bytes");
  return result;
}
Handle<Object> ResourceObject(ResourceShape* schema, size_t start,
                              uint32_t count) {
  Handle<JSObject> object;
  if (schema->indexed) {
    MaterializeNumbers(start, count);
    object = isolate_->factory()->NewJSObject(isolate_->object_function());
    for (uint32_t i = 0; i < count; ++i) {
      const Property& field = properties_[start + i];
      PropertyKey key(isolate_, Handle<Name>::cast(field.key));
      LookupIterator lookup(isolate_, object, key, LookupIterator::OWN);
      if (JSObject::DefineOwnPropertyIgnoreAttributes(&lookup, field.value,
                                                      NONE)
              .is_null())
        return {};
    }
  } else {
    Handle<Map> expected = schema->map;
    bool dense = schema->dense_layout;
    if (!expected.is_null() && expected->is_deprecated()) {
      expected = Map::Update(isolate_, expected);
      dense = false;
    }
    bool direct =
        TryBuildExpected(expected, start, count, true, dense).ToHandle(&object);
    if (!direct) {
      MaterializeNumbers(start, count);
      JSDataObjectBuilder builder(
          isolate_, HOLEY_ELEMENTS, static_cast<int>(count), expected,
          JSDataObjectBuilder::kHeapNumbersGuaranteedUniquelyOwned);
      const Property* begin = properties_.data() + start;
      PropertyIterator it(begin, begin + count);
      object = builder.BuildFromIterator(it);
    }
    if (!object->map()->is_dictionary_map() &&
        object->map()->NumberOfOwnDescriptors() == static_cast<int>(count)) {
      if (schema->map.is_null())
        schema->map = handle(object->map(), isolate_);
      else
        schema->map.PatchValue(object->map());
      schema->dense_layout = direct;
    } else {
      schema->map = {};
      schema->dense_layout = false;
    }
  }
  properties_.resize(start);
  return object;
}
struct ResourceState {
  uint64_t metadata_bytes = 0;
  std::vector<ResourceShape> shapes;
  std::vector<Handle<String>> strings;
};
