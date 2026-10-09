// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <limits>

#include "src/api/api-inl.h"
#include "src/base/platform/condition-variable.h"
#include "src/base/platform/mutex.h"
#include "src/base/platform/semaphore.h"
#include "src/base/platform/time.h"
#include "src/json/json-parser-background.h"
#include "src/json/json-stringifier-async.h"
#include "src/json/json-stringifier-escape.h"
#include "src/json/json-stringifier.h"
#include "src/numbers/conversions.h"
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
  // Each fixture exceeds the physical node cap, so GC runs between slices
  // even when primitive-array materialization is faster than the deadline.
  const struct {
    const char* input;
    const char* check;
  } cases[] = {{"'[' + '1.5,-0,'.repeat(100000) + '2]'",
                "result.length === 200001 && result[0] === 1.5 && "
                "Object.is(result[1], -0) && result[200000] === 2"},
               {"JSON.stringify(Array.from({length:100000}, (_, i) "
                "=> 'long-string-value-' + i))",
                "result.length === 100000 && "
                "result[99999] === 'long-string-value-99999'"},
               {"JSON.stringify(Array.from({length:100000}, (_, i) "
                "=> i & 1 ? 'BB' : 'Aa'))",
                "result.length === 100000 && result[0] === 'Aa' && "
                "result[99999] === 'BB'"}};
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

TEST_WITH_PLATFORM(JsonParseAsyncMaterializationProfile, JsonTestPlatform) {
  using v8::base::TimeTicks;
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  for (int fields : {2, 12, 60}) {
    const int records = fields == 2 ? 100000 : fields == 12 ? 30000 : 10000;
    std::string setup = "var profileRecord = {}; for (var k = 0; k < " +
                        std::to_string(fields) +
                        "; ++k) profileRecord['k' + k] = "
                        "[0, 3.5, 'value'][k % 3]; var profileInput = '[' + "
                        "(JSON.stringify(profileRecord) + ',').repeat(" +
                        std::to_string(records) + ") + 'null]'";
    CompileRun(setup.c_str());
    for (int round = -2; round < 5; ++round) {
      isolate->LowMemoryNotification();
      v8::HandleScope discard(isolate);
      auto start = TimeTicks::Now();
      auto promise =
          CompileRun("JSON.parseAsync(profileInput)").As<v8::Promise>();
      double call_ms = (TimeTicks::Now() - start).InMillisecondsF();
      double wait_ms = 0, foreground_ms = 0;
      std::vector<double> slices;
      while (true) {
        auto before_wait = TimeTicks::Now();
        auto task = runner->TakeOne(isolate);
        wait_ms += (TimeTicks::Now() - before_wait).InMillisecondsF();
        if (!task) break;
        auto before_slice = TimeTicks::Now();
        task->Run();
        task.reset();
        double elapsed = (TimeTicks::Now() - before_slice).InMillisecondsF();
        foreground_ms += elapsed;
        slices.push_back(elapsed);
        CHECK_LT(slices.size(), 100000u);
      }
      double total_ms = (TimeTicks::Now() - start).InMillisecondsF();
      CHECK_EQ(v8::Promise::kFulfilled, promise->State());
      CHECK_EQ(static_cast<uint32_t>(records + 1),
               promise->Result().As<v8::Array>()->Length());
      CHECK_GT(slices.size(), 1u);
      if (round < 0) continue;
      // Diagnostic only: GC and allocations do not have hard timing bounds.
      printf(
          "JSON_ASYNC_PROFILE {\"fields\":%d,\"records\":%d,"
          "\"round\":%d,\"call_ms\":%.3f,\"wait_ms\":%.3f,"
          "\"foreground_ms\":%.3f,\"total_ms\":%.3f,\"slices_ms\":[",
          fields, records, round, call_ms, wait_ms, foreground_ms, total_ms);
      for (size_t i = 0; i < slices.size(); ++i) {
        printf("%s%.3f", i ? "," : "", slices[i]);
      }
      printf("]}\n");
    }
  }
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

TEST(JsonStringifyAsyncNativeNumericText) {
  std::vector<double> values = {0.0, -0.0, 1.25, -1.25, 1e-6, -1e-6,
                                -0.0000010000000000000002, 1e20, 1e21,
                                2147483647.0, -2147483648.0, 4294967295.0,
                                std::numeric_limits<double>::min(),
                                std::numeric_limits<double>::denorm_min(),
                                std::numeric_limits<double>::max(),
                                -std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()};
  uint64_t bits = 0x123456789abcdef0;
  for (int i = 0; i < 2000; ++i) {
    bits = bits * UINT64_C(2862933555777941757) + UINT64_C(3037000493);
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    for (int repeat = 0; repeat < 4; ++repeat) values.push_back(value);
  }
  std::string expected = "[";
  for (size_t index = 0; index < values.size(); ++index) {
    if (index) expected += ',';
    char text[100];
    expected += std::isfinite(values[index])
                    ? i::DoubleToStringView(values[index], v8::base::VectorOf(text))
                    : std::string_view("null");
  }
  expected += ']';
  for (bool wide : {false, true}) {
    i::JsonStringifyData data;
    data.wide_output = wide;
    data.numbers = values;
    data.number_count = values.size();
    CHECK(data.AddLength(values.size() * 2 + 1));
    i::JsonStringifyPart part;
    part.kind = i::JsonStringifyPart::kNumbers;
    part.prefix_length = 1;
    part.prefix[0] = '[';
    part.data.span.length = static_cast<uint32_t>(values.size());
    data.parts.push_back(part);
    part = {};
    part.prefix_length = 1;
    part.prefix[0] = ']';
    data.parts.push_back(part);
    i::EncodeJsonStringify(&data);
    if (wide) {
      CHECK(data.result16);
      CHECK_EQ(expected.size(), data.result16->length());
      for (size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(static_cast<uint16_t>(expected[i]), data.result16->data()[i]);
      }
    } else {
      CHECK(data.result8);
      CHECK_EQ(expected.size(), data.result8->length());
      CHECK_EQ(0, std::memcmp(expected.data(), data.result8->data(),
                              expected.size()));
    }
  }
}

TEST(JsonStringifyAsyncNativeSurrogateAdoption) {
  for (size_t offset :
       {0u, 7u, 8u, 15u, 16u, 31u, 32u, 4094u, 4095u, 4096u, 4097u, 8191u}) {
    for (bool paired : {false, true}) {
      std::vector<uint16_t> chars(offset, 0x6f22);
      chars.push_back(0xd83d);
      if (paired) chars.push_back(0xde00);
      chars.push_back('x');
      i::JsonStringifyData data;
      data.wide_output = true;
      data.two_byte.push_back('"');
      data.two_byte.insert(data.two_byte.end(), chars.begin(), chars.end());
      data.two_byte.push_back('"');
      CHECK(data.AddLength(chars.size() + 2));
      i::JsonStringifyPart part;
      part.kind = i::JsonStringifyPart::kString16;
      part.data.span.offset = 1;
      part.data.span.length = static_cast<uint32_t>(chars.size());
      data.parts.push_back(part);
      const uint16_t* captured = data.two_byte.data();
      std::vector<uint16_t> expected(chars.size() * 6 + 2);
      expected[0] = '"';
      i::JsonStringSpanWriter<uint16_t> writer{expected.data() + 1};
      i::WriteJsonString<false>(
          v8::base::Vector<const uint16_t>(chars.data(), chars.size()),
          &writer);
      *writer.cursor++ = '"';
      size_t length = writer.cursor - expected.data();
      i::EncodeJsonStringify(&data);
      CHECK(data.result16);
      CHECK_EQ(paired, data.result16->data() == captured);
      CHECK_EQ(length, data.result16->length());
      CHECK_EQ(0, std::memcmp(data.result16->data(), expected.data(),
                              length * sizeof(uint16_t)));
    }
  }
}

TEST(JsonUpstreamEscapeScanner) {
  uint8_t narrow[80];
  uint16_t wide[80];
  std::fill(std::begin(narrow), std::end(narrow), 'a');
  std::fill(std::begin(wide), std::end(wide), 0x1234);
  for (size_t offset = 0; offset < 16; ++offset) {
    for (size_t length = 0; length <= 64; ++length) {
      CHECK_EQ(length, i::FindJsonEscape(narrow + offset, length));
      CHECK_EQ(length, i::FindJsonEscape(wide + offset, length));
    }
  }
  for (uint32_t c = 0; c <= 0xffff; ++c) {
    wide[31] = c;
    CHECK_EQ(i::JsonStringDoNotEscape(static_cast<uint16_t>(c)) ? 64u : 30u,
             i::FindJsonEscape(wide + 1, 64));
    if (c < 256) {
      narrow[31] = c;
      CHECK_EQ(i::JsonStringDoNotEscape(static_cast<uint8_t>(c)) ? 64u : 30u,
               i::FindJsonEscape(narrow + 1, 64));
    }
  }
}

TEST(JsonUpstreamDescriptorMetadata) {
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto object = CompileRun("var metadataTarget = {a: 1, b: 2}; metadataTarget");
  auto raw = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*object));
  CompileRun("JSON.stringify(metadataTarget)");
  auto descriptors = i::handle(raw->map()->instance_descriptors(), internal);
  using State = i::DescriptorArray::FastIterableState;
  CHECK_EQ(State::kJsonFast, descriptors->fast_iterable());
  i::InternalIndex first(0);
  descriptors->Sort();  // Sorting only changes lookup-order details.
  CHECK_EQ(State::kJsonFast, descriptors->fast_iterable());
  descriptors->Set(first, descriptors->GetKey(first),
                    descriptors->GetValue(first),
                    descriptors->GetDetails(first));
  CHECK_EQ(State::kUnknown, descriptors->fast_iterable());
  CompileRun("JSON.stringify(metadataTarget)");
  auto copy = i::DescriptorArray::CopyUpTo(internal, descriptors, 2);
  CHECK_EQ(State::kUnknown, copy->fast_iterable());
  isolate->LowMemoryNotification();
  CHECK(CompileRun(
            "Object.defineProperty(metadataTarget, 'b', {enumerable: false});"
            "JSON.stringify(metadataTarget) === '{\"a\":1}'")
            ->IsTrue());
}

TEST(JsonUpstreamFastCapture) {
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto object = CompileRun("({asyncCaptureKey: 1.25, text: 'hello'})");
  auto raw = i::Handle<i::JSObject>::cast(v8::Utils::OpenHandle(*object));
  auto descriptors = i::handle(raw->map()->instance_descriptors(), internal);
  using State = i::DescriptorArray::FastIterableState;
  CHECK_NE(State::kJsonFast, descriptors->fast_iterable());
  i::JsonStringifyData data;
  auto undefined = internal->factory()->undefined_value();
  CHECK(!i::CaptureJsonStringify(internal, raw, undefined, undefined, &data)
             .is_null());
  CHECK_EQ(State::kJsonFast, descriptors->fast_iterable());
  CHECK_EQ(1u, data.number_count);
  CHECK(!data.result8 && !data.result16);
  i::EncodeJsonStringify(&data);
  CHECK(data.result8);
  CHECK_EQ(std::string(data.result8->data(), data.result8->length()),
           "{\"asyncCaptureKey\":1.25,\"text\":\"hello\"}");
}

TEST(JsonUpstreamFastTermination) {
  auto isolate = CcTest::isolate();
  auto internal = CcTest::i_isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto value = CompileRun("Array.from({length: 20000}, (_, i) => i / 7)");
  auto raw = v8::Utils::OpenHandle(*value);
  auto undefined = internal->factory()->undefined_value();
  v8::TryCatch caught(isolate);
  isolate->TerminateExecution();
  CHECK(i::JsonStringify(internal, raw, undefined, undefined).is_null());
  CHECK(isolate->IsExecutionTerminating());
  isolate->CancelTerminateExecution();
  caught.Reset();
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
       {"'x'.repeat(100000)", "String.fromCharCode(0x6f22).repeat(100000)",
        "'\\ud83d\\ude00'.repeat(50000)",
        "'x'.repeat(4095) + '\\ud83d\\ude00' + 'x'.repeat(4096)"}) {
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

TEST_WITH_PLATFORM(MessagePackAsyncYieldsAndSurvivesGC, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun(
      "var original = Array.from({length: 100000}, (_,i) => ({id:i, n:i/10}));"
      "var bytes = MSGPACK.encode(original)");
  auto large = CompileRun("MSGPACK.decodeAsync(bytes)").As<v8::Promise>();
  CHECK_EQ(v8::Promise::kPending, large->State());
  CHECK(isolate->HasPendingBackgroundTasks());
  CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kPending, large->State());
  isolate->LowMemoryNotification();
  int slices = 1;
  {
    auto other = v8::Context::New(isolate);
    v8::Context::Scope other_scope(other);
    while (large->State() == v8::Promise::kPending) {
      CHECK(runner->RunOne(isolate));
      CHECK(isolate->GetCurrentContext() == other);
      CHECK_LT(++slices, 100000);
      if (slices == 3) isolate->LowMemoryNotification();
    }
  }
  CHECK_EQ(v8::Promise::kFulfilled, large->State());
  CHECK(!isolate->HasPendingBackgroundTasks());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("restored"), large->Result())
            .FromJust());
  CHECK(CompileRun("Object.getPrototypeOf(restored) === Array.prototype && "
                   "JSON.stringify(restored) === JSON.stringify(original)")
            ->IsTrue());
  printf("MessagePack decode materialization slices: %d\n", slices);
}

TEST_WITH_PLATFORM(MessagePackAsyncConcurrentWorkersAndSnapshot,
                   JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  auto gate = std::make_shared<JsonWorkerGate>();
  CompileRun(
      "var input = {text: '中'.repeat(10000), values: [1, 0.1, -0]}; "
      "var expected = MSGPACK.encode(input)");
  platform.SetWorkerGate(gate);
  auto first = CompileRun("MSGPACK.encodeAsync(input)").As<v8::Promise>();
  auto second = CompileRun("MSGPACK.decodeAsync(expected)").As<v8::Promise>();
  platform.SetWorkerGate(nullptr);
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CompileRun(
      "input.text = 'changed'; input.values[0] = 999; expected.fill(0xc1)");
  isolate->LowMemoryNotification();
  gate->proceed.Signal();
  gate->proceed.Signal();
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  while (isolate->HasPendingBackgroundTasks()) CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kFulfilled, first->State());
  CHECK_EQ(v8::Promise::kFulfilled, second->State());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("encoded"), first->Result())
            .FromJust());
  CHECK(context->Global()
            ->Set(context.local(), v8_str("decoded"), second->Result())
            .FromJust());
  CHECK(CompileRun("JSON.stringify(MSGPACK.decode(encoded)) === "
                   "JSON.stringify(decoded) && "
                   "decoded.text.length === 10000 && decoded.values[0] === 1")
            ->IsTrue());
}

TEST_WITH_PLATFORM(MessagePackAsyncUnsupportedPlatform, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  for (const char* expression :
       {"MSGPACK.encodeAsync({a:1})",
        "MSGPACK.decodeAsync(new Uint8Array([1]))",
        "MSGPACK.encodeResourceAsync({a:1})",
        "MSGPACK.decodeResourceAsync(new Uint8Array([1]))"}) {
    runner->supported = false;
    CHECK_EQ(v8::Promise::kRejected,
             CompileRun(expression).As<v8::Promise>()->State());
    runner->supported = true;
    platform.worker_threads_supported = false;
    CHECK_EQ(v8::Promise::kRejected,
             CompileRun(expression).As<v8::Promise>()->State());
    platform.worker_threads_supported = true;
  }
  CHECK(!isolate->HasPendingBackgroundTasks());
}

TEST_WITH_PLATFORM(MessagePackAsyncTermination, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  for (const char* expression :
       {"MSGPACK.encodeAsync('中'.repeat(100000))",
        "MSGPACK.decodeAsync(MSGPACK.encode(Array."
        "from({length:100000}, (_,i)=>i/10)))",
        "MSGPACK.encodeResourceAsync('中'.repeat(100000))",
        "MSGPACK.decodeResourceAsync(MSGPACK.encodeResource(Array."
        "from({length:100000}, (_,i)=>i/10)))"}) {
    auto promise = CompileRun(expression).As<v8::Promise>();
    runner->WaitForTasks(1);
    isolate->TerminateExecution();
    CHECK(runner->RunOne(isolate));
    CHECK_EQ(v8::Promise::kPending, promise->State());
    CHECK(isolate->IsExecutionTerminating());
    CHECK(!isolate->HasPendingBackgroundTasks());
    isolate->CancelTerminateExecution();
  }
}

TEST_WITH_PLATFORM(MessagePackAsyncTasksOutliveIsolate, JsonTestPlatform) {
  for (int test = 0; test < 8; ++test) {
    int mode = test % 4;
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
      if (mode == 0) platform.SetWorkerGate(gate);
      CompileRun(
          test < 4
              ? "MSGPACK.encodeAsync('中'.repeat(100000));"
                "MSGPACK.decodeAsync(MSGPACK.encode(Array.from({length:100000}, "
                "(_,i)=>({n:i/10}))))"
              : "MSGPACK.encodeResourceAsync('中'.repeat(100000));"
                "MSGPACK.decodeResourceAsync(MSGPACK.encodeResource(Array.from("
                "{length:100000}, (_,i)=>({long_property_name:i/10}))))");
      platform.SetWorkerGate(nullptr);
      if (mode == 0) {
        for (int i = 0; i < 2; ++i)
          CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
      } else {
        runner->WaitForTasks(2);
        if (mode == 2) CHECK(runner->RunOne(isolate));
        if (mode == 3) {
          auto task = runner->TakeOne(isolate);
          CHECK(task);
          task.reset();
        }
      }
    }
    isolate->Dispose();
    if (mode == 0) {
      for (int i = 0; i < 2; ++i) gate->proceed.Signal();
      for (int i = 0; i < 2; ++i)
        CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
    }
    while (runner->RunOne()) {
    }
  }
}

TEST_WITH_PLATFORM(MessagePackResourceAsyncWorkersAndSlices, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun(
      "var resourceOriginal = Array.from({length:20000}, (_,i) => "
      "({long_identifier:i, repeated_text:'common中'.repeat(30), "
      "numbers:[i/10, -0, Infinity]}));"
      "var resourceExpected = MSGPACK.encodeResource(resourceOriginal);"
      "var resourceInput = resourceExpected.slice()");
  auto gate = std::make_shared<JsonWorkerGate>();
  platform.SetWorkerGate(gate);
  auto encoding = CompileRun("MSGPACK.encodeResourceAsync(resourceOriginal)")
                      .As<v8::Promise>();
  auto decoding = CompileRun("MSGPACK.decodeResourceAsync(resourceInput)")
                      .As<v8::Promise>();
  platform.SetWorkerGate(nullptr);
  for (int i = 0; i < 2; ++i)
    CHECK(gate->arrived.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  CHECK_EQ(v8::Promise::kPending, encoding->State());
  CHECK_EQ(v8::Promise::kPending, decoding->State());
  CHECK(isolate->HasPendingBackgroundTasks());
  CompileRun(
      "resourceOriginal[0].long_identifier = 999; resourceInput.fill(0xc1)");
  isolate->LowMemoryNotification();
  for (int i = 0; i < 2; ++i) gate->proceed.Signal();
  for (int i = 0; i < 2; ++i)
    CHECK(gate->finished.WaitFor(v8::base::TimeDelta::FromSeconds(5)));
  int slices = 0;
  {
    auto other = v8::Context::New(isolate);
    v8::Context::Scope other_scope(other);
    while (isolate->HasPendingBackgroundTasks()) {
      CHECK(runner->RunOne(isolate));
      CHECK(isolate->GetCurrentContext() == other);
      CHECK_LT(++slices, 100000);
      if (slices == 2 || slices == 4) isolate->LowMemoryNotification();
    }
  }
  CHECK_GT(slices, 2);
  CHECK_EQ(v8::Promise::kFulfilled, encoding->State());
  CHECK_EQ(v8::Promise::kFulfilled, decoding->State());
  CHECK(
      context->Global()
          ->Set(context.local(), v8_str("resourceEncoded"), encoding->Result())
          .FromJust());
  CHECK(
      context->Global()
          ->Set(context.local(), v8_str("resourceDecoded"), decoding->Result())
          .FromJust());
  CHECK(CompileRun(
            "resourceExpected[0] === 86 && resourceExpected[1] === 56 && "
            "resourceEncoded.length === resourceExpected.length && "
            "resourceEncoded.every((b,i)=>b === resourceExpected[i]) && "
            "Object.getPrototypeOf(resourceDecoded) === Array.prototype && "
            "resourceDecoded.length === 20000 && "
            "resourceDecoded[0].long_identifier === 0 && "
            "resourceDecoded[19999].long_identifier === 19999 && "
            "Object.is(resourceDecoded[19999].numbers[1], -0) && "
            "resourceDecoded[19999].numbers[2] === Infinity")
            ->IsTrue());
  printf("MessagePack resource async foreground slices: %d\n", slices);
}

TEST_WITH_PLATFORM(MessagePackAsyncNativeAccounting, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun("var text = '中'.repeat(1000000)");
  v8::HeapStatistics before, pending, encoded, decoded;
  isolate->GetHeapStatistics(&before);
  int64_t baseline_native = isolate->AdjustAmountOfExternalAllocatedMemory(0);
  auto encoding = CompileRun("MSGPACK.encodeAsync(text)").As<v8::Promise>();
  isolate->GetHeapStatistics(&pending);
  printf("capture accounting: before=%zu pending=%zu\n", before.external_memory(), pending.external_memory()); fflush(stdout);
  CHECK_GE(isolate->AdjustAmountOfExternalAllocatedMemory(0), baseline_native + 2000000);
  while (isolate->HasPendingBackgroundTasks()) CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kFulfilled, encoding->State());
  auto bytes = encoding->Result().As<v8::Uint8Array>();
  CHECK(context->Global()->Set(context.local(), v8_str("bytes"), bytes).FromJust());
  isolate->GetHeapStatistics(&encoded);
  printf("encode accounting: before=%zu encoded=%zu payload=%zu\n", before.external_memory(), encoded.external_memory(), bytes->ByteLength()); fflush(stdout);
  CHECK_GE(encoded.external_memory(), before.external_memory() + bytes->ByteLength());
  CHECK_LE(encoded.external_memory(), before.external_memory() + bytes->ByteLength() + 4096);
  auto decoding = CompileRun("MSGPACK.decodeAsync(bytes)").As<v8::Promise>();
  isolate->GetHeapStatistics(&pending);
  printf("decode capture accounting: encoded=%zu pending=%zu\n", encoded.external_memory(), pending.external_memory()); fflush(stdout);
  CHECK_GE(isolate->AdjustAmountOfExternalAllocatedMemory(0), baseline_native + 2 * static_cast<int64_t>(bytes->ByteLength()));
  while (isolate->HasPendingBackgroundTasks()) CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kFulfilled, decoding->State());
  CHECK(decoding->Result()->StrictEquals(CompileRun("text")));
  isolate->GetHeapStatistics(&decoded);
  printf("decode accounting: encoded=%zu decoded=%zu\n", encoded.external_memory(), decoded.external_memory()); fflush(stdout);
  CHECK_LE(decoded.external_memory(), encoded.external_memory() + 4096);
  CHECK_LE(isolate->AdjustAmountOfExternalAllocatedMemory(0), baseline_native + static_cast<int64_t>(bytes->ByteLength()) + 4096);
  printf("MessagePack native accounting: baseline=%zu encoded=%zu decoded=%zu payload=%zu\n",
         before.external_memory(), encoded.external_memory(), decoded.external_memory(), bytes->ByteLength());
}

TEST_WITH_PLATFORM(MessagePackAsyncCompleteWireAccounting, JsonTestPlatform) {
  auto isolate = CcTest::isolate();
  v8::HandleScope scope(isolate);
  LocalContext context;
  auto runner = platform.HoldTasks(isolate);
  CompileRun("var input = Array.from({length:20000}, (_,i) => ({id:i, score:i, name:'short', ok:true}));");
  v8::HeapStatistics before, after;
  isolate->GetHeapStatistics(&before);
  int64_t baseline = isolate->AdjustAmountOfExternalAllocatedMemory(0);
  auto promise = CompileRun("MSGPACK.encodeAsync(input)").As<v8::Promise>();
  CompileRun("input[0].name = 'changed'");
  while (isolate->HasPendingBackgroundTasks()) CHECK(runner->RunOne(isolate));
  CHECK_EQ(v8::Promise::kFulfilled, promise->State());
  auto bytes = promise->Result().As<v8::Uint8Array>();
  CHECK(context->Global()->Set(context.local(), v8_str("bytes"), bytes).FromJust());
  isolate->GetHeapStatistics(&after);
  CHECK_GE(after.external_memory(), before.external_memory() + bytes->ByteLength());
  CHECK_LE(after.external_memory(), before.external_memory() + bytes->ByteLength() + 4096);
  CHECK_LE(isolate->AdjustAmountOfExternalAllocatedMemory(0), baseline + static_cast<int64_t>(bytes->ByteLength()) + 4096);
  CHECK(CompileRun("MSGPACK.decode(bytes)[0].name === 'short'")->IsTrue());
}
