// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "src/msgpack/messagepack-async.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

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
struct Jobs {
  base::Mutex mutex;
  std::unordered_map<Isolate*, States> jobs;
};
DEFINE_LAZY_LEAKY_OBJECT_GETTER(Jobs, GetJobs)
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
    ReleaseRoots();
  }
  bool Run() {
    if (!handles_) return true;
    HandleScope scope(isolate_);
    SaveAndSwitchContext context(isolate_, *context_);
    v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate_));
    StackLimitCheck check(isolate_);
    if (check.InterruptRequested())
      USE(isolate_->stack_guard()->HandleInterrupts());
    Account();  // Worker writes have been published through the task runner.
    bool done = true;
    if (!isolate_->has_exception() && data_->error.empty()) {
      if (data_->encode) {
        Handle<Object> bytes = MessagePackOutput(
            isolate_, data_->parts.empty() ? &data_->input : &data_->output);
        if (!bytes.is_null()) result_.PatchValue(*bytes);
      } else
        done = BuildMessagePackSlice(isolate_, data_.get(), handles_.get(),
                                     result_, &cursor_, &frames_);
    }
    if (!data_->encode) {
      data_->tokens.DiscardBefore(cursor_);
      Account();
    }
    if (!isolate_->has_exception() && !data_->error.empty())
      ThrowMessagePackError(isolate_, data_->error, data_->encode);
    if (isolate_->has_exception())
      Reject(isolate_, promise_, &caught);
    else if (!done)
      return false;
    else if (JSPromise::Resolve(promise_, result_).is_null())
      Reject(isolate_, promise_, &caught);
    ReleaseRoots();
    auto* jobs = GetJobs();
    base::MutexGuard lock(&jobs->mutex);
    auto it = jobs->jobs.find(isolate_);
    DCHECK(it != jobs->jobs.end());
    it->second.erase(this);
    if (it->second.empty()) jobs->jobs.erase(it);
    return true;
  }

 private:
  void Account() {
    int64_t bytes = sizeof(*this) + static_cast<int64_t>(data_->MemoryUsage());
    int64_t delta = bytes - accounted_;
    accounted_ = bytes;
    if (delta)
      reinterpret_cast<v8::Isolate*>(isolate_)
          ->AdjustAmountOfExternalAllocatedMemory(delta);
  }
  void ReleaseRoots() {
    if (!handles_) return;
    frames_.clear();
    handles_.reset();
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
  uint32_t cursor_ = 0;
  int64_t accounted_ = 0;
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
    RunMessagePackWorker(data_.get());
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
  auto* jobs = GetJobs();
  {
    base::MutexGuard lock(&jobs->mutex);
    auto it = jobs->jobs.find(isolate);
    if (it == jobs->jobs.end()) return;
    states.swap(it->second);
    jobs->jobs.erase(it);
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
      std::memcpy(data->input.Append(length),
                  static_cast<const uint8_t*>(backing->buffer_start()) + offset,
                  length);
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
    platform->CallOnWorkerThread(std::move(task));
    return promise;
  } catch (const std::bad_alloc&) {
    ThrowMessagePackError(isolate, "MessagePack native allocation failed",
                          encode);
    Reject(isolate, promise, &caught);
    return promise;
  }
}
}  // namespace v8::internal
