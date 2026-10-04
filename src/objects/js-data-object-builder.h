// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_OBJECTS_JS_DATA_OBJECT_BUILDER_H_
#define V8_OBJECTS_JS_DATA_OBJECT_BUILDER_H_
#include "src/heap/factory.h"
#include "src/objects/field-type.h"
#include "src/objects/heap-number-inl.h"
#include "src/objects/lookup.h"
#include "src/objects/map-updater.h"
#include "src/objects/objects-inl.h"
#include "src/objects/transitions-inl.h"
#include "src/roots/roots.h"
#include "src/utils/boxed-float.h"
namespace v8 {
namespace internal {
class FoldedMutableHeapNumberAllocation {
 public:
  // TODO(leszeks): If allocation alignment is ever enabled, we'll need to add
  // padding fillers between heap numbers.
  static_assert(!USE_ALLOCATION_ALIGNMENT_BOOL);

  FoldedMutableHeapNumberAllocation(Isolate* isolate, int count) {
    if (count == 0) return;
    int size = count * sizeof(HeapNumber);
    raw_bytes_ = isolate->factory()->NewByteArray(size);
  }

  Handle<ByteArray> raw_bytes() const { return raw_bytes_; }

 private:
  Handle<ByteArray> raw_bytes_ = {};
};

class FoldedMutableHeapNumberAllocator {
 public:
  FoldedMutableHeapNumberAllocator(
      Isolate* isolate, FoldedMutableHeapNumberAllocation* allocation,
      DisallowGarbageCollection& no_gc)
      : isolate_(isolate), roots_(isolate) {
    if (allocation->raw_bytes().is_null()) return;

    raw_bytes_ = allocation->raw_bytes();
    mutable_double_address_ =
        reinterpret_cast<Address>(allocation->raw_bytes()->begin());
  }

  ~FoldedMutableHeapNumberAllocator() {
    // Make all mutable HeapNumbers alive.
    if (mutable_double_address_ == 0) {
      DCHECK(raw_bytes_.is_null());
      return;
    }

    DCHECK_EQ(mutable_double_address_,
              reinterpret_cast<Address>(raw_bytes_->end()));
    // Before setting the length of mutable_double_buffer back to zero, we
    // must ensure that the sweeper is not running or has already swept the
    // object's page. Otherwise the GC can add the contents of
    // mutable_double_buffer to the free list.
    isolate_->heap()->EnsureSweepingCompletedForObject(*raw_bytes_);
    raw_bytes_->set_length(0);
  }

  Tagged<HeapNumber> AllocateNext(ReadOnlyRoots roots, Float64 value) {
    DCHECK_GE(mutable_double_address_,
              reinterpret_cast<Address>(raw_bytes_->begin()));
    Tagged<HeapObject> hn = HeapObject::FromAddress(mutable_double_address_);
    hn->set_map_after_allocation(roots.heap_number_map());
    HeapNumber::cast(hn)->set_value_as_bits(value.get_bits());
    mutable_double_address_ +=
        ALIGN_TO_ALLOCATION_ALIGNMENT(sizeof(HeapNumber));
    DCHECK_LE(mutable_double_address_,
              reinterpret_cast<Address>(raw_bytes_->end()));
    return HeapNumber::cast(hn);
  }

 private:
  Isolate* isolate_;
  ReadOnlyRoots roots_;
  Handle<ByteArray> raw_bytes_ = {};
  Address mutable_double_address_ = 0;
};

// JSDataObjectBuilder is a helper for efficiently building a data object,
// similar (in semantics and efficiency) to a JS object literal, based on
// key/value pairs.
//
// The JSDataObjectBuilder works by first trying to find the right map for the
// object, and then letting the caller stamp out the object fields linearly.
// There are several fast paths that can be fallen out of; if the builder bails
// out, then it's still possible to stamp out the object partially based on the
// last map found, and then continue with slow object setup afterward.
//
// The maps start from the object literal cache (to try to share maps with
// equivalent object literals in JS code). From there, when adding properties,
// there are several fast paths that the builder follows:
//
//   1. At construction, it can be passed an expected final map for the object
//      (e.g. cached from previous runs, or assumed from surrounding objects).
//      If given, then we first check whether the property matches the
//      entry in the DescriptorArray of the final map; if yes, then we don't
//      need to do any map transitions.
//   2. When given a property key, it looks for whether there is exactly one
//      transition away from the current map ("ExpectedTransitionTarget").
//      If yes, it tries to match against the key for this transition. The
//      expected key is passed as a hint to the current property key getter,
//      for e.g. faster internalised string materialisation.
//   3. Otherwise, it searches for whether there is any transition in the
//      current map that matches the key.
//   4. For all of the above, it checks whether the field represntation of the
//      found map matches the representation of the value. If it doesn't, it
//      migrates the map, potentially deprecating it too.
//   5. If there is no transition, it tries to allocate a new map transition,
//      bailing out if this fails.
class JSDataObjectBuilder {
 public:
  // HeapNumberMode determines whether incoming HeapNumber values will be
  // guaranteed to be uniquely owned by this object, and therefore can be used
  // directly as mutable HeapNumbers for double representation fields.
  enum HeapNumberMode {
    kNormalHeapNumbers,
    kHeapNumbersGuaranteedUniquelyOwned
  };
  JSDataObjectBuilder(Isolate* isolate, ElementsKind elements_kind,
                      int expected_named_properties,
                      Handle<Map> expected_final_map,
                      HeapNumberMode heap_number_mode)
      : isolate_(isolate),
        elements_kind_(elements_kind),
        expected_property_count_(expected_named_properties),
        heap_number_mode_(heap_number_mode),
        expected_final_map_(expected_final_map) {
    if (!TryInitializeMapFromExpectedFinalMap()) {
      InitializeMapFromZero();
    }
  }

  // Builds and returns an object whose properties are based on a property
  // iterator.
  //
  // Expects an iterator of the form:
  //
  // struct Iterator {
  //   void Advance();
  //   bool Done();
  //
  //   // Get the key of the current property, optionally returning the hinted
  //   // expected key if applicable.
  //   Handle<String> GetKey(Handle<String> expected_key_hint);
  //
  //   // Get the value of the current property. `will_revisit_value` is true
  //   // if this value will need to be revisited later via RevisitValues().
  //   Handle<Object> GetValue(bool will_revisit_value);
  //
  //   // Return an iterator over the values that were already visited by
  //   // GetValue. Might require caching those values if necessary.
  //   ValueIterator RevisitValues();
  // }
  template <typename PropertyIterator>
  Handle<JSObject> BuildFromIterator(
      PropertyIterator&& it, MaybeHandle<FixedArrayBase> maybe_elements = {}) {
    Handle<String> failed_property_add_key;
    for (; !it.Done(); it.Advance()) {
      Handle<String> property_key;
      if constexpr (requires { it.GetKnownKey(); }) {
        // Async keys are already rooted and internalized. Compare them without
        // allocating the expected-key handle used by raw-string iterators.
        if (IsOnExpectedFinalMapFastPath()) {
          property_key = it.GetKnownKey();
          if (!property_key.is_null() &&
              expected_final_map_->instance_descriptors(isolate_)->GetKey(
                  InternalIndex(current_property_index_)) == *property_key) {
            if (!TryGeneralizeFieldToValue(it.GetValue(true))) {
              failed_property_add_key = property_key;
              break;
            }
            AdvanceToNextProperty();
            continue;
          }
        }
      }
      if (!TryAddFastPropertyForValue(
              [&](Handle<String> expected_key) {
                return property_key = it.GetKey(expected_key);
              },
              [&]() { return it.GetValue(true); })) {
        failed_property_add_key = property_key;
        break;
      }
    }

    Handle<FixedArrayBase> elements;
    if (!maybe_elements.ToHandle(&elements)) {
      elements = isolate_->factory()->empty_fixed_array();
    }
    CreateAndInitialiseObject(it.RevisitValues(), elements);

    // Slow path: define remaining named properties.
    for (; !it.Done(); it.Advance()) {
      Handle<String> key;
      if (!failed_property_add_key.is_null()) {
        key = std::exchange(failed_property_add_key, {});
      } else {
        key = it.GetKey({});
      }
#ifdef DEBUG
      uint32_t index;
      DCHECK(!key->AsArrayIndex(&index));
#endif
      Handle<Object> value = it.GetValue(false);
      AddSlowProperty(key, value);
    }

    return object();
  }

  template <typename GetKeyFunction, typename GetValueFunction>
  V8_INLINE bool TryAddFastPropertyForValue(GetKeyFunction&& get_key,
                                            GetValueFunction&& get_value) {
    // The fast path is only valid as long as we haven't allocated an object
    // yet.
    DCHECK(object_.is_null());

    Handle<String> key;
    bool existing_map_found = TryFastTransitionToPropertyKey(get_key, &key);
    // Unconditionally get the value after getting the transition result.
    Handle<Object> value = get_value();
    if (existing_map_found) {
      // We found a map with a field for our value -- now make sure that field
      // is compatible with our value.
      if (!TryGeneralizeFieldToValue(value)) {
        // TODO(leszeks): Try to stay on the fast path if we just deprecate
        // here.
        return false;
      }
      AdvanceToNextProperty();
      return true;
    }

    // Try to stay on a semi-fast path (being able to stamp out the object
    // fields after creating the correct map) by manually creating the next
    // map here.

    Tagged<DescriptorArray> descriptors = map_->instance_descriptors(isolate_);
    InternalIndex descriptor_number =
        descriptors->SearchWithCache(isolate_, *key, *map_);
    if (descriptor_number.is_found()) {
      // Duplicate property, we need to bail out of even the semi-fast path
      // because we can no longer stamp out values linearly.
      return false;
    }

    if (!TransitionsAccessor::CanHaveMoreTransitions(isolate_, map_)) {
      return false;
    }

    Representation representation =
        Object::OptimalRepresentation(*value, isolate_);
    Handle<FieldType> type =
        Object::OptimalType(*value, isolate_, representation);
    MaybeHandle<Map> maybe_map = Map::CopyWithField(
        isolate_, map_, key, type, NONE, PropertyConstness::kConst,
        representation, INSERT_TRANSITION);
    Handle<Map> next_map;
    if (!maybe_map.ToHandle(&next_map)) return false;
    if (next_map->is_dictionary_map()) return false;

    map_ = next_map;
    if (representation.IsDouble()) {
      RegisterFieldNeedsFreshHeapNumber(value);
    }
    AdvanceToNextProperty();
    return true;
  }

  template <typename ValueIterator>
  V8_INLINE void CreateAndInitialiseObject(ValueIterator value_it,
                                           Handle<FixedArrayBase> elements) {
    // We've created a map for the first `i` property stack values (which might
    // be all of them). We need to write these properties to a newly allocated
    // object.
    DCHECK(object_.is_null());

    if (current_property_index_ < property_count_in_expected_final_map_) {
      // If we were on the expected map fast path all the way, but never reached
      // the expected final map itself, then finalize the map by rewinding to
      // the one whose property is the actual current property index.
      //
      // TODO(leszeks): Do we actually want to use the final map fast path when
      // we know that the current map _can't_ reach the final map? Will we even
      // hit this case given that we check for matching instance size?
      RewindExpectedFinalMapFastPathToBeforeCurrent();
    }

    if (map_->is_dictionary_map()) {
      // It's only safe to emit a dictionary map when we've not set up any
      // properties, as the caller assumes it can set up the first N properties
      // as fast data properties.
      DCHECK_EQ(current_property_index_, 0);

      Handle<JSObject> object = isolate_->factory()->NewSlowJSObjectFromMap(
          map_, expected_property_count_);
      object->set_elements(*elements);
      object_ = object;
      return;
    }

    // The map should have as many own descriptors as the number of properties
    // we've created so far...
    DCHECK_EQ(current_property_index_, map_->NumberOfOwnDescriptors());

    // ... and all of those properties should be in-object data properties.
    DCHECK_EQ(current_property_index_,
              map_->GetInObjectProperties() - map_->UnusedInObjectProperties());

    // Create a folded mutable HeapNumber allocation area before allocating the
    // object -- this ensures that there is no allocation between the object
    // allocation and its initial fields being initialised, where the verifier
    // would see invalid double field state.
    FoldedMutableHeapNumberAllocation hn_allocation(isolate_,
                                                    extra_heap_numbers_needed_);

    // Allocate the object then immediately start a no_gc scope -- again, this
    // is so the verifier doesn't see invalid double field state.
    Handle<JSObject> object = isolate_->factory()->NewJSObjectFromMap(map_);
    DisallowGarbageCollection no_gc;
    Tagged<JSObject> raw_object = *object;

    raw_object->set_elements(*elements);
    Tagged<DescriptorArray> descriptors =
        raw_object->map()->instance_descriptors();

    WriteBarrierMode mode = raw_object->GetWriteBarrierMode(no_gc);
    FoldedMutableHeapNumberAllocator hn_allocator(isolate_, &hn_allocation,
                                                  no_gc);

    ReadOnlyRoots roots(isolate_);

    // Initialize the in-object properties up to the last added property.
    int current_property_offset = raw_object->GetInObjectPropertyOffset(0);
    for (int i = 0; i < current_property_index_; ++i, ++value_it) {
      InternalIndex descriptor_index(i);
      Tagged<Object> value = **value_it;

      // See comment in RegisterFieldNeedsFreshHeapNumber, we need to allocate
      // HeapNumbers for double representation fields when we can't make
      // existing HeapNumbers mutable, or when we only have a Smi value.
      if (heap_number_mode_ != kHeapNumbersGuaranteedUniquelyOwned ||
          IsSmi(value)) {
        PropertyDetails details = descriptors->GetDetails(descriptor_index);
        if (details.representation().IsDouble()) {
          value = hn_allocator.AllocateNext(
              roots, Float64(static_cast<double>(Smi::cast(value).value())));
        }
      }

      DCHECK(FieldIndex::ForPropertyIndex(object->map(), i).is_inobject());
      DCHECK_EQ(current_property_offset,
                FieldIndex::ForPropertyIndex(object->map(), i).offset());
      DCHECK_EQ(current_property_offset,
                object->map()->GetInObjectPropertyOffset(i));
      FieldIndex index = FieldIndex::ForInObjectOffset(current_property_offset,
                                                       FieldIndex::kTagged);
      raw_object->RawFastInobjectPropertyAtPut(index, value, mode);
      current_property_offset += kTaggedSize;
    }
    DCHECK_EQ(current_property_offset, object->map()->GetInObjectPropertyOffset(
                                           current_property_index_));

    object_ = object;
  }

  void AddSlowProperty(Handle<String> key, Handle<Object> value) {
    DCHECK(!object_.is_null());

    LookupIterator it(isolate_, object_, key, object_, LookupIterator::OWN);
    JSObject::DefineOwnPropertyIgnoreAttributes(&it, value, NONE).Check();
  }

  Handle<JSObject> object() {
    DCHECK(!object_.is_null());
    return object_;
  }

 private:
  template <typename GetKeyFunction>
  V8_INLINE bool TryFastTransitionToPropertyKey(GetKeyFunction&& get_key,
                                                Handle<String>* key_out) {
    Handle<String> expected_key;
    Handle<Map> target_map;

    InternalIndex descriptor_index(current_property_index_);
    if (IsOnExpectedFinalMapFastPath()) {
      expected_key = handle(
          String::cast(
              expected_final_map_->instance_descriptors(isolate_)->GetKey(
                  descriptor_index)),
          isolate_);
      target_map = expected_final_map_;
    } else {
      TransitionsAccessor transitions(isolate_, *map_);
      expected_key = transitions.ExpectedTransitionKey();
      if (!expected_key.is_null()) {
        // Directly read out the target while reading out the key, otherwise it
        // might die if `get_key` can allocate.
        target_map =
            TransitionsAccessor(isolate_, *map_).ExpectedTransitionTarget();
      }
    }

    Handle<String> key = *key_out = get_key(expected_key);
    if (key.is_identical_to(expected_key)) {
      // We were successful and we are done.
      DCHECK_EQ(target_map->instance_descriptors()
                    ->GetDetails(descriptor_index)
                    .location(),
                PropertyLocation::kField);
      map_ = target_map;
      return true;
    }

    if (IsOnExpectedFinalMapFastPath()) {
      // We were on the expected map fast path, but this missed that fast
      // path, so rewind the optimistic setting of the current map and disable
      // this fast path.
      RewindExpectedFinalMapFastPathToBeforeCurrent();
      property_count_in_expected_final_map_ = 0;
    }

    MaybeHandle<Map> maybe_target =
        TransitionsAccessor(isolate_, *map_).FindTransitionToField(key);
    if (!maybe_target.ToHandle(&target_map)) return false;

    map_ = target_map;
    return true;
  }

  V8_INLINE bool TryGeneralizeFieldToValue(Handle<Object> value) {
    DCHECK_LT(current_property_index_, map_->NumberOfOwnDescriptors());

    InternalIndex descriptor_index(current_property_index_);
    PropertyDetails current_details =
        map_->instance_descriptors(isolate_)->GetDetails(descriptor_index);
    Representation expected_representation = current_details.representation();

    DCHECK_EQ(current_details.kind(), PropertyKind::kData);
    DCHECK_EQ(current_details.location(), PropertyLocation::kField);

    if (!Object::FitsRepresentation(*value, expected_representation)) {
      Representation representation =
          Object::OptimalRepresentation(*value, isolate_);
      representation = representation.generalize(expected_representation);
      if (!expected_representation.CanBeInPlaceChangedTo(representation)) {
        // Reconfigure the map for the value, deprecating if necessary. This
        // will only happen for double representation fields.
        if (IsOnExpectedFinalMapFastPath()) {
          // If we're on the fast path, we will have advanced the current map
          // all the way to the final expected map. Make sure to rewind to the
          // "real" current map if this happened.
          //
          // An alternative would be to deprecate the expected final map,
          // migrate it to the new representation, and stay on the fast path.
          // However, this would mean allocating all-new maps (with the new
          // representation) all the way between the current map and the new
          // expected final map; if we later fall off the fast path anyway, then
          // all those newly allocated maps will end up unused.
          RewindExpectedFinalMapFastPathToIncludeCurrent();
          property_count_in_expected_final_map_ = 0;
        }
        MapUpdater mu(isolate_, map_);
        Handle<Map> new_map = mu.ReconfigureToDataField(
            descriptor_index, current_details.attributes(),
            current_details.constness(), representation,
            FieldType::Any(isolate_));

        // We only want to stay on the fast path if we got a fast map.
        if (new_map->is_dictionary_map()) return false;
        map_ = new_map;
        DCHECK(representation.IsDouble());
        RegisterFieldNeedsFreshHeapNumber(value);
      } else {
        // Do the in-place reconfiguration.
        DCHECK(!representation.IsDouble());
        Handle<FieldType> value_type =
            Object::OptimalType(*value, isolate_, representation);
        MapUpdater::GeneralizeField(isolate_, map_, descriptor_index,
                                    current_details.constness(), representation,
                                    value_type);
      }
    } else if (expected_representation.IsHeapObject() &&
               !FieldType::NowContains(
                   map_->instance_descriptors(isolate_)->GetFieldType(
                       descriptor_index),
                   value)) {
      Handle<FieldType> value_type =
          Object::OptimalType(*value, isolate_, expected_representation);
      MapUpdater::GeneralizeField(isolate_, map_, descriptor_index,
                                  current_details.constness(),
                                  expected_representation, value_type);
    } else if (expected_representation.IsDouble()) {
      RegisterFieldNeedsFreshHeapNumber(value);
    }

    DCHECK(FieldType::NowContains(
        map_->instance_descriptors(isolate_)->GetFieldType(descriptor_index),
        value));
    return true;
  }

  bool TryInitializeMapFromExpectedFinalMap() {
    if (expected_final_map_.is_null()) return false;
    if (expected_final_map_->elements_kind() != elements_kind_) return false;

    int property_count_in_expected_final_map =
        expected_final_map_->NumberOfOwnDescriptors();
    if (property_count_in_expected_final_map < expected_property_count_)
      return false;

    map_ = expected_final_map_;
    property_count_in_expected_final_map_ =
        property_count_in_expected_final_map;
    return true;
  }

  void InitializeMapFromZero() {
    // Must be called before any properties are registered.
    DCHECK_EQ(current_property_index_, 0);

    map_ = isolate_->factory()->ObjectLiteralMapFromCache(
        isolate_->native_context(), expected_property_count_);
    if (elements_kind_ == DICTIONARY_ELEMENTS) {
      map_ = Map::AsElementsKind(isolate_, map_, elements_kind_);
    } else {
      DCHECK_EQ(map_->elements_kind(), elements_kind_);
    }
  }

  V8_INLINE bool IsOnExpectedFinalMapFastPath() const {
    DCHECK_IMPLIES(property_count_in_expected_final_map_ > 0,
                   !expected_final_map_.is_null());
    return current_property_index_ < property_count_in_expected_final_map_;
  }

  void RewindExpectedFinalMapFastPathToBeforeCurrent() {
    DCHECK_GT(property_count_in_expected_final_map_, 0);
    if (current_property_index_ == 0) {
      InitializeMapFromZero();
      DCHECK_EQ(0, map_->NumberOfOwnDescriptors());
    }
    if (current_property_index_ == 0) {
      return;
    }
    DCHECK_EQ(*map_, *expected_final_map_);
    map_ = handle(map_->FindFieldOwner(
                      isolate_, InternalIndex(current_property_index_ - 1)),
                  isolate_);
  }

  void RewindExpectedFinalMapFastPathToIncludeCurrent() {
    DCHECK_EQ(*map_, *expected_final_map_);
    map_ = handle(expected_final_map_->FindFieldOwner(
                      isolate_, InternalIndex(current_property_index_)),
                  isolate_);
  }

  V8_INLINE void RegisterFieldNeedsFreshHeapNumber(Handle<Object> value) {
    // We need to allocate a new HeapNumber for double representation fields if
    // the HeapNumber values is not guaranteed to be uniquely owned by this
    // object (and therefore can't be made mutable), or if the value is a Smi
    // and there is no HeapNumber box for this value yet at all.
    if (heap_number_mode_ == kHeapNumbersGuaranteedUniquelyOwned &&
        !IsSmi(*value)) {
      DCHECK(IsHeapNumber(*value));
      return;
    }
    extra_heap_numbers_needed_++;
  }

  V8_INLINE void AdvanceToNextProperty() { current_property_index_++; }

  Isolate* isolate_;
  ElementsKind elements_kind_;
  int expected_property_count_;
  HeapNumberMode heap_number_mode_;

  Handle<Map> map_;
  int current_property_index_ = 0;
  int extra_heap_numbers_needed_ = 0;

  Handle<JSObject> object_;

  Handle<Map> expected_final_map_ = {};
  int property_count_in_expected_final_map_ = 0;
};

}  // namespace internal
}  // namespace v8
#endif  // V8_OBJECTS_JS_DATA_OBJECT_BUILDER_H_
