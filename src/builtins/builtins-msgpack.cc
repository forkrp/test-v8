// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cstdlib>

#include "src/base/page-allocator.h"
#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/msgpack/messagepack.h"
#include "src/objects/backing-store.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/objects-inl.h"

namespace v8 {
namespace internal {
namespace {
bool ResourceError(const std::string& message) {
  return message.find("limit exceeded") != std::string::npos ||
         message == "MessagePack native allocation failed";
}
Tagged<Object> CodecFailure(Isolate* isolate, const std::string& message,
                            bool encode) {
  if (isolate->has_exception()) return ReadOnlyRoots(isolate).exception();
  Handle<String> text =
      isolate->factory()->NewStringFromAsciiChecked(message.c_str());
  if (ResourceError(message)) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewRangeError(MessageTemplate::kPlaceholderOnly, text));
  }
  if (encode) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kPlaceholderOnly, text));
  }
  THROW_NEW_ERROR_RETURN_FAILURE(
      isolate, NewSyntaxError(MessageTemplate::kPlaceholderOnly, text));
}
void FreeMessagePackBytes(void* data, size_t, void* mapping_size) {
  size_t size = reinterpret_cast<uintptr_t>(mapping_size);
  if (size)
    CHECK(base::PageAllocator().FreePages(data, size));
  else
    std::free(data);
}
}  // namespace

BUILTIN(MsgpackEncode) {
  HandleScope scope(isolate);
#if defined(V8_TARGET_OS_ANDROID) && defined(V8_TARGET_ARCH_ARM)
  // Large malloc buffers released in sweeping batches trigger expensive
  // Scudo32 page-release scans. Dedicated mappings avoid that allocator path;
  // each output remains independently owned and is freed when its store dies.
  MessagePackBuffer bytes(true);
#else
  MessagePackBuffer bytes;
#endif
  std::string error;
  // A float32 tag is used only when widening exactly reproduces the Number.
  // Other values keep float64, including all NaNs and non-representable values.
  if (!EncodeMessagePack(isolate, args.atOrUndefined(isolate, 1), &bytes,
                         &error, true))
    return CodecFailure(isolate, error, true);
  size_t length = bytes.size();
  size_t mapping_size;
  uint8_t* data = bytes.Release(&mapping_size);
  std::shared_ptr<BackingStore> backing = BackingStore::WrapAllocation(
      data, length, FreeMessagePackBytes, reinterpret_cast<void*>(mapping_size),
      SharedFlag::kNotShared);
  Handle<JSArrayBuffer> buffer =
      isolate->factory()->NewJSArrayBuffer(std::move(backing));
  return *isolate->factory()->NewJSTypedArray(kExternalUint8Array, buffer, 0,
                                              length);
}

BUILTIN(MsgpackDecode) {
  HandleScope scope(isolate);
  Handle<Object> input = args.atOrUndefined(isolate, 1);
  Handle<JSArrayBuffer> buffer;
  size_t offset = 0;
  size_t length = 0;
  if (IsJSArrayBuffer(*input)) {
    buffer = Handle<JSArrayBuffer>::cast(input);
    length = buffer->GetByteLength();
  } else if (IsJSTypedArray(*input)) {
    Handle<JSTypedArray> view = Handle<JSTypedArray>::cast(input);
    if (view->IsDetachedOrOutOfBounds())
      return CodecFailure(isolate,
                          "Detached or out-of-bounds MessagePack input", true);
    // On-heap typed arrays must be externalized before decoding can allocate
    // or move V8 objects. The retained backing store then has a stable address.
    buffer = view->GetBuffer();
    offset = view->byte_offset();
    length = view->GetByteLength();
  } else if (IsJSArrayBufferView(*input)) {
    Handle<JSArrayBufferView> view = Handle<JSArrayBufferView>::cast(input);
    buffer = handle(JSArrayBuffer::cast(view->buffer()), isolate);
    offset = view->byte_offset();
    if (IsJSRabGsabDataView(*view)) {
      Handle<JSRabGsabDataView> data = Handle<JSRabGsabDataView>::cast(view);
      if (data->IsOutOfBounds())
        return CodecFailure(isolate, "Out-of-bounds MessagePack input", true);
      length = data->GetByteLength();
    } else {
      length = view->byte_length();
    }
  } else {
    return CodecFailure(isolate,
                        "MSGPACK.decode requires an ArrayBuffer or view", true);
  }
  if (buffer->was_detached() || buffer->is_shared())
    return CodecFailure(
        isolate, "Detached or shared MessagePack input is unsupported", true);
  if (length > 256 * 1024 * 1024)
    return CodecFailure(isolate, "MessagePack input limit exceeded", false);
  if (!length) return CodecFailure(isolate, "Empty MessagePack input", false);
  std::shared_ptr<BackingStore> backing = buffer->GetBackingStore();
  if (!backing || offset > backing->byte_length() ||
      length > backing->byte_length() - offset)
    return CodecFailure(isolate, "Out-of-bounds MessagePack input", true);
  const uint8_t* bytes =
      static_cast<const uint8_t*>(backing->buffer_start()) + offset;
  std::string error;
  Handle<Object> value;
  if (!DecodeMessagePack(isolate, base::Vector<const uint8_t>(bytes, length),
                         &error)
           .ToHandle(&value))
    return CodecFailure(isolate, error, false);
  return *value;
}
}  // namespace internal
}  // namespace v8
