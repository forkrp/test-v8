// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstdio>
#include <deque>

#include "src/base/platform/condition-variable.h"
#include "src/base/platform/mutex.h"
#include "src/base/platform/semaphore.h"
#include "src/base/platform/time.h"
#include "src/json/json-parser-background.h"
#include "src/json/json-stringifier-async.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/objects-inl.h"
#include "test/cctest/cctest.h"

namespace {

class JsonTaskRunner final : public v8::TaskRunner {
 public:
  explicit JsonTaskRunner(std::shared_ptr<v8::TaskRunner> delegate)
      : delegate_(std::move(delegate)) {}

  bool IdleTasksEnabled() override { return delegate_->IdleTasksEnabled(); }
  bool NonNestableTasksEnabled() const override { return supported; }
  bool NonNestableDelayedTasksEnabled() const override {
    return delegate_->NonNestableDelayedTasksEnabled();
  }

  std::unique_ptr<v8::Task> TakeOne(v8::Isolate* isolate = nullptr) {
    auto deadline =
        v8::base::TimeTicks::Now() + v8::base::TimeDelta::FromSeconds(10);
    v8::base::MutexGuard lock(&mutex_);
    while (tasks_.empty()) {
      if (!isolate || !isolate->HasPendingBackgroundTasks()) return {};
      CHECK(v8::base::TimeTicks::Now() < deadline);
      USE(ready_.WaitFor(&mutex_, v8::base::TimeDelta::FromMilliseconds(10)));
    }
    auto task = std::move(tasks_.front());
    tasks_.pop_front();
    return task;
  }
  void WaitForTasks(size_t count) {
    auto deadline =
        v8::base::TimeTicks::Now() + v8::base::TimeDelta::FromSeconds(10);
    v8::base::MutexGuard lock(&mutex_);
    while (tasks_.size() < count) {
      CHECK(v8::base::TimeTicks::Now() < deadline);
      USE(ready_.WaitFor(&mutex_, v8::base::TimeDelta::FromMilliseconds(10)));
    }
  }
  bool RunOne(v8::Isolate* isolate = nullptr) {
    auto task = TakeOne(isolate);
    if (!task) return false;
    task->Run();
    return true;
  }

  bool supported = true;

 protected:
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    delegate_->PostTask(std::move(task), location);
  }
  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation&) override {
    v8::base::MutexGuard lock(&mutex_);
    tasks_.push_back(std::move(task));
    ready_.NotifyAll();
  }
  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task, double delay,
                           const v8::SourceLocation& location) override {
    delegate_->PostDelayedTask(std::move(task), delay, location);
  }
  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task, double delay,
      const v8::SourceLocation& location) override {
    delegate_->PostNonNestableDelayedTask(std::move(task), delay, location);
  }
  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> task,
                        const v8::SourceLocation& location) override {
    delegate_->PostIdleTask(std::move(task), location);
  }

 private:
  std::shared_ptr<v8::TaskRunner> delegate_;
  v8::base::Mutex mutex_;
  v8::base::ConditionVariable ready_;
  std::deque<std::unique_ptr<v8::Task>> tasks_;
};

struct JsonWorkerGate {
  const int main_thread = i::ThreadId::Current().ToInteger();
  v8::base::Semaphore arrived{0}, proceed{0}, finished{0};
};

class GatedJsonWorker final : public v8::Task {
 public:
  GatedJsonWorker(std::unique_ptr<v8::Task> task,
                  std::shared_ptr<JsonWorkerGate> gate)
      : task_(std::move(task)), gate_(std::move(gate)) {}
  void Run() override {
    CHECK_NE(i::ThreadId::Current().ToInteger(), gate_->main_thread);
    gate_->arrived.Signal();
    gate_->proceed.Wait();
    task_->Run();
    gate_->finished.Signal();
  }

 private:
  std::unique_ptr<v8::Task> task_;
  std::shared_ptr<JsonWorkerGate> gate_;
};

class JsonTestPlatform final : public TestPlatform {
 public:
  bool worker_threads_supported = true;
  int NumberOfWorkerThreads() override {
    return worker_threads_supported ? TestPlatform::NumberOfWorkerThreads() : 0;
  }
  void SetWorkerGate(std::shared_ptr<JsonWorkerGate> gate) {
    v8::base::MutexGuard lock(&gate_mutex_);
    gate_ = std::move(gate);
  }
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                  std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override {
    {
      v8::base::MutexGuard lock(&gate_mutex_);
      if (gate_ && priority == v8::TaskPriority::kUserVisible) {
        task = std::make_unique<GatedJsonWorker>(std::move(task), gate_);
      }
    }
    TestPlatform::PostTaskOnWorkerThreadImpl(priority, std::move(task),
                                             location);
  }
  std::shared_ptr<JsonTaskRunner> HoldTasks(v8::Isolate* isolate) {
    target_ = isolate;
    runner_ = std::make_shared<JsonTaskRunner>(
        TestPlatform::GetForegroundTaskRunner(isolate));
    return runner_;
  }

  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate) override {
    if (isolate == target_) return runner_;
    return TestPlatform::GetForegroundTaskRunner(isolate);
  }

 private:
  v8::base::Mutex gate_mutex_;
  std::shared_ptr<JsonWorkerGate> gate_;
  v8::Isolate* target_ = nullptr;
  std::shared_ptr<JsonTaskRunner> runner_;
};

}  // namespace

TEST(JsonParseAsyncCompactRecords) {
  static_assert(sizeof(i::BackgroundJsonNode) == 12);
  static_assert(sizeof(i::JsonString) == 12);
  CHECK_EQ(i::JsonString::kNoCacheSlot, i::JsonString().cache_slot());
  i::BackgroundJsonNode nodes[3];
  const double values[] = {1.25, -0.0, 1.23456789012345e100};
  for (int index = 0; index < 3; ++index) {
    nodes[index].SetNumber(values[index]);
    // Adjacent 12-byte records need not have 8-byte-aligned double payloads.
    CHECK_EQ(v8::base::bit_cast<uint64_t>(values[index]),
             v8::base::bit_cast<uint64_t>(nodes[index].Number()));
  }
  nodes[0].kind = i::BackgroundJsonNode::kKey;
  nodes[0].flags = i::BackgroundJsonNode::kIndex;
  nodes[0].data.string.start = UINT32_MAX - 1;
  CHECK_EQ(UINT32_MAX - 1, nodes[0].AsString().index());
  nodes[0].flags = i::BackgroundJsonNode::kCachedString;
  nodes[0].data.string = {4, 10};
  for (uint16_t slot : {0, 63}) {
    nodes[0].depth = slot;
    auto key = nodes[0].AsString(5, true);
    CHECK_EQ(slot, key.cache_slot());
    CHECK_EQ(16, key.start());
    CHECK_EQ(4, key.length());
    CHECK_EQ(i::JsonString::kNoCacheSlot,
             nodes[0].AsString().cache_slot());
  }
  nodes[0].kind = i::BackgroundJsonNode::kString;
  CHECK_EQ(i::JsonString::kNoCacheSlot,
           nodes[0].AsString(0, true).cache_slot());

  i::BackgroundJsonData data(false);
  for (int index = 0; index < 10000; ++index) {
    i::BackgroundJsonNode node;
    node.kind = i::BackgroundJsonNode::kNumber;
    node.SetNumber(index);
    data.nodes.push_back(node);
  }
  size_t bytes = data.MemoryUsage();
  data.DiscardBefore(4097);
  CHECK_EQ(10000u, data.NodeCount());
  CHECK_EQ(4097.0, data.NodeAt(4097).Number());
  CHECK_EQ(9999.0, data.NodeAt(9999).Number());
  CHECK_LT(data.MemoryUsage(), bytes);
  data.DiscardBefore(10000);
  CHECK(data.nodes.empty());
  CHECK_EQ(10000u, data.NodeCount());
}

TEST(JsonParseAsyncSnapshotTableGrowth) {
  auto isolate = CcTest::i_isolate();
  i::HandleScope scope(isolate);
  auto table = i::ObjectTwoHashTable::New(isolate, 0);
  for (int n = 0; n < 128; ++n) {
    auto key = isolate->factory()->Uint32ToString(n);
    i::Handle<i::Object> value(i::Smi::FromInt(n), isolate);
    table = i::ObjectTwoHashTable::Put(isolate, table, key, {value, value});
    CHECK_EQ(n + 1, table->NumberOfElements());
  }
  for (int n = 0; n < 128; ++n) {
    auto key = isolate->factory()->Uint32ToString(n);
    i::Handle<i::Object> replacement(i::Smi::FromInt(-n), isolate);
    table = i::ObjectTwoHashTable::Put(isolate, table, key,
                                       {replacement, replacement});
    CHECK_EQ(128, table->NumberOfElements());
    CHECK_EQ(-n, i::Smi::cast(table->Lookup(key)[0]).value());
  }
}

TEST_WITH_PLATFORM(JsonParseAsyncYieldsAndSurvivesGC, JsonTestPlatform) {
  v8::Isolate* isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto large = CompileRun(
                   "JSON.parseAsync('[0,'.repeat(20000) + '1' + "
                   "']'.repeat(20000))")
                   .As<v8::Promise>();
  auto small = CompileRun("JSON.parseAsync('42')").As<v8::Promise>();
  CHECK_EQ(v8::Promise::kPending, large->State());
  CHECK_EQ(v8::Promise::kPending, small->State());
  CHECK(isolate->HasPendingBackgroundTasks());
  runner->WaitForTasks(2);
  while (small->State() == v8::Promise::kPending)
    CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kFulfilled, small->State());
  CHECK_EQ(v8::Promise::kPending, large->State());

  isolate->LowMemoryNotification();
  {
    auto other = v8::Context::New(isolate);
    v8::Context::Scope other_scope(other);
    int steps = 0;
    while (runner->RunOne(isolate)) {
      CHECK(isolate->GetCurrentContext() == other);
      CHECK_LT(++steps, 100000);
      if (steps == 3) isolate->LowMemoryNotification();
    }
  }
  CHECK_EQ(v8::Promise::kFulfilled, large->State());
  CHECK(!isolate->HasPendingBackgroundTasks());
  auto result = large->Result().As<v8::Array>();
  CHECK(context->Global()
            ->Set(context.local(), v8_str("result"), result)
            .FromJust());
  CHECK(CompileRun("Object.getPrototypeOf(result) === Array.prototype")
            ->IsTrue());
  CHECK(CompileRun("let leaf = result; for (let i = 0; i < 20000; ++i) "
                   "leaf = leaf[1]; leaf === 1")
            ->IsTrue());
}

TEST_WITH_PLATFORM(JsonParseAsyncRejectsWithoutThrowing, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  v8::TryCatch caught(isolate);
  auto conversion =
      CompileRun("JSON.parseAsync({toString() {throw 123}})").As<v8::Promise>();
  CHECK(!caught.HasCaught());
  CHECK_EQ(v8::Promise::kRejected, conversion->State());
  CHECK_EQ(123, conversion->Result()->Int32Value(context.local()).FromJust());
  auto invalid = CompileRun("JSON.parseAsync('[1,]')").As<v8::Promise>();
  while (runner->RunOne(isolate)) {
  }
  CHECK(!caught.HasCaught());
  CHECK_EQ(v8::Promise::kRejected, invalid->State());
  CHECK(invalid->Result()->IsNativeError());
  auto reviver =
      CompileRun("JSON.parseAsync('{}', () => {throw 456})").As<v8::Promise>();
  while (runner->RunOne(isolate)) {
  }
  CHECK(!caught.HasCaught());
  CHECK_EQ(v8::Promise::kRejected, reviver->State());
  CHECK_EQ(456, reviver->Result()->Int32Value(context.local()).FromJust());
  runner->supported = false;
  auto unsupported = CompileRun("JSON.parseAsync('{}')").As<v8::Promise>();
  CHECK(!caught.HasCaught());
  CHECK_EQ(v8::Promise::kRejected, unsupported->State());
  CHECK(!runner->RunOne());
  runner->supported = true;
  platform.worker_threads_supported = false;
  auto no_workers = CompileRun("JSON.parseAsync('{}')").As<v8::Promise>();
  CHECK_EQ(v8::Promise::kRejected, no_workers->State());
  CHECK(!isolate->HasPendingBackgroundTasks());
  platform.worker_threads_supported = true;
}

TEST_WITH_PLATFORM(JsonParseAsyncConcurrentWorkers, JsonTestPlatform) {
  if (platform.NumberOfWorkerThreads() < 2) return;
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun("var input = '[' + '{\"id\":1},'.repeat(30000) + 'null]'");
  isolate->LowMemoryNotification();
  auto gate = std::make_shared<JsonWorkerGate>();
  platform.SetWorkerGate(gate);
  auto first = CompileRun("JSON.parseAsync(input)").As<v8::Promise>();
  auto second = CompileRun("JSON.parseAsync(input)").As<v8::Promise>();
  platform.SetWorkerGate(nullptr);
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(isolate->HasPendingBackgroundTasks());
  CHECK(CompileRun("6 * 7 === 42")->IsTrue());
  isolate->LowMemoryNotification();
  gate->proceed.Signal();
  gate->proceed.Signal();
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  while (runner->RunOne(isolate)) {
  }
  CHECK_EQ(v8::Promise::kFulfilled, first->State());
  CHECK_EQ(v8::Promise::kFulfilled, second->State());
  CHECK_EQ(30001u, first->Result().As<v8::Array>()->Length());
  CHECK_EQ(30001u, second->Result().As<v8::Array>()->Length());
  CHECK(!isolate->HasPendingBackgroundTasks());
}

TEST_WITH_PLATFORM(JsonParseAsyncPrimitiveArraysGC, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  const struct {
    const char* input;
    const char* check;
  } cases[] = {{"'[' + '1.5,-0,'.repeat(20000) + '2]'",
                "result.length === 40001 && result[0] === 1.5 && "
                "Object.is(result[1], -0) && result[40000] === 2"},
               {"JSON.stringify(Array.from({length:20000}, (_, i) "
                "=> 'long-string-value-' + i))",
                "result.length === 20000 && "
                "result[19999] === 'long-string-value-19999'"},
               {"JSON.stringify(Array.from({length:20000}, (_, i) "
                "=> i & 1 ? 'BB' : 'Aa'))",
                "result.length === 20000 && result[0] === 'Aa' && "
                "result[19999] === 'BB'"}};
  for (const auto& test : cases) {
    std::string script = "JSON.parseAsync(" + std::string(test.input) + ")";
    auto promise = CompileRun(script.c_str()).As<v8::Promise>();
    CHECK(runner->RunOne(isolate));
    CHECK_EQ(v8::Promise::kPending, promise->State());
    isolate->LowMemoryNotification();
    CHECK(runner->RunOne(isolate));
    isolate->LowMemoryNotification();
    while (runner->RunOne(isolate)) {
    }
    CHECK_EQ(v8::Promise::kFulfilled, promise->State());
    CHECK(context->Global()
              ->Set(context.local(), v8_str("result"), promise->Result())
              .FromJust());
    CHECK(CompileRun(test.check)->IsTrue());
  }
}

TEST_WITH_PLATFORM(JsonParseAsyncKeyCacheSurvivesGC, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto promise = CompileRun(
      "JSON.parseAsync(JSON.stringify(Array.from({length:20000}, (_, i) => "
      "({['dynamic' + String.fromCharCode(0x6f22)]: i, id: i, "
      "nested: {id: i + 0.25}, '': i & 1}))))").As<v8::Promise>();
  CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kPending, promise->State());
  isolate->LowMemoryNotification();
  CHECK(runner->RunOne(isolate));
  isolate->LowMemoryNotification();
  while (runner->RunOne(isolate)) {
  }
  CHECK_EQ(v8::Promise::kFulfilled, promise->State());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("result"), promise->Result())
            .FromJust());
  CHECK(CompileRun(
      "result.length === 20000 && result.every((v, i) => "
      "v['dynamic' + String.fromCharCode(0x6f22)] === i && v.id === i && "
      "v.nested.id === i + 0.25 && v[''] === (i & 1))")->IsTrue());
  CHECK(CompileRun("result[0].nested.id = 99.5; "
                   "result[1].nested.id === 1.25")->IsTrue());
}

TEST_WITH_PLATFORM(JsonParseAsyncTaskTiming, JsonTestPlatform) {
  using v8::base::TimeTicks;
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun(
      "var input = '[' + '{\"id\":1,\"text\":\"value\"},'.repeat(100000) + "
      "'null]'");
  isolate->LowMemoryNotification();
  double synchronous_ms;
  {
    v8::HandleScope discard(isolate);
    auto start = TimeTicks::Now();
    auto result = CompileRun("JSON.parse(input)").As<v8::Array>();
    synchronous_ms = (TimeTicks::Now() - start).InMillisecondsF();
    CHECK_EQ(100001u, result->Length());
  }
  isolate->LowMemoryNotification();
  auto start = TimeTicks::Now();
  auto promise = CompileRun("JSON.parseAsync(input)").As<v8::Promise>();
  double call_ms = (TimeTicks::Now() - start).InMillisecondsF();
  double max_slice_ms = 0, foreground_ms = 0, worker_wait_ms = 0;
  int slices = 0;
  while (true) {
    auto wait_start = TimeTicks::Now();
    auto task = runner->TakeOne(isolate);
    worker_wait_ms += (TimeTicks::Now() - wait_start).InMillisecondsF();
    if (!task) break;
    auto slice_start = TimeTicks::Now();
    task->Run();
    task.reset();
    double slice_ms = (TimeTicks::Now() - slice_start).InMillisecondsF();
    foreground_ms += slice_ms;
    max_slice_ms = std::max(max_slice_ms, slice_ms);
    CHECK_LT(++slices, 100000);
  }
  double total_ms = (TimeTicks::Now() - start).InMillisecondsF();
  CHECK_EQ(v8::Promise::kFulfilled, promise->State());
  CHECK_EQ(100001u, promise->Result().As<v8::Array>()->Length());
  CHECK_GT(slices, 1);
  // Timing is diagnostic, not a flaky wall-clock assertion. Allocations and GC
  // can exceed the cooperative deadline on any particular machine or run.
  printf(
      "JSON.parseAsync 2.4 MB: sync=%.3f ms call=%.3f ms "
      "worker_wait=%.3f ms foreground=%.3f ms slices=%d "
      "max_slice=%.3f ms total=%.3f ms workers=%d\n",
      synchronous_ms, call_ms, worker_wait_ms, foreground_ms, slices,
      max_slice_ms, total_ms, platform.NumberOfWorkerThreads());
}

TEST_WITH_PLATFORM(JsonParseAsyncTermination, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto promise = CompileRun(
                     "JSON.parseAsync('[1,'.repeat(10000) + '0' + "
                     "']'.repeat(10000))")
                     .As<v8::Promise>();
  CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kPending, promise->State());
  isolate->TerminateExecution();
  CHECK(runner->RunOne(isolate));
  CHECK(isolate->IsExecutionTerminating());
  CHECK_EQ(v8::Promise::kPending, promise->State());
  CHECK(!runner->RunOne());
  isolate->CancelTerminateExecution();
}

TEST_WITH_PLATFORM(JsonParseAsyncTasksOutliveIsolate, JsonTestPlatform) {
  for (bool run_first_slice : {false, true}) {
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = CcTest::array_buffer_allocator();
    auto isolate = v8::Isolate::New(params);
    auto runner = platform.HoldTasks(isolate);
    {
      v8::Isolate::Scope isolate_scope(isolate);
      v8::HandleScope scope(isolate);
      auto context = v8::Context::New(isolate);
      v8::Context::Scope context_scope(context);
      CompileRun(
          "JSON.parseAsync('[0,'.repeat(20000) + '1' + "
          "']'.repeat(20000))");
      if (run_first_slice) CHECK(runner->RunOne(isolate));
    }
    isolate->Dispose();
    // Both running and destroying these canceled tasks must be safe. Managed
    // has already released the GC roots before GlobalHandles were torn down.
    while (runner->RunOne()) {
    }
  }
}

TEST_WITH_PLATFORM(JsonParseAsyncQueuedWorkerOutlivesIsolate,
                   JsonTestPlatform) {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  auto isolate = v8::Isolate::New(params);
  auto runner = platform.HoldTasks(isolate);
  auto gate = std::make_shared<JsonWorkerGate>();
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope scope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    platform.SetWorkerGate(gate);
    CompileRun("JSON.parseAsync('[1,2,3]')");
    platform.SetWorkerGate(nullptr);
    CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
    CHECK(isolate->HasPendingBackgroundTasks());
  }
  isolate->Dispose();
  // The pool owns a task whose CancelableTask has not started yet. Its Run
  // and destructor must not touch the disposed isolate or post a completion.
  gate->proceed.Signal();
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(!runner->RunOne());
}

TEST(JsonStringifyAsyncNativeBounds) {
  i::JsonStringifyData data;
  CHECK(data.AddLength(i::String::kMaxLength));
  CHECK(!data.AddLength(1));
  CHECK(data.overflowed);
  i::EncodeJsonStringify(&data);
  CHECK(!data.result8 && !data.result16);
  data.Reset();
  CHECK(data.AddLength(i::String::kMaxLength));
  data.cancelled.store(true, std::memory_order_relaxed);
  i::EncodeJsonStringify(&data);
  CHECK(!data.result8 && !data.result16);
}

TEST_WITH_PLATFORM(JsonStringifyAsyncConcurrentWorkers, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto gate = std::make_shared<JsonWorkerGate>();
  CompileRun(
      "var input = Array.from({length: 40001}, (_, i) => i / 8); "
      "var expected = JSON.stringify(input)");
  platform.SetWorkerGate(gate);
  auto first = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
  auto second = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
  platform.SetWorkerGate(nullptr);
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(isolate->HasPendingBackgroundTasks());
  CHECK(CompileRun("input[0] = 'changed'; input.length = 0; 6 * 7 === 42")
            ->IsTrue());
  isolate->LowMemoryNotification();
  gate->proceed.Signal();
  gate->proceed.Signal();
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  while (runner->RunOne(isolate)) {
  }
  CHECK_EQ(v8::Promise::kFulfilled, first->State());
  CHECK_EQ(v8::Promise::kFulfilled, second->State());
  CHECK(first->Result()->StrictEquals(CompileRun("expected")));
  CHECK(second->Result()->StrictEquals(CompileRun("expected")));
  CHECK(!isolate->HasPendingBackgroundTasks());
}

TEST_WITH_PLATFORM(JsonStringifyAsyncResourceAndRealm, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  for (const char* expression :
       {"'x'.repeat(100000)", "String.fromCharCode(0x6f22).repeat(100000)"}) {
    std::string setup = "var input = " + std::string(expression) +
                        "; var expected = JSON.stringify(input)";
    CompileRun(setup.c_str());
    auto promise = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
    runner->WaitForTasks(1);
    isolate->LowMemoryNotification();
    {
      auto other = v8::Context::New(isolate);
      v8::Context::Scope other_scope(other);
      CHECK(runner->RunOne(isolate));
      CHECK(isolate->GetCurrentContext() == other);
    }
    CHECK_EQ(v8::Promise::kFulfilled, promise->State());
    CHECK(promise->Result()->IsString());
    auto text = promise->Result().As<v8::String>();
    CHECK(text->IsExternalOneByte() || text->IsExternalTwoByte());
    isolate->LowMemoryNotification();
    CHECK(text->StrictEquals(CompileRun("expected")));
    CHECK(context->Global()
              ->Set(context.local(), v8_str("encoded"), text)
              .FromJust());
    CHECK(CompileRun("JSON.parse(encoded) === input")->IsTrue());
  }
}

TEST_WITH_PLATFORM(JsonStringifyAsyncUnsupported, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun("var calls = 0; var input = {toJSON() { ++calls; return 42; }}");
  runner->supported = false;
  auto first = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
  CHECK_EQ(v8::Promise::kRejected, first->State());
  runner->supported = true;
  platform.worker_threads_supported = false;
  auto second = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
  CHECK_EQ(v8::Promise::kRejected, second->State());
  CHECK(CompileRun("calls === 0")->IsTrue());
  CHECK(!isolate->HasPendingBackgroundTasks());
  platform.worker_threads_supported = true;
}

TEST_WITH_PLATFORM(JsonStringifyAsyncTermination, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto promise =
      CompileRun("JSON.stringifyAsync('x'.repeat(100000))").As<v8::Promise>();
  runner->WaitForTasks(1);
  isolate->TerminateExecution();
  CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kPending, promise->State());
  CHECK(isolate->IsExecutionTerminating());
  CHECK(!isolate->HasPendingBackgroundTasks());
  isolate->CancelTerminateExecution();
}

TEST_WITH_PLATFORM(JsonStringifyAsyncTaskTiming, JsonTestPlatform) {
  using v8::base::TimeTicks;
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun(
      "var input = 'abcdefghij'.repeat(1000000); "
      "var expected = JSON.stringify(input)");
  isolate->LowMemoryNotification();
  auto start = TimeTicks::Now();
  auto promise = CompileRun("JSON.stringifyAsync(input)").As<v8::Promise>();
  double call_ms = (TimeTicks::Now() - start).InMillisecondsF();
  auto wait_start = TimeTicks::Now();
  auto task = runner->TakeOne(isolate);
  double wait_ms = (TimeTicks::Now() - wait_start).InMillisecondsF();
  CHECK(task);
  auto complete_start = TimeTicks::Now();
  task->Run();
  task.reset();
  double complete_ms = (TimeTicks::Now() - complete_start).InMillisecondsF();
  double total_ms = (TimeTicks::Now() - start).InMillisecondsF();
  CHECK_EQ(v8::Promise::kFulfilled, promise->State());
  CHECK(promise->Result()->StrictEquals(CompileRun("expected")));
  CHECK(!isolate->HasPendingBackgroundTasks());
  // Diagnostic only: host scheduling and GC are not hard real-time bounds.
  printf(
      "JSON.stringifyAsync 10 MB: capture=%.3f ms wait=%.3f ms "
      "complete=%.3f ms total=%.3f ms workers=%d\n",
      call_ms, wait_ms, complete_ms, total_ms,
      platform.NumberOfWorkerThreads());
}

TEST_WITH_PLATFORM(JsonStringifyAsyncDroppedCompletion, JsonTestPlatform) {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  auto isolate = v8::Isolate::New(params);
  auto runner = platform.HoldTasks(isolate);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope scope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    CompileRun("JSON.stringifyAsync('x'.repeat(100000))");
    auto task = runner->TakeOne(isolate);
    CHECK(task);
    task.reset();  // A host may discard a queued foreground task.
    isolate->LowMemoryNotification();
    CHECK(isolate->HasPendingBackgroundTasks());
  }
  // Registry ownership must release the persistent roots on this thread.
  isolate->Dispose();
}

TEST_WITH_PLATFORM(JsonStringifyAsyncTasksOutliveIsolate, JsonTestPlatform) {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  auto isolate = v8::Isolate::New(params);
  auto runner = platform.HoldTasks(isolate);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope scope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    CompileRun("JSON.stringifyAsync('x'.repeat(100000))");
    runner->WaitForTasks(1);
  }
  isolate->Dispose();
  while (runner->RunOne()) {
  }
}

TEST_WITH_PLATFORM(JsonStringifyAsyncQueuedWorkerOutlivesIsolate,
                   JsonTestPlatform) {
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = CcTest::array_buffer_allocator();
  auto isolate = v8::Isolate::New(params);
  auto runner = platform.HoldTasks(isolate);
  auto gate = std::make_shared<JsonWorkerGate>();
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope scope(isolate);
    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    platform.SetWorkerGate(gate);
    CompileRun("JSON.stringifyAsync('x'.repeat(100000))");
    platform.SetWorkerGate(nullptr);
    CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
    CHECK(isolate->HasPendingBackgroundTasks());
  }
  isolate->Dispose();
  gate->proceed.Signal();
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(!runner->RunOne());
}
