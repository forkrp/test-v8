// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-stringifier-async.h"

#include <unordered_map>

#include "include/v8-exception.h"
#include "src/base/lazy-instance.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/persistent-handles.h"
#include "src/json/json-stringifier.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

class JsonStringifyAsyncState;
namespace {

// Registry ownership keeps roots alive even if a platform drops a queued task.
// Completion or isolate teardown releases roots on the isolate thread first;
// later task destruction, on any thread, then touches only native memory.
using StringifyStates =
    std::unordered_map<JsonStringifyAsyncState*,
                       std::shared_ptr<JsonStringifyAsyncState>>;
struct JsonStringifyJobs {
  base::Mutex mutex;
  std::unordered_map<Isolate*, StringifyStates> jobs;
};
DEFINE_LAZY_LEAKY_OBJECT_GETTER(JsonStringifyJobs, GetJsonStringifyJobs)

void RejectStringifyException(Isolate* isolate, Handle<JSPromise> promise,
                              v8::TryCatch* caught) {
  DCHECK(isolate->has_exception());
  if (isolate->is_execution_terminating()) {
    caught->ReThrow();
    return;
  }
  Handle<Object> reason(isolate->exception(), isolate);
  isolate->clear_exception();
  caught->Reset();
  isolate->clear_pending_message();
  JSPromise::Reject(promise, reason, false);
}

}  // namespace

class JsonStringifyAsyncState {
 public:
  JsonStringifyAsyncState(Isolate* isolate, Handle<JSPromise> promise,
                          std::shared_ptr<JsonStringifyData> data)
      : isolate_(isolate),
        handles_(std::make_unique<PersistentHandles>(isolate)),
        context_(handles_->NewHandle(isolate->native_context())),
        promise_(handles_->NewHandle(promise)),
        data_(std::move(data)),
        accounted_(sizeof(*this) + data_->MemoryUsage()) {
    reinterpret_cast<v8::Isolate*>(isolate_)
        ->AdjustAmountOfExternalAllocatedMemory(accounted_);
  }
  ~JsonStringifyAsyncState() { DCHECK(!handles_); }

  void Cancel() {
    data_->cancelled.store(true, std::memory_order_relaxed);
    ReleaseRoots();
  }

  void Complete() {
    if (!handles_) return;
    HandleScope scope(isolate_);
    SaveAndSwitchContext context(isolate_, *context_);
    v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate_));
    // An external string can complete without entering JS or allocating a
    // normal string body. Deliver queued termination before resolving, rather
    // than relying on a later allocation/JS entry to handle the interrupt.
    StackLimitCheck interrupt_check(isolate_);
    if (interrupt_check.InterruptRequested()) {
      USE(isolate_->stack_guard()->HandleInterrupts());
    }
    if (isolate_->has_exception()) {
      RejectStringifyException(isolate_, promise_, &caught);
    } else {
      Handle<String> text;
      if (data_->overflowed) {
        isolate_->Throw(*isolate_->factory()->NewInvalidStringLengthError());
      } else if (data_->result8) {
        if (data_->result8->length() == 0) {
          text = isolate_->factory()->empty_string();
        } else if (isolate_->factory()
                       ->NewExternalStringFromOneByte(data_->result8.get())
                       .ToHandle(&text)) {
          data_->result8.release();  // Ownership now belongs to the V8 string.
        }
      } else {
        DCHECK(data_->result16);
        if (data_->result16->length() == 0) {
          text = isolate_->factory()->empty_string();
        } else if (isolate_->factory()
                       ->NewExternalStringFromTwoByte(data_->result16.get())
                       .ToHandle(&text)) {
          data_->result16.release();
        }
      }
      if (isolate_->has_exception()) {
        RejectStringifyException(isolate_, promise_, &caught);
      } else if (JSPromise::Resolve(promise_, text).is_null()) {
        RejectStringifyException(isolate_, promise_, &caught);
      }
    }
    ReleaseRoots();
    auto* jobs = GetJsonStringifyJobs();
    base::MutexGuard lock(&jobs->mutex);
    auto entry = jobs->jobs.find(isolate_);
    DCHECK(entry != jobs->jobs.end());
    entry->second.erase(this);
    if (entry->second.empty()) jobs->jobs.erase(entry);
  }

 private:
  void ReleaseRoots() {
    if (!handles_) return;
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
  std::shared_ptr<JsonStringifyData> data_;
  int64_t accounted_;
};

bool HasPendingJsonStringifyTasks(Isolate* isolate) {
  auto* jobs = GetJsonStringifyJobs();
  base::MutexGuard lock(&jobs->mutex);
  return jobs->jobs.find(isolate) != jobs->jobs.end();
}

void CancelJsonStringifyTasks(Isolate* isolate) {
  auto* jobs = GetJsonStringifyJobs();
  StringifyStates states;
  {
    base::MutexGuard lock(&jobs->mutex);
    auto entry = jobs->jobs.find(isolate);
    if (entry == jobs->jobs.end()) return;
    states.swap(entry->second);
    jobs->jobs.erase(entry);
  }
  for (auto& entry : states) entry.second->Cancel();
}

namespace {

class JsonStringifyCompletion final : public CancelableTask {
 public:
  JsonStringifyCompletion(Isolate* isolate,
                          std::shared_ptr<JsonStringifyAsyncState> state)
      : CancelableTask(isolate), isolate_(isolate), state_(std::move(state)) {}
  void RunInternal() override {
    v8::Isolate::Scope scope(reinterpret_cast<v8::Isolate*>(isolate_));
    state_->Complete();
  }

 private:
  Isolate* const isolate_;
  std::shared_ptr<JsonStringifyAsyncState> state_;
};

class JsonStringifyWorker final : public CancelableTask {
 public:
  JsonStringifyWorker(Isolate* isolate, std::shared_ptr<JsonStringifyData> data,
                      std::shared_ptr<v8::TaskRunner> runner,
                      std::unique_ptr<v8::Task> completion)
      : CancelableTask(isolate),
        data_(std::move(data)),
        runner_(std::move(runner)),
        completion_(std::move(completion)) {}
  void RunInternal() override {
    EncodeJsonStringify(data_.get());
    runner_->PostNonNestableTask(std::move(completion_));
  }

 private:
  std::shared_ptr<JsonStringifyData> data_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::unique_ptr<v8::Task> completion_;
};

}  // namespace

MaybeHandle<JSPromise> JsonStringifyAsync(Isolate* isolate,
                                          Handle<Object> value,
                                          Handle<Object> replacer,
                                          Handle<Object> gap) {
  auto promise = isolate->factory()->NewJSPromise();
  v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate));
  auto* platform = V8::GetCurrentPlatform();
  auto runner = platform->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  if (!runner || !runner->NonNestableTasksEnabled() ||
      platform->NumberOfWorkerThreads() == 0) {
    JSPromise::Reject(
        promise, isolate->factory()->NewError(MessageTemplate::kUnsupported));
    return promise;
  }
  auto data = std::make_shared<JsonStringifyData>();
  Handle<Object> captured;
  if (!CaptureJsonStringify(isolate, value, replacer, gap, data.get())
           .ToHandle(&captured)) {
    RejectStringifyException(isolate, promise, &caught);
    return isolate->is_execution_terminating() ? MaybeHandle<JSPromise>()
                                               : promise;
  }
  if (IsUndefined(*captured, isolate)) {
    if (JSPromise::Resolve(promise, captured).is_null()) {
      RejectStringifyException(isolate, promise, &caught);
    }
    return promise;
  }
  auto state =
      std::make_shared<JsonStringifyAsyncState>(isolate, promise, data);
  {
    auto* jobs = GetJsonStringifyJobs();
    base::MutexGuard lock(&jobs->mutex);
    jobs->jobs[isolate].emplace(state.get(), state);
  }
  platform->CallOnWorkerThread(std::make_unique<JsonStringifyWorker>(
      isolate, std::move(data), runner,
      std::make_unique<JsonStringifyCompletion>(isolate, std::move(state))));
  return promise;
}

}  // namespace internal
}  // namespace v8
