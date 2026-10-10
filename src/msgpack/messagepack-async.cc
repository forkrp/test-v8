// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "src/msgpack/messagepack-async.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#ifdef MSGPACK_RESOURCE_TIMINGS
#include <cstdio>
#endif

#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "src/base/lazy-instance.h"
#include "src/base/page-allocator.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/persistent-handles.h"
#include "src/init/v8.h"
#include "src/objects/backing-store.h"
#include "src/objects/js-array-buffer-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8::internal {
namespace {
class State;
using States = std::unordered_map<State*, std::shared_ptr<State>>;
struct DecodeBuffers {
  MessagePackBuffer input, strings;
  size_t bytes() const {
    return sizeof(*this) + input.capacity() + strings.capacity();
  }
};
struct Jobs {
  base::Mutex mutex;
  std::unordered_map<Isolate*, States> jobs;
  std::unordered_map<Isolate*, std::unique_ptr<DecodeBuffers>> buffers;
};
DEFINE_LAZY_LEAKY_OBJECT_GETTER(Jobs, GetJobs)
constexpr size_t kCachedDecodeBufferLimit = 8 * 1024 * 1024;
bool SmallResourcePlan(const MessagePackAsyncData& data) {
  const auto& input = data.input;
  if (input.size() > 65536 || input.size() < 8 ||
      std::memcmp(input.data(), "V8MR\x01\x93", 6) != 0)
    return false;
  size_t position = 6;
  auto read = [&](size_t width, uint32_t* value) {
    if (width > input.size() - position) return false;
    uint32_t n = 0;
    for (size_t i = 0; i < width; ++i) n = (n << 8) | input.data()[position++];
    *value = n;
    return true;
  };
  auto array = [&](uint32_t* count) {
    uint32_t tag;
    if (!read(1, &tag)) return false;
    if ((tag & 0xf0) == 0x90) {
      *count = tag & 15;
      return true;
    }
    return tag == 0xdc ? read(2, count) : tag == 0xdd && read(4, count);
  };
  auto string = [&]() {
    uint32_t tag, length;
    if (!read(1, &tag)) return false;
    if ((tag & 0xe0) == 0xa0)
      length = tag & 31;
    else if (tag >= 0xd9 && tag <= 0xdb) {
      if (!read(size_t{1} << (tag - 0xd9), &length)) return false;
    } else
      return false;
    if (length > input.size() - position) return false;
    position += length;
    return true;
  };
  uint32_t count, fields, keys = 0;
  uint32_t shape_fields[64];
  if (!array(&count) || count > 64) return false;
  uint32_t shapes = count;
  for (uint32_t i = 0; i < count; ++i) {
    if (!array(&fields) || fields > 64 || keys + fields > 512) return false;
    keys += fields;
    shape_fields[i] = fields;
    for (uint32_t j = 0; j < fields; ++j)
      if (!string()) return false;
  }
  if (!array(&count) || count > 256) return false;
  uint32_t strings = count;
  for (uint32_t i = 0; i < count; ++i)
    if (!string()) return false;
  uint32_t work = keys + strings;
  auto value = [&](auto&& self, size_t depth) -> bool {
    if (depth > 32 || work >= 32768) return false;
    ++work;
    uint32_t tag;
    if (!read(1, &tag)) return false;
    if ((tag & 0xe0) == 0xa0 || (tag >= 0xd9 && tag <= 0xdb)) {
      --position;
      return string();
    }
    if (tag <= 0x7f || tag >= 0xe0 || tag == 0xc0 || tag == 0xc2 || tag == 0xc3)
      return true;
    if (tag >= 0xca && tag <= 0xd3) {
      uint32_t ignored;
      return read(tag == 0xca   ? 4
                  : tag == 0xcb ? 8
                                : size_t{1} << ((tag - 0xcc) & 3),
                  &ignored);
    }
    bool map = (tag & 0xf0) == 0x80 || tag == 0xde || tag == 0xdf;
    bool list = (tag & 0xf0) == 0x90 || tag == 0xdc || tag == 0xdd;
    uint32_t size;
    if (map || list) {
      work += map ? 7 : 3;
      if ((tag & 0xf0) == (map ? 0x80 : 0x90))
        size = tag & 15;
      else if (!read(tag == 0xdc || tag == 0xde ? 2 : 4, &size))
        return false;
      if (size > 32768 - std::min(work, uint32_t{32768})) return false;
      for (uint32_t i = 0; i < size; ++i) {
        if (map && !self(self, depth + 1)) return false;
        if (!self(self, depth + 1)) return false;
      }
      return work <= 32768;
    }
    if ((tag >= 0xc7 && tag <= 0xc9) || (tag >= 0xd4 && tag <= 0xd8)) {
      if (tag >= 0xd4)
        size = uint32_t{1} << (tag - 0xd4);
      else if (!read(size_t{1} << (tag - 0xc7), &size))
        return false;
      uint32_t type;
      if (!read(1, &type) || size > input.size() - position) return false;
      size_t limit = position + size;
      auto integer = [&](uint32_t* n) {
        uint32_t t;
        if (!read(1, &t)) return false;
        if (t <= 127) {
          *n = t;
          return true;
        }
        return t >= 0xcc && t <= 0xce && read(size_t{1} << (t - 0xcc), n);
      };
      if (type == 0x51) {
        uint32_t n;
        if (!integer(&n) || n >= strings) return false;
      } else if (type == 0x50) {
        uint32_t fields, index;
        if (!array(&fields) || fields != 2 || !integer(&index) ||
            index >= shapes || !array(&fields) || fields != shape_fields[index])
          return false;
        work += 7;
        for (uint32_t i = 0; i < fields; ++i)
          if (!self(self, depth + 1)) return false;
      } else
        return false;
      return position == limit && work <= 32768;
    }
    return false;
  };
  return value(value, 0) && position == input.size();
}
void BorrowDecodeBuffers(Isolate* isolate, MessagePackAsyncData* data,
                         size_t length) {
  size_t bytes = 0;
  {
    auto* jobs = GetJobs();
    base::MutexGuard lock(&jobs->mutex);
    auto it = jobs->buffers.find(isolate);
    if (it == jobs->buffers.end()) return;
    auto& buffers = *it->second;
    if (buffers.input.capacity() >= length) {
      bytes += buffers.input.capacity();
      buffers.input.Swap(&data->input);
      data->input.Clear();
    }
    if (buffers.strings.capacity() &&
        buffers.strings.capacity() <= length * 2) {
      bytes += buffers.strings.capacity();
      buffers.strings.Swap(&data->decoded_strings);
      data->decoded_strings.Clear();
    }
  }
  // Transfer accounting to the pending State outside the registry mutex.
  if (bytes)
    reinterpret_cast<v8::Isolate*>(isolate)
        ->AdjustAmountOfExternalAllocatedMemory(-static_cast<int64_t>(bytes));
}
void RecycleDecodeBuffers(Isolate* isolate, MessagePackBuffer* input,
                          MessagePackBuffer* strings) {
  int64_t delta = 0;
  try {
    auto* jobs = GetJobs();
    base::MutexGuard lock(&jobs->mutex);
    bool eligible = (input->capacity() >= 32768 &&
                     input->capacity() <= kCachedDecodeBufferLimit) ||
                    (strings->capacity() >= 32768 &&
                     strings->capacity() <= kCachedDecodeBufferLimit);
    if (eligible) {
      auto it = jobs->buffers.find(isolate);
      size_t before = it == jobs->buffers.end() ? 0 : it->second->bytes();
      if (it == jobs->buffers.end()) {
        auto cache = std::make_unique<DecodeBuffers>();
        it = jobs->buffers.emplace(isolate, std::move(cache)).first;
      }
      auto& entry = it->second;
      auto retain = [](MessagePackBuffer* buffer, MessagePackBuffer* cached) {
        if (buffer->capacity() >= 32768 &&
            buffer->capacity() <= kCachedDecodeBufferLimit &&
            buffer->capacity() >= cached->capacity()) {
          buffer->Clear();
          buffer->Swap(cached);
        }
      };
      retain(input, &entry->input);
      retain(strings, &entry->strings);
      delta =
          static_cast<int64_t>(entry->bytes()) - static_cast<int64_t>(before);
    }
  } catch (const std::bad_alloc&) {
    // Reuse is optional; an allocation failure must not escape a completion.
  }
  // Discard replaced/unretained allocations before charging the retained
  // cache. Neither a V8 allocation nor a GC runs while the mutex is held.
  {
    MessagePackBuffer discard;
    input->Swap(&discard);
  }
  {
    MessagePackBuffer discard;
    strings->Swap(&discard);
  }
  if (delta)
    reinterpret_cast<v8::Isolate*>(isolate)
        ->AdjustAmountOfExternalAllocatedMemory(delta);
}
void Reject(Isolate* isolate, Handle<JSPromise> promise, v8::TryCatch* caught) {
  DCHECK(isolate->has_exception());
  if (isolate->is_execution_terminating()) {
    caught->ReThrow();
    return;
  }
  Handle<Object> reason(isolate->exception(), isolate);
  isolate->clear_exception();
  isolate->clear_pending_message();
  caught->Reset();
  JSPromise::Reject(promise, reason, false);
}
class State {
 public:
  State(Isolate* isolate, Handle<JSPromise> promise,
        std::shared_ptr<MessagePackAsyncData> data)
      : isolate_(isolate),
        handles_(std::make_unique<PersistentHandles>(isolate)),
        context_(handles_->NewHandle(isolate->native_context())),
        promise_(handles_->NewHandle(promise)),
        result_(handles_->NewHandle(*isolate->factory()->undefined_value())),
        data_(std::move(data)) {
    Account();
  }
  ~State() { DCHECK(!handles_); }
  void Cancel() {
    data_->cancelled.store(true, std::memory_order_relaxed);
    ReleaseRoots(false);
  }
  bool Run() {
    if (!handles_) return true;
#ifdef MSGPACK_RESOURCE_TIMINGS
    uint64_t profile_begin = MessagePackProfileNow();
    if (!data_->profile_first)
      data_->profile_first = profile_begin;
    else
      data_->profile_queue += profile_begin - data_->profile_last;
#endif
    HandleScope scope(isolate_);
    SaveAndSwitchContext context(isolate_, *context_);
    v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate_));
    StackLimitCheck check(isolate_);
    if (check.InterruptRequested())
      USE(isolate_->stack_guard()->HandleInterrupts());
    Account();  // Worker writes have been published through the task runner.
#ifdef MSGPACK_RESOURCE_TIMINGS
    uint64_t profile_build = MessagePackProfileNow();
    data_->profile_account += profile_build - profile_begin;
#endif
    bool done = true;
    if (!isolate_->has_exception() && data_->error.empty()) {
      if (data_->encode) {
        Handle<Object> bytes = MessagePackOutput(
            isolate_, data_->parts.empty() ? &data_->input : &data_->output);
        if (!bytes.is_null()) result_.PatchValue(*bytes);
      } else
        done = BuildMessagePackSlice(isolate_, data_.get(), handles_.get(),
                                     result_, &cursor_, &frames_,
                                     &resource_state_);
    }
#ifdef MSGPACK_RESOURCE_TIMINGS
    uint64_t profile_built = MessagePackProfileNow();
    data_->profile_build += profile_built - profile_build;
    ++data_->profile_slices;
#endif
    if (!data_->encode) {
      data_->tokens.DiscardBefore(cursor_);
      Account();
    }
#ifdef MSGPACK_RESOURCE_TIMINGS
    data_->profile_last = MessagePackProfileNow();
    data_->profile_account += data_->profile_last - profile_built;
    if (done && data_->resource && !data_->encode) {
      std::fprintf(
          stderr,
          "resource async copy=%.3f setup=%.3f worker_queue=%.3f worker=%.3f "
          "foreground_queue=%.3f build=%.3f account=%.3f slices=%llu "
          "native_total=%.3f ms\n",
          data_->profile_capture / 1e6,
          (data_->profile_queued - data_->profile_start -
           data_->profile_capture) /
              1e6,
          (data_->profile_worker_start - data_->profile_queued) / 1e6,
          (data_->profile_worker_end - data_->profile_worker_start) / 1e6,
          (data_->profile_first - data_->profile_worker_end +
           data_->profile_queue) /
              1e6,
          data_->profile_build / 1e6, data_->profile_account / 1e6,
          static_cast<unsigned long long>(data_->profile_slices),
          (data_->profile_last - data_->profile_start) / 1e6);
    }
#endif
    if (!isolate_->has_exception() && !data_->error.empty())
      ThrowMessagePackError(isolate_, data_->error, data_->encode);
    if (isolate_->has_exception())
      Reject(isolate_, promise_, &caught);
    else if (!done)
      return false;
    else if (JSPromise::Resolve(promise_, result_).is_null())
      Reject(isolate_, promise_, &caught);
#ifdef MSGPACK_RESOURCE_TIMINGS
    bool profile_cleanup = data_->resource && !data_->encode;
    uint64_t profile_end = data_->profile_last;
    uint64_t profile_start = data_->profile_start;
#endif
    bool recycle_buffers = data_->resource && !data_->encode;
    ReleaseRoots(recycle_buffers);
#ifdef MSGPACK_RESOURCE_TIMINGS
    if (profile_cleanup) {
      uint64_t end = MessagePackProfileNow();
      std::fprintf(stderr,
                   "resource cleanup settle_release=%.3f total=%.3f ms\n",
                   (end - profile_end) / 1e6, (end - profile_start) / 1e6);
    }
#endif
    {
      auto* jobs = GetJobs();
      base::MutexGuard lock(&jobs->mutex);
      auto it = jobs->jobs.find(isolate_);
      DCHECK(it != jobs->jobs.end());
      it->second.erase(this);
      if (it->second.empty()) jobs->jobs.erase(it);
    }
    if (recycle_buffers)
      RecycleDecodeBuffers(isolate_, &recycled_input_, &recycled_strings_);
    return true;
  }

 private:
  void Account() {
    int64_t bytes = sizeof(*this) + static_cast<int64_t>(data_->MemoryUsage());
    if (resource_state_)
      bytes += static_cast<int64_t>(data_->resource_metadata);
    int64_t delta = bytes - accounted_;
    accounted_ = bytes;
    if (delta)
      reinterpret_cast<v8::Isolate*>(isolate_)
          ->AdjustAmountOfExternalAllocatedMemory(delta);
  }
  void ReleaseRoots(bool recycle) {
    if (!handles_) return;
    frames_.clear();
    resource_state_.reset();
    handles_.reset();
    if (recycle && data_->resource && !data_->encode) {
      data_->input.Swap(&recycled_input_);
      data_->decoded_strings.Swap(&recycled_strings_);
    }
    data_.reset();
    reinterpret_cast<v8::Isolate*>(isolate_)
        ->AdjustAmountOfExternalAllocatedMemory(-accounted_);
    accounted_ = 0;
  }
  Isolate* const isolate_;
  std::unique_ptr<PersistentHandles> handles_;
  Handle<NativeContext> context_;
  Handle<JSPromise> promise_;
  Handle<Object> result_;
  std::shared_ptr<MessagePackAsyncData> data_;
  std::vector<MessagePackBuildFrame> frames_;
  std::shared_ptr<void> resource_state_;
  uint32_t cursor_ = 0;
  int64_t accounted_ = 0;
  MessagePackBuffer recycled_input_, recycled_strings_;
};
class Completion final : public CancelableTask {
 public:
  Completion(Isolate* isolate, std::shared_ptr<State> state,
             std::shared_ptr<v8::TaskRunner> runner)
      : CancelableTask(isolate),
        isolate_(isolate),
        state_(std::move(state)),
        runner_(std::move(runner)) {}
  void RunInternal() override {
    v8::Isolate::Scope scope(reinterpret_cast<v8::Isolate*>(isolate_));
    if (!state_->Run())
      runner_->PostNonNestableTask(
          std::make_unique<Completion>(isolate_, state_, runner_));
  }

 private:
  Isolate* const isolate_;
  std::shared_ptr<State> state_;
  std::shared_ptr<v8::TaskRunner> runner_;
};
class Worker final : public CancelableTask {
 public:
  Worker(Isolate* isolate, std::shared_ptr<MessagePackAsyncData> data,
         std::shared_ptr<v8::TaskRunner> runner,
         std::unique_ptr<v8::Task> completion)
      : CancelableTask(isolate),
        data_(std::move(data)),
        runner_(std::move(runner)),
        completion_(std::move(completion)) {}
  void RunInternal() override {
#ifdef MSGPACK_RESOURCE_TIMINGS
    data_->profile_worker_start = MessagePackProfileNow();
#endif
    RunMessagePackWorker(data_.get());
#ifdef MSGPACK_RESOURCE_TIMINGS
    data_->profile_worker_end = MessagePackProfileNow();
#endif
    runner_->PostNonNestableTask(std::move(completion_));
  }

 private:
  std::shared_ptr<MessagePackAsyncData> data_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::unique_ptr<v8::Task> completion_;
};
}  // namespace
Handle<Object> MessagePackOutput(Isolate* isolate, MessagePackBuffer* bytes) {
  size_t length = bytes->size();
  size_t mapping_size;
  uint8_t* data = bytes->Release(&mapping_size);
  auto free = [](void* allocation, size_t, void* mapping) {
    size_t size = reinterpret_cast<uintptr_t>(mapping);
    if (size)
      CHECK(base::PageAllocator().FreePages(allocation, size));
    else
      std::free(allocation);
  };
  struct Guard {
    void* data;
    void* mapping;
    v8::BackingStore::DeleterCallback free;
    ~Guard() {
      if (data) free(data, 0, mapping);
    }
  } guard{data, reinterpret_cast<void*>(mapping_size), free};
  try {
    auto backing = BackingStore::WrapAllocation(
        data, length, free, guard.mapping, SharedFlag::kNotShared);
    guard.data = nullptr;  // The backing store now owns the allocation.
    Handle<JSArrayBuffer> buffer =
        isolate->factory()->NewJSArrayBuffer(std::move(backing));
    return isolate->factory()->NewJSTypedArray(kExternalUint8Array, buffer, 0,
                                               length);
  } catch (const std::bad_alloc&) {
    ThrowMessagePackError(isolate, "MessagePack native allocation failed",
                          true);
    return {};
  }
}
bool HasPendingMessagePackTasks(Isolate* isolate) {
  auto* jobs = GetJobs();
  base::MutexGuard lock(&jobs->mutex);
  return jobs->jobs.find(isolate) != jobs->jobs.end();
}
void CancelMessagePackTasks(Isolate* isolate) {
  States states;
  std::unique_ptr<DecodeBuffers> buffers;
  auto* jobs = GetJobs();
  {
    base::MutexGuard lock(&jobs->mutex);
    auto it = jobs->jobs.find(isolate);
    if (it != jobs->jobs.end()) {
      states.swap(it->second);
      jobs->jobs.erase(it);
    }
    auto cached = jobs->buffers.find(isolate);
    if (cached != jobs->buffers.end()) {
      buffers = std::move(cached->second);
      jobs->buffers.erase(cached);
    }
  }
  if (buffers) {
    size_t bytes = buffers->bytes();
    buffers.reset();
    reinterpret_cast<v8::Isolate*>(isolate)
        ->AdjustAmountOfExternalAllocatedMemory(-static_cast<int64_t>(bytes));
  }
  for (auto& entry : states) entry.second->Cancel();
}
MaybeHandle<JSPromise> MessagePackAsync(Isolate* isolate, Handle<Object> value,
                                        bool encode, bool resource) {
  auto promise = isolate->factory()->NewJSPromise();
  v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate));
  auto* platform = V8::GetCurrentPlatform();
  auto runner = platform->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  if (!runner || !runner->NonNestableTasksEnabled() ||
      !platform->NumberOfWorkerThreads()) {
    JSPromise::Reject(
        promise, isolate->factory()->NewError(MessageTemplate::kUnsupported));
    return promise;
  }
  {
    auto* jobs = GetJobs();
    base::MutexGuard lock(&jobs->mutex);
    auto it = jobs->jobs.find(isolate);
    if (it != jobs->jobs.end() && it->second.size() >= 32) {
      // Reject after unlocking: creating an error may GC.
      runner.reset();
    }
  }
  if (!runner) {
    ThrowMessagePackError(
        isolate, "MessagePack async pending job limit exceeded", encode);
    Reject(isolate, promise, &caught);
    return promise;
  }
  try {
    auto data = std::make_shared<MessagePackAsyncData>();
    data->encode = encode;
    data->resource = resource;
#ifdef MSGPACK_RESOURCE_TIMINGS
    data->profile_start = MessagePackProfileNow();
#endif
    if (encode) {
      if (!CaptureMessagePack(isolate, value, data.get())) {
        ThrowMessagePackError(isolate, data->error, true);
        Reject(isolate, promise, &caught);
        return isolate->is_execution_terminating() ? MaybeHandle<JSPromise>()
                                                   : promise;
      }
    } else {
      std::shared_ptr<BackingStore> backing;
      size_t offset, length;
      if (!GetMessagePackInput(isolate, value, &backing, &offset, &length)) {
        Reject(isolate, promise, &caught);
        return isolate->is_execution_terminating() ? MaybeHandle<JSPromise>()
                                                   : promise;
      }
      if (resource && length > 65536)
        BorrowDecodeBuffers(isolate, data.get(), length);
      std::memcpy(data->input.Append(length),
                  static_cast<const uint8_t*>(backing->buffer_start()) + offset,
                  length);
    }
#ifdef MSGPACK_RESOURCE_TIMINGS
    data->profile_capture = MessagePackProfileNow() - data->profile_start;
#endif
    // Dispatch and two task hops dominate tiny resources. Bound inline work
    // by the owned snapshot, not a mutable JS graph estimate. Promise handlers
    // still execute as microtasks and all platform/job admission checks above
    // remain in force. Larger resources retain workers and cooperative slices.
    if (resource &&
        (data->input.size() <= 256 || (!encode && SmallResourcePlan(*data)))) {
      Handle<Object> result;
      if (encode) {
        RunMessagePackWorker(data.get());
        if (data->error.empty())
          result = MessagePackOutput(
              isolate, data->parts.empty() ? &data->input : &data->output);
      } else if (!DecodeMessagePack(
                      isolate,
                      base::Vector<const uint8_t>(data->input.data(),
                                                  data->input.size()),
                      &data->error, MessagePackDecodeMode::kDirect, true)
                      .ToHandle(&result)) {
        // Decode reports either its native error or an existing V8 exception.
      }
      if (!data->error.empty() && !isolate->has_exception())
        ThrowMessagePackError(isolate, data->error, encode);
      if (result.is_null() && !isolate->has_exception())
        ThrowMessagePackError(isolate, "MessagePack native operation failed",
                              encode);
      if (isolate->has_exception())
        Reject(isolate, promise, &caught);
      else if (JSPromise::Resolve(promise, result).is_null())
        Reject(isolate, promise, &caught);
      return isolate->is_execution_terminating() ? MaybeHandle<JSPromise>()
                                                 : promise;
    }
    auto state = std::make_shared<State>(isolate, promise, data);
    // Construct tasks before registering so a native allocation failure leaves
    // no registered job or retained handles.
    std::unique_ptr<Worker> task;
    try {
      task = std::make_unique<Worker>(
          isolate, data, runner,
          std::make_unique<Completion>(isolate, state, runner));
      auto* jobs = GetJobs();
      base::MutexGuard lock(&jobs->mutex);
      jobs->jobs[isolate].emplace(state.get(), state);
    } catch (...) {
      {
        auto* jobs = GetJobs();
        base::MutexGuard lock(&jobs->mutex);
        auto it = jobs->jobs.find(isolate);
        if (it != jobs->jobs.end() && it->second.empty()) jobs->jobs.erase(it);
      }
      state->Cancel();
      throw;
    }
#ifdef MSGPACK_RESOURCE_TIMINGS
    data->profile_queued = MessagePackProfileNow();
#endif
    platform->CallOnWorkerThread(std::move(task));
    return isolate->is_execution_terminating() ? MaybeHandle<JSPromise>()
                                               : promise;
  } catch (const std::bad_alloc&) {
    ThrowMessagePackError(isolate, "MessagePack native allocation failed",
                          encode);
    Reject(isolate, promise, &caught);
    return promise;
  }
}
}  // namespace v8::internal
