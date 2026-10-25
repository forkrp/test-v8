// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-clone.h"

#include <cmath>

#include "src/base/small-vector.h"
#include "src/common/assert-scope.h"
#include "src/execution/isolate-inl.h"
#include "src/execution/stack-guard.h"
#include "src/flags/flags.h"
#include "src/heap/factory-inl.h"
#include "src/json/json-parser.h"
#include "src/json/json-stringifier.h"
#include "src/objects/field-index-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-objects-inl.h"
#include "src/objects/property-array-inl.h"

namespace v8 {
namespace internal {
namespace {

// This attempt never runs user code. An empty result without an exception
// means that the entire operation must restart using the JSON algorithms.
// In particular, getters and toJSON must not run once during an unsuccessful
// attempt and then again during the fallback.
class FastJsonCloner {
 public:
  explicit FastJsonCloner(Isolate* isolate)
      : isolate_(isolate),
        object_prototype_(isolate->native_context()->initial_object_prototype(),
                          isolate),
        array_prototype_(isolate->native_context()->initial_array_prototype(),
                         isolate) {}

  MaybeHandle<Object> Clone(Handle<Object> value) {
    if (IsJsonPrimitive(*value)) return value;
    if (object_prototype_->map()->may_have_interesting_properties() ||
        array_prototype_->map()->may_have_interesting_properties() ||
        !IsNull(object_prototype_->map()->prototype(), isolate_) ||
        array_prototype_->map()->prototype() != *object_prototype_) {
      return {};
    }
    return CloneValue(value);
  }

 private:
  // Keep native recursion bounded. Deeper inputs use the existing JSON path,
  // which also owns circular-reference errors and their diagnostics.
  static constexpr size_t kMaxDepth = 128;
  static constexpr uint32_t kInterruptInterval = 1024;

  bool IsJsonPrimitive(Tagged<Object> value) const {
    if (IsSmi(value) || IsString(value) || IsBoolean(value) ||
        IsNull(value, isolate_)) {
      return true;
    }
    if (!IsHeapNumber(value)) return false;
    double number = HeapNumber::cast(value)->value();
    // These values require JSON normalization, possibly changing a field's
    // representation. Let the fallback build the appropriate result map.
    return std::isfinite(number) && !(number == 0 && std::signbit(number));
  }

  bool CheckInterrupt() {
    if (++work_ < kInterruptInterval) return true;
    work_ = 0;
    StackLimitCheck check(isolate_);
    return !check.InterruptRequested() ||
           !IsException(isolate_->stack_guard()->HandleInterrupts(
                            StackGuard::InterruptLevel::kNoGC),
                        isolate_);
  }

  Handle<JSObject> CopyContainer(Handle<JSObject> source) {
    Handle<JSObject> clone = isolate_->factory()->CopyJSObject(source);
    // CopyJSObject also copies an existing identity hash. Reusing that hash
    // for every clone would create avoidable collisions in Map/Set tables.
    Tagged<PropertyArray> properties = clone->property_array();
    if (properties->length() == 0) {
      clone->set_raw_properties_or_hash(
          ReadOnlyRoots(isolate_).empty_fixed_array(), kRelaxedStore);
    } else {
      properties->SetHash(PropertyArray::kNoHashSentinel);
    }
    return clone;
  }

  MaybeHandle<Object> CloneValue(Handle<Object> value) {
    HandleScope scope(isolate_);
    if (IsJsonPrimitive(*value)) return scope.CloseAndEscape(value);
    if (!IsJSObject(*value)) return {};

    StackLimitCheck check(isolate_);
    if (ancestors_.size() >= kMaxDepth || check.HasOverflowed() ||
        !CheckInterrupt()) {
      return {};
    }
    Handle<JSObject> source = Handle<JSObject>::cast(value);
    Tagged<Map> map = source->map();
    InstanceType type = map->instance_type();
    if ((type != JS_OBJECT_TYPE && type != JS_ARRAY_TYPE) ||
        IsCustomElementsReceiverMap(map) || map->has_named_interceptor() ||
        map->is_access_check_needed() || map->is_deprecated() ||
        map->is_prototype_map() || map->is_undetectable() ||
        !map->is_extensible() || !source->HasFastProperties() ||
        source->GetEmbedderFieldCount() != 0 ||
        map->may_have_interesting_properties()) {
      return {};
    }
    for (Handle<JSObject> ancestor : ancestors_) {
      if (*ancestor == *source) return {};
    }

    ancestors_.push_back(source);
    MaybeHandle<Object> result = type == JS_ARRAY_TYPE
                                     ? CloneArray(Handle<JSArray>::cast(source))
                                     : CloneObject(source);
    ancestors_.pop_back();
    Handle<Object> clone;
    if (!result.ToHandle(&clone)) return {};
    return scope.CloseAndEscape(clone);
  }

  MaybeHandle<Object> CloneObject(Handle<JSObject> source) {
    Tagged<Map> map = source->map();
    if (map->prototype() != *object_prototype_ ||
        map->GetConstructor() !=
            isolate_->native_context()->object_function()) {
      return {};
    }
    auto roots = ReadOnlyRoots(isolate_);
    if (source->elements() != roots.empty_fixed_array() &&
        source->elements() != roots.empty_slow_element_dictionary()) {
      return {};
    }

    // Reject a known-ineligible shape before copying any of its children.
    // A late getter on a large record must not cause an avoidable full copy
    // followed by another traversal in the fallback.
    for (InternalIndex i : map->IterateOwnDescriptors()) {
      Tagged<DescriptorArray> descriptors = map->instance_descriptors();
      PropertyDetails details = descriptors->GetDetails(i);
      if (!IsString(descriptors->GetKey(i)) || details.attributes() != NONE ||
          details.kind() != PropertyKind::kData ||
          details.location() != PropertyLocation::kField) {
        return {};
      }
    }

    Handle<JSObject> clone = CopyContainer(source);
    for (InternalIndex i : source->map()->IterateOwnDescriptors()) {
      if (!CheckInterrupt()) return {};
      // Reload heap pointers after each child allocation: GC may move both
      // the objects and their descriptor array.
      PropertyDetails details =
          source->map()->instance_descriptors()->GetDetails(i);
      FieldIndex field = FieldIndex::ForDetails(source->map(), details);
      Tagged<Object> value = source->RawFastPropertyAt(field);
      if (details.representation().IsDouble()) {
        if (!IsJsonPrimitive(value)) return {};
        // CopyJSObject is shallow, including mutable double field boxes.
        // A new box is required even when the numeric value is unchanged.
        Handle<HeapNumber> number = isolate_->factory()->NewHeapNumber(
            HeapNumber::cast(value)->value());
        clone->FastPropertyAtPut(field, *number);
      } else if (!IsJsonPrimitive(value)) {
        HandleScope child_scope(isolate_);
        Handle<Object> child;
        if (!CloneValue(handle(value, isolate_)).ToHandle(&child)) return {};
        clone->FastPropertyAtPut(field, *child);
      }
    }
    return clone;
  }

  MaybeHandle<Object> CloneArray(Handle<JSArray> source) {
    Tagged<Map> map = source->map();
    ElementsKind kind = source->GetElementsKind();
    if (map->prototype() != *array_prototype_ ||
        map->GetConstructor() != isolate_->native_context()->array_function() ||
        map->NumberOfOwnDescriptors() != 1 ||
        map->instance_descriptors()
            ->GetDetails(InternalIndex(0))
            .IsReadOnly() ||
        (kind != PACKED_SMI_ELEMENTS && kind != PACKED_DOUBLE_ELEMENTS &&
         kind != PACKED_ELEMENTS) ||
        !IsSmi(source->length())) {
      return {};
    }
    const int length = Smi::ToInt(source->length());
    Handle<JSObject> clone = CopyContainer(source);
    if (length == 0 || kind == PACKED_SMI_ELEMENTS) return clone;

    if (kind == PACKED_DOUBLE_ELEMENTS) {
      Handle<FixedDoubleArray> elements(
          FixedDoubleArray::cast(clone->elements()), isolate_);
      for (int i = 0; i < length; ++i) {
        if (!CheckInterrupt()) return {};
        double number = elements->get_scalar(i);
        if (!std::isfinite(number) || (number == 0 && std::signbit(number))) {
          return {};
        }
      }
      return clone;
    }

    // Array literals may share a copy-on-write backing store. Never replace
    // child references in that store until it belongs to the clone.
    JSObject::EnsureWritableFastElements(clone);
    Handle<FixedArray> elements(FixedArray::cast(clone->elements()), isolate_);
    for (int i = 0; i < length; ++i) {
      if (!CheckInterrupt()) return {};
      Tagged<Object> value = elements->get(i);
      if (IsJsonPrimitive(value)) continue;
      // Once stored in the result array, a child's temporary handles are no
      // longer needed. Do not retain two handles per input element until the
      // entire (potentially very wide) array has been copied.
      HandleScope child_scope(isolate_);
      Handle<Object> child;
      if (!CloneValue(handle(value, isolate_)).ToHandle(&child)) return {};
      elements->set(i, *child);
    }
    return clone;
  }

  Isolate* const isolate_;
  Handle<JSObject> object_prototype_;
  Handle<JSObject> array_prototype_;
  base::SmallVector<Handle<JSObject>, 16> ancestors_;
  uint32_t work_ = 0;
};

}  // namespace

MaybeHandle<Object> JsonClone(Isolate* isolate, Handle<Object> value) {
  if (v8_flags.json_clone_fast_path) {
    DisallowJavascriptExecution no_js(isolate);
    FastJsonCloner cloner(isolate);
    Handle<Object> clone;
    if (cloner.Clone(value).ToHandle(&clone)) return clone;
    if (isolate->has_exception()) return {};
  }

  Handle<Object> undefined = isolate->factory()->undefined_value();
  Handle<Object> serialized;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate, serialized, JsonStringify(isolate, value, undefined, undefined),
      Object);
  Handle<String> text;
  // JSON.stringify(undefined/function/symbol) returns undefined. ToString and
  // parsing it preserves the SyntaxError of JSON.parse(JSON.stringify(value)).
  ASSIGN_RETURN_ON_EXCEPTION(isolate, text,
                             Object::ToString(isolate, serialized), Object);
  text = String::Flatten(isolate, text);
  return String::IsOneByteRepresentationUnderneath(*text)
             ? JsonParser<uint8_t>::Parse(isolate, text, undefined)
             : JsonParser<uint16_t>::Parse(isolate, text, undefined);
}

}  // namespace internal
}  // namespace v8
