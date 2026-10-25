// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api/api-inl.h"
#include "src/json/json-clone.h"
#include "src/objects/field-index-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-objects-inl.h"
#include "test/cctest/cctest.h"
#include "test/common/flag-utils.h"

TEST(JsonCloneDirectDoubleStorage) {
  FLAG_SCOPE(json_clone_fast_path);
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto value = CompileRun(
      "var record = {first: 1.25};"
      "for (var i = 0; i < 12; ++i) record['field' + i] = i + 0.25;"
      "record");
  auto source = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*value));
  auto clone = i::Handle<i::JSObject>::cast(
      i::JsonClone(internal, source).ToHandleChecked());
  CHECK_NE(*source, *clone);
  CHECK_EQ(source->map(), clone->map());  // The direct path actually ran.
  isolate->LowMemoryNotification();

  int double_fields = 0;
  int out_of_object_fields = 0;
  i::DisallowGarbageCollection no_gc;
  for (i::InternalIndex index : source->map()->IterateOwnDescriptors()) {
    auto details = source->map()->instance_descriptors()->GetDetails(index);
    CHECK(details.representation().IsDouble());
    auto field = i::FieldIndex::ForDetails(source->map(), details);
    auto before = i::HeapNumber::cast(source->RawFastPropertyAt(field));
    auto after = i::HeapNumber::cast(clone->RawFastPropertyAt(field));
    CHECK_NE(before, after);
    double original = before->value();
    CHECK_EQ(original, after->value());
    after->set_value(original + 100);
    CHECK_EQ(original, before->value());
    ++double_fields;
    if (!field.is_inobject()) ++out_of_object_fields;
  }
  CHECK_EQ(13, double_fields);
  CHECK_GT(out_of_object_fields, 0);
}

TEST(JsonCloneCopyOnWriteStorage) {
  FLAG_SCOPE(json_clone_fast_path);
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto value = CompileRun("['left', 'right', null, true]");
  auto source = i::Handle<i::JSArray>::cast(v8::Utils::OpenHandle(*value));
  CHECK_EQ(source->elements()->map(),
           i::ReadOnlyRoots(internal).fixed_cow_array_map());
  auto clone = i::Handle<i::JSArray>::cast(
      i::JsonClone(internal, source).ToHandleChecked());
  CHECK_EQ(source->map(), clone->map());
  CHECK_NE(source->elements(), clone->elements());
  CHECK_EQ(clone->elements()->map(),
           i::ReadOnlyRoots(internal).fixed_array_map());
  isolate->LowMemoryNotification();
  auto elements = i::FixedArray::cast(clone->elements());
  elements->set(0, i::Smi::FromInt(42));
  CHECK(i::IsString(i::FixedArray::cast(source->elements())->get(0)));
}

TEST(JsonClonePrototypeMapFallback) {
  FLAG_SCOPE(json_clone_fast_path);
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto value = CompileRun(
      "var proto = {number: 1.25};"
      "var inheritor = Object.create(proto); proto");
  auto source = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*value));
  CHECK(source->map()->is_prototype_map());
  auto clone = i::Handle<i::JSObject>::cast(
      i::JsonClone(internal, source).ToHandleChecked());
  CHECK(!clone->map()->is_prototype_map());
  CHECK_NE(source->map(), clone->map());
}

TEST(JsonCloneFreshIdentityHash) {
  FLAG_SCOPE(json_clone_fast_path);
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  for (const char* expression :
       {"({x: 1.25})", "[1, 2, 3]", "['left', 'right']",
        "(() => { var v = {x: 1.25};"
        "for (var i = 0; i < 12; ++i) v['out' + i] = i; return v; })()"}) {
    auto value = CompileRun(expression);
    auto source = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*value));
    auto hash = source->GetOrCreateIdentityHash(internal);
    CHECK(i::IsSmi(hash));
    auto clone = i::Handle<i::JSObject>::cast(
        i::JsonClone(internal, source).ToHandleChecked());
    CHECK_EQ(source->map(), clone->map());
    CHECK(i::IsUndefined(clone->GetIdentityHash(), internal));
    CHECK_EQ(hash, source->GetIdentityHash());
  }
}

TEST(JsonCloneTermination) {
  FLAG_SCOPE(json_clone_fast_path);
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto value = CompileRun(
      "var numbers = [];"
      "for (var i = 0; i < 20000; ++i) numbers.push(i + 0.25); numbers");
  auto source = i::Handle<i::JSArray>::cast(v8::Utils::OpenHandle(*value));
  CHECK_EQ(i::PACKED_DOUBLE_ELEMENTS, source->GetElementsKind());
  v8::TryCatch caught(isolate);
  isolate->TerminateExecution();
  CHECK(i::JsonClone(internal, source).is_null());
  CHECK(isolate->IsExecutionTerminating());
  isolate->CancelTerminateExecution();
  caught.Reset();
}
