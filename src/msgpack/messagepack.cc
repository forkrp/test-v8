// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// This experimental adapter is compiled with exceptions enabled, and catches
// all dependency exceptions at the boundary. Other V8 sources keep
// -fno-exceptions.
#define MSGPACK_NO_BOOST
#include "src/msgpack/messagepack.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <msgpack.hpp>

#include "mpack.h"
#include "src/api/api-inl.h"
#include "src/common/assert-scope.h"
#include "src/msgpack/messagepack-string.h"
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

using messagepack_strings::DecodeUtf8;
using messagepack_strings::ScanUtf8;
using messagepack_strings::Utf8Info;

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
    if (size) std::memcpy(&first, data, size);
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

struct Property {
  Handle<String> key;
  Handle<Object> value;
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
    bool internalize = key || size <= 10;
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
    {
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
  explicit ByteWriter(std::vector<uint8_t>* output) : output_(output) {}
  void write(const char* bytes, size_t n) {
    if (n > kMaxOutputBytes - output_->size())
      throw std::length_error("MessagePack output limit exceeded");
    output_->insert(output_->end(), reinterpret_cast<const uint8_t*>(bytes),
                    reinterpret_cast<const uint8_t*>(bytes) + n);
  }
  uint8_t* Append(size_t n) {
    if (n > kMaxOutputBytes - output_->size())
      throw std::length_error("MessagePack output limit exceeded");
    size_t start = output_->size();
    output_->resize(start + n);
    return output_->data() + start;
  }

 private:
  std::vector<uint8_t>* output_;
};

class Encoder {
 public:
  Encoder(Isolate* isolate, std::vector<uint8_t>* output, std::string* error,
          bool lossless_float32)
      : isolate_(isolate),
        writer_(output),
        packer_(writer_),
        error_(error),
        lossless_float32_(lossless_float32) {}
  bool Fail(const char* s) {
    *error_ = s;
    return false;
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
  bool StringValue(Handle<String> string) {
    string = String::Flatten(isolate_, string);
    DisallowGarbageCollection no_gc;
    String::FlatContent flat = string->GetFlatContent(no_gc);
    if (flat.IsOneByte()) {
      auto bytes = flat.ToOneByteVector();
      if (String::IsAscii(bytes.begin(), bytes.length())) {
        packer_.pack_str(bytes.length());
        packer_.pack_str_body(reinterpret_cast<const char*>(bytes.begin()),
                              bytes.length());
        return true;
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
    if (messagepack_strings::UseSimdForEncoding(chars, count))
      return WriteStringImpl<Char, true>(chars, count);
    return WriteStringImpl<Char, false>(chars, count);
  }
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
    if (IsSmi(*value) || IsHeapNumber(*value))
      return Number(Object::Number(*value));
    if (IsNull(*value, isolate_)) {
      packer_.pack_nil();
      return true;
    }
    if (IsBoolean(*value, isolate_)) {
      if (IsTrue(*value, isolate_))
        packer_.pack_true();
      else
        packer_.pack_false();
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
      packer_.pack_array(length);
      ElementsKind kind = array->GetElementsKind();
      if (length > 0 && kind == PACKED_DOUBLE_ELEMENTS) {
        Handle<FixedDoubleArray> storage(
            FixedDoubleArray::cast(array->elements()), isolate_);
        for (uint32_t i = 0; i < length && ok; ++i)
          ok = Number(storage->get_scalar(i));
      } else if (length > 0 &&
                 (kind == PACKED_SMI_ELEMENTS || kind == PACKED_ELEMENTS)) {
        Handle<FixedArray> storage(FixedArray::cast(array->elements()),
                                   isolate_);
        for (uint32_t i = 0; i < length && ok; ++i) {
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
      if (ok) packer_.pack_map(count);
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
        ok = StringValue(key) && Value(field, depth + 1);
      }
    } else {
      Handle<FixedArray> keys;
      if (!KeyAccumulator::GetKeys(
               isolate_, object, KeyCollectionMode::kOwnOnly,
               ENUMERABLE_STRINGS, GetKeysConversion::kConvertToString)
               .ToHandle(&keys))
        return false;
      packer_.pack_map(keys->length());
      for (int i = 0; i < keys->length() && ok; ++i) {
        HandleScope scope(isolate_);
        Handle<String> key(handle(String::cast(keys->get(i)), isolate_));
        PropertyKey property_key(isolate_, Handle<Name>::cast(key));
        LookupIterator lookup(isolate_, object, property_key,
                              LookupIterator::OWN);
        if (lookup.state() != LookupIterator::DATA)
          ok = Fail("Accessors are unsupported");
        else
          ok = StringValue(key) && Value(lookup.GetDataValue(), depth + 1);
      }
    }
    ancestors_.pop_back();
    return ok;
  }

 private:
  Isolate* isolate_;
  ByteWriter writer_;
  msgpack::packer<ByteWriter> packer_;
  std::string* error_;
  std::vector<Handle<Object>> ancestors_;
  bool lossless_float32_;
};
}  // namespace

MaybeHandle<Object> DecodeMessagePack(Isolate* isolate,
                                      base::Vector<const uint8_t> input,
                                      std::string* error,
                                      MessagePackDecodeMode mode) {
  error->clear();
  if (input.empty() || input.size() > kMaxOutputBytes) {
    *error = "Invalid MessagePack input size";
    return {};
  }
  DisallowJavascriptExecution no_js(isolate);
  try {
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
  error->clear();
  output->clear();
  DisallowJavascriptExecution no_js(isolate);
  try {
    Encoder encoder(isolate, output, error, lossless_float32);
    bool ok = encoder.Value(value);
    if (!ok) output->clear();
    return ok;
  } catch (const std::exception& e) {
    *error = e.what();
    output->clear();
    return false;
  } catch (...) {
    *error = "Native MessagePack exception";
    output->clear();
    return false;
  }
}
}  // namespace internal
}  // namespace v8
