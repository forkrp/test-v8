// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/msgpack/messagepack-async.h"
#include "src/msgpack/messagepack.h"
#include "src/objects/backing-store.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-promise.h"
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
  ThrowMessagePackError(isolate, message, encode);
  return ReadOnlyRoots(isolate).exception();
}
}  // namespace

void ThrowMessagePackError(Isolate* isolate, const std::string& message,
                           bool encode) {
  if (isolate->has_exception()) return;
  auto text = isolate->factory()->NewStringFromAsciiChecked(message.c_str());
  Handle<JSObject> error =
      ResourceError(message) ? isolate->factory()->NewRangeError(
                                   MessageTemplate::kPlaceholderOnly, text)
      : encode ? isolate->factory()->NewTypeError(
                     MessageTemplate::kPlaceholderOnly, text)
               : isolate->factory()->NewSyntaxError(
                     MessageTemplate::kPlaceholderOnly, text);
  isolate->Throw(*error);
}
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
  Handle<Object> result = MessagePackOutput(isolate, &bytes);
  return result.is_null() ? ReadOnlyRoots(isolate).exception() : *result;
}

namespace {
bool InputFailure(Isolate* isolate, const std::string& error, bool encode) {
  ThrowMessagePackError(isolate, error, encode);
  return false;
}
}  // namespace
bool GetMessagePackInput(Isolate* isolate, Handle<Object> input,
                         std::shared_ptr<BackingStore>* backing_out,
                         size_t* offset_out, size_t* length_out) {
  Handle<JSArrayBuffer> buffer;
  size_t offset = 0;
  size_t length = 0;
  if (IsJSArrayBuffer(*input)) {
    buffer = Handle<JSArrayBuffer>::cast(input);
    length = buffer->GetByteLength();
  } else if (IsJSTypedArray(*input)) {
    Handle<JSTypedArray> view = Handle<JSTypedArray>::cast(input);
    if (view->IsDetachedOrOutOfBounds())
      return InputFailure(isolate,
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
        return InputFailure(isolate, "Out-of-bounds MessagePack input", true);
      length = data->GetByteLength();
    } else {
      length = view->byte_length();
    }
  } else {
    return InputFailure(isolate,
                        "MSGPACK.decode requires an ArrayBuffer or view", true);
  }
  if (buffer->was_detached() || buffer->is_shared())
    return InputFailure(
        isolate, "Detached or shared MessagePack input is unsupported", true);
  if (length > 256 * 1024 * 1024)
    return InputFailure(isolate, "MessagePack input limit exceeded", false);
  if (!length) return InputFailure(isolate, "Empty MessagePack input", false);
  *backing_out = buffer->GetBackingStore();
  const auto& backing = *backing_out;
  if (!backing || offset > backing->byte_length() ||
      length > backing->byte_length() - offset)
    return InputFailure(isolate, "Out-of-bounds MessagePack input", true);
  *offset_out = offset;
  *length_out = length;
  return true;
}
BUILTIN(MsgpackDecode) {
  HandleScope scope(isolate);
  std::shared_ptr<BackingStore> backing;
  size_t offset, length;
  if (!GetMessagePackInput(isolate, args.atOrUndefined(isolate, 1), &backing,
                           &offset, &length))
    return ReadOnlyRoots(isolate).exception();
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
BUILTIN(MsgpackEncodeAsync) {
  HandleScope scope(isolate);
  Handle<JSPromise> promise;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, promise,
      MessagePackAsync(isolate, args.atOrUndefined(isolate, 1), true));
  return *promise;
}
BUILTIN(MsgpackDecodeAsync) {
  HandleScope scope(isolate);
  Handle<JSPromise> promise;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, promise,
      MessagePackAsync(isolate, args.atOrUndefined(isolate, 1), false));
  return *promise;
}
BUILTIN(MsgpackEncodeResource) {
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
  if (!EncodeMessagePackResource(isolate, args.atOrUndefined(isolate, 1),
                                 &bytes, &error))
    return CodecFailure(isolate, error, true);
  Handle<Object> result = MessagePackOutput(isolate, &bytes);
  return result.is_null() ? ReadOnlyRoots(isolate).exception() : *result;
}

BUILTIN(MsgpackDecodeResource) {
  HandleScope scope(isolate);
  std::shared_ptr<BackingStore> backing;
  size_t offset, length;
  if (!GetMessagePackInput(isolate, args.atOrUndefined(isolate, 1), &backing,
                           &offset, &length))
    return ReadOnlyRoots(isolate).exception();
  const uint8_t* bytes =
      static_cast<const uint8_t*>(backing->buffer_start()) + offset;
  std::string error;
  Handle<Object> value;
  if (!DecodeMessagePack(isolate, base::Vector<const uint8_t>(bytes, length),
                         &error, MessagePackDecodeMode::kDirect, true)
           .ToHandle(&value))
    return CodecFailure(isolate, error, false);
  return *value;
}
}  // namespace internal
}  // namespace v8
