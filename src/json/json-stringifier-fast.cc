// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Backported from e368b713b918f6a5ad0cf6db46c4a72c2f375b32; see
// docs/json-upstream-backport.md for the 12.6 compatibility boundary.

#include <optional>
#include <string_view>
#include <unordered_set>

#include "absl/functional/overload.h"
#include "hwy/highway.h"
#include "src/base/memory.h"
#include "src/base/strings.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/common/message-template.h"
#include "src/execution/protectors-inl.h"
#include "src/json/json-stringifier-capture.h"
#include "src/json/json-stringifier-escape.h"
#include "src/json/json-stringifier.h"
#include "src/numbers/conversions.h"
#include "src/objects/elements-kind.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-raw-json-inl.h"
#include "src/objects/lookup.h"
#include "src/objects/objects-inl.h"
#include "src/objects/oddball-inl.h"
#include "src/objects/ordered-hash-table.h"
#include "src/objects/smi.h"
#include "src/objects/tagged.h"
#include "src/strings/string-builder-inl.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {
namespace {
static constexpr char kJsonStringifierZoneName[] = "json-stringifier-zone";
// Local adapters keep the 14.4 algorithm independent of unrelated API churn.
template <typename Visitor>
auto DispatchString(Tagged<String> string, Visitor&& visitor) {
  switch (StringShape(string).representation_and_encoding_tag()) {
#define CASE(tag, Type) \
  case tag:             \
    return visitor(Type::cast(string));
    CASE(kSeqStringTag | kOneByteStringTag, SeqOneByteString)
    CASE(kSeqStringTag | kTwoByteStringTag, SeqTwoByteString)
    CASE(kExternalStringTag | kOneByteStringTag, ExternalOneByteString)
    CASE(kExternalStringTag | kTwoByteStringTag, ExternalTwoByteString)
    case kConsStringTag | kOneByteStringTag:
      CASE(kConsStringTag | kTwoByteStringTag, ConsString)
    case kSlicedStringTag | kOneByteStringTag:
      CASE(kSlicedStringTag | kTwoByteStringTag, SlicedString)
    case kThinStringTag | kOneByteStringTag:
      CASE(kThinStringTag | kTwoByteStringTag, ThinString)
#undef CASE
    default:
      UNREACHABLE();
  }
}

constexpr int kJsonEscapeTableEntrySize = 8;

// Translation table to escape Latin1 characters.
// Table entries start at a multiple of 8 and are null-terminated.
constexpr const char* const JsonEscapeTable =
    "\\u0000\0 \\u0001\0 \\u0002\0 \\u0003\0 "
    "\\u0004\0 \\u0005\0 \\u0006\0 \\u0007\0 "
    "\\b\0     \\t\0     \\n\0     \\u000b\0 "
    "\\f\0     \\r\0     \\u000e\0 \\u000f\0 "
    "\\u0010\0 \\u0011\0 \\u0012\0 \\u0013\0 "
    "\\u0014\0 \\u0015\0 \\u0016\0 \\u0017\0 "
    "\\u0018\0 \\u0019\0 \\u001a\0 \\u001b\0 "
    "\\u001c\0 \\u001d\0 \\u001e\0 \\u001f\0 "
    " \0      !\0      \\\"\0     #\0      "
    "$\0      %\0      &\0      '\0      "
    "(\0      )\0      *\0      +\0      "
    ",\0      -\0      .\0      /\0      "
    "0\0      1\0      2\0      3\0      "
    "4\0      5\0      6\0      7\0      "
    "8\0      9\0      :\0      ;\0      "
    "<\0      =\0      >\0      ?\0      "
    "@\0      A\0      B\0      C\0      "
    "D\0      E\0      F\0      G\0      "
    "H\0      I\0      J\0      K\0      "
    "L\0      M\0      N\0      O\0      "
    "P\0      Q\0      R\0      S\0      "
    "T\0      U\0      V\0      W\0      "
    "X\0      Y\0      Z\0      [\0      "
    "\\\\\0     ]\0      ^\0      _\0      ";

// LINT.IfChange(StringDoesNotContainEscapeCharacters)
constexpr bool JsonDoNotEscapeFlagTable[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

template <typename Char>
constexpr bool DoNotEscape(Char c);

template <>
constexpr bool DoNotEscape(uint8_t c) {
  // https://tc39.github.io/ecma262/#table-json-single-character-escapes
  return JsonDoNotEscapeFlagTable[c];
}

// Checks if characters need escaping in a packed input (4 bytes in uint32_t).
constexpr bool NeedsEscape(uint32_t input) {
  constexpr uint32_t mask_0x20 = 0x20202020u;
  constexpr uint32_t mask_0x22 = 0x22222222u;
  constexpr uint32_t mask_0x5c = 0x5C5C5C5Cu;
  constexpr uint32_t mask_0x01 = 0x01010101u;
  constexpr uint32_t mask_msb = 0x80808080u;
  // Escape control characters (< 0x20).
  const uint32_t has_lt_0x20 = input - mask_0x20;
  // Escape double quotation mark (0x22).
  const uint32_t has_0x22 = (input ^ mask_0x22) - mask_0x01;
  // Escape backslash (0x5C).
  const uint32_t has_0x5c = (input ^ mask_0x5c) - mask_0x01;
  // Chars >= 0x7F don't need escaping.
  const uint32_t result_mask = ~input & mask_msb;
  const uint32_t result = ((has_lt_0x20 | has_0x22 | has_0x5c) & result_mask);
  return result != 0;
}
// LINT.ThenChange(/src/objects/string.cc:StringDoesNotContainEscapeCharacters)

template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(uint8_t))
bool DoNotEscape(const SrcChar* chars, size_t length,
                 const DisallowGarbageCollection& no_gc) {
  bool no_escape = true;
  using PackedT = uint32_t;
  static constexpr size_t stride = sizeof(PackedT);
  size_t i = 0;
  for (; i + (stride - 1) < length; i += stride) {
    PackedT packed =
        base::ReadUnalignedValue<PackedT>(reinterpret_cast<Address>(chars + i));
    if (V8_UNLIKELY(NeedsEscape(packed))) break;
  }
  for (; i < length; i++) {
    no_escape = no_escape && DoNotEscape(chars[i]);
  }
  return no_escape;
}

bool IsFastKey(Tagged<String> key, const DisallowGarbageCollection& no_gc) {
  return DispatchString(
      key, absl::Overload{[&](Tagged<SeqOneByteString> str) {
                            const uint8_t* chars = str->GetChars(no_gc);
                            return DoNotEscape(chars, str->length(), no_gc);
                          },
                          [&](Tagged<ExternalOneByteString> str) {
                            const uint8_t* chars = str->GetChars();
                            return DoNotEscape(chars, str->length(), no_gc);
                          },
                          [&](Tagged<String> str) { return false; }});
}

bool MayHaveInterestingProperties(Isolate* isolate, Tagged<JSReceiver> object) {
  for (PrototypeIterator iter(isolate, object, kStartAtReceiver);
       !iter.IsAtEnd(); iter.Advance()) {
    if (iter.GetCurrent()->map()->may_have_interesting_properties()) {
      return true;
    }
  }
  return false;
}

template <typename Char>
class OutBuffer {
 public:
  explicit OutBuffer(AccountingAllocator* allocator) : allocator_(allocator) {
    cur_ = stack_buffer_;
    segment_end_ = cur_ + kStackBufferSize;
  }
  template <typename SrcChar>
    requires(sizeof(Char) >= sizeof(SrcChar))
  V8_INLINE void AppendCharacter(SrcChar c) {
    ReduceCurrentCapacity(1);
    DCHECK_GE(SegmentFreeChars(), 1);
    *cur_++ = c;
  }
  template <typename SrcChar>
    requires(sizeof(Char) >= sizeof(SrcChar))
  void Append(const SrcChar* chars, size_t length) {
    ReduceCurrentCapacity(length);
    DCHECK_GE(SegmentFreeChars(), length);
    CopyChars(cur_, chars, length);
    cur_ += length;
  }
  template <size_t N, typename SrcChar>
  void AppendPrefix(const SrcChar* chars, size_t length) {
    // Like the legacy NoExtendBuilder::AppendChars: copy a fixed-width span,
    // but commit only the ordinary prefix. The next write replaces the rest.
    DCHECK_LE(length, N);
    DCHECK_GE(SegmentFreeChars(), N);
    CopyChars(cur_, chars, N);
    ReduceCurrentCapacity(length);
    cur_ += length;
  }
  void EnsureCapacity(size_t size) {
#ifdef DEBUG
    current_requested_capacity_ = size;
#endif
    if (V8_LIKELY(size <= SegmentFreeChars())) return;
    Extend(size);
    DCHECK_GE(CurSegmentCapacity(), size);
  }
  template <typename SrcChar>
  bool AppendEscaped(base::Vector<const SrcChar> chars) {
    JsonStringSpanWriter<Char> writer{cur_};
    bool escaped = WriteJsonString<false>(chars, &writer);
    size_t written = writer.cursor - cur_;
    DCHECK_LE(written, SegmentFreeChars());
    ReduceCurrentCapacity(written);
    cur_ = writer.cursor;
    return escaped;
  }
  size_t length() const {
    if (ZoneUsed()) {
      DCHECK_GT(segments_->length(), 0);
      size_t length = stack_buffer_size_;
      for (int i = 0; i < segments_->length() - 1; i++) {
        length += segments_->at(i).size();
      }
      length += CurSegmentLength();
      return length;
    } else {
      return StackBufferLength();
    }
  }
  template <typename Dst>
  void CopyTo(Dst* dst) {
    if (ZoneUsed()) {
      // Copy stack segment.
      CopyChars(dst, stack_buffer_, stack_buffer_size_);
      dst += stack_buffer_size_;
      // Copy full segments.
      DCHECK_GT(segments_->length(), 0);
      for (int i = 0; i < segments_->length() - 1; i++) {
        base::Vector<Char> segment = segments_.value()[i];
        size_t segment_length = segment.size();
        CopyChars(dst, segment.begin(), segment_length);
        dst += segment_length;
      }
      // Copy last (partially filled) segment.
      base::Vector<Char> segment = segments_->last();
      CopyChars(dst, segment.begin(), CurSegmentLength());
    } else {
      // Copy (partially filled) stack segment.
      CopyChars(dst, stack_buffer_, StackBufferLength());
    }
  }

 private:
  static constexpr uint32_t kInitialSegmentSize = 2 * KB;
  static constexpr uint32_t kMaxSegmentSize = 256 * KB;
  static_assert(base::bits::IsPowerOfTwo(kInitialSegmentSize));
  static_assert(base::bits::IsPowerOfTwo(kMaxSegmentSize));
  static constexpr uint8_t kInitialSegmentSizeHighestBit =
      kBitsPerInt - base::bits::CountLeadingZeros32(kInitialSegmentSize) - 1;
  static constexpr uint8_t kMaxSegmentSizeHighestBit =
      kBitsPerInt - base::bits::CountLeadingZeros32(kMaxSegmentSize) - 1;
  static constexpr uint8_t kNumVariableSegments =
      kMaxSegmentSizeHighestBit - kInitialSegmentSizeHighestBit;
  static constexpr size_t kStackBufferSize = 256;

  V8_NOINLINE void Extend(size_t min_size) {
    if (ZoneUsed()) {
      segments_->last().Truncate(CurSegmentLength());
    } else {
      stack_buffer_size_ = StackBufferLength();
      zone_.emplace(allocator_, kJsonStringifierZoneName);
      segments_.emplace(1, &zone_.value());
    }
    const size_t new_segment_size =
        std::max(min_size, SegmentCapacity(segments_->length()));
    segments_->Add(zone_->AllocateVector<Char>(new_segment_size),
                   &zone_.value());
    cur_ = segments_->last().begin();
    segment_end_ = segments_->last().end();
  }
  V8_INLINE size_t SegmentFreeChars() const { return segment_end_ - cur_; }
  V8_INLINE size_t StackBufferLength() const {
    DCHECK(!ZoneUsed());
    return cur_ - stack_buffer_;
  }
  V8_INLINE size_t CurSegmentLength() const {
    DCHECK(ZoneUsed());
    return cur_ - segments_->last().begin();
  }
  V8_INLINE size_t SegmentCapacity(size_t segment) {
    return 1u << std::min<size_t>(segment + kInitialSegmentSizeHighestBit,
                                  kMaxSegmentSizeHighestBit);
  }
  V8_INLINE size_t CurSegmentCapacity() {
    DCHECK(ZoneUsed());
    DCHECK_GT(segments_->length(), 0);
    return segments_->last().size();
  }
  V8_INLINE void ReduceCurrentCapacity(size_t size) {
#ifdef DEBUG
    DCHECK_LE(size, current_requested_capacity_);
    current_requested_capacity_ -= size;
#endif
  }
  V8_INLINE bool ZoneUsed() const { return zone_.has_value(); }

  AccountingAllocator* allocator_;
  Char stack_buffer_[kStackBufferSize];
  size_t stack_buffer_size_;
  Char* cur_;
  Char* segment_end_;
  std::optional<Zone> zone_;
  std::optional<ZoneList<base::Vector<Char>>> segments_;
#ifdef DEBUG
  size_t current_requested_capacity_;
#endif
};

enum FastJsonStringifierResult {
  SUCCESS,
  JS_OBJECT,
  JS_ARRAY,
  UNDEFINED,
  CHANGE_ENCODING,
  SLOW_PATH,
  EXCEPTION
};

enum class FastJsonStringifierObjectKeyResult : uint8_t {
  kSuccess,
  kChangeEncoding,  // Two-byte key in one-byte stringifier.
  kSlow  // Two-byte key (in two-byte stringifier) or requires escaping.
};

class ContinuationRecord {
 public:
  enum Type {
    kObject,
    kArray,
    kObjectResume_FastIterable,
    kObjectResume_SlowIterable,
    kObjectResume_Uninitialized,
    kArrayResume,
    kArrayResume_Holey,
    kArrayResume_WithInterrupts,
    kArrayResume_Holey_WithInterrupts,
    kSimpleObject,  // Resume after encoding change.
                    // Object is any simple object that can be serialized
                    // successfully with TrySerializeSimpleObject().
    kObjectKey      // For encoding changes triggered by object keys.
                    // This is required (instead of simply resuming with the
                    // object/index that triggered the change), to avoid
                    // re-serializing '{' if the first property key triggered
                    // the change.
  };

  using ObjectT = Object;

  static constexpr ContinuationRecord ForSimpleObject(Tagged<Object> obj) {
    return ContinuationRecord(Type::kSimpleObject, obj, 0, 0);
  }
  static constexpr ContinuationRecord ForJSAny(
      Tagged<Object> obj, FastJsonStringifierResult result) {
    return ContinuationRecord(ContinuationTypeFromResult(result), obj, 0, 0);
  }
  static constexpr ContinuationRecord ForJSArray(Tagged<Object> obj) {
    DCHECK(IsJSArray(obj));
    return ContinuationRecord(Type::kArray, obj, 0, 0);
  }
  template <ElementsKind kind, bool with_interrupt_check>
  static constexpr ContinuationRecord ForJSArrayResume(
      Tagged<FixedArrayBase> obj, uint32_t index, uint32_t length) {
    return ContinuationRecord(
        ContinuationTypeForArray(kind, with_interrupt_check), obj, index,
        length);
  }
  static constexpr ContinuationRecord ForJSObject(Tagged<Object> obj) {
    DCHECK(IsJSObject(obj));
    return ContinuationRecord(Type::kObject, obj, 0, 0, 0, 0,
                              Tagged<DescriptorArray>());
  }
  template <DescriptorArray::FastIterableState fast_iterable_state>
  static constexpr ContinuationRecord ForJSObjectResume(
      Tagged<Object> obj, uint16_t descriptor_idx, uint16_t nof_descriptors,
      uint8_t in_object_properties, uint8_t in_object_properties_start,
      Tagged<DescriptorArray> descriptors) {
    DCHECK(IsJSObject(obj));
    Type type;
    using enum DescriptorArray::FastIterableState;
    switch (fast_iterable_state) {
      case kJsonFast:
        type = Type::kObjectResume_FastIterable;
        break;
      case kJsonSlow:
        type = Type::kObjectResume_SlowIterable;
        break;
      case kUnknown:
        type = Type::kObjectResume_Uninitialized;
        break;
    }
    return ContinuationRecord(type, obj, descriptor_idx, nof_descriptors,
                              in_object_properties, in_object_properties_start,
                              descriptors);
  }
  static constexpr ContinuationRecord ForObjectKey(Tagged<String> key,
                                                   bool comma) {
    return ContinuationRecord(Type::kObjectKey, key, comma);
  }

  Type type() const { return type_; }
  Tagged<ObjectT> object() const { return object_; }
  Tagged<Object> simple_object() const {
    DCHECK_EQ(type(), Type::kSimpleObject);
    return Object::cast(object_);
  }
  Tagged<JSArray> js_array() const {
    DCHECK_EQ(type(), Type::kArray);
    return JSArray::cast(object_);
  }
  Tagged<FixedArrayBase> array_elements() const {
    DCHECK(IsArrayResumeType(type()));
    return FixedArrayBase::cast(object_);
  }
  Tagged<JSObject> js_object() const {
    DCHECK(type() == Type::kObject || IsObjectResumeType(type()));
    return JSObject::cast(object_);
  }
  Tagged<String> object_key() const {
    DCHECK_EQ(type(), Type::kObjectKey);
    return String::cast(object_);
  }

  uint32_t array_index() const {
    DCHECK(IsArrayResumeType(type()));
    return js_array_.index;
  }
  uint32_t array_length() const {
    DCHECK(IsArrayResumeType(type()));
    return js_array_.length;
  }

  uint16_t object_descriptor_idx() const {
    DCHECK(IsObjectResumeType(type()));
    return js_object_.descriptor_idx;
  }
  uint16_t object_nof_descriptors() const {
    DCHECK(IsObjectResumeType(type()));
    return js_object_.nof_descriptors;
  }
  uint8_t object_in_object_properties() const {
    DCHECK(IsObjectResumeType(type()));
    return js_object_.in_object_properties;
  }
  uint8_t object_in_object_properties_start() const {
    DCHECK(IsObjectResumeType(type()));
    return js_object_.in_object_properties_start;
  }
  Tagged<DescriptorArray> object_descriptors() const {
    DCHECK(IsObjectResumeType(type()));
    return js_object_.descriptors;
  }

  bool object_key_comma() const {
    DCHECK_EQ(type(), Type::kObjectKey);
    return object_key_.comma;
  }

  static constexpr bool IsObjectResumeType(Type type) {
    return type == Type::kObjectResume_FastIterable ||
           type == Type::kObjectResume_SlowIterable ||
           type == Type::kObjectResume_Uninitialized;
  }
  static constexpr bool IsArrayResumeType(Type type) {
    return type == Type::kArrayResume || type == Type::kArrayResume_Holey ||
           type == Type::kArrayResume_WithInterrupts ||
           type == Type::kArrayResume_Holey_WithInterrupts;
  }

 private:
  constexpr ContinuationRecord(Type type, Tagged<ObjectT> obj, uint32_t index,
                               uint32_t length)
      : type_(type), object_(obj), js_array_({index, length}) {}
  constexpr ContinuationRecord(Type type, Tagged<ObjectT> obj,
                               uint16_t descriptor_idx,
                               uint16_t nof_descriptors,
                               uint8_t in_object_properties,
                               uint8_t in_object_properties_start,
                               Tagged<DescriptorArray> descriptors)
      : type_(type),
        object_(obj),
        js_object_({descriptor_idx, nof_descriptors, in_object_properties,
                    in_object_properties_start, descriptors}) {}
  constexpr ContinuationRecord(Type type, Tagged<ObjectT> obj, bool comma)
      : type_(type), object_(obj), object_key_{comma} {}

  static constexpr Type ContinuationTypeFromResult(
      FastJsonStringifierResult result) {
    DCHECK(result == JS_OBJECT || result == JS_ARRAY);
    static_assert(JS_OBJECT - 1 == Type::kObject);
    static_assert(JS_ARRAY - 1 == Type::kArray);
    return static_cast<Type>(result - 1);
  }

  static consteval Type ContinuationTypeForArray(ElementsKind kind,
                                                 bool with_interrupt_check) {
    DCHECK(IsObjectElementsKind(kind));
    if (IsHoleyElementsKind(kind)) {
      if (with_interrupt_check) {
        return kArrayResume_Holey_WithInterrupts;
      } else {
        return kArrayResume_Holey;
      }
    } else {
      if (with_interrupt_check) {
        return kArrayResume_WithInterrupts;
      } else {
        return kArrayResume;
      }
    }
  }

  Type type_;
  Tagged<ObjectT> object_;
  union {
    struct {
      uint32_t index;
      uint32_t length;
    } js_array_;
    struct {
      uint16_t descriptor_idx;
      uint16_t nof_descriptors;
      uint8_t in_object_properties;
      uint8_t in_object_properties_start;
      Tagged<DescriptorArray> descriptors;
    } js_object_;
    struct {
      bool comma;
    } object_key_;
  };
};

// Only punctuation and small integer literals enter this buffer. Strings and
// doubles are copied as native semantic parts, never formatted on the caller.
class CaptureBuffer {
 public:
  explicit CaptureBuffer(JsonStringifyCapture* capture) : capture_(capture) {}
  void Flush() {
    capture_->Literal(base::Vector<const uint8_t>(chars_, size_));
    size_ = 0;
  }
  void EnsureCapacity(size_t size) {
    if (size > sizeof(chars_) - size_) Flush();
  }
  template <typename Char>
  void AppendCharacter(Char c) {
    EnsureCapacity(1);
    chars_[size_++] = static_cast<uint8_t>(c);
  }
  template <typename Char>
  void Append(const Char* chars, size_t length) {
    EnsureCapacity(length);
    if (length > sizeof(chars_)) {
      capture_->Literal(base::Vector<const Char>(chars, length));
    } else {
      for (size_t i = 0; i < length; ++i) {
        chars_[size_++] = static_cast<uint8_t>(chars[i]);
      }
    }
  }

 private:
  JsonStringifyCapture* const capture_;
  uint8_t chars_[256];
  size_t size_ = 0;
};

template <typename Char, bool capture_mode = false>
class FastJsonStringifier {
 public:
  explicit FastJsonStringifier(Isolate* isolate);
  FastJsonStringifier(Isolate* isolate, JsonStringifyCapture* capture)
      : isolate_(isolate), buffer_(capture), capture_(capture) {}
  void FinishCapture() {
    buffer_.Flush();
    capture_->Finish();
  }

  size_t ResultLength() const { return buffer_.length(); }
  template <typename DstChar>
  void CopyResultTo(DstChar* out_buffer) {
    buffer_.CopyTo(out_buffer);
  }
  V8_INLINE FastJsonStringifierResult SerializeObject(
      Tagged<Object> object, const DisallowGarbageCollection& no_gc);

  template <typename OldChar>
    requires(sizeof(OldChar) < sizeof(Char))
  V8_NOINLINE FastJsonStringifierResult
  ResumeFrom(FastJsonStringifier<OldChar, capture_mode>& old_stringifier,
             const DisallowGarbageCollection& no_gc);

 private:
  static constexpr bool is_one_byte = sizeof(Char) == sizeof(uint8_t);

  V8_INLINE void SeparatorUnchecked(bool comma) {
    if (comma) AppendCharacterUnchecked(',');
  }
  V8_INLINE void Separator(bool comma) {
    if (comma) AppendCharacter(',');
  }
  V8_INLINE void SerializeSmi(Tagged<Smi> object);
  void SerializeDouble(double number);
  template <bool no_escaping>
  FastJsonStringifierObjectKeyResult SerializeObjectKey(
      Tagged<String> key, bool comma, const DisallowGarbageCollection& no_gc);
  template <typename StringT, bool no_escaping>
  FastJsonStringifierObjectKeyResult SerializeObjectKey(
      Tagged<String> key, bool comma, const DisallowGarbageCollection& no_gc);
  template <typename StringT>
  V8_INLINE FastJsonStringifierResult SerializeString(
      Tagged<HeapObject> str, const DisallowGarbageCollection& no_gc);

  FastJsonStringifierResult TrySerializeSimpleObject(Tagged<Object> object);
  FastJsonStringifierResult SerializeObject(
      ContinuationRecord cont, const DisallowGarbageCollection& no_gc);
  V8_NOINLINE FastJsonStringifierResult SerializeJSPrimitiveWrapper(
      Tagged<JSPrimitiveWrapper> obj, const DisallowGarbageCollection& no_gc);
  V8_INLINE FastJsonStringifierResult SerializeJSObject(
      Tagged<JSObject> obj, const DisallowGarbageCollection& no_gc);
  template <DescriptorArray::FastIterableState fast_iterable_state>
  V8_INLINE FastJsonStringifierResult ResumeJSObject(
      Tagged<JSObject> obj, uint16_t start_descriptor_idx,
      uint16_t nof_descriptors, uint8_t in_object_properties,
      uint8_t in_object_properties_start, Tagged<DescriptorArray> descriptors,
      bool comma, const DisallowGarbageCollection& no_gc);
  FastJsonStringifierResult SerializeJSArray(Tagged<JSArray> array);
  template <ElementsKind kind>
  FastJsonStringifierResult SerializeFixedArrayWithInterruptCheck(
      Tagged<FixedArrayBase> elements, uint32_t start_index, uint32_t length);
  template <ElementsKind kind>
  V8_INLINE FastJsonStringifierResult SerializeFixedArray(
      Tagged<FixedArrayBase> array, uint32_t start_idx, uint32_t length);
  template <ElementsKind kind, bool with_interrupt_checks, typename T>
  V8_INLINE FastJsonStringifierResult
  SerializeFixedArrayElement(Tagged<T> elements, uint32_t i, uint32_t length);

  V8_NOINLINE FastJsonStringifierResult HandleInterruptAndCheckCycle();
  V8_NOINLINE bool CheckCycle();

  V8_INLINE void EnsureCapacity(size_t size) { buffer_.EnsureCapacity(size); }
  template <typename SrcChar>
  V8_INLINE void AppendCharacterUnchecked(SrcChar c) {
    buffer_.AppendCharacter(c);
  }
  template <typename SrcChar>
  V8_INLINE void AppendCharacter(SrcChar c) {
    EnsureCapacity(1);
    AppendCharacterUnchecked(c);
  }
  template <size_t N>
  V8_INLINE void AppendCStringLiteralUnchecked(const char (&literal)[N]) {
    // Note that the literal contains the zero char.
    constexpr size_t length = N - 1;
    static_assert(length > 0);
    if constexpr (length == 1) return AppendCharacterUnchecked(literal[0]);
    const uint8_t* chars = reinterpret_cast<const uint8_t*>(literal);
    buffer_.Append(chars, length);
  }
  template <size_t N>
  V8_INLINE void AppendCStringLiteral(const char (&literal)[N]) {
    // Note that the literal contains the zero char.
    constexpr size_t length = N - 1;
    static_assert(length > 0);
    EnsureCapacity(length);
    AppendCStringLiteralUnchecked(literal);
  }

  V8_INLINE void AppendCStringUnchecked(const char* chars, size_t len) {
    buffer_.Append(reinterpret_cast<const unsigned char*>(chars), len);
  }
  V8_INLINE void AppendCStringUnchecked(const char* chars) {
    AppendCStringUnchecked(chars, strlen(chars));
  }
  V8_INLINE void AppendStringUnchecked(std::string_view str) {
    AppendCStringUnchecked(str.data(), str.length());
  }
  V8_INLINE void AppendCString(const char* chars, size_t len) {
    EnsureCapacity(len);
    AppendCStringUnchecked(chars, len);
  }
  V8_INLINE void AppendCString(const char* chars) {
    AppendCString(chars, strlen(chars));
  }
  V8_INLINE void AppendString(std::string_view str) {
    AppendCString(str.data(), str.length());
  }

  template <typename SrcChar>
    requires(sizeof(SrcChar) == sizeof(uint8_t))
  V8_INLINE bool AppendString(const SrcChar* chars, size_t length,
                              const DisallowGarbageCollection& no_gc);

  template <typename SrcChar>
  V8_INLINE void AppendStringNoEscapes(const SrcChar* chars, size_t length,
                                       const DisallowGarbageCollection& no_gc);

  template <typename SrcChar>
    requires(sizeof(SrcChar) == sizeof(uint8_t))
  bool AppendStringScalar(const SrcChar* chars, size_t length, size_t start,
                          size_t uncopied_src_index,
                          const DisallowGarbageCollection& no_gc);

  template <typename SrcChar>
    requires(sizeof(SrcChar) == sizeof(uint8_t))
  V8_INLINE bool AppendStringSWAR(const SrcChar* chars, size_t length,
                                  size_t start, size_t uncopied_src_index,
                                  const DisallowGarbageCollection& no_gc);

  template <typename SrcChar>
    requires(sizeof(SrcChar) == sizeof(uint8_t))
  V8_INLINE bool AppendStringSIMD(const SrcChar* chars, size_t length,
                                  const DisallowGarbageCollection& no_gc);

  template <typename SrcChar>
    requires(sizeof(SrcChar) == sizeof(base::uc16))
  V8_INLINE bool AppendString(const SrcChar* chars, size_t length,
                              const DisallowGarbageCollection& no_gc);

  using FastIterableState = DescriptorArray::FastIterableState;
  static constexpr uint32_t kGlobalInterruptBudget = 200000;
  static constexpr uint32_t kArrayInterruptLength = 4000;

  Isolate* isolate_;
  std::conditional_t<capture_mode, CaptureBuffer, OutBuffer<Char>> buffer_;
  JsonStringifyCapture* capture_ = nullptr;
  base::SmallVector<ContinuationRecord, 16> stack_;

  Tagged<HeapObject> initial_jsobject_proto_;
  Tagged<HeapObject> initial_jsarray_proto_;
  template <typename, bool>
  friend class FastJsonStringifier;
};

namespace {

V8_INLINE bool CanFastSerializeJSArrayFastPath(Tagged<JSArray> object,
                                               Tagged<HeapObject> initial_proto,
                                               Isolate* isolate) {
  // Check if the prototype is the initial array prototype without interesting
  // properties (toJSON).
  Tagged<Map> map = object->map();
  if (V8_UNLIKELY(map->may_have_interesting_properties())) return false;
  Tagged<HeapObject> proto = map->prototype();
  // Note: This will also fail for sub-objects in different native contexts.
  // This should be rare and bailing-out to the slow-path is fine.
  return V8_LIKELY(proto == initial_proto);
}

V8_INLINE bool CanFastSerializeJSObjectFastPath(
    Tagged<JSObject> object, Tagged<HeapObject> initial_proto, Tagged<Map> map,
    Isolate* isolate) {
  if (V8_UNLIKELY(IsCustomElementsReceiverMap(map))) return false;
  if (V8_UNLIKELY(!object->HasFastProperties())) return false;
  auto roots = ReadOnlyRoots(isolate);
  auto elements = object->elements();
  if (V8_UNLIKELY(elements != roots.empty_fixed_array() &&
                  elements != roots.empty_slow_element_dictionary())) {
    return false;
  }
  if (V8_UNLIKELY(map->may_have_interesting_properties())) return false;
  Tagged<HeapObject> proto = map->prototype();
  // Note: This will also fail for sub-objects in different native contexts.
  // This should be rare and bailing-out to the slow-path is fine.
  return V8_LIKELY(proto == initial_proto);
}

}  // namespace

template <typename Char, bool capture_mode>
FastJsonStringifier<Char, capture_mode>::FastJsonStringifier(Isolate* isolate)
    : isolate_(isolate), buffer_(isolate->allocator()) {}

template <typename Char, bool capture_mode>
void FastJsonStringifier<Char, capture_mode>::SerializeSmi(Tagged<Smi> object) {
#if defined(V8_TARGET_ARCH_ARM64)
  // Extra parts outweigh submission savings on ARM32. On ARM64, defer larger
  // individual integers while keeping short literals in the punctuation run.
  if constexpr (capture_mode) {
    if (object.value() >= 10000 || object.value() <= -10000) {
      buffer_.Flush();
      capture_->Integer(object.value());
      return;
    }
  }
#endif
  static_assert(Smi::kMaxValue <= 2147483647);
  static_assert(Smi::kMinValue >= -2147483648);
  // sizeof(string) includes \0.
  static constexpr uint32_t kBufferSize = sizeof("-2147483648") - 1;
  char chars[kBufferSize];
  base::Vector<char> buffer(chars, kBufferSize);
  std::string_view str = IntToStringView(object.value(), buffer);
  AppendString(str);
}

template <typename Char, bool capture_mode>
void FastJsonStringifier<Char, capture_mode>::SerializeDouble(double number) {
  if constexpr (capture_mode) {
    buffer_.Flush();
    capture_->Number(number);
    return;
  }
  if (V8_UNLIKELY(std::isinf(number) || std::isnan(number))) {
    AppendCStringLiteral("null");
    return;
  }
  static constexpr uint32_t kBufferSize = 100;
  char chars[kBufferSize];
  base::Vector<char> buffer(chars, kBufferSize);
  std::string_view str = DoubleToStringView(number, buffer);
  AppendString(str);
}

template <typename Char, bool capture_mode>
template <bool no_escaping>
FastJsonStringifierObjectKeyResult
FastJsonStringifier<Char, capture_mode>::SerializeObjectKey(
    Tagged<String> key, bool comma, const DisallowGarbageCollection& no_gc) {
  if constexpr (capture_mode) {
    Separator(comma);
    buffer_.Flush();
    capture_->StringValue(key, false, true, no_gc);
    AppendCharacter(':');
    return no_escaping || IsFastKey(key, no_gc)
               ? FastJsonStringifierObjectKeyResult::kSuccess
               : FastJsonStringifierObjectKeyResult::kSlow;
  }
#if V8_STATIC_ROOTS_BOOL
  // This is slightly faster than the switch over instance types.
  ReadOnlyRoots roots(isolate_);
  Tagged<Map> map = key->map();
  if (map == roots.internalized_one_byte_string_map()) {
    return SerializeObjectKey<SeqOneByteString, no_escaping>(key, comma, no_gc);
  } else if (map == roots.external_internalized_one_byte_string_map() ||
             map ==
                 roots.uncached_external_internalized_one_byte_string_map()) {
    return SerializeObjectKey<ExternalOneByteString, no_escaping>(key, comma,
                                                                  no_gc);
  } else {
    if constexpr (is_one_byte) {
      DCHECK(!key->IsOneByteRepresentation());
      // no_escaping implies that all keys are one-byte.
      if constexpr (no_escaping) {
        UNREACHABLE();
      } else {
        return FastJsonStringifierObjectKeyResult::kChangeEncoding;
      }
    } else {
      if (map == roots.internalized_two_byte_string_map()) {
        return SerializeObjectKey<SeqTwoByteString, no_escaping>(key, comma,
                                                                 no_gc);
      } else if (
          map == roots.external_internalized_two_byte_string_map() ||
          map == roots.uncached_external_internalized_two_byte_string_map()) {
        return SerializeObjectKey<ExternalTwoByteString, no_escaping>(
            key, comma, no_gc);
      }
    }
  }
#else
  InstanceType instance_type = key->map()->instance_type();
  switch (instance_type) {
    case INTERNALIZED_ONE_BYTE_STRING_TYPE:
      return SerializeObjectKey<SeqOneByteString, no_escaping>(key, comma,
                                                               no_gc);
    case EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
      return SerializeObjectKey<ExternalOneByteString, no_escaping>(key, comma,
                                                                    no_gc);
    case INTERNALIZED_TWO_BYTE_STRING_TYPE:
      return SerializeObjectKey<SeqTwoByteString, no_escaping>(key, comma,
                                                               no_gc);
    case EXTERNAL_INTERNALIZED_TWO_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_INTERNALIZED_TWO_BYTE_STRING_TYPE:
      return SerializeObjectKey<ExternalTwoByteString, no_escaping>(key, comma,
                                                                    no_gc);
    default:
      UNREACHABLE();
  }
#endif
  UNREACHABLE();
}

namespace {

// The worst case length of an escaped character is 6. Shifting the string
// length left by 3 is a more pessimistic estimate, but faster to calculate.
size_t MaxEscapedStringLength(size_t length) { return length << 3; }

}  // namespace

template <typename Char, bool capture_mode>
template <typename StringT, bool no_escaping>
FastJsonStringifierObjectKeyResult
FastJsonStringifier<Char, capture_mode>::SerializeObjectKey(
    Tagged<String> obj, bool comma, const DisallowGarbageCollection& no_gc) {
  using StringChar =
      std::conditional_t<std::is_same_v<StringT, SeqOneByteString> ||
                             std::is_same_v<StringT, ExternalOneByteString>,
                         uint8_t, base::uc16>;
  if constexpr (is_one_byte && sizeof(StringChar) == 2) {
    // no_escaping is only possible if we have already seen all the keys in a
    // map. But it is not possible we have seen a two-byte string and are still
    // in one-byte mode.
    if constexpr (no_escaping) {
      UNREACHABLE();
    } else {
      return FastJsonStringifierObjectKeyResult::kChangeEncoding;
    }
  } else {
    Tagged<StringT> string = StringT::cast(obj);
    const StringChar* chars;
    if constexpr (requires { string->GetChars(no_gc); }) {
      chars = string->GetChars(no_gc);
    } else {
      chars = string->GetChars();
    }
    const uint32_t length = string->length();
    size_t max_length;
    if constexpr (no_escaping) {
      max_length = length;
    } else {
      max_length = MaxEscapedStringLength(length);
    }
    max_length += 4 /* optional comma + 2x double quote + colon */;
    EnsureCapacity(max_length);
    SeparatorUnchecked(comma);
    AppendCharacterUnchecked('"');
    FastJsonStringifierObjectKeyResult result;
    if constexpr (no_escaping) {
      DCHECK(IsFastKey(obj, no_gc));
      SLOW_DCHECK(String::DoesNotContainEscapeCharacters(obj));
      AppendStringNoEscapes(chars, length, no_gc);
      result = FastJsonStringifierObjectKeyResult::kSuccess;
    } else {
      bool needs_escaping = AppendString(chars, length, no_gc);
      DCHECK_IMPLIES(needs_escaping, !IsFastKey(obj, no_gc));
      SLOW_DCHECK(needs_escaping !=
                  String::DoesNotContainEscapeCharacters(obj));
      result = sizeof(StringChar) == 1 && !needs_escaping
                   ? FastJsonStringifierObjectKeyResult::kSuccess
                   : FastJsonStringifierObjectKeyResult::kSlow;
    }
    AppendCharacterUnchecked('"');
    AppendCharacterUnchecked(':');
    return result;
  }
}

template <typename Char, bool capture_mode>
template <typename StringT>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeString(
    Tagged<HeapObject> obj, const DisallowGarbageCollection& no_gc) {
  if constexpr (capture_mode) {
    buffer_.Flush();
    capture_->StringValue(String::cast(obj), false, false, no_gc);
    return SUCCESS;
  }
  using StringChar =
      std::conditional_t<std::is_same_v<StringT, SeqOneByteString> ||
                             std::is_same_v<StringT, ExternalOneByteString>,
                         uint8_t, base::uc16>;
  if constexpr (is_one_byte && sizeof(StringChar) == 2) {
    return CHANGE_ENCODING;
  } else {
    Tagged<StringT> string = StringT::cast(obj);
    const StringChar* chars;
    if constexpr (requires { string->GetChars(no_gc); }) {
      chars = string->GetChars(no_gc);
    } else {
      chars = string->GetChars();
    }
    const uint32_t length = string->length();
    EnsureCapacity(MaxEscapedStringLength(length) + 2 /* 2x double quote */);
    AppendCharacterUnchecked('"');
    AppendString(chars, length, no_gc);
    AppendCharacterUnchecked('"');
    return SUCCESS;
  }
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::TrySerializeSimpleObject(
    Tagged<Object> object) {
  DisallowGarbageCollection no_gc;
  // GCMole doesn't handle kNoGC interrupts correctly.
  DisableGCMole no_gc_mole;

  if (IsSmi(object)) {
    SerializeSmi(Smi::cast(object));
    return SUCCESS;
  }
  Tagged<HeapObject> obj = HeapObject::cast(object);
  Tagged<Map> map = obj->map();
  InstanceType instance_type = map->instance_type();
  switch (instance_type) {
    case INTERNALIZED_ONE_BYTE_STRING_TYPE:
    case SEQ_ONE_BYTE_STRING_TYPE:
      return SerializeString<SeqOneByteString>(obj, no_gc);
    case EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
    case EXTERNAL_ONE_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_ONE_BYTE_STRING_TYPE:
      return SerializeString<ExternalOneByteString>(obj, no_gc);
    case THIN_ONE_BYTE_STRING_TYPE: {
      Tagged<String> actual = ThinString::cast(obj)->actual();
      if (IsExternalString(actual)) {
        return SerializeString<ExternalOneByteString>(actual, no_gc);
      } else {
        return SerializeString<SeqOneByteString>(actual, no_gc);
      }
    }
    case INTERNALIZED_TWO_BYTE_STRING_TYPE:
    case SEQ_TWO_BYTE_STRING_TYPE:
      return SerializeString<SeqTwoByteString>(obj, no_gc);
    case EXTERNAL_INTERNALIZED_TWO_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_INTERNALIZED_TWO_BYTE_STRING_TYPE:
    case EXTERNAL_TWO_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_TWO_BYTE_STRING_TYPE:
      return SerializeString<ExternalTwoByteString>(obj, no_gc);
    case THIN_TWO_BYTE_STRING_TYPE: {
      if constexpr (is_one_byte && !capture_mode) {
        return CHANGE_ENCODING;
      } else {
        Tagged<String> actual = ThinString::cast(obj)->actual();
        if (IsExternalString(actual)) {
          return SerializeString<ExternalTwoByteString>(actual, no_gc);
        } else {
          return SerializeString<SeqTwoByteString>(actual, no_gc);
        }
      }
    }
    case HEAP_NUMBER_TYPE:
      SerializeDouble(HeapNumber::cast(obj)->value());
      return SUCCESS;
    case ODDBALL_TYPE:
      switch (Oddball::cast(obj)->kind()) {
        case Oddball::kFalse:
          AppendCStringLiteral("false");
          return SUCCESS;
        case Oddball::kTrue:
          AppendCStringLiteral("true");
          return SUCCESS;
        case Oddball::kNull:
          AppendCStringLiteral("null");
          return SUCCESS;
        default:
          return UNDEFINED;
      }
    case SYMBOL_TYPE:
      return UNDEFINED;
    case JS_PRIMITIVE_WRAPPER_TYPE:
      return SerializeJSPrimitiveWrapper(JSPrimitiveWrapper::cast(obj), no_gc);
    case JS_OBJECT_TYPE:
      return JS_OBJECT;
    case JS_ARRAY_TYPE:
      return JS_ARRAY;
    default:
      return SLOW_PATH;
  }

  UNREACHABLE();
}

namespace {

Builtin GetBuiltin(Isolate* isolate, Tagged<JSObject> obj, Handle<Name> name) {
  HandleScope handle_scope(isolate);

  Handle<JSObject> obj_handle = handle(obj, isolate);
  LookupIterator it(isolate, obj_handle, name, obj_handle);
  if (!it.IsFound()) {
    return Builtin::kNoBuiltinId;
  }
  if (it.state() != LookupIterator::DATA) {
    return Builtin::kNoBuiltinId;
  }
  Handle<Object> fun = Object::GetProperty(&it).ToHandleChecked();
  if (!IsJSFunction(*fun)) {
    return Builtin::kNoBuiltinId;
  }
  return JSFunction::cast(*fun)->code(isolate)->builtin_id();
}

}  // namespace

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeJSPrimitiveWrapper(
    Tagged<JSPrimitiveWrapper> obj, const DisallowGarbageCollection& no_gc) {
  // TODO(pthier): Consider extending IsStringWrapperToPrimitive to also cover
  // toString() and all JSPrimitiveWrappers to avoid some of the checks here.
  if (V8_UNLIKELY(MayHaveInterestingProperties(isolate_, obj))) {
    return SLOW_PATH;
  }

  Tagged<Object> raw = obj->value();
  if (IsSymbol(raw)) {
    // Symbol object wrapper is treated as a regular object.
    Tagged<Map> map = obj->map();
    // Check that object has no own properties.
    if (V8_UNLIKELY(!obj->HasFastProperties())) return SLOW_PATH;
    if (V8_UNLIKELY(map->NumberOfOwnDescriptors() != 0)) return SLOW_PATH;
    // Check that object has no elements.
    auto roots = ReadOnlyRoots(isolate_);
    auto elements = obj->elements();
    if (V8_UNLIKELY(elements != roots.empty_fixed_array() &&
                    elements != roots.empty_slow_element_dictionary())) {
      return SLOW_PATH;
    }

    AppendCStringLiteral("{}");
    return SUCCESS;
  } else if (IsString(raw)) {
    if (V8_UNLIKELY(!Protectors::IsStringWrapperToPrimitiveIntact(isolate_))) {
      return SLOW_PATH;
    }
    // We only fetch data properties in GetBuiltin, but GCMole doesn't know
    // that and assumes GCs can happen.
    DisableGCMole no_gc_mole;
    // Check that toString() on the prototype chain is the expected builtin.
    if (V8_UNLIKELY(
            GetBuiltin(isolate_, obj, isolate_->factory()->toString_string()) !=
            Builtin::kStringPrototypeToString)) {
      return SLOW_PATH;
    }

    Tagged<String> string = String::cast(raw);
    while (true) {
      FastJsonStringifierResult result = DispatchString(
          string,
          absl::Overload{
              [&](Tagged<SeqOneByteString> str) {
                return SerializeString<SeqOneByteString>(str, no_gc);
              },
              [&](Tagged<SeqTwoByteString> str) {
                return SerializeString<SeqTwoByteString>(str, no_gc);
              },
              [&](Tagged<ExternalOneByteString> str) {
                return SerializeString<ExternalOneByteString>(str, no_gc);
              },
              [&](Tagged<ExternalTwoByteString> str) {
                return SerializeString<ExternalTwoByteString>(str, no_gc);
              },
              [&](Tagged<ThinString> str) {
                string = str->actual();
                // UNDEFINED is misused here to indicate that we continue after
                // unwrapping.
                return UNDEFINED;
              },
              [&](Tagged<ConsString> str) { return SLOW_PATH; },
              [&](Tagged<SlicedString> str) { return SLOW_PATH; }});
      if (V8_LIKELY(result != UNDEFINED)) return result;
    }
    UNREACHABLE();
  } else if (IsNumber(raw)) {
    // We only fetch data properties in GetBuiltin, but GCMole doesn't know
    // that and assumes GCs can happen.
    DisableGCMole no_gc_mole;
    // Check that valueOf() on the prototype chain is the expected builtin.
    if (V8_UNLIKELY(
            GetBuiltin(isolate_, obj, isolate_->factory()->valueOf_string()) !=
            Builtin::kNumberPrototypeValueOf)) {
      return SLOW_PATH;
    }
    if (IsSmi(raw)) {
      SerializeSmi(Smi::cast(raw));
      return SUCCESS;
    }
    DCHECK(IsHeapNumber(raw));
    SerializeDouble(HeapNumber::cast(raw)->value());
    return SUCCESS;
  } else if (IsBoolean(raw)) {
    if (IsTrue(raw, isolate_)) {
      AppendCStringLiteral("true");
    } else {
      AppendCStringLiteral("false");
    }
    return SUCCESS;
  } else if (IsBigInt(raw)) {
    return SLOW_PATH;
  }

  UNREACHABLE();
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeJSObject(
    Tagged<JSObject> obj, const DisallowGarbageCollection& no_gc) {
  Tagged<Map> map = obj->map();
  if (V8_UNLIKELY(!CanFastSerializeJSObjectFastPath(
          obj, initial_jsobject_proto_, map, isolate_))) {
    return SLOW_PATH;
  }
  AppendCharacter('{');
  const uint16_t nof_descriptors = map->NumberOfOwnDescriptors();
  const uint8_t in_object_properties = map->GetInObjectProperties();
  const uint8_t in_object_properties_start =
      map->GetInObjectPropertiesStartInWords();
  const Tagged<DescriptorArray> descriptors = map->instance_descriptors();
  FastIterableState fast_iterable_state = descriptors->fast_iterable();
  switch (fast_iterable_state) {
#define CASE(state)                                    \
  case FastIterableState::state:                       \
    return ResumeJSObject<FastIterableState::state>(   \
        obj, 0, nof_descriptors, in_object_properties, \
        in_object_properties_start, descriptors, false, no_gc)
    CASE(kUnknown);
    CASE(kJsonFast);
    CASE(kJsonSlow);
#undef CASE
  }
  UNREACHABLE();
}

template <typename Char, bool capture_mode>
template <DescriptorArray::FastIterableState fast_iterable_state>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::ResumeJSObject(
    Tagged<JSObject> obj, uint16_t start_descriptor_idx,
    uint16_t nof_descriptors, uint8_t in_object_properties,
    uint8_t in_object_properties_start, Tagged<DescriptorArray> descriptors,
    bool comma, const DisallowGarbageCollection& no_gc) {
  PtrComprCageBase cage_base = PtrComprCageBase(isolate_);
  InternalIndex::Range range{start_descriptor_idx, nof_descriptors};
  for (InternalIndex i : range) {
    // We don't deal with dictionary mode objects on the fast-path, so the index
    // is guaranteed to be less than uint16_t::max.
    static_assert(kMaxNumberOfDescriptors <
                  std::numeric_limits<uint16_t>::max());
    DCHECK_LE(i.as_uint32(), kMaxNumberOfDescriptors);
    const uint16_t descriptor_idx = static_cast<uint16_t>(i.as_uint32());

    Tagged<Name> name = descriptors->GetKey(i);
    int property_index;
    if constexpr (fast_iterable_state != FastIterableState::kJsonFast) {
      if (V8_UNLIKELY(IsSymbol(name))) {
        if constexpr (fast_iterable_state == FastIterableState::kUnknown) {
          descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        }
        continue;
      }
      PropertyDetails details = descriptors->GetDetails(i);
      if (V8_UNLIKELY(details.IsDontEnum())) {
        if constexpr (fast_iterable_state == FastIterableState::kUnknown) {
          descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        }
        continue;
      }
      if (V8_UNLIKELY(details.location() != PropertyLocation::kField)) {
        descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        return SLOW_PATH;
      }
      DCHECK_EQ(PropertyKind::kData, details.kind());
      property_index = details.field_index();
    } else {
      DCHECK_EQ(descriptor_idx, descriptors->GetDetails(i).field_index());
      property_index = descriptor_idx;
    }
    const bool is_inobject = property_index < in_object_properties;
    Tagged<Object> property;
    if (is_inobject) {
      int offset = (in_object_properties_start + property_index) * kTaggedSize;
      property = TaggedField<Object>::Relaxed_Load(cage_base, obj, offset);
    } else {
      property_index -= in_object_properties;
      property = obj->property_array(cage_base)->get(cage_base, property_index);
    }

    DCHECK(IsInternalizedString(name));
    Tagged<String> key_name = String::cast(name);

    if (V8_UNLIKELY(IsUndefined(property) || IsSymbol(property))) {
      if constexpr (fast_iterable_state == FastIterableState::kUnknown) {
        // Even if we don't serialize the key, check if it is fast iterable to
        // avoid false positives in DescriptorArray::fast_iterable.
        if (!IsFastKey(key_name, no_gc)) {
          descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        }
      }
      continue;
    }

    FastJsonStringifierObjectKeyResult key_result;
    if constexpr (fast_iterable_state == FastIterableState::kJsonFast) {
      key_result = SerializeObjectKey<true>(key_name, comma, no_gc);
      DCHECK_EQ(key_result, FastJsonStringifierObjectKeyResult::kSuccess);
    } else {
      key_result = SerializeObjectKey<false>(key_name, comma, no_gc);
      if (V8_UNLIKELY(key_result !=
                      FastJsonStringifierObjectKeyResult::kSuccess)) {
        descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        if constexpr (is_one_byte) {
          if (key_result ==
              FastJsonStringifierObjectKeyResult::kChangeEncoding) {
            stack_.emplace_back(
                ContinuationRecord::ForJSObjectResume<fast_iterable_state>(
                    obj, descriptor_idx + 1, nof_descriptors,
                    in_object_properties, in_object_properties_start,
                    descriptors));
            if (IsJSObject(property)) {
              stack_.emplace_back(ContinuationRecord::ForJSObject(property));
            } else if (IsJSArray(property)) {
              stack_.emplace_back(ContinuationRecord::ForJSArray(property));
            } else {
              stack_.emplace_back(
                  ContinuationRecord::ForSimpleObject(property));
            }
            stack_.emplace_back(
                ContinuationRecord::ForObjectKey(key_name, comma));
            return CHANGE_ENCODING;
          }
        } else {
          DCHECK_NE(key_result,
                    FastJsonStringifierObjectKeyResult::kChangeEncoding);
        }
      }
    }
    // TrySerializeSimpleObject won't trigger GCs. See DisableGCMole scopes in
    // SerializeJSPrimitiveWrapper for explanation.
    DisableGCMole no_gc_mole;
    FastJsonStringifierResult result;
    if constexpr (fast_iterable_state == FastIterableState::kJsonFast) {
      result = TrySerializeSimpleObject(property);
    } else {
      result = TrySerializeSimpleObject(property);
    }
    switch (result) {
      case SUCCESS:
        comma = true;
        break;
      case UNDEFINED:
        break;
      case JS_OBJECT:
      case JS_ARRAY:
        stack_.push_back(
            ContinuationRecord::ForJSObjectResume<fast_iterable_state>(
                obj, descriptor_idx + 1, nof_descriptors, in_object_properties,
                in_object_properties_start, descriptors));
        stack_.push_back(ContinuationRecord::ForJSAny(property, result));
        return result;
      case CHANGE_ENCODING:
        if constexpr (is_one_byte) {
          stack_.push_back(
              ContinuationRecord::ForJSObjectResume<fast_iterable_state>(
                  obj, descriptor_idx + 1, nof_descriptors,
                  in_object_properties, in_object_properties_start,
                  descriptors));
          stack_.push_back(ContinuationRecord::ForSimpleObject(property));
          return result;
        } else {
          UNREACHABLE();
        }
      case SLOW_PATH:
      case EXCEPTION:
        return result;
    }
  }
  AppendCharacter('}');
  if constexpr (fast_iterable_state == FastIterableState::kUnknown) {
    if (nof_descriptors == descriptors->number_of_descriptors()) {
      descriptors->set_fast_iterable_if(FastIterableState::kJsonFast,
                                        FastIterableState::kUnknown);
    }
  }
  return SUCCESS;
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeJSArray(
    Tagged<JSArray> array) {
  if (V8_UNLIKELY(!CanFastSerializeJSArrayFastPath(
          array, initial_jsarray_proto_, isolate_))) {
    return SLOW_PATH;
  }
  AppendCharacter('[');
  uint32_t length = static_cast<uint32_t>(Object::Number(array->length()));
  Tagged<FixedArrayBase> elements = array->elements();
  switch (array->GetElementsKind()) {
#define CASE(kind)                                                             \
  case kind:                                                                   \
    if constexpr (IsHoleyElementsKind(kind)) {                                 \
      if (V8_UNLIKELY(!Protectors::IsNoElementsIntact(isolate_))) {            \
        return SLOW_PATH;                                                      \
      }                                                                        \
    }                                                                          \
    if (V8_UNLIKELY(length > kArrayInterruptLength)) {                         \
      return SerializeFixedArrayWithInterruptCheck<kind>(elements, 0, length); \
    } else {                                                                   \
      return SerializeFixedArray<kind>(elements, 0, length);                   \
    }
    CASE(PACKED_SMI_ELEMENTS)
    CASE(PACKED_ELEMENTS)
    CASE(PACKED_DOUBLE_ELEMENTS)
    CASE(HOLEY_SMI_ELEMENTS)
    CASE(HOLEY_ELEMENTS)
    CASE(HOLEY_DOUBLE_ELEMENTS)
#undef CASE
    default:
      return SLOW_PATH;
  }
  UNREACHABLE();
}

template <typename Char, bool capture_mode>
template <ElementsKind kind>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeFixedArrayWithInterruptCheck(
    Tagged<FixedArrayBase> elements, uint32_t start_index, uint32_t length) {
  using ArrayT = std::conditional_t<IsDoubleElementsKind(kind),
                                    FixedDoubleArray, FixedArray>;

  StackLimitCheck interrupt_check(isolate_);
  uint32_t limit = std::min(length, start_index + kArrayInterruptLength);
  constexpr uint32_t kMaxAllowedFastPackedLength =
      std::numeric_limits<uint32_t>::max() - kArrayInterruptLength;
  static_assert(FixedArray::kMaxLength < kMaxAllowedFastPackedLength);

  // GCMole doesn't handle kNoGC interrupts correctly.
  DisableGCMole no_gc_mole;

  uint32_t i = start_index;
  while (true) {
    if constexpr (capture_mode && !IsHoleyElementsKind(kind) &&
                  (IsSmiElementsKind(kind) || IsDoubleElementsKind(kind))) {
      buffer_.Flush();
      capture_->Numbers<kind>(ArrayT::cast(elements), i, limit);
      i = limit;
    }
    for (; i < limit; i++) {
      FastJsonStringifierResult result = SerializeFixedArrayElement<kind, true>(
          ArrayT::cast(elements), i, length);
      if (result != SUCCESS) return result;
    }
    if (i >= length) {
      AppendCharacter(']');
      return SUCCESS;
    }
    DCHECK_LT(limit, kMaxAllowedFastPackedLength);
    limit = std::min(length, limit + kArrayInterruptLength);
    {
      // A TerminationException can actually trigger a GC when allocating the
      // message object. We don't touch any of the heap objects after we
      // encountered an exception, so this is fine.
      AllowGarbageCollection allow_gc;
      if (interrupt_check.InterruptRequested() &&
          IsException(isolate_->stack_guard()->HandleInterrupts(
                          StackGuard::InterruptLevel::kNoGC),
                      isolate_)) {
        return EXCEPTION;
      }
    }
  }
  UNREACHABLE();
}

template <typename Char, bool capture_mode>
template <ElementsKind kind>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeFixedArray(
    Tagged<FixedArrayBase> elements, uint32_t start_index, uint32_t length) {
  using ArrayT = std::conditional_t<IsDoubleElementsKind(kind),
                                    FixedDoubleArray, FixedArray>;

  if constexpr (capture_mode && !IsHoleyElementsKind(kind) &&
                (IsSmiElementsKind(kind) || IsDoubleElementsKind(kind))) {
    if (start_index < length && (IsDoubleElementsKind(kind) || length >= 16)) {
      buffer_.Flush();
      capture_->Numbers<kind>(ArrayT::cast(elements), start_index, length);
      start_index = length;
    }
  }
  for (uint32_t i = start_index; i < length; i++) {
    FastJsonStringifierResult result = SerializeFixedArrayElement<kind, false>(
        ArrayT::cast(elements), i, length);
    if (result != SUCCESS) return result;
  }
  AppendCharacter(']');
  return SUCCESS;
}

template <typename Char, bool capture_mode>
template <ElementsKind kind, bool with_interrupt_checks, typename T>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeFixedArrayElement(
    Tagged<T> elements, uint32_t i, uint32_t length) {
  if constexpr (IsHoleyElementsKind(kind)) {
    if (elements->is_the_hole(isolate_, i)) {
      EnsureCapacity(5 /* "null" + optional comma */);
      SeparatorUnchecked(i > 0);
      AppendCStringLiteralUnchecked("null");
      return SUCCESS;
    }
  }
  DCHECK(!elements->is_the_hole(isolate_, i));
  Separator(i > 0);
  if constexpr (IsSmiElementsKind(kind)) {
    SerializeSmi(Smi::cast(elements->get(i)));
  } else if constexpr (IsDoubleElementsKind(kind)) {
    SerializeDouble(elements->get_scalar(i));
  } else {
    Tagged<Object> obj = Object::cast(elements->get(i));
    // TrySerializeSimpleObject won't trigger GCs. See DisableGCMole scopes in
    // SerializeJSPrimitiveWrapper for explanation.
    DisableGCMole no_gc_mole;
    FastJsonStringifierResult result;
    result = TrySerializeSimpleObject(obj);
    switch (result) {
      case UNDEFINED:
        AppendCStringLiteral("null");
        return SUCCESS;
      case CHANGE_ENCODING:
        if constexpr (is_one_byte) {
          DCHECK(IsString(obj) || IsStringWrapper(obj));
          stack_.push_back(
              ContinuationRecord::ForJSArrayResume<kind, with_interrupt_checks>(
                  elements, i + 1, length));
          stack_.push_back(ContinuationRecord::ForSimpleObject(obj));
          return result;
        } else {
          UNREACHABLE();
        }
      case JS_OBJECT:
      case JS_ARRAY:
        stack_.push_back(
            ContinuationRecord::ForJSArrayResume<kind, with_interrupt_checks>(
                elements, i + 1, length));
        stack_.push_back(ContinuationRecord::ForJSAny(obj, result));
        return result;
      case SUCCESS:
      case SLOW_PATH:
      case EXCEPTION:
        return result;
    }
    UNREACHABLE();
  }
  return SUCCESS;
}

template <typename Char, bool capture_mode>
template <typename OldChar>
  requires(sizeof(OldChar) < sizeof(Char))
FastJsonStringifierResult FastJsonStringifier<Char, capture_mode>::ResumeFrom(
    FastJsonStringifier<OldChar, capture_mode>& old_stringifier,
    const DisallowGarbageCollection& no_gc) {
  DCHECK_EQ(ResultLength(), 0);
  DCHECK(stack_.empty());
  DCHECK(!old_stringifier.stack_.empty());

  initial_jsobject_proto_ = old_stringifier.initial_jsobject_proto_;
  initial_jsarray_proto_ = old_stringifier.initial_jsarray_proto_;
  stack_ = old_stringifier.stack_;
  ContinuationRecord cont = stack_.back();
  stack_.pop_back();
  if (cont.type() == ContinuationRecord::kObjectKey) {
    // Serializing an object key caused an encoding change.
    FastJsonStringifierObjectKeyResult key_result = SerializeObjectKey<false>(
        cont.object_key(), cont.object_key_comma(), no_gc);
    USE(key_result);
    DCHECK_NE(key_result, FastJsonStringifierObjectKeyResult::kChangeEncoding);
    // Resuming due to encoding change of an object key guarantees that there
    // are at least two other objects on the stack (the value for that key, and
    // the continuation record for the object the key is a member of).
    DCHECK_GE(stack_.size(), 2);
    cont = stack_.back();
    stack_.pop_back();
    // Check that we have the object's continuation record.
    DCHECK(ContinuationRecord::IsObjectResumeType(stack_.back().type()));
  }
  if (cont.type() == ContinuationRecord::kSimpleObject) {
    // TrySerializeSimpleObject won't trigger GCs. See DisableGCMole scopes in
    // SerializeJSPrimitiveWrapper for explanation.
    DisableGCMole no_gc_mole;
    FastJsonStringifierResult result =
        TrySerializeSimpleObject(cont.simple_object());
    if (V8_UNLIKELY(result != SUCCESS)) {
      DCHECK_EQ(result, SLOW_PATH);
      return result;
    }

    // Early return if a top-level string triggered the encoding change.
    if (stack_.empty()) return result;

    cont = stack_.back();
    stack_.pop_back();
  }
  DCHECK(cont.type() != ContinuationRecord::kSimpleObject &&
         cont.type() != ContinuationRecord::kObjectKey);
  return SerializeObject(cont, no_gc);
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeObject(
    Tagged<Object> object, const DisallowGarbageCollection& no_gc) {
  // Initial checks if using the fast-path is possible in general.
  if (!object.IsSmi() && (IsJSObject(object) || IsJSArray(object))) {
    Tagged<HeapObject> obj = HeapObject::cast(object);
    // Load initial JSObject/JSArray prototypes from native context.
    Tagged<Map> meta_map = obj->map()->map();
    // Don't deal with context-less meta maps.
    if (V8_UNLIKELY(meta_map == ReadOnlyRoots(isolate_).meta_map())) {
      return SLOW_PATH;
    }
    Tagged<NativeContext> native_context = meta_map->native_context();
    initial_jsobject_proto_ = native_context->initial_object_prototype();
    initial_jsarray_proto_ = native_context->initial_array_prototype();
    // Prototypes don't have interesting properties (toJSON).
    if (V8_UNLIKELY(
            initial_jsarray_proto_->map()->may_have_interesting_properties())) {
      return SLOW_PATH;
    }
    if (V8_UNLIKELY(initial_jsobject_proto_->map()
                        ->may_have_interesting_properties())) {
      return SLOW_PATH;
    }
    // JSArray's prototype is the initial object prototype.
    Tagged<HeapObject> jsarray_proto_proto =
        initial_jsarray_proto_->map()->prototype();
    if (V8_UNLIKELY(jsarray_proto_proto != initial_jsobject_proto_)) {
      return SLOW_PATH;
    }
  }

  // TrySerializeSimpleObject won't trigger GCs. See DisableGCMole scopes in
  // SerializeJSPrimitiveWrapper for explanation.
  DisableGCMole no_gc_mole;
  // Serialize object.
  FastJsonStringifierResult result = TrySerializeSimpleObject(object);
  if constexpr (is_one_byte) {
    if (V8_UNLIKELY(result == CHANGE_ENCODING)) {
      DCHECK(IsString(object) || IsStringWrapper(object));
      stack_.push_back(ContinuationRecord::ForSimpleObject(object));
      return result;
    }
  } else {
    DCHECK_NE(result, CHANGE_ENCODING);
  }
  if (result != JS_OBJECT && result != JS_ARRAY) {
    return result;
  }
  return SerializeObject(ContinuationRecord::ForJSAny(object, result), no_gc);
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::SerializeObject(
    ContinuationRecord cont, const DisallowGarbageCollection& no_gc) {
  // GCMole doesn't handle kNoGC interrupts correctly.
  DisableGCMole no_gc_mole;

  uint32_t interrupt_budget = kGlobalInterruptBudget;

  FastJsonStringifierResult result;
  while (true) {
    --interrupt_budget;
    if (V8_UNLIKELY(interrupt_budget == 0)) {
      result = HandleInterruptAndCheckCycle();
      if (V8_UNLIKELY(result != SUCCESS)) return result;
      interrupt_budget = kGlobalInterruptBudget;
    }
    switch (cont.type()) {
      case ContinuationRecord::kObject: {
        result = SerializeJSObject(cont.js_object(), no_gc);
        break;
      }
      case ContinuationRecord::kObjectResume_Uninitialized: {
        result = ResumeJSObject<FastIterableState::kUnknown>(
            cont.js_object(), cont.object_descriptor_idx(),
            cont.object_nof_descriptors(), cont.object_in_object_properties(),
            cont.object_in_object_properties_start(), cont.object_descriptors(),
            true, no_gc);
        break;
      }
      case ContinuationRecord::kObjectResume_FastIterable: {
        result = ResumeJSObject<FastIterableState::kJsonFast>(
            cont.js_object(), cont.object_descriptor_idx(),
            cont.object_nof_descriptors(), cont.object_in_object_properties(),
            cont.object_in_object_properties_start(), cont.object_descriptors(),
            true, no_gc);
        break;
      }
      case ContinuationRecord::kObjectResume_SlowIterable: {
        result = ResumeJSObject<FastIterableState::kJsonSlow>(
            cont.js_object(), cont.object_descriptor_idx(),
            cont.object_nof_descriptors(), cont.object_in_object_properties(),
            cont.object_in_object_properties_start(), cont.object_descriptors(),
            true, no_gc);
        break;
      }
      case ContinuationRecord::kArray: {
        result = SerializeJSArray(cont.js_array());
        break;
      }
      case ContinuationRecord::kArrayResume: {
        result = SerializeFixedArray<PACKED_ELEMENTS>(
            cont.array_elements(), cont.array_index(), cont.array_length());
        break;
      }
      case ContinuationRecord::kArrayResume_Holey: {
        result = SerializeFixedArray<HOLEY_ELEMENTS>(
            cont.array_elements(), cont.array_index(), cont.array_length());
        break;
      }
      case ContinuationRecord::kArrayResume_WithInterrupts: {
        result = SerializeFixedArrayWithInterruptCheck<PACKED_ELEMENTS>(
            cont.array_elements(), cont.array_index(), cont.array_length());
        break;
      }
      case ContinuationRecord::kArrayResume_Holey_WithInterrupts: {
        result = SerializeFixedArrayWithInterruptCheck<HOLEY_ELEMENTS>(
            cont.array_elements(), cont.array_index(), cont.array_length());
        break;
      }
      default:
        UNREACHABLE();
    }
    static_assert(SUCCESS == 0);
    static_assert(JS_OBJECT == 1);
    static_assert(JS_ARRAY == 2);
    if (V8_UNLIKELY(result > JS_ARRAY)) return result;
    if (stack_.empty()) return SUCCESS;
    cont = stack_.back();
    stack_.pop_back();
  }
}

template <typename Char, bool capture_mode>
FastJsonStringifierResult
FastJsonStringifier<Char, capture_mode>::HandleInterruptAndCheckCycle() {
  StackLimitCheck interrupt_check(isolate_);
  {
    // A TerminationException can actually trigger a GC when allocating the
    // message object. We don't touch any of the heap objects after we
    // encountered an exception, so this is fine.
    AllowGarbageCollection allow_gc;
    if (V8_UNLIKELY(interrupt_check.InterruptRequested() &&
                    IsException(isolate_->stack_guard()->HandleInterrupts(
                                    StackGuard::InterruptLevel::kNoGC),
                                isolate_))) {
      return EXCEPTION;
    }
  }

  if (V8_UNLIKELY(CheckCycle())) {
    // TODO(pthier): Construct exception message on fast-path and avoid falling
    // back to slow path just to handle the exception.
    return SLOW_PATH;
  }

  return SUCCESS;
}

template <typename Char, bool capture_mode>
bool FastJsonStringifier<Char, capture_mode>::CheckCycle() {
  std::unordered_set<Address> set;
  for (uint32_t i = 0; i < stack_.size(); i++) {
    ContinuationRecord rec = stack_[i];
    if (rec.type() == ContinuationRecord::kObjectKey ||
        rec.type() == ContinuationRecord::kSimpleObject)
      continue;
    Tagged<Object> obj = rec.object();
    if (V8_UNLIKELY(set.find(obj.ptr()) != set.end())) {
      return true;
    }
    set.insert(obj.ptr());
  }
  return false;
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(uint8_t))
bool FastJsonStringifier<Char, capture_mode>::AppendString(
    const SrcChar* chars, size_t length,
    const DisallowGarbageCollection& no_gc) {
  constexpr int kUseSimdLengthThreshold = 32;
  if (length >= kUseSimdLengthThreshold) {
    return AppendStringSIMD(chars, length, no_gc);
  }
  return AppendStringSWAR(chars, length, 0, 0, no_gc);
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
void FastJsonStringifier<Char, capture_mode>::AppendStringNoEscapes(
    const SrcChar* chars, size_t length,
    const DisallowGarbageCollection& no_gc) {
  buffer_.Append(chars, length);
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(uint8_t))
bool FastJsonStringifier<Char, capture_mode>::AppendStringScalar(
    const SrcChar* chars, size_t length, size_t start,
    size_t uncopied_src_index, const DisallowGarbageCollection& no_gc) {
  bool needs_escaping = false;
  for (size_t i = start; i < length; i++) {
    SrcChar c = chars[i];
    if (V8_LIKELY(DoNotEscape(c))) continue;
    needs_escaping = true;
    buffer_.Append(chars + uncopied_src_index, i - uncopied_src_index);
    AppendCStringUnchecked(&JsonEscapeTable[c * kJsonEscapeTableEntrySize]);
    uncopied_src_index = i + 1;
  }
  if (V8_LIKELY(uncopied_src_index < length)) {
    buffer_.Append(chars + uncopied_src_index, length - uncopied_src_index);
  }
  return needs_escaping;
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(uint8_t))
V8_CLANG_NO_SANITIZE("alignment")
bool FastJsonStringifier<Char, capture_mode>::AppendStringSWAR(
    const SrcChar* chars, size_t length, size_t start,
    size_t uncopied_src_index, const DisallowGarbageCollection& no_gc) {
  using PackedT = uint32_t;
  static constexpr size_t stride = sizeof(PackedT);
  size_t i = start;
  for (; i + (stride - 1) < length; i += stride) {
    PackedT packed =
        base::ReadUnalignedValue<PackedT>(reinterpret_cast<Address>(chars + i));
    if (V8_UNLIKELY(NeedsEscape(packed))) break;
  }
  return AppendStringScalar(chars, length, i, uncopied_src_index, no_gc);
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(uint8_t))
bool FastJsonStringifier<Char, capture_mode>::AppendStringSIMD(
    const SrcChar* chars, size_t length,
    const DisallowGarbageCollection& no_gc) {
  namespace hw = hwy::HWY_NAMESPACE;

  bool needs_escaping = false;
  size_t uncopied_src_index = 0;  // Index of first char not copied yet.
  const SrcChar* block = chars;
  const SrcChar* end = chars + length;
  hw::FixedTag<SrcChar, 16> tag;
  static const size_t stride = hw::Lanes(tag);

  const auto mask_0x20 = hw::Set(tag, 0x20);
  const auto mask_0x22 = hw::Set(tag, 0x22);
  const auto mask_0x5c = hw::Set(tag, 0x5c);

  for (; block + (stride - 1) < end; block += stride) {
    const auto input = hw::LoadU(tag, block);
    // TODO(floitsch): use operators for the comparisons when they are available
    // on RISC-V.
    const auto has_lower_than_0x20 = hw::Lt(input, mask_0x20);
    const auto has_0x22 = hw::Eq(input, mask_0x22);
    const auto has_0x5c = hw::Eq(input, mask_0x5c);
    const auto result = hw::Or(hw::Or(has_lower_than_0x20, has_0x22), has_0x5c);

    // No character that needs escaping found in block.
    if (V8_LIKELY(hw::AllFalse(tag, result))) continue;

    needs_escaping = true;
    size_t index = hw::FindKnownFirstTrue(tag, result);
    Char found_char = block[index];
    const size_t char_index = block - chars + index;
    const size_t copy_length = char_index - uncopied_src_index;
    buffer_.Append(chars + uncopied_src_index, copy_length);
    SBXCHECK_LT(found_char, 0x60);
    AppendCStringUnchecked(
        &JsonEscapeTable[found_char * kJsonEscapeTableEntrySize]);
    uncopied_src_index = char_index + 1;
    // Advance to character after the one that was found to need escaping.
    block += index + 1;
    // Subtract stride as it will be added again at the beginning of the loop.
    block -= stride;
  }

  // Handle remaining characters.
  const size_t start_index = block - chars;
  return AppendStringSWAR(chars, length, start_index, uncopied_src_index,
                          no_gc) ||
         needs_escaping;
}

template <typename Char, bool capture_mode>
template <typename SrcChar>
  requires(sizeof(SrcChar) == sizeof(base::uc16))
bool FastJsonStringifier<Char, capture_mode>::AppendString(
    const SrcChar* chars, size_t length,
    const DisallowGarbageCollection& no_gc) {
  namespace hw = hwy::HWY_NAMESPACE;
  hw::FixedTag<SrcChar, 8> tag;
  const size_t stride = hw::Lanes(tag);
  const auto space = hw::Set(tag, 0x20);
  const auto quote = hw::Set(tag, 0x22);
  const auto slash = hw::Set(tag, 0x5c);
  const auto surrogate_mask = hw::Set(tag, 0xf800);
  const auto surrogate = hw::Set(tag, 0xd800);
  size_t position = 0;
  bool escaped = false;
  while (length - position >= stride) {
    const auto input = hw::LoadU(tag, chars + position);
    const auto special =
        hw::Or(hw::Or(hw::Lt(input, space), hw::Eq(input, quote)),
               hw::Or(hw::Eq(input, slash),
                      hw::Eq(hw::And(input, surrogate_mask), surrogate)));
    if (hw::AllFalse(tag, special)) {
      buffer_.Append(chars + position, stride);
      position += stride;
      continue;
    }
    size_t prefix = hw::FindKnownFirstTrue(tag, special);
    size_t index = position + prefix;
    if (prefix < stride / 2 || chars[index] >= 0xd800) {
      // ponytail: dense escapes and surrogates use the shared scalar tail;
      // revisit only with a profitable vector compaction encoder.
      return buffer_.AppendEscaped(base::Vector<const SrcChar>(
                 chars + position, length - position)) ||
             escaped;
    }
    buffer_.template AppendPrefix<8>(chars + position, prefix);
    buffer_.AppendEscaped(base::Vector<const SrcChar>(chars + index, 1));
    escaped = true;
    position = index + 1;
  }
  return buffer_.AppendEscaped(base::Vector<const SrcChar>(
             chars + position, length - position)) ||
         escaped;
}

namespace {

MaybeHandle<Object> FastJsonStringify(Isolate* isolate, Handle<Object> object) {
  DisallowGarbageCollection no_gc;

  FastJsonStringifier<uint8_t> one_byte_stringifier(isolate);
  std::optional<FastJsonStringifier<base::uc16>> two_byte_stringifier;
  FastJsonStringifierResult result =
      one_byte_stringifier.SerializeObject(*object, no_gc);
  bool result_is_one_byte = true;

  if (result == CHANGE_ENCODING) {
    two_byte_stringifier.emplace(isolate);
    result = two_byte_stringifier->ResumeFrom(one_byte_stringifier, no_gc);
    DCHECK_NE(result, CHANGE_ENCODING);
    result_is_one_byte = false;
  }

  if (V8_LIKELY(result == SUCCESS)) {
    if (result_is_one_byte) {
      const size_t length = one_byte_stringifier.ResultLength();
      Handle<SeqOneByteString> ret;
      {
        AllowGarbageCollection allow_gc;
        if (length > String::kMaxLength) {
          THROW_NEW_ERROR(isolate, NewInvalidStringLengthError(), String);
        }
        ASSIGN_RETURN_ON_EXCEPTION(
            isolate, ret,
            isolate->factory()->NewRawOneByteString(static_cast<int>(length)),
            Object);
      }
      one_byte_stringifier.CopyResultTo(ret->GetChars(no_gc));
      return ret;
    } else {
      DCHECK(two_byte_stringifier.has_value());
      const size_t one_byte_length = one_byte_stringifier.ResultLength();
      const size_t two_byte_length = two_byte_stringifier->ResultLength();
      const size_t total_length = one_byte_length + two_byte_length;
      Handle<SeqTwoByteString> ret;
      {
        AllowGarbageCollection allow_gc;
        if (total_length > String::kMaxLength) {
          THROW_NEW_ERROR(isolate, NewInvalidStringLengthError(), String);
        }
        ASSIGN_RETURN_ON_EXCEPTION(isolate, ret,
                                   isolate->factory()->NewRawTwoByteString(
                                       static_cast<int>(total_length)),
                                   Object);
      }
      base::uc16* chars = ret->GetChars(no_gc);
      if (one_byte_length > 0) {
        one_byte_stringifier.CopyResultTo(chars);
      }
      DCHECK_GT(two_byte_length, 0);
      two_byte_stringifier->CopyResultTo(chars + one_byte_length);
      return ret;
    }
  } else if (result == UNDEFINED) {
    return isolate->factory()->undefined_value();
  } else if (result == SLOW_PATH) {
    // TODO(pthier): Resume instead of restarting.
    AllowGarbageCollection allow_gc;

    Handle<Object> undefined = isolate->factory()->undefined_value();
    return JsonStringifySlow(isolate, object, undefined, undefined);
  }
  DCHECK(result == EXCEPTION);
  CHECK(isolate->has_exception());
  return MaybeHandle<Object>();
}

}  // namespace

}  // namespace

MaybeHandle<Object> JsonStringifyFast(Isolate* isolate, Handle<Object> value) {
  return FastJsonStringify(isolate, value);
}

MaybeHandle<Object> TryCaptureJsonStringifyFast(Isolate* isolate,
                                                Handle<Object> value,
                                                JsonStringifyCapture* capture) {
  DisallowGarbageCollection no_gc;
  FastJsonStringifier<uint8_t, true> stringifier(isolate, capture);
  auto result = stringifier.SerializeObject(*value, no_gc);
  if (result == SUCCESS) {
    stringifier.FinishCapture();
    if (capture->overflowed()) {
      AllowGarbageCollection allow_gc;
      THROW_NEW_ERROR(isolate, NewInvalidStringLengthError(), Object);
    }
    return isolate->factory()->true_value();
  }
  if (result == UNDEFINED) return isolate->factory()->undefined_value();
  DCHECK(result == SLOW_PATH || result == EXCEPTION);
  DCHECK_EQ(result == EXCEPTION, isolate->has_exception());
  return MaybeHandle<Object>();
}

}  // namespace internal
}  // namespace v8
