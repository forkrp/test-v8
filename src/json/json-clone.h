// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_JSON_JSON_CLONE_H_
#define V8_JSON_JSON_CLONE_H_

#include "src/objects/objects.h"

namespace v8 {
namespace internal {

// Non-standard JSON round trip, with direct copying for plain data trees.
V8_WARN_UNUSED_RESULT MaybeHandle<Object> JsonClone(Isolate* isolate,
                                                    Handle<Object> value);

}  // namespace internal
}  // namespace v8

#endif  // V8_JSON_JSON_CLONE_H_
