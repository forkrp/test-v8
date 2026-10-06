// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Included inside messagepack.cc's anonymous namespace after string helpers.
// The direct reader keeps every V8 reference in rooted handles and never
// retains a raw V8 pointer across allocation. Native input remains immutable.
class DirectMessagePackDecoder {
 public:
  DirectMessagePackDecoder(Isolate* isolate, base::Vector<const uint8_t> input,
                           std::string* error, Handle<WeakFixedArray> shapes)
      : isolate_(isolate),
        cursor_(input.begin()),
        end_(input.end()),
        error_(error),
        shapes_(shapes),
        object_prototype_(handle(
            isolate->object_function()->initial_map()->prototype(), isolate)) {
    properties_.reserve(64);
    values_.reserve(64);
  }
  MaybeHandle<Object> Decode() {
    Handle<Object> value = Value(0, {});
    if (value.is_null()) return {};
    if (cursor_ != end_) return Fail("Trailing bytes after MessagePack value");
    return value;
  }
#ifdef MSGPACK_PROFILE_CACHE
  ~DirectMessagePackDecoder() {
    std::fprintf(stderr,
                 "decoder cache hits=%u empty=%u collision=%u deprecated=%u "
                 "direct=%u failures=%u,%u,%u,%u\n",
                 hits_, empty_, collisions_, deprecated_, direct_, failures_[0],
                 failures_[1], failures_[2], failures_[3]);
  }
#endif
  MaybeHandle<Object> DecodeResource() {
    resource_ = std::make_unique<ResourceState>();
    if (!ResourceTables()) return {};
    return Decode();
  }

 private:
  Handle<Object> Fail(const char* message) {
    if (error_->empty()) *error_ = message;
    return {};
  }
  bool Read(size_t size, const uint8_t** data) {
    if (size > static_cast<size_t>(end_ - cursor_)) {
      Fail("Truncated MessagePack input");
      return false;
    }
    *data = cursor_;
    cursor_ += size;
    return true;
  }
  template <typename T>
  bool Unsigned(T* value) {
    const uint8_t* data;
    if (!Read(sizeof(T), &data)) return false;
    T result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) result = (result << 8) | data[i];
    *value = result;
    return true;
  }
  bool Length(uint8_t tag, uint32_t* count) {
    if ((tag & 0xe0) == 0xa0) {
      *count = tag & 31;
      return true;
    }
    if ((tag & 0xf0) == 0x90 || (tag & 0xf0) == 0x80) {
      *count = tag & 15;
      return true;
    }
    if (tag == 0xd9) {
      uint8_t n;
      if (!Unsigned(&n)) return false;
      *count = n;
      return true;
    }
    if (tag == 0xda || tag == 0xdc || tag == 0xde) {
      uint16_t n;
      if (!Unsigned(&n)) return false;
      *count = n;
      return true;
    }
    return Unsigned(count);
  }
#include "src/msgpack/messagepack-resource-decoder.h"
  struct StringEntry {
    const uint8_t* bytes = nullptr;
    uint32_t size = 0;
    Handle<String> value;
  };
  Handle<String> StringValue(uint8_t tag, bool key) {
    uint32_t size;
    const uint8_t* bytes;
    if (!Length(tag, &size) || !Read(size, &bytes)) return {};
    if (size > static_cast<uint32_t>(String::kMaxLength)) {
      Fail("Invalid MessagePack UTF-8 string");
      return {};
    }
    StringEntry* entry = nullptr;
    if (size <= 4096) {
      uint32_t hash = StringCacheHash(bytes, size);
      entry = key ? &keys_[hash & 2047] : &strings_[hash & 127];
      if (!entry->value.is_null() && entry->size == size &&
          EqualBytes(entry->bytes, bytes, size))
        return entry->value;
    }
    Utf8Info info;
    if (!ScanUtf8(bytes, size, &info)) {
      Fail("Invalid MessagePack UTF-8 string");
      return {};
    }
    Handle<String> result;
    if (key && info.ascii) {
      result = isolate_->factory()->InternalizeString(
          base::Vector<const uint8_t>(bytes, static_cast<int>(size)));

#if defined(V8_TARGET_ARCH_ARM64)
    } else if (info.ascii && size > 1 && size <= 64 &&
               !v8_flags.single_generation) {
      // The complete native span is validated and stays alive across GC.
      // Match the factory's regular young-string layout, initializing the
      // header, padding and characters before publishing a rooted handle.
      int length = static_cast<int>(size);
      Tagged<HeapObject> raw =
          isolate_->heap()
              ->allocator()
              ->AllocateRawWith<HeapAllocator::kRetryOrFail>(
                  SeqOneByteString::SizeFor(length), AllocationType::kYoung);
      DisallowGarbageCollection no_gc;
      raw->set_map_after_allocation(
          ReadOnlyRoots(isolate_).seq_one_byte_string_map(),
          SKIP_WRITE_BARRIER);
      Tagged<SeqOneByteString> string = SeqOneByteString::cast(raw);
      string->clear_padding_destructively(length);
      string->set_length(length);
      string->set_raw_hash_field(String::kEmptyHashField);
      std::memcpy(string->GetChars(no_gc), bytes, size);
      result = handle(string, isolate_);
#endif
    } else {
      if (!MakeUtf8String(isolate_, bytes, size, info).ToHandle(&result))
        return {};
      if (key) result = isolate_->factory()->InternalizeString(result);
    }
    if (entry) *entry = {bytes, size, result};
    return result;
  }
  Handle<Object> Integer(uint64_t bits, bool negative) {
    if (negative) {
      int64_t n;
      std::memcpy(&n, &bits, sizeof(n));
      if (n >= -static_cast<int64_t>(kMaxSafeInteger))
        return isolate_->factory()->NewNumber(static_cast<double>(n));
      return BigInt::FromInt64(isolate_, n);
    }
    if (bits <= kMaxSafeInteger)
      return isolate_->factory()->NewNumber(static_cast<double>(bits));
    return BigInt::FromUint64(isolate_, bits);
  }
  Handle<Object> Value(size_t depth, Handle<String> role) {
    const uint8_t* data;
    if (!Read(1, &data)) return {};
    uint8_t tag = *data;
    if (tag <= 0x7f) return handle(Smi::FromInt(tag), isolate_);
    if (tag >= 0xe0)
      return handle(Smi::FromInt(static_cast<int8_t>(tag)), isolate_);
    if ((tag & 0xe0) == 0xa0 || (tag >= 0xd9 && tag <= 0xdb))
      return StringValue(tag, false);
    if ((tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf)
      return Container(tag, depth, role, true);
    if ((tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd)
      return Container(tag, depth, role, false);
    uint64_t bits = 0;
    switch (tag) {
      case 0xc7:
      case 0xc8:
      case 0xc9:
      case 0xd4:
      case 0xd5:
      case 0xd6:
      case 0xd7:
      case 0xd8:
        if (resource_) return ResourceExtension(tag, depth, role);
        return Fail("Unsupported MessagePack value");
      case 0xc0:
        return isolate_->factory()->null_value();
      case 0xc2:
      case 0xc3:
        return isolate_->factory()->ToBoolean(tag == 0xc3);
      case 0xca: {
        uint32_t word;
        if (!Unsigned(&word)) return {};
        float number;
        std::memcpy(&number, &word, sizeof(number));
        return isolate_->factory()->NewNumber(number);
      }
      case 0xcb: {
        if (!Unsigned(&bits)) return {};
        double number;
        std::memcpy(&number, &bits, sizeof(number));
        return isolate_->factory()->NewNumber(number);
      }
      case 0xcc:
      case 0xd0: {
        uint8_t n;
        if (!Unsigned(&n)) return {};
        bits = tag == 0xd0 ? static_cast<uint64_t>(static_cast<int8_t>(n)) : n;
        break;
      }
      case 0xcd:
      case 0xd1: {
        uint16_t n;
        if (!Unsigned(&n)) return {};
        bits = tag == 0xd1 ? static_cast<uint64_t>(static_cast<int16_t>(n)) : n;
        break;
      }
      case 0xce:
      case 0xd2: {
        uint32_t n;
        if (!Unsigned(&n)) return {};
        bits = tag == 0xd2 ? static_cast<uint64_t>(static_cast<int32_t>(n)) : n;
        break;
      }
      case 0xcf:
      case 0xd3:
        if (!Unsigned(&bits)) return {};
        break;
      default:
        return Fail("Unsupported MessagePack value");
    }
    return Integer(bits,
                   tag >= 0xd0 && tag <= 0xd3 && (bits & (uint64_t{1} << 63)));
  }
  struct KeyHint {
    Handle<String> key;
    bool ascii = false;
  };
  struct Feedback {
    size_t depth = 0;
    Handle<String> role;
    Handle<Map> map;
    bool dense_layout = false;
    std::vector<KeyHint> keys;
  };
  Handle<String> StringKey(uint8_t tag, const KeyHint* hint,
                           bool* known_named) {
    *known_named = false;
    if (!hint || !hint->ascii) return StringValue(tag, true);
    const uint8_t* start = cursor_;
    uint32_t size;
    const uint8_t* bytes;
    if (!Length(tag, &size) || !Read(size, &bytes)) return {};
    if (size == static_cast<uint32_t>(hint->key->length())) {
      DisallowGarbageCollection no_gc;
      const uint8_t* chars =
          SeqOneByteString::cast(*hint->key)->GetChars(no_gc);
      if (EqualBytes(chars, bytes, size)) {
        *known_named = true;
        return hint->key;
      }
    }
    cursor_ = start;
    return StringValue(tag, true);
  }
  MaybeHandle<JSObject> TryBuildExpected(Handle<Map> map, size_t start,
                                         uint32_t count, bool schema_verified,
                                         bool dense_layout) {
    if (map.is_null() || map->is_deprecated() || map->is_dictionary_map() ||
        map->is_prototype_map() || map->elements_kind() != HOLEY_ELEMENTS ||
        map->NumberOfOwnDescriptors() != static_cast<int>(count) ||
        map->GetInObjectProperties() < static_cast<int>(count) || count > 256) {
#ifdef MSGPACK_PROFILE_CACHE
      ++failures_[0];
#endif
      return {};
    }
    int boxes = 0;
    bool double_fields[256];
    {
      // Validation is allocation-free. Keep one descriptor reference here and
      // preserve only native representation flags across the later allocation.
      DisallowGarbageCollection no_gc;
      Tagged<DescriptorArray> descriptors = map->instance_descriptors(isolate_);
      for (uint32_t i = 0; i < count; ++i) {
        InternalIndex index(i);
        PropertyDetails details = descriptors->GetDetails(index);
        const Property& property = properties_[start + i];
        if ((!schema_verified &&
             descriptors->GetKey(index) != *properties_[start + i].key) ||
            (!dense_layout && (details.kind() != PropertyKind::kData ||
                               details.attributes() != NONE ||
                               details.location() != PropertyLocation::kField ||
                               details.field_index() != static_cast<int>(i)))) {
#ifdef MSGPACK_PROFILE_CACHE
          ++failures_[1];
#endif
          return {};
        }
        Representation representation = details.representation();
        double_fields[i] = representation.IsDouble();
        if (property.is_number) {
          if (representation.IsDouble() || representation.IsTagged()) {
            boxes += representation.IsDouble() || !IsSmiDouble(property.number);
            continue;
          }
          if (representation.IsSmi() && IsSmiDouble(property.number)) continue;
          return {};
        }
        Handle<Object> value = property.value;
        if (!Object::FitsRepresentation(*value, representation)) {
#ifdef MSGPACK_PROFILE_CACHE
          ++failures_[2];
#endif
          return {};
        }
        if (representation.IsHeapObject() &&
            !FieldType::NowContains(descriptors->GetFieldType(index), value)) {
#ifdef MSGPACK_PROFILE_CACHE
          ++failures_[3];
#endif
          return {};
        }
        if (representation.IsDouble() && IsSmi(*value)) ++boxes;
      }
    }
    int object_size = map->instance_size();
    int total_size = object_size + boxes * sizeof(HeapNumber);
    if (!v8_flags.single_generation && map->instance_type() == JS_OBJECT_TYPE &&
        !map->IsInobjectSlackTrackingInProgress() &&
        map->GetInObjectPropertiesStartInWords() * kTaggedSize ==
            JSObject::kHeaderSize &&
        ALIGN_TO_ALLOCATION_ALIGNMENT(object_size) == object_size &&
        total_size <= isolate_->heap()->MaxRegularHeapObjectSize(
                          AllocationType::kYoung)) {
      // Fold only fully checked, ordinary objects into a regular young-space
      // allocation. No wrapper object or interior raw pointer crosses a GC.
      // The allocation may GC first; the rooted map and property values are
      // read again afterward. Initialize every object before publishing it.
      Tagged<HeapObject> raw =
          isolate_->heap()
              ->allocator()
              ->AllocateRawWith<HeapAllocator::kRetryOrFail>(
                  total_size, AllocationType::kYoung);
      DisallowGarbageCollection no_gc;
      raw->set_map_after_allocation(*map, SKIP_WRITE_BARRIER);
      Tagged<JSObject> object = JSObject::cast(raw);
      ReadOnlyRoots roots(isolate_);
      object->set_raw_properties_or_hash(roots.empty_fixed_array(),
                                         SKIP_WRITE_BARRIER);
      object->initialize_elements();
      WriteBarrierMode mode = object->GetWriteBarrierMode(no_gc);
      Address next_number = raw.address() + object_size;
      auto allocate_number = [&](double number) -> Tagged<HeapNumber> {
        Tagged<HeapObject> box = HeapObject::FromAddress(next_number);
        box->set_map_after_allocation(roots.heap_number_map(),
                                      SKIP_WRITE_BARRIER);
        HeapNumber::cast(box)->set_value(number);
        next_number += sizeof(HeapNumber);
        return HeapNumber::cast(box);
      };
      for (uint32_t i = 0; i < count; ++i) {
        const Property& property = properties_[start + i];
        bool double_field = double_fields[i];
        Tagged<Object> value;
        if (property.is_number) {
          value = double_field || !IsSmiDouble(property.number)
                      ? Tagged<Object>(allocate_number(property.number))
                      : Tagged<Object>(
                            Smi::FromInt(static_cast<int>(property.number)));
        } else {
          value = *property.value;
          if (IsSmi(value) && double_field)
            value =
                allocate_number(static_cast<double>(Smi::cast(value).value()));
        }
        object->RawFastInobjectPropertyAtPut(
            FieldIndex::ForInObjectOffset(map->GetInObjectPropertyOffset(i),
                                          FieldIndex::kTagged),
            value, mode);
      }
      for (int i = count; i < map->GetInObjectProperties(); ++i)
        object->RawFastInobjectPropertyAtPut(
            FieldIndex::ForInObjectOffset(map->GetInObjectPropertyOffset(i),
                                          FieldIndex::kTagged),
            roots.undefined_value(), mode);
      DCHECK_EQ(next_number, raw.address() + total_size);
      return handle(object, isolate_);
    }
    // Stage numeric fields in native doubles until the final map is known.
    // Allocate their uniquely owned boxes together and write all fields in a
    // no-GC span. This avoids one allocation and one GC root per numeric field.
    // A one-box group does not benefit from a ByteArray wrapper or a sweeping
    // synchronization. Keep its ordinary, uniquely owned HeapNumber rooted
    // while allocating the object; larger groups retain folded allocation.
    Handle<HeapNumber> single;
    if (boxes == 1) single = isolate_->factory()->NewHeapNumber(0);
    FoldedMutableHeapNumberAllocation allocation(isolate_,
                                                 boxes == 1 ? 0 : boxes);
    Handle<JSObject> result = isolate_->factory()->NewJSObjectFromMap(map);
    DisallowGarbageCollection no_gc;
    FoldedMutableHeapNumberAllocator allocator(isolate_, &allocation, no_gc);
    ReadOnlyRoots roots(isolate_);
    auto allocate_number = [&](double number) -> Tagged<HeapNumber> {
      if (boxes != 1)
        return allocator.AllocateNext(
            roots, Float64::FromBits(base::bit_cast<uint64_t>(number)));
      single->set_value(number);
      return *single;
    };
    WriteBarrierMode mode = result->GetWriteBarrierMode(no_gc);
    for (uint32_t i = 0; i < count; ++i) {
      const Property& property = properties_[start + i];
      bool double_field = double_fields[i];
      Tagged<Object> value;
      if (property.is_number) {
        value = double_field || !IsSmiDouble(property.number)
                    ? Tagged<Object>(allocate_number(property.number))
                    : Tagged<Object>(
                          Smi::FromInt(static_cast<int>(property.number)));
      } else {
        value = *property.value;
      }
      if (!property.is_number && IsSmi(value) && double_field)
        value = allocate_number(static_cast<double>(Smi::cast(value).value()));
      FieldIndex index = FieldIndex::ForInObjectOffset(
          map->GetInObjectPropertyOffset(i), FieldIndex::kTagged);
      result->RawFastInobjectPropertyAtPut(index, value, mode);
    }
#ifdef MSGPACK_PROFILE_CACHE
    ++direct_;
#endif
    return result;
  }
  struct Prefix {
    uint32_t count = 0;
    Handle<Map> map;
  };
  int ShapeSlot(size_t start, uint32_t count) {
    uint32_t hash = count;
    for (uint32_t i = 0; i < count; ++i)
      hash = (hash * 16777619) ^ properties_[start + i].key->EnsureHash();
    hash ^= hash >> 16;
    return static_cast<int>(hash & kShapeSignatureMask);
  }
  Handle<Map> Shape(size_t start, uint32_t count, int signature,
                    bool* unchanged, bool* dense_layout) {
    *unchanged = false;
    *dense_layout = false;
    int slot = (signature & (kDecodeShapeCacheEntries / kShapeCacheWays - 1)) *
               kShapeCacheWays * 2;
    [[maybe_unused]] bool occupied = false;
    for (int way = 0; way < kShapeCacheWays; ++way) {
      int candidate = slot + way * 2;
      Tagged<MaybeObject> code = shapes_->get(candidate + 1);
      if (!code.IsSmi() ||
          (code.ToSmi().value() & kShapeSignatureMask) != signature)
        continue;
      Tagged<HeapObject> object;
      if (!shapes_->get(candidate).GetHeapObjectIfWeak(&object) ||
          !IsMap(object))
        continue;
      occupied = true;
      Tagged<Map> raw_map = Map::cast(object);
      // Only a matching fingerprint enters full descriptor validation. Keep
      // rejected candidates raw; no allocation occurs during these checks.
      if (raw_map->is_dictionary_map() || raw_map->is_prototype_map() ||
          raw_map->NumberOfOwnDescriptors() != static_cast<int>(count) ||
          raw_map->prototype() != *object_prototype_)
        continue;
      bool same = true;
      for (uint32_t i = 0; i < count; ++i) {
        if (raw_map->instance_descriptors(isolate_)->GetKey(InternalIndex(i)) !=
            *properties_[start + i].key) {
          same = false;
          break;
        }
      }
      if (!same) continue;
      Handle<Map> map = handle(raw_map, isolate_);
      if (map->is_deprecated()) {
#ifdef MSGPACK_PROFILE_CACHE
        ++deprecated_;
#endif
        map = Map::Update(isolate_, map);
      } else {
        *unchanged = true;
        *dense_layout = (code.ToSmi().value() & kShapeDenseLayout) != 0;
      }
#ifdef MSGPACK_PROFILE_CACHE
      ++hits_;
#endif
      return map;
    }
#ifdef MSGPACK_PROFILE_CACHE
    if (occupied)
      ++collisions_;
    else
      ++empty_;
#endif
    return {};
  }
  void StoreShape(int signature, Tagged<Map> map, bool dense_layout) {
    int slot = (signature & (kDecodeShapeCacheEntries / kShapeCacheWays - 1)) *
               kShapeCacheWays * 2;
    for (int way = 0; way < kShapeCacheWays; ++way) {
      int candidate = slot + way * 2;
      Tagged<HeapObject> current;
      if (!shapes_->get(candidate).GetHeapObjectIfWeak(&current) ||
          current == map ||
          (IsMap(current) && Map::cast(current)->is_deprecated())) {
        shapes_->set(candidate, MakeWeak(map));
        shapes_->set(
            candidate + 1,
            Smi::FromInt(signature | (dense_layout ? kShapeDenseLayout : 0)));
        return;
      }
    }
    size_t bucket = static_cast<size_t>(slot / (kShapeCacheWays * 2));
    int way = replacement_[bucket]++ & (kShapeCacheWays - 1);
    shapes_->set(slot + way * 2, MakeWeak(map));
    shapes_->set(
        slot + way * 2 + 1,
        Smi::FromInt(signature | (dense_layout ? kShapeDenseLayout : 0)));
  }
  void MarkDenseLayout(int signature, Tagged<Map> map) {
    int slot = (signature & (kDecodeShapeCacheEntries / kShapeCacheWays - 1)) *
               kShapeCacheWays * 2;
    for (int way = 0; way < kShapeCacheWays; ++way) {
      int candidate = slot + way * 2;
      Tagged<HeapObject> current;
      if (shapes_->get(candidate).GetHeapObjectIfWeak(&current) &&
          current == map) {
        shapes_->set(candidate + 1,
                     Smi::FromInt(signature | kShapeDenseLayout));
        return;
      }
    }
  }
  size_t PrefixSlot(size_t start, uint32_t count) {
    uint32_t hash = count ^ properties_[start].key->EnsureHash();
    if (count > 1) hash = hash * 33 ^ properties_[start + 1].key->EnsureHash();
    return hash & (count <= 2 ? 63 : 511);
  }
  Handle<Map> ExpectedFromPrefix(size_t start, uint32_t count) {
    if (!count) return {};
    Prefix& cached = count <= 2 ? small_prefixes_[PrefixSlot(start, count)]
                                : prefixes_[PrefixSlot(start, count)];
    if (cached.count != count || cached.map.is_null()) return {};
    Handle<Map> map = cached.map;
    if (map->is_deprecated()) map = Map::Update(isolate_, map);
    uint32_t prefix = std::min(count, uint32_t{2});
    if (map->NumberOfOwnDescriptors() != static_cast<int>(prefix)) return {};
    for (uint32_t i = 0; i < prefix; ++i)
      if (map->instance_descriptors(isolate_)->GetKey(InternalIndex(i)) !=
          *properties_[start + i].key)
        return {};
    // Resolve only the remaining fields. The object builder still validates
    // every key, representation, field type, and deprecated transition.
    for (uint32_t i = prefix; i < count; ++i) {
      Handle<Map> next;
      if (!TransitionsAccessor(isolate_, *map)
               .FindTransitionToField(properties_[start + i].key)
               .ToHandle(&next))
        return {};
      map = next;
    }
    return map;
  }
  void CachePrefix(size_t start, uint32_t count, Handle<Map> map) {
    if (!count || map->is_dictionary_map()) return;
    int prefix = static_cast<int>(std::min(count, uint32_t{2}));
    Prefix& cached = count <= 2 ? small_prefixes_[PrefixSlot(start, count)]
                                : prefixes_[PrefixSlot(start, count)];
    if (cached.count == count && !cached.map.is_null() &&
        cached.map->NumberOfOwnDescriptors() == prefix) {
      bool same = true;
      for (int i = 0; i < prefix; ++i)
        same &= cached.map->instance_descriptors(isolate_)->GetKey(
                    InternalIndex(i)) == *properties_[start + i].key;
      if (same) return;
    }
    // Walking parent maps needs no allocation. Allocate at most one rooted
    // handle per cache slot, avoiding a growing GC root list per record.
    DisallowGarbageCollection no_gc;
    Tagged<Map> raw = *map;
    while (raw->NumberOfOwnDescriptors() > prefix) {
      Tagged<Object> parent = raw->GetBackPointer();
      if (!IsMap(parent)) return;
      raw = Map::cast(parent);
    }
    if (raw->NumberOfOwnDescriptors() != prefix) return;
    cached.count = count;
    if (cached.map.is_null())
      cached.map = handle(raw, isolate_);
    else
      cached.map.PatchValue(raw);
  }
  V8_INLINE bool NumericAt(const uint8_t** position, double* number) {
    const uint8_t* p = *position;
    if (p == end_) return false;
    uint8_t tag = *p;
    if (tag <= 0x7f) {
      *number = tag;
      *position = p + 1;
      return true;
    }
    if (tag >= 0xe0) {
      *number = static_cast<int8_t>(tag);
      *position = p + 1;
      return true;
    }
    if (tag < 0xca || tag > 0xd3) return false;
    return NumericBody(position, number);
  }
  V8_NOINLINE bool NumericBody(const uint8_t** position, double* number) {
    const uint8_t* p = *position;
    uint8_t tag = *p++;
    size_t bytes;
    if (tag == 0xca)
      bytes = 4;
    else if (tag == 0xcb)
      bytes = 8;
    else if (tag >= 0xcc && tag <= 0xd3)
      bytes = size_t{1} << ((tag - 0xcc) & 3);
    else
      return false;
    if (bytes > static_cast<size_t>(end_ - p)) return false;
    uint64_t bits = 0;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // Unaligned memcpy loads are bounded by the complete numeric span. A
    // single byte swap replaces a variable-count byte/shift loop on ARM.
    switch (bytes) {
      case 1:
        bits = *p;
        break;
      case 2: {
        uint16_t word;
        std::memcpy(&word, p, 2);
        bits = base::bits::ReverseBytes(word);
        break;
      }
      case 4: {
        uint32_t word;
        std::memcpy(&word, p, 4);
        bits = base::bits::ReverseBytes(word);
        break;
      }
      case 8:
        std::memcpy(&bits, p, 8);
        bits = base::bits::ReverseBytes(bits);
        break;
    }
#else
    for (size_t i = 0; i < bytes; ++i) bits = (bits << 8) | p[i];
#endif
    if (tag == 0xca) {
      uint32_t word = static_cast<uint32_t>(bits);
      float value;
      std::memcpy(&value, &word, sizeof(value));
      *number = value;
    } else if (tag == 0xcb) {
      std::memcpy(number, &bits, sizeof(*number));
    } else if (tag >= 0xd0 && (bits & (uint64_t{1} << (bytes * 8 - 1)))) {
      if (bytes < 8) bits |= ~uint64_t{0} << (bytes * 8);
      int64_t value;
      std::memcpy(&value, &bits, sizeof(value));
      if (value < -static_cast<int64_t>(kMaxSafeInteger)) return false;
      *number = static_cast<double>(value);
    } else {
      if (bits > kMaxSafeInteger) return false;
      *number = static_cast<double>(bits);
    }
    *position = p + bytes;
    return true;
  }
  void MaterializeNumbers(size_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      Property& property = properties_[start + i];
      if (!property.is_number) continue;
      property.value = isolate_->factory()->NewNumber(property.number);
      property.is_number = false;
    }
  }
  Handle<Object> NumericArray(uint32_t count, size_t depth,
                              Handle<String> role) {
    if (!count || (count > 8 && count < 64)) return {};
    const uint8_t* p = cursor_;
    double first;
    if (!NumericAt(&p, &first)) return {};
    double small_numbers[8];
    std::unique_ptr<double[]> storage;
    if (count > 8) storage.reset(new double[count]);
    double* numbers = count <= 8 ? small_numbers : storage.get();
    numbers[0] = first;
    uint32_t prefix = 1;
    bool smi = IsSmiDouble(first);
    for (; prefix < count; ++prefix) {
      double number;
      if (!NumericAt(&p, &number)) break;
      numbers[prefix] = number;
      smi &= IsSmiDouble(number);
    }
    if (prefix != count) {
      // A late mixed value must not reparse a long numeric prefix. Box the
      // native prefix once into rooted, initialized storage and continue the
      // general reader from the exact first nonnumeric tag.
      Handle<FixedArray> storage = isolate_->factory()->NewFixedArray(count);
      // Fold small batches of boxes into regular heap allocations. Each batch
      // is completed before another allocation can GC; never create a run of
      // multiple objects inside a large-object-space allocation.
      for (uint32_t start = 0; start < prefix; start += 256) {
        uint32_t stop = std::min(prefix, start + 256);
        int boxes = 0;
        for (uint32_t i = start; i < stop; ++i)
          boxes += !IsSmiDouble(numbers[i]);
        FoldedMutableHeapNumberAllocation allocation(isolate_, boxes);
        DisallowGarbageCollection no_gc;
        FoldedMutableHeapNumberAllocator allocator(isolate_, &allocation,
                                                   no_gc);
        ReadOnlyRoots roots(isolate_);
        WriteBarrierMode mode = storage->GetWriteBarrierMode(no_gc);
        for (uint32_t i = start; i < stop; ++i) {
          Tagged<Object> value =
              IsSmiDouble(numbers[i])
                  ? Tagged<Object>(Smi::FromInt(static_cast<int>(numbers[i])))
                  : Tagged<Object>(allocator.AllocateNext(
                        roots, Float64::FromBits(
                                   base::bit_cast<uint64_t>(numbers[i]))));
          storage->set(i, value, mode);
        }
      }
      cursor_ = p;
      for (uint32_t i = prefix; i < count; ++i) {
        Handle<Object> value = Value(depth + 1, role);
        if (value.is_null()) return {};
        storage->set(i, *value);
      }
      return isolate_->factory()->NewJSArrayWithElements(
          storage, PACKED_ELEMENTS, count);
    }
    Handle<JSArray> result = isolate_->factory()->NewJSArray(
        smi ? PACKED_SMI_ELEMENTS : PACKED_DOUBLE_ELEMENTS, count, count);
    {
      DisallowGarbageCollection no_gc;
      Tagged<FixedArrayBase> storage = result->elements();
      for (uint32_t i = 0; i < count; ++i) {
        double number = numbers[i];
        if (smi)
          FixedArray::cast(storage)->set(
              i, Smi::FromInt(static_cast<int>(number)));
        else
          FixedDoubleArray::cast(storage)->set(i, number);
      }
    }
    cursor_ = p;
    return result;
  }
#if defined(V8_TARGET_ARCH_ARM64)
  V8_NOINLINE MaybeHandle<JSArray> TryBuildTaggedArray(uint32_t count,
                                                       ElementsKind kind,
                                                       size_t start) {
    if (!count || count > 256 || v8_flags.single_generation ||
        (kind != PACKED_ELEMENTS && kind != PACKED_SMI_ELEMENTS))
      return {};
    Tagged<Map> raw_map =
        isolate_->raw_native_context()->GetInitialJSArrayMap(kind);
    if (raw_map.is_null() || raw_map->is_deprecated() ||
        raw_map->instance_type() != JS_ARRAY_TYPE ||
        raw_map->instance_size() != JSArray::kHeaderSize ||
        raw_map->IsInobjectSlackTrackingInProgress() ||
        raw_map->elements_kind() != kind)
      return {};
    int array_size = JSArray::kHeaderSize;
    int storage_size = FixedArray::SizeFor(count);
    int total_size = array_size + storage_size;
    if (ALIGN_TO_ALLOCATION_ALIGNMENT(array_size) != array_size ||
        ALIGN_TO_ALLOCATION_ALIGNMENT(storage_size) != storage_size ||
        total_size >
            isolate_->heap()->MaxRegularHeapObjectSize(AllocationType::kYoung))
      return {};
    HandleScope scope(isolate_);
    Handle<Map> map = handle(raw_map, isolate_);
    // All source values and the realm's array map are rooted before allocation.
    // Complete both ordinary objects in one regular young-space span, without
    // temporary undefined element writes or an interior pointer across GC.
    Tagged<HeapObject> raw = isolate_->heap()
                                 ->allocator()
                                 ->AllocateRawWith<HeapAllocator::kRetryOrFail>(
                                     total_size, AllocationType::kYoung);
    DisallowGarbageCollection no_gc;
    ReadOnlyRoots roots(isolate_);
    raw->set_map_after_allocation(*map, SKIP_WRITE_BARRIER);
    Tagged<JSArray> array = JSArray::cast(raw);
    array->set_raw_properties_or_hash(roots.empty_fixed_array(),
                                      SKIP_WRITE_BARRIER);
    array->initialize_elements();
    array->set_length(Smi::FromInt(count));
    Tagged<HeapObject> backing =
        HeapObject::FromAddress(raw.address() + array_size);
    backing->set_map_after_allocation(roots.fixed_array_map(),
                                      SKIP_WRITE_BARRIER);
    Tagged<FixedArray> storage = FixedArray::cast(backing);
    storage->set_length(count);
    WriteBarrierMode mode = storage->GetWriteBarrierMode(no_gc);
    for (uint32_t i = 0; i < count; ++i)
      storage->set(i, *values_[start + i], mode);
    array->set_elements(storage, SKIP_WRITE_BARRIER);
    return scope.CloseAndEscape(handle(array, isolate_));
  }
#endif
  template <bool resource_shape = false>
  Handle<Object> Container(uint8_t tag, size_t depth, Handle<String> role,
                           bool map, ResourceShape* schema = nullptr) {
    uint32_t count;
    if (!Length(tag, &count)) return {};
    if (depth >= kMaxDepth) return Fail("MessagePack nesting limit exceeded");
    if (resource_shape && count != schema->keys.size())
      return Fail("MessagePack resource field count mismatch");
    if (count >
        static_cast<size_t>(end_ - cursor_) / (map && !resource_shape ? 2 : 1))
      return Fail("Malformed MessagePack container count");
    if (count > static_cast<uint32_t>(FixedArray::kMaxLength))
      return Fail("MessagePack container limit exceeded");
    if (map) {
      size_t start = properties_.size();
      bool indexed = resource_shape && schema->indexed;
      // The same role often alternates between several optional-field counts.
      // Keep those schemas in separate hint slots, then validate every key.
      size_t slot = (depth * 17 + count * 31 +
                     (role.is_null() ? 0 : role->EnsureHash())) &
                    127;
      Feedback& cached = feedback_[slot];
      for (uint32_t i = 0; i < count; ++i) {
        Handle<String> key;
        if constexpr (resource_shape) {
          key = schema->keys[i];
        } else {
          const uint8_t* marker;
          if (!Read(1, &marker)) return {};
          if ((*marker & 0xe0) != 0xa0 && (*marker < 0xd9 || *marker > 0xdb))
            return Fail("MessagePack object keys must be strings");
          const uint8_t* key_start = cursor_;
          // Feedback may be replaced by a nested container at the same cache
          // slot. Check its role on every key; a full byte comparison is still
          // required before accepting an ASCII named-key hint.
          bool same_role = cached.depth == depth &&
                           (role.is_null() ? cached.role.is_null()
                                           : !cached.role.is_null() &&
                                                 *role == *cached.role);
          const KeyHint* hint = same_role && cached.keys.size() == count
                                    ? &cached.keys[i]
                                    : nullptr;
          bool known_named;
          key = StringKey(*marker, hint, &known_named);
          if (key.is_null()) return {};
          uint32_t index;
          if (!known_named && key->length()) {
            // StringKey has validated the complete header/body. Array-index
            // names must start with an ASCII digit, so ordinary wire keys need
            // no V8 hash/index query on this path.
            size_t header = *marker == 0xd9   ? 1
                            : *marker == 0xda ? 2
                            : *marker == 0xdb ? 4
                                              : 0;
            uint8_t first = key_start[header];
            if (first >= '0' && first <= '9')
              indexed |= key->AsArrayIndex(&index);
          }
        }
        double number;
        if (NumericAt(&cursor_, &number)) {
          properties_.push_back({key, {}, number, true});
        } else {
          Handle<Object> value = Value(depth + 1, key);
          if (value.is_null()) return {};
          properties_.push_back({key, value});
        }
      }
      if constexpr (resource_shape) return ResourceObject(schema, start, count);
      Handle<JSObject> object;
      if (indexed) {
        MaterializeNumbers(start, count);
        object = isolate_->factory()->NewJSObject(isolate_->object_function());
        for (size_t i = start; i < properties_.size(); ++i) {
          PropertyKey key(isolate_, Handle<Name>::cast(properties_[i].key));
          LookupIterator lookup(isolate_, object, key, LookupIterator::OWN);
          if (JSObject::DefineOwnPropertyIgnoreAttributes(
                  &lookup, properties_[i].value, NONE)
                  .is_null())
            return {};
        }
      } else {
        Handle<Map> expected;
        if (cached.depth == depth &&
            (role.is_null() ? cached.role.is_null()
                            : !cached.role.is_null() && *role == *cached.role))
          expected = cached.map;
        if (!expected.is_null() && expected->is_deprecated())
          expected = Map::Update(isolate_, expected);
        bool local_schema =
            !expected.is_null() &&
            expected->NumberOfOwnDescriptors() == static_cast<int>(count);
        if (local_schema) {
          Tagged<DescriptorArray> descriptors =
              expected->instance_descriptors(isolate_);
          for (uint32_t i = 0; i < count; ++i)
            if (descriptors->GetKey(InternalIndex(i)) !=
                *properties_[start + i].key) {
              local_schema = false;
              break;
            }
        }
        bool dense_layout =
            local_schema && cached.dense_layout && *expected == *cached.map;
        // Repeated records already have exact role-local feedback. Avoid a
        // second schema hash and realm-table lookup on that common path.
        int shape_slot = !local_schema && count && count <= 256
                             ? ShapeSlot(start, count)
                             : -1;
        bool shape_unchanged = false;
        if (shape_slot >= 0) {
          Handle<Map> shape =
              Shape(start, count, shape_slot, &shape_unchanged, &dense_layout);
          if (!shape.is_null()) expected = shape;
        }
        if (count &&
            (expected.is_null() ||
             expected->NumberOfOwnDescriptors() < static_cast<int>(count) ||
             expected->instance_descriptors(isolate_)->GetKey(
                 InternalIndex(0)) != *properties_[start].key)) {
          expected = ExpectedFromPrefix(start, count);
        }
        bool direct =
            TryBuildExpected(expected, start, count,
                             local_schema || shape_unchanged, dense_layout)
                .ToHandle(&object);
        if (!direct) {
          MaterializeNumbers(start, count);
          JSDataObjectBuilder builder(
              isolate_, HOLEY_ELEMENTS, static_cast<int>(count), expected,
              JSDataObjectBuilder::kHeapNumbersGuaranteedUniquelyOwned);
          const Property* begin = properties_.data() + start;
          PropertyIterator it(begin, begin + count);
          object = builder.BuildFromIterator(it);
        }
        // A successful unchanged realm-cache hit needs neither a weak-table
        // write nor rebuilding prefix metadata. Moving GC updates both the
        // cached weak reference and the rooted expected map automatically.
        bool keep_shape = shape_unchanged && object->map() == *expected;
        // Attribute/kind/location/index changes create a different map or
        // deprecate it; in-place field generalization only changes constness,
        // representation and field type. Representations and field types are
        // checked on every value; construction may use either field constness.
        // Record layout only after a complete direct validation succeeds.
        if (keep_shape && direct && !dense_layout)
          MarkDenseLayout(shape_slot, object->map());
        cached.depth = depth;
        cached.role = role;
        cached.dense_layout = direct;
        if (cached.map.is_null())
          cached.map = handle(object->map(), isolate_);
        else
          cached.map.PatchValue(object->map());
        if (!local_schema) cached.keys.clear();
        // Admit hints only after the role repeats the exact schema. Building
        // key metadata on every varying shape costs more than generic lookup.
        if (local_schema && cached.keys.empty() && count <= 256) {
          cached.keys.reserve(count);
          DisallowGarbageCollection no_gc;
          for (uint32_t i = 0; i < count; ++i) {
            Handle<String> key = properties_[start + i].key;
            bool ascii =
                IsSeqOneByteString(*key) &&
                String::IsAscii(SeqOneByteString::cast(*key)->GetChars(no_gc),
                                key->length());
            cached.keys.push_back({key, ascii});
          }
        }
        if (shape_slot >= 0 && !keep_shape &&
            !object->map()->is_dictionary_map() &&
            object->map()->NumberOfOwnDescriptors() == static_cast<int>(count))
          StoreShape(shape_slot, object->map(), direct);
        if (!local_schema && !keep_shape) CachePrefix(start, count, cached.map);
      }
      properties_.resize(start);
      return object;
    }
    // Empty arrays use the read-only empty elements backing store. No stores
    // or write-barrier mode queries are needed for that shared root.
    if (!count)
      return isolate_->factory()->NewJSArray(PACKED_SMI_ELEMENTS, 0, 0);
    if (Handle<Object> numeric = NumericArray(count, depth, role);
        !numeric.is_null())
      return numeric;
    if (!error_->empty() || isolate_->has_exception()) return {};
    size_t start = values_.size();
    ElementsKind kind = PACKED_SMI_ELEMENTS;
    for (uint32_t i = 0; i < count; ++i) {
      Handle<Object> value = Value(depth + 1, role);
      if (value.is_null()) return {};
      if (kind != PACKED_ELEMENTS) {
        if (IsHeapNumber(*value))
          kind = PACKED_DOUBLE_ELEMENTS;
        else if (!IsSmi(*value))
          kind = PACKED_ELEMENTS;
      }
      values_.push_back(value);
    }
    Handle<JSArray> array;
#if defined(V8_TARGET_ARCH_ARM64)
    if (TryBuildTaggedArray(count, kind, start).ToHandle(&array)) {
      values_.resize(start);
      return array;
    }
#endif
    array = isolate_->factory()->NewJSArray(kind, count, count);
    {
      DisallowGarbageCollection no_gc;
      if (kind == PACKED_DOUBLE_ELEMENTS) {
        Tagged<FixedDoubleArray> storage =
            FixedDoubleArray::cast(array->elements());
        for (uint32_t i = 0; i < count; ++i)
          storage->set(i, Object::Number(*values_[start + i]));
      } else {
        Tagged<FixedArray> storage = FixedArray::cast(array->elements());
        WriteBarrierMode mode = storage->GetWriteBarrierMode(no_gc);
        for (uint32_t i = 0; i < count; ++i)
          storage->set(i, *values_[start + i], mode);
      }
    }
    values_.resize(start);
    return array;
  }
  Isolate* isolate_;
  const uint8_t* cursor_;
  const uint8_t* end_;
  std::string* error_;
  Handle<WeakFixedArray> shapes_;
  Handle<HeapObject> object_prototype_;
  uint8_t replacement_[kDecodeShapeCacheEntries / kShapeCacheWays]{};
  StringEntry keys_[2048]{};
  StringEntry strings_[128]{};
  Feedback feedback_[128]{};
  Prefix small_prefixes_[64]{};
  Prefix prefixes_[512]{};
  std::vector<Property> properties_;
  std::vector<Handle<Object>> values_;
  std::unique_ptr<ResourceState> resource_;
#ifdef MSGPACK_PROFILE_CACHE
  uint32_t hits_ = 0, empty_ = 0, collisions_ = 0, deprecated_ = 0;
  uint32_t direct_ = 0, failures_[4]{};
#endif
};
