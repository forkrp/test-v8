// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// This experimental adapter is compiled with exceptions enabled, and catches
// all dependency exceptions at the boundary. Other V8 sources keep
// -fno-exceptions.
#define MSGPACK_NO_BOOST
#include "src/msgpack/messagepack.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <msgpack.hpp>
#include <unordered_map>
#include <unordered_set>

#include "mpack.h"
#include "src/api/api-inl.h"
#include "src/base/page-allocator.h"
#include "src/base/platform/time.h"
#include "src/common/assert-scope.h"
#include "src/handles/global-handles-inl.h"
#include "src/handles/persistent-handles.h"
#include "src/heap/heap-allocator-inl.h"
#include "src/heap/heap-inl.h"
#include "src/msgpack/messagepack-async.h"
#include "src/msgpack/messagepack-string.h"
#include "src/numbers/conversions-inl.h"
#include "src/objects/bigint.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-data-object-builder.h"
#include "src/objects/keys.h"
#include "src/objects/lookup-inl.h"
#include "src/objects/string-inl.h"
#include "src/utils/memcopy.h"

namespace v8 {
namespace internal {
namespace {
constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;
constexpr size_t kMaxDepth = 256;
constexpr size_t kMaxOutputBytes = 256 * 1024 * 1024;

#include "src/msgpack/messagepack-resource-format.h"

using messagepack_strings::DecodeUtf8;
using messagepack_strings::ScanUtf8;
using messagepack_strings::Utf8Info;

// Realm-owned, bounded schema feedback. A private name on the intrinsic JSON
// object gives the cache the native context's GC lifetime without embedder
// data slots, process globals, or retaining an isolate after disposal. Private
// names are invisible to JS enumeration. Maps are weak references; input bytes
// and decoded graphs never enter this cache.
constexpr int kShapeCacheEntries = 8192;
constexpr int kDecodeShapeCacheEntries = 16384;
constexpr int kShapeCacheWays = 8;
constexpr int kShapeSignatureMask = (1 << 29) - 1;
constexpr int kShapeDenseLayout = 1 << 29;
MaybeHandle<FixedArray> GetMessagePackRealmCache(Isolate* isolate) {
  constexpr char kName[] = "v8.msgpack.realm.cache.v8";
  Handle<JSObject> owner(isolate->native_context()->json_object(), isolate);
  Handle<Symbol> symbol;
  auto matches = [&](Tagged<Object> key) {
    if (!IsSymbol(key) || !Symbol::cast(key)->is_private_name()) return false;
    Tagged<Object> description = Symbol::cast(key)->description();
    if (!IsString(description)) return false;
    return String::cast(description)
        ->IsEqualTo(base::Vector<const char>(kName, sizeof(kName) - 1),
                    isolate);
  };
  if (owner->HasFastProperties(isolate)) {
    Tagged<DescriptorArray> descriptors =
        owner->map()->instance_descriptors(isolate);
    for (InternalIndex i : owner->map()->IterateOwnDescriptors())
      if (matches(descriptors->GetKey(i))) {
        symbol = handle(Symbol::cast(descriptors->GetKey(i)), isolate);
        break;
      }
  } else {
    Handle<FixedArray> names;
    if (!KeyAccumulator::GetKeys(isolate, owner, KeyCollectionMode::kOwnOnly,
                                 PRIVATE_NAMES_ONLY)
             .ToHandle(&names))
      return {};
    for (int i = 0; i < names->length(); ++i)
      if (matches(names->get(i))) {
        symbol = handle(Symbol::cast(names->get(i)), isolate);
        break;
      }
  }
  if (!symbol.is_null()) {
    LookupIterator lookup(isolate, owner, symbol, LookupIterator::OWN);
    if (lookup.state() == LookupIterator::DATA) {
      Handle<Object> value = lookup.GetDataValue();
      if (IsFixedArray(*value) && FixedArray::cast(*value)->length() == 7)
        return Handle<FixedArray>::cast(value);
    }
    return {};
  }
  Handle<FixedArray> cache =
      isolate->factory()->NewFixedArray(7, AllocationType::kOld);
  Handle<WeakFixedArray> decode = isolate->factory()->NewWeakFixedArray(
      kDecodeShapeCacheEntries * 2, AllocationType::kOld);
  Handle<WeakFixedArray> encode = isolate->factory()->NewWeakFixedArray(
      kShapeCacheEntries * 2, AllocationType::kOld);
  Handle<WeakFixedArray> size_hint =
      isolate->factory()->NewWeakFixedArray(3, AllocationType::kOld);
  Handle<WeakFixedArray> capture_hint =
      isolate->factory()->NewWeakFixedArray(4, AllocationType::kOld);
  cache->set(0, Smi::FromInt(8));
  cache->set(1, *decode);
  cache->set(2, *encode);
  cache->set(3, Smi::zero());
  cache->set(4, *size_hint);
  cache->set(6, *capture_hint);
  Handle<String> name = isolate->factory()->NewStringFromAsciiChecked(kName);
  symbol = isolate->factory()->NewPrivateNameSymbol(name);
  LookupIterator lookup(isolate, owner, symbol, LookupIterator::OWN);
  if (JSObject::DefineOwnPropertyIgnoreAttributes(&lookup, cache, DONT_ENUM)
          .is_null())
    return {};
  cache->set(5, Smi::FromInt(isolate->heap()->ms_count() & Smi::kMaxValue));
  return cache;
}

MaybeHandle<String> MakeUtf8String(Isolate* isolate, const uint8_t* bytes,
                                   uint32_t size, const Utf8Info& info) {
  if (info.ascii) {
    return isolate->factory()->NewStringFromOneByte(
        base::Vector<const uint8_t>(bytes, static_cast<int>(size)));
  }
  if (info.one_byte) {
    Handle<SeqOneByteString> result;
    if (!isolate->factory()->NewRawOneByteString(info.length).ToHandle(&result))
      return {};
    DisallowGarbageCollection no_gc;
    DecodeUtf8(bytes, size, info.ascii_prefix, result->GetChars(no_gc));
    return result;
  }
  Handle<SeqTwoByteString> result;
  if (!isolate->factory()->NewRawTwoByteString(info.length).ToHandle(&result))
    return {};
  DisallowGarbageCollection no_gc;
  DecodeUtf8(bytes, size, info.ascii_prefix, result->GetChars(no_gc));
  return result;
}

// A cheap sampled hash chooses a cache slot. It is never sufficient to accept
// a cache hit: length and every byte must also match a previously validated
// input span. Cache input pointers refer only to caller-owned immutable bytes.
uint32_t StringCacheHash(const uint8_t* data, uint32_t size) {
  uint32_t first = 0, middle = 0, last = 0;
  if (size <= 4) {
    // Constant-width loads avoid a fortified variable-size memcpy call for
    // tiny keys. Slot selection need not preserve native-endian word layout;
    // cache acceptance still compares the entire validated byte span.
    switch (size) {
      case 1:
        first = data[0];
        break;
      case 2:
        first = data[0] | (uint32_t{data[1]} << 8);
        break;
      case 3:
        first = data[0] | (uint32_t{data[1]} << 8) | (uint32_t{data[2]} << 16);
        break;
      case 4:
        std::memcpy(&first, data, 4);
        break;
    }
  } else {
    std::memcpy(&first, data, 4);
    std::memcpy(&middle, data + (size - 4) / 2, 4);
    std::memcpy(&last, data + size - 4, 4);
  }
  uint32_t hash = first ^ (middle << 11) ^ (middle >> 21) ^ (last << 22) ^
                  (last >> 10) ^ size;
  hash ^= hash >> 16;
  hash *= 0x7feb352d;
  hash ^= hash >> 15;
  return hash;
}

// Short keys dominate object data. Avoid a libc call while comparing every
// byte, including overlapping tails; neither load crosses the input span.
V8_INLINE bool EqualBytes(const uint8_t* a, const uint8_t* b, uint32_t size) {
  if (size > 16) return std::memcmp(a, b, size) == 0;
  if (size >= 8) {
    uint64_t a0, a1, b0, b1;
    std::memcpy(&a0, a, 8);
    std::memcpy(&b0, b, 8);
    std::memcpy(&a1, a + size - 8, 8);
    std::memcpy(&b1, b + size - 8, 8);
    return ((a0 ^ b0) | (a1 ^ b1)) == 0;
  }
  if (size >= 4) {
    uint32_t a0, a1, b0, b1;
    std::memcpy(&a0, a, 4);
    std::memcpy(&b0, b, 4);
    std::memcpy(&a1, a + size - 4, 4);
    std::memcpy(&b1, b + size - 4, 4);
    return ((a0 ^ b0) | (a1 ^ b1)) == 0;
  }
  if (size >= 2) {
    uint16_t a0, a1, b0, b1;
    std::memcpy(&a0, a, 2);
    std::memcpy(&b0, b, 2);
    std::memcpy(&a1, a + size - 2, 2);
    std::memcpy(&b1, b + size - 2, 2);
    return ((a0 ^ b0) | (a1 ^ b1)) == 0;
  }
  return !size || *a == *b;
}

struct Property {
  Handle<String> key;
  Handle<Object> value;
  double number = 0;
  bool is_number = false;
};
class PropertyIterator {
 public:
  PropertyIterator(const Property* begin, const Property* end)
      : begin_(begin), it_(begin), end_(end) {}
  void Advance() { ++it_; }
  bool Done() { return it_ == end_; }
  Handle<String> GetKnownKey() { return it_->key; }
  Handle<String> GetKey(Handle<String>) { return it_->key; }
  Handle<Object> GetValue(bool) { return it_->value; }
  struct ValueIterator {
    const Property* it;
    Handle<Object> operator*() { return it->value; }
    ValueIterator& operator++() {
      ++it;
      return *this;
    }
  };
  ValueIterator RevisitValues() { return {begin_}; }

 private:
  const Property* begin_;
  const Property* it_;
  const Property* end_;
};

#include "src/msgpack/messagepack-decoder.h"

class V8Visitor : public msgpack::null_visitor {
 public:
  V8Visitor(Isolate* isolate, size_t input_size, std::string* error)
      : isolate_(isolate), input_size_(input_size), error_(error) {
    frames_.reserve(16);
    values_.reserve(64);
    properties_.reserve(64);
  }
  bool Fail(const char* message) {
    if (error_->empty()) *error_ = message;
    return false;
  }
  bool Add(Handle<Object> value) {
    if (frames_.empty()) {
      result_ = value;
      return true;
    }
    Frame& frame = frames_.back();
    if (frame.map) {
      if (frame.key) {
        if (!IsString(*value))
          return Fail("MessagePack object keys must be strings");
        frame.pending_key =
            isolate_->factory()->InternalizeString(Handle<String>::cast(value));
        uint32_t index;
        frame.indexed |= frame.pending_key->AsArrayIndex(&index);
      } else {
        properties_.push_back({frame.pending_key, value});
        ++frame.completed;
      }
    } else {
      values_.push_back(value);
      ++frame.completed;
    }
    return true;
  }
  bool visit_nil() { return Add(isolate_->factory()->null_value()); }
  bool visit_boolean(bool v) { return Add(isolate_->factory()->ToBoolean(v)); }
  bool visit_positive_integer(uint64_t v) {
    return Add(v <= kMaxSafeInteger
                   ? isolate_->factory()->NewNumber(static_cast<double>(v))
                   : Handle<Object>(BigInt::FromUint64(isolate_, v)));
  }
  bool visit_negative_integer(int64_t v) {
    if (v >= 0) return visit_positive_integer(static_cast<uint64_t>(v));
    return Add(v >= -static_cast<int64_t>(kMaxSafeInteger)
                   ? isolate_->factory()->NewNumber(static_cast<double>(v))
                   : Handle<Object>(BigInt::FromInt64(isolate_, v)));
  }
  bool visit_float32(float v) { return visit_float64(v); }
  bool visit_float64(double v) {
    return Add(isolate_->factory()->NewNumber(v));
  }
  bool visit_str(const char* data, uint32_t size) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    if (size > static_cast<uint32_t>(String::kMaxLength))
      return Fail("Invalid MessagePack UTF-8 string");
    bool key = !frames_.empty() && frames_.back().map && frames_.back().key;
    StringCacheEntry* cached = nullptr;
    if (size <= kMaxCachedStringBytes) {
      uint32_t hash = StringCacheHash(bytes, size);
      cached = key ? &key_cache_[hash & 255] : &value_cache_[hash & 127];
      if (!cached->value.is_null() && cached->size == size &&
          (!size || std::memcmp(cached->bytes, bytes, size) == 0)) {
        if (key) {
          frames_.back().pending_key = cached->value;
          frames_.back().indexed |= cached->indexed;
          return true;
        }
        return Add(cached->value);
      }
    }
    Utf8Info info;
    if (!ScanUtf8(bytes, size, &info))
      return Fail("Invalid MessagePack UTF-8 string");
    Handle<String> string;
    // Unique short values must not enter the isolate-wide string table. The
    // per-decode value cache already shares repeated primitive strings.
    bool internalize = key;
    if (internalize && info.ascii) {
      string = isolate_->factory()->InternalizeString(
          base::Vector<const uint8_t>(bytes, static_cast<int>(size)));
    } else if (!MakeUtf8String(isolate_, bytes, size, info).ToHandle(&string)) {
      return false;
    } else if (internalize) {
      string = isolate_->factory()->InternalizeString(string);
    }
    uint32_t index;
    bool indexed = key && string->AsArrayIndex(&index);
    if (cached) *cached = {bytes, size, string, indexed};
    if (key) {
      frames_.back().pending_key = string;
      frames_.back().indexed |= indexed;
      return true;
    }
    return Add(string);
  }
  bool visit_bin(const char*, uint32_t) {
    return Fail("Binary values are outside the initial JSON-shaped profile");
  }
  bool visit_ext(const char*, size_t) {
    return Fail("Extensions are outside the initial JSON-shaped profile");
  }
  bool Start(bool map, uint32_t count) {
    if (frames_.size() >= kMaxDepth || count > input_size_ / (map ? 2 : 1) ||
        count > static_cast<uint32_t>(FixedArray::kMaxLength))
      return Fail("MessagePack container limit exceeded");
    if (!frames_.empty() && frames_.back().map && frames_.back().key)
      return Fail("MessagePack object keys must be strings");
    size_t depth = frames_.size();
    Handle<String> role;
    if (!frames_.empty()) {
      const Frame& parent = frames_.back();
      role = parent.map ? parent.pending_key : parent.role;
    }
    size_t feedback_slot =
        (depth * 17 + (role.is_null() ? 0 : role->EnsureHash())) & 63;
    const MapFeedback& cached = feedback_[feedback_slot];
    bool same_role = role.is_null()
                         ? cached.role.is_null()
                         : !cached.role.is_null() && *role == *cached.role;
    Handle<Map> feedback =
        map && same_role && cached.depth == depth ? cached.map : Handle<Map>();
    frames_.push_back({map,
                       true,
                       count,
                       0,
                       map ? properties_.size() : values_.size(),
                       {},
                       feedback,
                       role,
                       feedback_slot,
                       false});
    return true;
  }
  bool start_map(uint32_t n) { return Start(true, n); }
  bool start_array(uint32_t n) { return Start(false, n); }
  bool start_map_key() {
    frames_.back().key = true;
    return true;
  }
  bool start_map_value() {
    frames_.back().key = false;
    return true;
  }
  bool end_map() {
    Frame frame = frames_.back();
    frames_.pop_back();
    if (frame.completed != frame.count)
      return Fail("Invalid MessagePack map count");
    const Property* begin = properties_.data() + frame.start;
    const Property* end = properties_.data() + properties_.size();
    Handle<JSObject> object;
    if (frame.indexed) {
      // Keep full own-property semantics for index keys. Optimize this less
      // common path independently after measuring a representative corpus.
      object = isolate_->factory()->NewJSObject(isolate_->object_function());
      for (auto it = begin; it != end; ++it) {
        PropertyKey property_key(isolate_, Handle<Name>::cast(it->key));
        LookupIterator lookup(isolate_, object, property_key,
                              LookupIterator::OWN);
        if (JSObject::DefineOwnPropertyIgnoreAttributes(&lookup, it->value,
                                                        NONE)
                .is_null())
          return false;
      }
    } else {
      if (!frame.feedback.is_null() && frame.feedback->is_deprecated())
        frame.feedback = Map::Update(isolate_, frame.feedback);
      JSDataObjectBuilder builder(
          isolate_, HOLEY_ELEMENTS, static_cast<int>(frame.count),
          frame.feedback,
          JSDataObjectBuilder::kHeapNumbersGuaranteedUniquelyOwned);
      PropertyIterator it(begin, end);
      object = builder.BuildFromIterator(it);
    }
    feedback_[frame.feedback_slot] = {frames_.size(), frame.role,
                                      handle(object->map(), isolate_)};
    properties_.resize(frame.start);
    return Add(object);
  }
  bool end_array() {
    Frame frame = frames_.back();
    frames_.pop_back();
    if (frame.completed != frame.count)
      return Fail("Invalid MessagePack array count");
    ElementsKind kind = PACKED_SMI_ELEMENTS;
    for (size_t i = frame.start; i < values_.size(); ++i) {
      if (IsHeapNumber(*values_[i]))
        kind = PACKED_DOUBLE_ELEMENTS;
      else if (!IsSmi(*values_[i])) {
        kind = PACKED_ELEMENTS;
        break;
      }
    }
    Handle<JSArray> array =
        isolate_->factory()->NewJSArray(kind, frame.count, frame.count);
    if (frame.count) {
      DisallowGarbageCollection no_gc;
      if (kind == PACKED_DOUBLE_ELEMENTS) {
        Tagged<FixedDoubleArray> storage =
            FixedDoubleArray::cast(array->elements());
        for (uint32_t i = 0; i < frame.count; ++i)
          storage->set(i, Object::Number(*values_[frame.start + i]));
      } else {
        Tagged<FixedArray> storage = FixedArray::cast(array->elements());
        WriteBarrierMode mode = storage->GetWriteBarrierMode(no_gc);
        for (uint32_t i = 0; i < frame.count; ++i) {
          storage->set(i, *values_[frame.start + i], mode);
        }
      }
    }
    values_.resize(frame.start);
    return Add(array);
  }
  void parse_error(size_t, size_t) { Fail("Malformed MessagePack input"); }
  void insufficient_bytes(size_t, size_t) {
    Fail("Truncated MessagePack input");
  }
  MaybeHandle<Object> result() {
    return error_->empty() ? MaybeHandle<Object>(result_)
                           : MaybeHandle<Object>();
  }

 private:
  struct Frame {
    bool map, key;
    uint32_t count, completed;
    size_t start;
    Handle<String> pending_key;
    Handle<Map> feedback;
    Handle<String> role;
    size_t feedback_slot;
    bool indexed;
  };
  Isolate* isolate_;
  size_t input_size_;
  std::string* error_;
  std::vector<Frame> frames_;
  std::vector<Property> properties_;
  std::vector<Handle<Object>> values_;
  struct MapFeedback {
    size_t depth = 0;
    Handle<String> role;
    Handle<Map> map;
  };
  MapFeedback feedback_[64]{};
  Handle<Object> result_;
  static constexpr uint32_t kMaxCachedStringBytes = 4096;
  struct StringCacheEntry {
    const uint8_t* bytes;
    uint32_t size;
    Handle<String> value;
    bool indexed;
  };
  StringCacheEntry key_cache_[256]{};
  StringCacheEntry value_cache_[128]{};
};

bool VisitTree(const msgpack::object& o, V8Visitor* v, size_t depth = 0) {
  if (depth > kMaxDepth) return v->Fail("MessagePack container limit exceeded");
  switch (o.type) {
    case msgpack::type::NIL:
      return v->visit_nil();
    case msgpack::type::BOOLEAN:
      return v->visit_boolean(o.via.boolean);
    case msgpack::type::POSITIVE_INTEGER:
      return v->visit_positive_integer(o.via.u64);
    case msgpack::type::NEGATIVE_INTEGER:
      return v->visit_negative_integer(o.via.i64);
    case msgpack::type::FLOAT32:
    case msgpack::type::FLOAT64:
      return v->visit_float64(o.via.f64);
    case msgpack::type::STR:
      return v->visit_str(o.via.str.ptr, o.via.str.size);
    case msgpack::type::ARRAY:
      if (!v->start_array(o.via.array.size)) return false;
      for (uint32_t i = 0; i < o.via.array.size; ++i)
        if (!VisitTree(o.via.array.ptr[i], v, depth + 1)) return false;
      return v->end_array();
    case msgpack::type::MAP:
      if (!v->start_map(o.via.map.size)) return false;
      for (uint32_t i = 0; i < o.via.map.size; ++i) {
        if (!v->start_map_key() ||
            !VisitTree(o.via.map.ptr[i].key, v, depth + 1) ||
            !v->start_map_value() ||
            !VisitTree(o.via.map.ptr[i].val, v, depth + 1))
          return false;
      }
      return v->end_map();
    default:
      return v->Fail("Unsupported MessagePack value");
  }
}

bool ReadMPack(mpack_reader_t* reader, V8Visitor* visitor, size_t depth = 0) {
  if (depth > kMaxDepth)
    return visitor->Fail("MessagePack nesting limit exceeded");
  mpack_tag_t tag = mpack_read_tag(reader);
  if (mpack_reader_error(reader) != mpack_ok)
    return visitor->Fail("Malformed or truncated MessagePack input");
  switch (mpack_tag_type(&tag)) {
    case mpack_type_nil:
      return visitor->visit_nil();
    case mpack_type_bool:
      return visitor->visit_boolean(mpack_tag_bool_value(&tag));
    case mpack_type_uint:
      return visitor->visit_positive_integer(mpack_tag_uint_value(&tag));
    case mpack_type_int:
      return visitor->visit_negative_integer(mpack_tag_int_value(&tag));
    case mpack_type_float:
      return visitor->visit_float32(mpack_tag_float_value(&tag));
    case mpack_type_double:
      return visitor->visit_float64(mpack_tag_double_value(&tag));
    case mpack_type_str: {
      uint32_t size = mpack_tag_str_length(&tag);
      const char* bytes = mpack_read_bytes_inplace(reader, size);
      if (mpack_reader_error(reader) != mpack_ok)
        return visitor->Fail("Truncated MessagePack string");
      bool ok = visitor->visit_str(bytes, size);
      mpack_done_str(reader);
      return ok;
    }
    case mpack_type_array: {
      uint32_t n = mpack_tag_array_count(&tag);
      if (!visitor->start_array(n)) return false;
      for (uint32_t i = 0; i < n; ++i)
        if (!ReadMPack(reader, visitor, depth + 1)) return false;
      mpack_done_array(reader);
      return visitor->end_array();
    }
    case mpack_type_map: {
      uint32_t n = mpack_tag_map_count(&tag);
      if (!visitor->start_map(n)) return false;
      for (uint32_t i = 0; i < n; ++i) {
        if (!visitor->start_map_key() ||
            !ReadMPack(reader, visitor, depth + 1) ||
            !visitor->start_map_value() ||
            !ReadMPack(reader, visitor, depth + 1))
          return false;
      }
      mpack_done_map(reader);
      return visitor->end_map();
    }
    default:
      return visitor->Fail("Unsupported MessagePack value");
  }
}

class ByteWriter {
 public:
  explicit ByteWriter(MessagePackBuffer* output,
                      MessagePackAsyncData* capture = nullptr)
      : output_(output), main_(output), capture_(capture) {}
  void write(const char* bytes, size_t n) {
    if (!n) return;
    uint8_t* destination = output_->Append(n);
#if defined(V8_TARGET_ARCH_ARM)
    // Encoded keys and numeric bodies usually fit in a few words. Constant
    // bounded copies avoid a libc call for each field; overlapping tails cover
    // every byte without reading or writing beyond either span.
    if (n > 16) {
      std::memcpy(destination, bytes, n);
      return;
    }
    if (n >= 8) {
      uint64_t first, last;
      std::memcpy(&first, bytes, 8);
      std::memcpy(&last, bytes + n - 8, 8);
      std::memcpy(destination, &first, 8);
      std::memcpy(destination + n - 8, &last, 8);
    } else if (n >= 4) {
      uint32_t first, last;
      std::memcpy(&first, bytes, 4);
      std::memcpy(&last, bytes + n - 4, 4);
      std::memcpy(destination, &first, 4);
      std::memcpy(destination + n - 4, &last, 4);
    } else if (n >= 2) {
      uint16_t first, last;
      std::memcpy(&first, bytes, 2);
      std::memcpy(&last, bytes + n - 2, 2);
      std::memcpy(destination, &first, 2);
      std::memcpy(destination + n - 2, &last, 2);
    } else {
      destination[0] = static_cast<uint8_t>(bytes[0]);
    }
#else
    std::memcpy(destination, bytes, n);
#endif
  }
  uint8_t* Append(size_t n) { return output_->Append(n); }
  size_t size() const { return output_->size(); }
  void Rewind(size_t size) {
    output_->Rewind(size);
    if (capturing()) {
      while (!capture_->parts.empty() && capture_->parts.back().offset >= size)
        capture_->parts.pop_back();
      if (!capture_->parts.empty()) {
        auto& part = capture_->parts.back();
        if (part.offset + part.length > size) {
          DCHECK(part.kind == MessagePackEncodePart::kNumber ||
                 part.kind == MessagePackEncodePart::kNumbers);
          DCHECK_EQ((size - part.offset) % sizeof(double), 0);
          part.length = static_cast<uint32_t>(size - part.offset);
        }
      }
    }
  }
  bool capturing() const { return capture_ && output_ == main_ && !keys_; }
  V8_INLINE uint8_t* CaptureSpan(size_t length,
                               MessagePackEncodePart::Kind kind) {
    size_t offset = size();
    uint8_t* destination = Append(length);
    // Mixed arrays can contain long adjacent runs of HeapNumbers. Keep one
    // native span for each run instead of one metadata entry per scalar.
    bool numbers = kind == MessagePackEncodePart::kNumber ||
                   kind == MessagePackEncodePart::kNumbers;
    auto* previous = capture_->parts.empty() ? nullptr : &capture_->parts.back();
    if (numbers && previous &&
        (previous->kind == MessagePackEncodePart::kNumber ||
         previous->kind == MessagePackEncodePart::kNumbers) &&
        previous->offset + previous->length == offset) {
      previous->kind = MessagePackEncodePart::kNumbers;
      previous->length += static_cast<uint32_t>(length);
    } else {
      capture_->parts.push_back(
          {static_cast<uint32_t>(offset), static_cast<uint32_t>(length), kind});
    }
    // The budget depends on capacities, which change only on allocation.
    // Avoid re-reading unrelated worker buffers and tape metadata per scalar.
    if (main_->capacity() != checked_input_ ||
        capture_->parts.capacity() != checked_parts_)
      CheckCaptureMemory();
    return destination;
  }
  V8_INLINE void Capture(const void* bytes, size_t length,
                         MessagePackEncodePart::Kind kind) {
    if (length) std::memcpy(CaptureSpan(length, kind), bytes, length);
  }
  bool keys_ = false;
  const uint8_t* data() const { return output_->data(); }
  MessagePackBuffer* SetBuffer(MessagePackBuffer* buffer) {
    MessagePackBuffer* previous = output_;
    output_ = buffer;
    return previous;
  }

 private:
  V8_NOINLINE void CheckCaptureMemory() {
    if (capture_->MemoryUsage() > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
    checked_input_ = main_->capacity();
    checked_parts_ = capture_->parts.capacity();
  }
  MessagePackBuffer* output_;
  MessagePackBuffer* main_;
  MessagePackAsyncData* capture_;
  size_t checked_input_ = 0, checked_parts_ = 0;
};

template <bool async = false>
class Encoder {
 public:
  Encoder(Isolate* isolate, MessagePackBuffer* output, std::string* error,
          bool lossless_float32, MessagePackAsyncData* capture = nullptr)
      : isolate_(isolate),
        writer_(output, capture),
        packer_(writer_),
        error_(error),
        lossless_float32_(lossless_float32),
        capture_(capture),
        candidates_(isolate->factory()->NewFixedArray(64)) {
    if (!GetMessagePackRealmCache(isolate).ToHandle(&realm_cache_))
      throw std::runtime_error("MessagePack cache initialization failed");
  }
#ifdef MSGPACK_PROFILE_CACHE
  ~Encoder() {
    std::fprintf(stderr, "encoder cache hits=%u misses=%u\n", cache_hits_,
                 cache_misses_);
  }
#endif
  bool Fail(const char* s) {
    *error_ = s;
    return false;
  }
  void Prepare(Handle<Object> value) {
    Tagged<Map> map;
    int count;
    if (!SizeHintKey(*value, &map, &count)) return;
    if constexpr (async) {
      auto hint = WeakFixedArray::cast(realm_cache_->get(6));
      Tagged<HeapObject> cached;
      if (hint->get(0).GetHeapObjectIfWeak(&cached) && cached == map &&
          hint->get(1).IsSmi() && hint->get(1).ToSmi().value() == count &&
          hint->get(2).IsSmi() && hint->get(3).IsSmi()) {
        int bytes = hint->get(2).ToSmi().value();
        int parts = hint->get(3).ToSmi().value();
        if (bytes > 0 && bytes <= 8 * 1024 * 1024 && parts >= 0 &&
            parts <= 262144) {
          writer_.Append(bytes);
          writer_.Rewind(0);
          capture_->parts.reserve(static_cast<size_t>(parts));
          return;
        }
      }
    }
    Tagged<WeakFixedArray> hint = WeakFixedArray::cast(realm_cache_->get(4));
    Tagged<HeapObject> cached;
    if (!hint->get(0).GetHeapObjectIfWeak(&cached) || cached != map ||
        !hint->get(1).IsSmi() || hint->get(1).ToSmi().value() != count ||
        !hint->get(2).IsSmi())
      return;
    int bytes = hint->get(2).ToSmi().value();
    // Scalar feedback only: no input graph or bytes are retained. Cap a stale
    // reserve at 8 MiB; a changing payload still grows with checked limits.
    if (bytes > 0 && bytes <= 8 * 1024 * 1024) {
      writer_.Append(bytes);
      writer_.Rewind(0);
    }
  }
  void RememberSize(Handle<Object> value) {
    Tagged<Map> map;
    int count;
    if (!SizeHintKey(*value, &map, &count) || writer_.size() > 8 * 1024 * 1024)
      return;
    Tagged<WeakFixedArray> hint = WeakFixedArray::cast(realm_cache_->get(4));
    hint->set(0, MakeWeak(map));
    hint->set(1, Smi::FromInt(count));
    hint->set(2, Smi::FromInt(static_cast<int>(writer_.size())));
  }
  void RememberCaptureSize(Handle<Object> value) {
    if constexpr (async) {
      Tagged<Map> map;
      int count;
      if (!SizeHintKey(*value, &map, &count) || !writer_.size() ||
          writer_.size() > 8 * 1024 * 1024 || capture_->parts.size() > 262144)
        return;
      // Weak map plus scalar allocation feedback only; captured objects and
      // bytes remain job-owned. The wire-size hint is independent because
      // native numeric spans and code units can be larger than their wire.
      auto hint = WeakFixedArray::cast(realm_cache_->get(6));
      hint->set(0, MakeWeak(map));
      hint->set(1, Smi::FromInt(count));
      hint->set(2, Smi::FromInt(static_cast<int>(writer_.size())));
      hint->set(3, Smi::FromInt(static_cast<int>(capture_->parts.size())));
    }
  }
  V8_INLINE void WriteTag(uint8_t tag) { writer_.Append(1)[0] = tag; }
  V8_INLINE void WriteContainerHeader(uint32_t count, bool map) {
    if (count < 16) {
      WriteTag(static_cast<uint8_t>((map ? 0x80 : 0x90) | count));
    } else if (map) {
      packer_.pack_map(count);
    } else {
      packer_.pack_array(count);
    }
  }
  V8_INLINE bool TryScalar(Tagged<Object> value,
                           const DisallowGarbageCollection& no_gc) {
    if (IsSmi(value)) {
      // Small tagged integers already have a cheap wire representation.
      // Capturing each one as a double plus a part entry costs more foreground
      // work than writing it here. Packed numeric arrays still use one span.
      int32_t number = Smi::cast(value).value();
      if (number >= 0)
        packer_.pack_uint32(static_cast<uint32_t>(number));
      else
        packer_.pack_int32(number);
      return true;
    }
    if (IsHeapNumber(value)) return Number(HeapNumber::cast(value)->value());
    if (IsNull(value, isolate_)) {
      WriteTag(0xc0);
      return true;
    }
    if (IsBoolean(value, isolate_)) {
      if (IsTrue(value, isolate_))
        WriteTag(0xc3);
      else
        WriteTag(0xc2);
      return true;
    }
#if defined(V8_TARGET_ARCH_ARM64)
    if (IsSeqOneByteString(value)) {
      Tagged<SeqOneByteString> string = SeqOneByteString::cast(value);
      uint32_t length = static_cast<uint32_t>(string->length());
      if (length <= 31) {
        const uint8_t* bytes = string->GetChars(no_gc);
        if (messagepack_strings::IsAscii(bytes, length)) {
          uint8_t* output = writer_.Append(length + 1);
          output[0] = static_cast<uint8_t>(0xa0 | length);
          if (length) std::memcpy(output + 1, bytes, length);
          return true;
        }
      }
    }
#endif
    if (IsString(value)) return RawString(String::cast(value), no_gc);
    return false;
  }
  V8_NOINLINE bool RawString(Tagged<String> string,
                             const DisallowGarbageCollection& no_gc) {
    if (IsSeqOneByteString(string)) {
      const uint8_t* bytes = SeqOneByteString::cast(string)->GetChars(no_gc);
      int length = string->length();
      if constexpr (async) {
        if (writer_.capturing() && length > 64) {
          writer_.Capture(bytes, length, MessagePackEncodePart::kString8);
          return true;
        }
      }
      if (!messagepack_strings::IsAscii(bytes, length))
        return WriteString(bytes, length);
      return WriteAscii(bytes, length);
    }
    if (string->IsFlat()) {
      String::FlatContent flat = string->GetFlatContent(no_gc);
      if (flat.IsOneByte()) {
        auto bytes = flat.ToOneByteVector();
        if constexpr (async) {
          if (writer_.capturing() && bytes.length() > 64) {
            writer_.Capture(bytes.begin(), bytes.length(),
                            MessagePackEncodePart::kString8);
            return true;
          }
        }
        if (!messagepack_strings::IsAscii(bytes.begin(), bytes.length()))
          return WriteString(bytes.begin(), bytes.length());
        return WriteAscii(bytes.begin(), bytes.length());
      }
      auto chars = flat.ToUC16Vector();
      return WriteString(chars.begin(), chars.length());
    }
    return false;
  }
  V8_INLINE bool WriteAscii(const uint8_t* bytes, uint32_t length) {
    if constexpr (async) {
      if (writer_.capturing() && length > 64) {
        writer_.Capture(bytes, length, MessagePackEncodePart::kString8);
        return true;
      }
    }
    // The caller has validated every byte. Reserve the complete wire string
    // once, including its header, so the common path needs one capacity check.
    uint32_t header = length < 32      ? 1
                      : length < 256   ? 2
                      : length < 65536 ? 3
                                       : 5;
    uint8_t* output = writer_.Append(static_cast<size_t>(length) + header);
    if (header == 1) {
      output[0] = static_cast<uint8_t>(0xa0 | length);
    } else if (header == 2) {
      output[0] = 0xd9;
      output[1] = static_cast<uint8_t>(length);
    } else if (header == 3) {
      output[0] = 0xda;
      output[1] = static_cast<uint8_t>(length >> 8);
      output[2] = static_cast<uint8_t>(length);
    } else {
      output[0] = 0xdb;
      output[1] = static_cast<uint8_t>(length >> 24);
      output[2] = static_cast<uint8_t>(length >> 16);
      output[3] = static_cast<uint8_t>(length >> 8);
      output[4] = static_cast<uint8_t>(length);
    }
    if (length) std::memcpy(output + header, bytes, length);
    return true;
  }
  bool CaptureNumberArray(Tagged<FixedArrayBase> storage, uint32_t length,
                          bool smi) {
    if constexpr (!async) return false;
    if (!writer_.capturing() || !length) return false;
    size_t width = smi ? sizeof(int32_t) : sizeof(double);
    uint8_t* bytes = writer_.CaptureSpan(static_cast<size_t>(length) * width,
                                         smi ? MessagePackEncodePart::kIntegers
                                             : MessagePackEncodePart::kNumbers);
    for (uint32_t i = 0; i < length; ++i) {
      if (smi) {
        int32_t number = Smi::cast(FixedArray::cast(storage)->get(i)).value();
        std::memcpy(bytes + i * width, &number, width);
      } else {
        double number = FixedDoubleArray::cast(storage)->get_scalar(i);
        std::memcpy(bytes + i * width, &number, width);
      }
    }
    return true;
  }
  uint32_t CaptureTaggedNumberPrefix(Tagged<FixedArray> storage,
                                     uint32_t length) {
    if constexpr (!async) return 0;
    if (!writer_.capturing() || length < 64) return 0;
    uint32_t count = 0;
    bool smi = true;
    while (count < length && IsNumber(storage->get(count))) {
      smi &= IsSmi(storage->get(count));
      ++count;
    }
    if (count < 64) return 0;
    if (smi) {
      CaptureNumberArray(storage, count, true);
      return count;
    }
    uint8_t* bytes = writer_.CaptureSpan(static_cast<size_t>(count) * 8,
                                         MessagePackEncodePart::kNumbers);
    for (uint32_t i = 0; i < count; ++i) {
      double number = Object::Number(storage->get(i));
      std::memcpy(bytes + static_cast<size_t>(i) * 8, &number, 8);
    }
    return count;
  }
  bool Number(double n) {
    if (std::isfinite(n) && !(n == 0 && std::signbit(n)) &&
        std::trunc(n) == n && n >= -static_cast<double>(kMaxSafeInteger) &&
        n <= static_cast<double>(kMaxSafeInteger)) {
      if (n >= 0)
        packer_.pack_uint64(static_cast<uint64_t>(n));
      else
        packer_.pack_int64(static_cast<int64_t>(n));
    } else if (lossless_float32_ && std::isfinite(n) &&
               std::fabs(n) <= std::numeric_limits<float>::max() &&
               static_cast<double>(static_cast<float>(n)) == n) {
      // IEEE float32 is selected only if widening reproduces the JS Number
      // exactly. Negative zero retains its sign; all other values use float64.
      packer_.pack_float(static_cast<float>(n));
    } else
      packer_.pack_double(n);
    return true;
  }
  bool KeyValue(Handle<String> string) {
    struct Restore {
      ByteWriter* writer;
      bool previous;
      ~Restore() { writer->keys_ = previous; }
    } restore{&writer_, writer_.keys_};
    writer_.keys_ = true;
    return StringValue(string);
  }
  bool StringValue(Handle<String> string) {
    string = String::Flatten(isolate_, string);
    DisallowGarbageCollection no_gc;
    String::FlatContent flat = string->GetFlatContent(no_gc);
    if (flat.IsOneByte()) {
      auto bytes = flat.ToOneByteVector();
      if (messagepack_strings::IsAscii(bytes.begin(), bytes.length())) {
        return WriteAscii(bytes.begin(), bytes.length());
      }
      return WriteString(bytes.begin(), bytes.length());
    }
    auto chars = flat.ToUC16Vector();
    return WriteString(chars.begin(), chars.length());
  }
  template <typename Char>
  // Keep Unicode conversion and its register spills out of the common ASCII
  // StringValue path, including compiler-vectorized scalar conversion.
  V8_NOINLINE bool WriteString(const Char* chars, size_t count) {
    if (!count) {
      WriteTag(0xa0);
      return true;
    }
    if constexpr (async) {
      if (writer_.capturing()) {
        writer_.Capture(chars, count * sizeof(Char),
                        sizeof(Char) == 1 ? MessagePackEncodePart::kString8
                                          : MessagePackEncodePart::kString16);
        return true;
      }
    }
    if (messagepack_strings::UseSimdForEncoding(chars, count))
      return WriteStringImpl<Char, true>(chars, count);
#if defined(V8_TARGET_ARCH_ARM64)
    if constexpr (sizeof(Char) == 2) {
      // Mixed UTF-16 pays for two scalar passes. For bounded strings, reserve
      // its worst-case bytes and validate while writing, then close the small
      // header gap. Larger strings keep exact preflight allocation. Near the
      // byte limit, use preflight so a fitting value cannot be rejected due
      // to this temporary reserve.
      if (count >= 64 && count <= 4096 &&
          writer_.size() <= kMaxOutputBytes - (count * 3 + 5))
        return WriteMixedString(chars, count);
    }
#endif
    return WriteStringImpl<Char, false>(chars, count);
  }
#if defined(V8_TARGET_ARCH_ARM64)
  V8_NOINLINE bool WriteMixedString(const uint16_t* chars, size_t count) {
    size_t start = writer_.size();
    uint8_t* body = writer_.Append(count * 3 + 5) + 5;
    uint8_t* output = body;
    for (size_t i = 0; i < count;) {
      uint32_t cp = chars[i++];
      if (cp >= 0xd800 && cp <= 0xdfff) {
        if (cp > 0xdbff || i == count || chars[i] < 0xdc00 ||
            chars[i] > 0xdfff)
          return Fail("Unpaired UTF-16 surrogate is outside the UTF-8 profile");
        cp = 0x10000 + ((cp - 0xd800) << 10) + (chars[i++] - 0xdc00);
      }
      if (cp < 0x80) {
        *output++ = static_cast<uint8_t>(cp);
      } else if (cp < 0x800) {
        *output++ = static_cast<uint8_t>(0xc0 | (cp >> 6));
        *output++ = static_cast<uint8_t>(0x80 | (cp & 63));
      } else if (cp < 0x10000) {
        *output++ = static_cast<uint8_t>(0xe0 | (cp >> 12));
        *output++ = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63));
        *output++ = static_cast<uint8_t>(0x80 | (cp & 63));
      } else {
        *output++ = static_cast<uint8_t>(0xf0 | (cp >> 18));
        *output++ = static_cast<uint8_t>(0x80 | ((cp >> 12) & 63));
        *output++ = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63));
        *output++ = static_cast<uint8_t>(0x80 | (cp & 63));
      }
    }
    uint32_t length = static_cast<uint32_t>(output - body);
    writer_.Rewind(start);
    packer_.pack_str(length);
    // Reserve already guaranteed capacity, so neither call can invalidate body.
    std::memmove(writer_.Append(length), body, length);
    return true;
  }
#endif
  template <typename Char, bool use_simd>
  // Compile scalar and SIMD conversion independently. Otherwise the combined
  // function's register pressure penalizes its scalar fallback on mixed text.
  V8_NOINLINE bool WriteStringImpl(const Char* chars, size_t count) {
    size_t length;
    bool ascii;
    if (!messagepack_strings::Utf8Length<Char, use_simd>(chars, count, &length,
                                                         &ascii))
      return Fail("Unpaired UTF-16 surrogate is outside the UTF-8 profile");
    if (length > kMaxOutputBytes)
      return Fail("MessagePack output limit exceeded");
    packer_.pack_str(static_cast<uint32_t>(length));
    messagepack_strings::EncodeUtf8<Char, use_simd>(chars, count,
                                                    writer_.Append(length));
    return true;
  }
  bool Value(Handle<Object> value, size_t depth = 0) {
    if (depth > kMaxDepth) return Fail("MessagePack nesting limit exceeded");
    RefreshWireCache();
    {
      size_t start = writer_.size();
      DisallowGarbageCollection no_gc;
      if (++fast_epoch_ == 0) {
        for (auto& plan : fast_plans_) plan.epoch = 0;
        ++fast_epoch_;
      }
      if (FastValue(*value, depth, 0, no_gc)) return true;
      writer_.Rewind(start);
      if (!error_->empty()) return false;
    }
    if (IsSmi(*value)) {
      int32_t number = Smi::cast(*value).value();
      if (number >= 0)
        packer_.pack_uint32(static_cast<uint32_t>(number));
      else
        packer_.pack_int32(number);
      return true;
    }
    if (IsHeapNumber(*value)) return Number(Object::Number(*value));
    if (IsNull(*value, isolate_)) {
      WriteTag(0xc0);
      return true;
    }
    if (IsBoolean(*value, isolate_)) {
      if (IsTrue(*value, isolate_))
        WriteTag(0xc3);
      else
        WriteTag(0xc2);
      return true;
    }
    if (IsString(*value)) return StringValue(Handle<String>::cast(value));
    if (IsBigInt(*value)) {
      auto integer = Handle<BigInt>::cast(value);
      bool lossless;
      int64_t signed_value = integer->AsInt64(&lossless);
      if (lossless)
        packer_.pack_int64(signed_value);
      else {
        uint64_t n = integer->AsUint64(&lossless);
        if (!lossless) return Fail("BigInt exceeds MessagePack 64-bit range");
        packer_.pack_uint64(n);
      }
      return true;
    }
    if (!IsJSObject(*value) ||
        (JSObject::cast(*value)->map()->instance_type() != JS_OBJECT_TYPE &&
         !IsJSArray(*value)))
      return Fail("Only data objects and dense arrays are supported");
    if (depth >= kMaxDepth) return Fail("MessagePack nesting limit exceeded");
    for (auto ancestor : ancestors_)
      if (*ancestor == *value)
        return Fail("Cyclic objects cannot be encoded as MessagePack trees");
    ancestors_.push_back(value);
    bool ok = true;
    Handle<JSObject> object = Handle<JSObject>::cast(value);
    if (IsJSArray(*object)) {
      Handle<JSArray> array = Handle<JSArray>::cast(object);
      uint32_t length = static_cast<uint32_t>(Object::Number(array->length()));
      WriteContainerHeader(length, false);
      ElementsKind kind = array->GetElementsKind();
      if (length > 0 && kind == PACKED_DOUBLE_ELEMENTS) {
        Handle<FixedDoubleArray> storage(
            FixedDoubleArray::cast(array->elements()), isolate_);
        if (!CaptureNumberArray(*storage, length, false))
          for (uint32_t i = 0; i < length && ok; ++i)
            ok = Number(storage->get_scalar(i));
      } else if (length > 0 && kind == PACKED_SMI_ELEMENTS) {
        DisallowGarbageCollection no_gc;
        Tagged<FixedArray> storage = FixedArray::cast(array->elements());
        for (uint32_t i = 0, count = CaptureNumberArray(storage, length, true)
                                         ? 0
                                         : length;
             i < count; ++i) {
          int32_t number = Smi::cast(storage->get(i)).value();
          if constexpr (async) {
            Number(number);
            continue;
          }
          if (number >= 0)
            packer_.pack_uint32(static_cast<uint32_t>(number));
          else
            packer_.pack_int32(number);
        }
      } else if (length > 0 && kind == PACKED_ELEMENTS) {
        Handle<FixedArray> storage(FixedArray::cast(array->elements()),
                                   isolate_);
        uint32_t prefix;
        {
          DisallowGarbageCollection no_gc;
          prefix = CaptureTaggedNumberPrefix(*storage, length);
        }
        for (uint32_t i = prefix; i < length && ok; ++i) {
          {
            DisallowGarbageCollection no_gc;
            if (TryScalar(storage->get(i), no_gc)) continue;
          }
          HandleScope scope(isolate_);
          ok = Value(handle(storage->get(i), isolate_), depth + 1);
        }
      } else
        for (uint32_t i = 0; i < length && ok; ++i) {
          HandleScope scope(isolate_);
          LookupIterator lookup(isolate_, object, i, object,
                                LookupIterator::OWN);
          if (lookup.state() != LookupIterator::DATA)
            ok = Fail("Sparse arrays and accessors are unsupported");
          else
            ok = Value(lookup.GetDataValue(), depth + 1);
        }
    } else if (object->HasFastProperties(isolate_) &&
               object->elements()->length() == 0) {
      Handle<ByteArray> wire = WirePlan(object);
      if (!wire.is_null()) {
        ok = EncodeWirePlan(object, wire, depth);
        ancestors_.pop_back();
        return ok;
      }
      if (EncodePlan* plan = GetPlan(object)) {
        ++plan->active;
        WriteContainerHeader(static_cast<uint32_t>(plan->fields.size()), true);
        for (auto& field : plan->fields) {
          HandleScope scope(isolate_);
          if (*plan->map != object->map()) {
            ok = Fail("Object changed during encoding");
            break;
          }
          if (field.wire.empty()) {
            size_t start = writer_.size();
            if (!KeyValue(field.key)) {
              ok = false;
              break;
            }
            field.wire.assign(writer_.data() + start,
                              writer_.data() + writer_.size());
          } else {
            writer_.write(reinterpret_cast<const char*>(field.wire.data()),
                          field.wire.size());
          }
          Handle<Object> value =
              field.constant.is_null()
                  ? handle(object->RawFastPropertyAt(field.index), isolate_)
                  : field.constant;
          if (!Value(value, depth + 1)) {
            ok = false;
            break;
          }
        }
        --plan->active;
        ancestors_.pop_back();
        return ok;
      }
      Handle<Map> map(object->map(), isolate_);
      uint32_t count = 0;
      for (InternalIndex i : map->IterateOwnDescriptors()) {
        Tagged<DescriptorArray> descriptors =
            map->instance_descriptors(isolate_);
        if (!IsString(descriptors->GetKey(i))) continue;
        PropertyDetails details = descriptors->GetDetails(i);
        if (details.IsDontEnum()) continue;
        if (details.kind() != PropertyKind::kData) {
          ok = Fail("Accessors are unsupported");
          break;
        }
        ++count;
      }
      if (ok) WriteContainerHeader(count, true);
      for (InternalIndex i : map->IterateOwnDescriptors()) {
        if (!ok) break;
        HandleScope scope(isolate_);
        if (*map != object->map()) {
          ok = Fail("Object changed during encoding");
          break;
        }
        Tagged<DescriptorArray> descriptors =
            map->instance_descriptors(isolate_);
        if (!IsString(descriptors->GetKey(i))) continue;
        PropertyDetails details = descriptors->GetDetails(i);
        if (details.IsDontEnum()) continue;
        Handle<String> key(
            handle(String::cast(descriptors->GetKey(i)), isolate_));
        Handle<Object> field =
            details.location() == PropertyLocation::kField
                ? handle(object->RawFastPropertyAt(
                             FieldIndex::ForDetails(*map, details)),
                         isolate_)
                : handle(descriptors->GetStrongValue(i), isolate_);
        ok = KeyValue(key) && Value(field, depth + 1);
      }
    } else {
      Handle<FixedArray> keys;
      if (!KeyAccumulator::GetKeys(
               isolate_, object, KeyCollectionMode::kOwnOnly,
               ENUMERABLE_STRINGS, GetKeysConversion::kConvertToString)
               .ToHandle(&keys))
        return false;
      WriteContainerHeader(keys->length(), true);
      for (int i = 0; i < keys->length() && ok; ++i) {
        HandleScope scope(isolate_);
        Handle<String> key(handle(String::cast(keys->get(i)), isolate_));
        PropertyKey property_key(isolate_, Handle<Name>::cast(key));
        LookupIterator lookup(isolate_, object, property_key,
                              LookupIterator::OWN);
        if (lookup.state() != LookupIterator::DATA)
          ok = Fail("Accessors are unsupported");
        else
          ok = KeyValue(key) && Value(lookup.GetDataValue(), depth + 1);
      }
    }
    ancestors_.pop_back();
    return ok;
  }

 private:
  bool SizeHintKey(Tagged<Object> value, Tagged<Map>* map, int* count) {
    if (IsJSArray(value)) {
      double length = Object::Number(JSArray::cast(value)->length());
      if (length > Smi::kMaxValue) return false;
      *count = static_cast<int>(length);
    } else if (IsString(value)) {
      *count = String::cast(value)->length();
    } else if (IsJSObject(value)) {
      *count = JSObject::cast(value)->map()->NumberOfOwnDescriptors();
    } else
      return false;
    *map = HeapObject::cast(value)->map();
    return true;
  }
  // A cached field plan contains only immutable key bytes and scalar offsets.
  // In a no-GC span we can traverse data-only graphs using raw tagged values,
  // without a handle scope or a new root for each property. A cache miss or an
  // unflattened string rewinds this subtree and uses the rooted general path.
  V8_INLINE bool FastValue(Tagged<Object> value, size_t depth,
                           size_t ancestor_count,
                           const DisallowGarbageCollection& no_gc) {
    if (TryScalar(value, no_gc)) return true;
    if (!error_->empty()) return false;
    return FastContainer(value, depth, ancestor_count, no_gc);
  }
  int WirePlanSlot(Tagged<Map> map) {
    return static_cast<int>(
        ((map.ptr() >> 4) & (kShapeCacheEntries / kShapeCacheWays - 1)) *
        kShapeCacheWays * 2);
  }
  void RefreshWireCache() {
    int count = isolate_->heap()->ms_count() & Smi::kMaxValue;
    if (Smi::ToInt(realm_cache_->get(5)) == count) return;
    // Weak references follow moving maps, but address-derived buckets do not.
    // Re-bucket only after major GC, before entering a raw traversal epoch.
    // All temporary V8 references stay within this no-GC span. Exact map
    // identity, checked on every lookup, remains the cache admission rule.
    DisallowGarbageCollection no_gc;
    Tagged<WeakFixedArray> table = WeakFixedArray::cast(realm_cache_->get(2));
    struct Entry {
      Tagged<Map> map;
      Tagged<ByteArray> bytes;
    };
    std::vector<Entry> entries;
    for (int i = 0; i < table->length(); i += 2) {
      Tagged<HeapObject> map, bytes;
      if (table->get(i).GetHeapObjectIfWeak(&map) && IsMap(map) &&
          table->get(i + 1).GetHeapObjectIfStrong(&bytes) && IsByteArray(bytes))
        entries.push_back({Map::cast(map), ByteArray::cast(bytes)});
    }
    for (int i = 0; i < table->length(); ++i) table->set(i, Smi::zero());
    int total_bytes = 0;
    for (const Entry& entry : entries) {
      int slot = WirePlanSlot(entry.map);
      bool free = false;
      for (int way = 0; way < kShapeCacheWays; ++way) {
        if (table->get(slot + way * 2).IsSmi()) {
          slot += way * 2;
          free = true;
          break;
        }
      }
      if (!free) continue;
      table->set(slot, MakeWeak(entry.map));
      table->set(slot + 1, entry.bytes);
      total_bytes += entry.bytes->length();
    }
    realm_cache_->set(3, Smi::FromInt(total_bytes));
    realm_cache_->set(5, Smi::FromInt(count));
  }
  bool FastContainer(Tagged<Object> value, size_t depth, size_t ancestor_count,
                     const DisallowGarbageCollection& no_gc) {
    if (depth >= kMaxDepth || !IsJSObject(value)) return false;
    Tagged<JSObject> object = JSObject::cast(value);
    bool array = IsJSArray(object);
    if (!array &&
        (object->map()->instance_type() != JS_OBJECT_TYPE ||
         !object->HasFastProperties(isolate_) || object->elements()->length()))
      return false;
    for (auto ancestor : ancestors_)
      if (*ancestor == value) return false;
    for (size_t i = 0; i < ancestor_count; ++i)
      if (fast_ancestors_[i] == value) return false;
    fast_ancestors_[ancestor_count++] = value;
    if (array) {
      Tagged<JSArray> data = JSArray::cast(object);
      uint32_t length = static_cast<uint32_t>(Object::Number(data->length()));
      ElementsKind kind = data->GetElementsKind();
      if (length && kind != PACKED_SMI_ELEMENTS &&
          kind != PACKED_DOUBLE_ELEMENTS && kind != PACKED_ELEMENTS)
        return false;
      WriteContainerHeader(length, false);
      if (!length) return true;
      if (kind == PACKED_DOUBLE_ELEMENTS) {
        Tagged<FixedDoubleArray> storage =
            FixedDoubleArray::cast(data->elements());
        if (!CaptureNumberArray(storage, length, false))
          for (uint32_t i = 0; i < length; ++i) Number(storage->get_scalar(i));
      } else if (kind == PACKED_SMI_ELEMENTS) {
        Tagged<FixedArray> storage = FixedArray::cast(data->elements());
        for (uint32_t i = 0, count = CaptureNumberArray(storage, length, true)
                                         ? 0
                                         : length;
             i < count; ++i) {
          int32_t number = Smi::cast(storage->get(i)).value();
          if constexpr (async) {
            Number(number);
            continue;
          }
          if (number >= 0)
            packer_.pack_uint32(static_cast<uint32_t>(number));
          else
            packer_.pack_int32(number);
        }
      } else {
        Tagged<FixedArray> storage = FixedArray::cast(data->elements());
        for (uint32_t i = CaptureTaggedNumberPrefix(storage, length); i < length;
             ++i)
          if (!FastValue(storage->get(i), depth + 1, ancestor_count, no_gc))
            return false;
      }
      return true;
    }
    Tagged<Map> object_map = object->map();
    RawPlan& local = fast_plans_[(object_map.ptr() >> 4) & 63];
    Tagged<ByteArray> wire;
    if (local.epoch == fast_epoch_ && local.map == object_map) {
      wire = local.wire;
    } else {
      Tagged<WeakFixedArray> table = WeakFixedArray::cast(realm_cache_->get(2));
      int base = WirePlanSlot(object_map);
      for (int way = 0; way < kShapeCacheWays; ++way) {
        Tagged<HeapObject> map, bytes;
        if (table->get(base + way * 2).GetHeapObjectIfWeak(&map) &&
            map == object_map &&
            table->get(base + way * 2 + 1).GetHeapObjectIfStrong(&bytes) &&
            IsByteArray(bytes)) {
          wire = ByteArray::cast(bytes);
          break;
        }
      }
      if (!wire.is_null()) local = {object_map, wire, fast_epoch_};
    }
    if (wire.is_null()) return false;
    uint32_t count;
    std::memcpy(&count, wire->begin(), 4);
    WriteContainerHeader(count, true);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
      int32_t field;
      uint32_t length;
      std::memcpy(&field, wire->begin() + offset, 4);
      std::memcpy(&length, wire->begin() + offset + 4, 4);
      offset += 8;
      writer_.write(reinterpret_cast<const char*>(wire->begin() + offset),
                    length);
      offset += length;
      Tagged<Object> item =
          field >= 0 ? object->RawFastPropertyAt(FieldIndex::ForInObjectOffset(
                           field, FieldIndex::kTagged))
                     : object->property_array()->get(-field - 1);
      if (!FastValue(item, depth + 1, ancestor_count, no_gc)) return false;
    }
    return true;
  }
  static uint32_t WireWord(Handle<ByteArray> wire, size_t offset) {
    uint32_t value;
    DisallowGarbageCollection no_gc;
    std::memcpy(&value, wire->begin() + offset, sizeof(value));
    return value;
  }
  bool EncodeWirePlan(Handle<JSObject> object, Handle<ByteArray> wire,
                      size_t depth) {
    uint32_t count = WireWord(wire, 0);
    WriteContainerHeader(count, true);
    size_t offset = 4;
    for (uint32_t i = 0; i < count; ++i) {
      Tagged<Object> raw_value;
      {
        DisallowGarbageCollection no_gc;
        int32_t field;
        uint32_t length;
        std::memcpy(&field, wire->begin() + offset, 4);
        std::memcpy(&length, wire->begin() + offset + 4, 4);
        offset += 8;
        writer_.write(reinterpret_cast<const char*>(wire->begin() + offset),
                      length);
        offset += length;
        raw_value =
            field >= 0
                ? object->RawFastPropertyAt(
                      FieldIndex::ForInObjectOffset(field, FieldIndex::kTagged))
                : object->property_array()->get(-field - 1);
        if (TryScalar(raw_value, no_gc)) continue;
      }
      HandleScope scope(isolate_);
      Handle<Object> value(handle(raw_value, isolate_));
      if (!Value(value, depth + 1)) return false;
    }
    return true;
  }
  Handle<ByteArray> WirePlan(Handle<JSObject> object) {
    Tagged<Map> raw_map = object->map();
    if (raw_map->NumberOfOwnDescriptors() > 256) return {};
    if (realm_cache_.is_null()) return {};
    Handle<WeakFixedArray> table(
        handle(WeakFixedArray::cast(realm_cache_->get(2)), isolate_));
    int base = WirePlanSlot(raw_map);
    int slot = base;
    Tagged<HeapObject> cached_map, cached_wire;
    for (int way = 0; way < kShapeCacheWays; ++way) {
      int candidate = base + way * 2;
      if (table->get(candidate).GetHeapObjectIfWeak(&cached_map) &&
          cached_map == object->map() &&
          table->get(candidate + 1).GetHeapObjectIfStrong(&cached_wire) &&
          IsByteArray(cached_wire)) {
#ifdef MSGPACK_PROFILE_CACHE
        ++cache_hits_;
#endif
        return handle(ByteArray::cast(cached_wire), isolate_);
      }
    }
#ifdef MSGPACK_PROFILE_CACHE
    ++cache_misses_;
#endif
    Handle<Map> map(object->map(), isolate_);
    for (InternalIndex i : map->IterateOwnDescriptors()) {
      Tagged<DescriptorArray> descriptors = map->instance_descriptors(isolate_);
      PropertyDetails details = descriptors->GetDetails(i);
      if (!IsString(descriptors->GetKey(i)) || details.IsDontEnum()) continue;
      if (details.kind() != PropertyKind::kData ||
          details.location() != PropertyLocation::kField)
        return {};
    }
    MessagePackBuffer metadata;
    uint32_t count = 0;
    std::memcpy(metadata.Append(4), &count, 4);
    {
      // Restore the output target even if a key conversion throws.
      struct Restore {
        ByteWriter* writer;
        MessagePackBuffer* previous;
        ~Restore() { writer->SetBuffer(previous); }
      } restore{&writer_, writer_.SetBuffer(&metadata)};
      for (InternalIndex i : map->IterateOwnDescriptors()) {
        Tagged<DescriptorArray> descriptors =
            map->instance_descriptors(isolate_);
        PropertyDetails details = descriptors->GetDetails(i);
        if (!IsString(descriptors->GetKey(i)) || details.IsDontEnum()) continue;
        FieldIndex index = FieldIndex::ForDetails(*map, details);
        int32_t field = index.is_inobject()
                            ? index.offset()
                            : -index.outobject_array_index() - 1;
        std::memcpy(metadata.Append(4), &field, 4);
        size_t length_offset = metadata.size();
        uint32_t length = 0;
        std::memcpy(metadata.Append(4), &length, 4);
        size_t start = metadata.size();
        Handle<String> key(
            handle(String::cast(descriptors->GetKey(i)), isolate_));
        if (!KeyValue(key)) return {};
        length = static_cast<uint32_t>(metadata.size() - start);
        std::memcpy(const_cast<uint8_t*>(metadata.data()) + length_offset,
                    &length, 4);
        ++count;
        if (metadata.size() > 16384) return {};
      }
    }
    std::memcpy(const_cast<uint8_t*>(metadata.data()), &count, 4);
    Handle<ByteArray> wire =
        isolate_->factory()->NewByteArray(static_cast<int>(metadata.size()));
    {
      DisallowGarbageCollection no_gc;
      std::memcpy(wire->begin(), metadata.data(), metadata.size());
    }
    // Allocation may move the map, so recompute its address bucket. A later
    // rooted entry refreshes all buckets if a major GC occurred here.
    base = WirePlanSlot(*map);
    slot = base;
    bool available = false;
    for (int way = 0; way < kShapeCacheWays; ++way) {
      int candidate = base + way * 2;
      if (!table->get(candidate).GetHeapObjectIfWeak(&cached_map) ||
          cached_map == *map ||
          (IsMap(cached_map) && Map::cast(cached_map)->is_deprecated())) {
        slot = candidate;
        available = true;
        break;
      }
    }
    if (!available) {
      size_t bucket = static_cast<size_t>(base / (kShapeCacheWays * 2));
      slot = base + ((wire_replacement_[bucket]++ & (kShapeCacheWays - 1)) * 2);
    }
    int bytes = Smi::ToInt(realm_cache_->get(3));
    if (table->get(slot + 1).GetHeapObjectIfStrong(&cached_wire) &&
        IsByteArray(cached_wire))
      bytes -= ByteArray::cast(cached_wire)->length();
    if (bytes + wire->length() > 8 * 1024 * 1024) return {};
    table->set(slot, MakeWeak(*map));
    table->set(slot + 1, *wire);
    realm_cache_->set(3, Smi::FromInt(bytes + wire->length()));
    return wire;
  }
  struct EncodeField {
    Handle<String> key;
    FieldIndex index;
    Handle<Object> constant;
    std::vector<uint8_t> wire;
  };
  struct EncodePlan {
    std::unique_ptr<GlobalHandleVector<Object>> roots;
    Handle<Map> map;
    std::vector<EncodeField> fields;
    int active = 0;
  };
  EncodePlan* GetPlan(Handle<JSObject> object) {
    Tagged<Map> map = object->map();
    if (map->NumberOfOwnDescriptors() > 256) return nullptr;
    // Hash lookup remains an optimization only, and identity is always
    // verified. Moving GC may change a map's slot and cause a harmless miss.
    // Collisions replace inactive plans. Nested encoding never invalidates an
    // active plan.
    size_t slot = (map.ptr() >> 4) & 63;
    EncodePlan& plan = plans_[slot];
    if (!plan.map.is_null() && *plan.map == map && plan.active == 0)
      return &plan;
    if (plan.active) return nullptr;
    // Creating a plan for every unique shape is expensive. Admit only maps
    // observed repeatedly in this encode, using a bounded rooted table.
    if (candidates_->get(static_cast<int>(slot)) != map) {
      candidates_->set(static_cast<int>(slot), map);
      candidate_hits_[slot] = 1;
      return nullptr;
    }
    if (candidate_hits_[slot] < 2) {
      ++candidate_hits_[slot];
      return nullptr;
    }
    plan.fields.clear();
    plan.roots = std::make_unique<GlobalHandleVector<Object>>(isolate_->heap());
    // Reserve once before creating handles: pushing must not move locations
    // retained by cached fields. All keys/constants are roots independent of
    // the per-property HandleScopes.
    plan.roots->Reserve(static_cast<size_t>(map->NumberOfOwnDescriptors()) * 2 +
                        1);
    auto root = [&](Tagged<Object> value) {
      size_t index = plan.roots->size();
      plan.roots->Push(value);
      return (*plan.roots)[index];
    };
    plan.map = Handle<Map>::cast(root(map));
    for (InternalIndex i : map->IterateOwnDescriptors()) {
      Tagged<DescriptorArray> descriptors = map->instance_descriptors(isolate_);
      if (!IsString(descriptors->GetKey(i))) continue;
      PropertyDetails details = descriptors->GetDetails(i);
      if (details.IsDontEnum()) continue;
      if (details.kind() != PropertyKind::kData) return nullptr;
      Handle<String> key = Handle<String>::cast(root(descriptors->GetKey(i)));
      bool in_field = details.location() == PropertyLocation::kField;
      plan.fields.push_back(
          {key,
           in_field ? FieldIndex::ForDetails(map, details) : FieldIndex(),
           in_field ? Handle<Object>() : root(descriptors->GetStrongValue(i)),
           {}});
    }
    return &plan;
  }
  Isolate* isolate_;
  ByteWriter writer_;
  msgpack::packer<ByteWriter> packer_;
  std::string* error_;
  std::vector<Handle<Object>> ancestors_;
  struct RawPlan {
    Tagged<Map> map;
    Tagged<ByteArray> wire;
    uint32_t epoch = 0;
  };
  // Raw entries are read only within the no-GC span that wrote them. Every
  // rooted fallback starts a new epoch before raw references can be reused.
  RawPlan fast_plans_[64];
  uint32_t fast_epoch_ = 0;
  Tagged<Object> fast_ancestors_[kMaxDepth];
  bool lossless_float32_;
  MessagePackAsyncData* capture_;
  EncodePlan plans_[64];
  Handle<FixedArray> candidates_;
  uint8_t candidate_hits_[64]{};
  Handle<FixedArray> realm_cache_;
  uint8_t wire_replacement_[kShapeCacheEntries / kShapeCacheWays]{};
#ifdef MSGPACK_PROFILE_CACHE
  uint32_t cache_hits_ = 0, cache_misses_ = 0;
#endif
};
#include "src/msgpack/messagepack-resource-encoder.h"
}  // namespace

void MessagePackBuffer::FreeAllocation() {
  if (mapped_)
    CHECK(base::PageAllocator().FreePages(data_, capacity_));
  else
    std::free(data_);
}
MessagePackBuffer::~MessagePackBuffer() { FreeAllocation(); }
void MessagePackBuffer::Clear() { size_ = 0; }
uint8_t* MessagePackBuffer::Release(size_t* mapping_size) {
  // Exposed buffers can outlive this call. Trim geometric growth slack before
  // transferring ownership so retained native memory tracks the payload size.
  if (mapped_ && !mapping_size) {
    uint8_t* copy = static_cast<uint8_t*>(std::malloc(size_));
    if (!copy && size_) throw std::bad_alloc();
    if (size_) std::memcpy(copy, data_, size_);
    FreeAllocation();
    data_ = copy;
    capacity_ = size_;
    mapped_ = false;
  } else if (mapped_) {
    size_t page = base::PageAllocator().AllocatePageSize();
    size_t needed = (size_ + page - 1) & ~(page - 1);
    if (needed && needed < capacity_) {
      // Return whole tail pages without allocating or copying the payload.
      if (base::PageAllocator().ReleasePages(data_, capacity_, needed)) {
        capacity_ = needed;
      }
    }
  } else if (size_ && size_ < capacity_) {
    if (void* trimmed = std::realloc(data_, size_))
      data_ = static_cast<uint8_t*>(trimmed);
  }
  uint8_t* result = data_;
  if (mapping_size) *mapping_size = mapped_ ? capacity_ : 0;
  data_ = nullptr;
  size_ = capacity_ = 0;
  mapped_ = false;
  return result;
}
uint8_t* MessagePackBuffer::AppendSlow(size_t count) {
  if (count > kMaxOutputBytes - size_)
    throw std::length_error("MessagePack output limit exceeded");
  size_t required = size_ + count;
  if (required > capacity_) {
    size_t capacity = std::max(
        required,
        std::min(kMaxOutputBytes, std::max(size_t{256}, capacity_ * 2)));
    bool pages = use_pages_ && capacity >= 32 * 1024;
    void* allocation;
    if (pages) {
      size_t page = base::PageAllocator().AllocatePageSize();
      capacity = (capacity + page - 1) & ~(page - 1);
      allocation = base::PageAllocator().AllocatePages(
          nullptr, capacity, page, v8::PageAllocator::kReadWrite);
      if (allocation) {
        if (size_) std::memcpy(allocation, data_, size_);
        FreeAllocation();
      }
    } else {
      allocation = std::realloc(data_, capacity);
    }
    if (!allocation) throw std::bad_alloc();
    data_ = static_cast<uint8_t*>(allocation);
    capacity_ = capacity;
    mapped_ = pages;
  }
  uint8_t* result = data_ + size_;
  size_ = required;
  return result;
}

MaybeHandle<Object> DecodeMessagePack(Isolate* isolate,
                                      base::Vector<const uint8_t> input,
                                      std::string* error,
                                      MessagePackDecodeMode mode,
                                      bool resource) {
  error->clear();
  if (input.empty() || input.size() > kMaxOutputBytes) {
    *error = "Invalid MessagePack input size";
    return {};
  }
  bool compact =
      resource && input.size() >= 4 &&
      std::memcmp(input.begin(), messagepack_resources::kMagic, 4) == 0;
  if (compact) {
    if (input.size() < 5 || input[4] != 1) {
      *error = "Unsupported MessagePack resource version";
      return {};
    }
    input = input.SubVector(5, input.size());
  }
  DisallowJavascriptExecution no_js(isolate);
  try {
    if (mode == MessagePackDecodeMode::kDirect) {
      Handle<FixedArray> cache;
      if (!GetMessagePackRealmCache(isolate).ToHandle(&cache)) return {};
      Handle<WeakFixedArray> shapes(
          handle(WeakFixedArray::cast(cache->get(1)), isolate));
      DirectMessagePackDecoder<false> decoder(isolate, input, error, shapes);
      return compact ? decoder.DecodeResource() : decoder.Decode();
    }
    V8Visitor visitor(isolate, input.size(), error);
    size_t offset = 0;
    if (mode == MessagePackDecodeMode::kVisitor) {
      if (!msgpack::parse(reinterpret_cast<const char*>(input.begin()),
                          input.size(), offset, visitor)) {
        if (error->empty()) *error = "MessagePack parse failed";
        return {};
      }
    } else if (mode == MessagePackDecodeMode::kMPackReader) {
      mpack_reader_t reader;
      mpack_reader_init_data(
          &reader, reinterpret_cast<const char*>(input.begin()), input.size());
      bool ok = ReadMPack(&reader, &visitor);
      if (ok && mpack_reader_remaining(&reader, nullptr) != 0)
        ok = visitor.Fail("Trailing bytes after MessagePack value");
      if (!ok) mpack_reader_flag_error(&reader, mpack_error_data);
      mpack_error_t status = mpack_reader_destroy(&reader);
      if (!ok || status != mpack_ok) {
        if (error->empty()) *error = "MPack reader failed";
        return {};
      }
      offset = input.size();
    } else {
      auto tree = msgpack::unpack(
          reinterpret_cast<const char*>(input.begin()), input.size(), offset,
          nullptr, nullptr,
          msgpack::unpack_limit(input.size(), input.size() / 2, input.size(),
                                input.size(), input.size(), kMaxDepth));
      if (!VisitTree(tree.get(), &visitor)) return {};
    }
    if (offset != input.size()) {
      *error = "Trailing bytes after MessagePack value";
      return {};
    }
    return visitor.result();
  } catch (const std::bad_alloc&) {
    *error = "MessagePack native allocation failed";
    return {};
  } catch (const std::exception& e) {
    *error = e.what();
    return {};
  } catch (...) {
    *error = "Native MessagePack exception";
    return {};
  }
}

bool EncodeMessagePack(Isolate* isolate, Handle<Object> value,
                       std::vector<uint8_t>* output, std::string* error,
                       bool lossless_float32) {
  MessagePackBuffer buffer;
  output->clear();
  if (!EncodeMessagePack(isolate, value, &buffer, error, lossless_float32))
    return false;
  try {
    output->assign(buffer.data(), buffer.data() + buffer.size());
    return true;
  } catch (const std::bad_alloc&) {
    *error = "MessagePack native allocation failed";
    output->clear();
    return false;
  } catch (const std::exception& e) {
    *error = e.what();
    output->clear();
    return false;
  }
}

bool EncodeMessagePack(Isolate* isolate, Handle<Object> value,
                       MessagePackBuffer* output, std::string* error,
                       bool lossless_float32) {
  error->clear();
  output->Clear();
  DisallowJavascriptExecution no_js(isolate);
  try {
    Encoder<false> encoder(isolate, output, error, lossless_float32);
    encoder.Prepare(value);
    bool ok = encoder.Value(value);
    if (ok)
      encoder.RememberSize(value);
    else
      output->Clear();
    return ok;
  } catch (const std::bad_alloc&) {
    *error = "MessagePack native allocation failed";
    output->Clear();
    return false;
  } catch (const std::exception& e) {
    *error = e.what();
    output->Clear();
    return false;
  } catch (...) {
    *error = "Native MessagePack exception";
    output->Clear();
    return false;
  }
}
bool BuildMessagePackSlice(Isolate* isolate, MessagePackAsyncData* data,
                           PersistentHandles* handles, Handle<Object> result,
                           uint32_t* cursor,
                           std::vector<MessagePackBuildFrame>* frames) {
  DisallowJavascriptExecution no_js(isolate);
  try {
    Handle<FixedArray> cache;
    if (!GetMessagePackRealmCache(isolate).ToHandle(&cache)) return true;
    Handle<WeakFixedArray> shapes(
        handle(WeakFixedArray::cast(cache->get(1)), isolate));
    // Decoder key/map caches hold handles allocated throughout this slice.
    // Keep them in the caller's slice scope, rather than iteration scopes.
    DirectMessagePackDecoder<true> decoder(
        isolate,
        base::Vector<const uint8_t>(data->input.data(), data->input.size()),
        &data->error, shapes, data);
    auto emit = [&](Handle<Object> value) {
      if (frames->empty()) {
        result.PatchValue(*value);
        return true;
      }
      auto& frame = frames->back();
      if (frame.map) {
        PropertyKey key(isolate, Handle<Name>::cast(frame.key));
        LookupIterator lookup(isolate, frame.object, key, LookupIterator::OWN);
        if (JSObject::DefineOwnPropertyIgnoreAttributes(&lookup, value, NONE)
                .is_null())
          return false;
        frame.key.PatchValue(*isolate->factory()->empty_string());
      } else {
        auto array = Handle<JSArray>::cast(frame.object);
        DCHECK(array->HasObjectElements());
        FixedArray::cast(array->elements())->set(frame.index++, *value);
      }
      return true;
    };
    const auto deadline =
        base::TimeTicks::Now() + base::TimeDelta::FromMilliseconds(2);
    size_t next_check = 0;
    for (size_t work = 0; work < 65536; ++work) {
      if (work >= next_check) {
        next_check = work + 128;
        StackLimitCheck check(isolate);
        if (check.InterruptRequested())
          USE(isolate->stack_guard()->HandleInterrupts());
        if (isolate->has_exception()) return true;
        if (work && base::TimeTicks::Now() >= deadline) return false;
      }
      if (!frames->empty() && *cursor == frames->back().end) {
        auto frame = frames->back();
        if (!frame.map)
          JSArray::cast(*frame.object)->set_length(Smi::FromInt(frame.index));
        frames->pop_back();
        if (!emit(frame.object)) return true;
        if (!frames->empty() && frames->back().map) ++frames->back().index;
        Handle<Object> released = frame.object;
        released.PatchValue(*isolate->factory()->undefined_value());
        if (!frame.key.is_null())
          frame.key.PatchValue(*isolate->factory()->empty_string());
        continue;
      }
      if (*cursor == data->tokens.size()) return true;
      auto* parent = frames->empty() ? nullptr : &frames->back();
      const auto& token = data->tokens[*cursor];
      if (parent && parent->map && !(parent->index & 1)) {
        Handle<Object> key =
            decoder.DecodeValue(token.offset, frames->size(), {});
        if (key.is_null()) return true;
        auto string =
            isolate->factory()->InternalizeString(Handle<String>::cast(key));
        parent->key.PatchValue(*string);
        ++parent->index;
        *cursor = token.next;
        continue;
      }
      if (parent && !parent->map &&
          (JSArray::cast(*parent->object)->HasDoubleElements() ||
           JSArray::cast(*parent->object)->HasSmiElements())) {
        DisallowGarbageCollection no_gc;
        Tagged<JSArray> array = JSArray::cast(*parent->object);
        bool doubles = array->HasDoubleElements();
        Tagged<FixedArrayBase> storage = array->elements();
        size_t count = 0;
        while (*cursor < parent->end && count < 128) {
          const auto& number = data->tokens[*cursor];
          DCHECK_EQ(number.kind, MessagePackToken::kNumber);
          if (doubles)
            FixedDoubleArray::cast(storage)->set(parent->index++,
                                                 number.number);
          else
            FixedArray::cast(storage)->set(
                parent->index++, Smi::FromInt(static_cast<int>(number.number)),
                SKIP_WRITE_BARRIER);
          *cursor = number.next;
          ++count;
        }
        work += count - 1;
        continue;
      }
      Handle<String> role =
          parent && parent->map ? parent->key : Handle<String>();
      bool container = token.kind == MessagePackToken::kArray ||
                       token.kind == MessagePackToken::kMap;
      if (container && (token.next - *cursor > 256 || frames->size() > 16)) {
        bool map = token.kind == MessagePackToken::kMap;
        Handle<JSObject> object;
        if (map)
          object = isolate->factory()->NewJSObject(isolate->object_function());
        else
          object = isolate->factory()->NewJSArray(
              token.container.all_smis      ? PACKED_SMI_ELEMENTS
              : token.container.all_numbers ? PACKED_DOUBLE_ELEMENTS
                                            : PACKED_ELEMENTS,
              0, token.container.count,
              ArrayStorageAllocationMode::INITIALIZE_ARRAY_ELEMENTS_WITH_HOLE);
        // A partial array is rooted across GC and task boundaries. Its length
        // stays zero and every unused slot is a valid hole until completion.
        MessagePackBuildFrame frame{
            token.next, 0, handles->NewHandle(object), {}, map};
        if (map)
          frame.key = handles->NewHandle(isolate->factory()->empty_string());
        frames->push_back(frame);
        ++*cursor;
        continue;
      }
      Handle<Object> value =
          decoder.DecodeValue(token.offset, frames->size(), role);
      if (value.is_null()) return true;
      work += token.next - *cursor - 1;
      *cursor = token.next;
      if (!emit(value)) return true;
      if (!frames->empty() && frames->back().map) ++frames->back().index;
    }
    return false;
  } catch (const std::bad_alloc&) {
    data->error = "MessagePack native allocation failed";
  } catch (const std::exception& e) {
    data->error = e.what();
  } catch (...) {
    data->error = "Native MessagePack exception";
  }
  return true;
}

bool CaptureMessagePack(Isolate* isolate, Handle<Object> value,
                        MessagePackAsyncData* data) {
  DisallowJavascriptExecution no_js(isolate);
  try {
    Encoder<true> encoder(isolate, &data->input, &data->error, true, data);
    encoder.Prepare(value);
    if (!encoder.Value(value)) return false;
    // A final run of inline scalars can grow the wire buffer after the last
    // deferred part checked the combined capture budget.
    if (data->MemoryUsage() > MessagePackAsyncData::kNativeLimit)
      throw std::length_error("MessagePack async native memory limit exceeded");
    encoder.RememberCaptureSize(value);
    return true;
  } catch (const std::bad_alloc&) {
    data->error = "MessagePack native allocation failed";
  } catch (const std::exception& e) {
    data->error = e.what();
  } catch (...) {
    data->error = "Native MessagePack exception";
  }
  return false;
}

bool EncodeMessagePackResource(Isolate* isolate, Handle<Object> value,
                               MessagePackBuffer* output, std::string* error) {
  if (!EncodeMessagePack(isolate, value, output, error, true)) return false;
  try {
    size_t offset = 0;
    auto tree = msgpack::unpack(reinterpret_cast<const char*>(output->data()),
                                output->size(), offset);
    MessagePackBuffer compact;
    ResourceEncoder encoder(&compact);
    encoder.Encode(tree.get());
    if (compact.size() < output->size()) {
      output->Clear();
      std::memcpy(output->Append(compact.size()), compact.data(),
                  compact.size());
    }
    return true;
  } catch (const std::length_error&) {
    // An unprofitable compact candidate can exceed the resource byte limit
    // while the validated standard value still fits. Keep those exact bytes.
    return true;
  } catch (const std::bad_alloc&) {
    *error = "MessagePack native allocation failed";
  } catch (const std::exception& e) {
    *error = e.what();
  } catch (...) {
    *error = "Native MessagePack resource exception";
  }
  output->Clear();
  return false;
}
}  // namespace internal
}  // namespace v8
