// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unordered_map>
#include <unordered_set>

#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "src/base/lazy-instance.h"
#include "src/base/platform/time.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/global-handles-inl.h"
#include "src/handles/persistent-handles.h"
#include "src/json/json-parser-background.h"
#include "src/json/json-parser.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/js-array-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/managed-inl.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {
namespace {

void RejectJsonException(Isolate* isolate, Handle<JSPromise> promise,
                         v8::TryCatch* caught) {
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

// ponytail: process-wide lock for task bookkeeping, never for parsing. Move
// bookkeeping into Isolate if registration/completion churn causes contention.
struct JsonParseJobs {
  base::Mutex mutex;
  std::unordered_map<Isolate*, std::unordered_set<JsonParseAsyncState*>> jobs;
};
DEFINE_LAZY_LEAKY_OBJECT_GETTER(JsonParseJobs, GetJsonParseJobs)

}  // namespace

// All V8 handles remain on the isolate thread. Workers see native data only.
class JsonParseAsyncState final {
 public:
  static constexpr ExternalPointerTag kManagedTag = kJsonParseAsyncStateTag;
  JsonParseAsyncState(Isolate* isolate, Handle<String> source,
                      Handle<Object> reviver, Handle<JSPromise> promise,
                      std::shared_ptr<BackgroundJsonData> data,
                      std::shared_ptr<v8::TaskRunner> runner)
      : isolate_(isolate),
        handles_(std::make_unique<PersistentHandles>(isolate)),
        context_(handles_->NewHandle(isolate->native_context())),
        source_(handles_->NewHandle(source)),
        reviver_(handles_->NewHandle(reviver)),
        promise_(handles_->NewHandle(promise)),
        result_(handles_->NewHandle(*isolate->factory()->undefined_value())),
        result_node_(
            handles_->NewHandle(*isolate->factory()->undefined_value())),
        data_(std::move(data)),
        runner_(std::move(runner)) {
    auto* jobs = GetJsonParseJobs();
    base::MutexGuard lock(&jobs->mutex);
    jobs->jobs[isolate_].insert(this);
  }
  ~JsonParseAsyncState() { ReleaseRoots(); }
  void Cancel() { data_->cancelled.store(true, std::memory_order_relaxed); }
  std::shared_ptr<v8::TaskRunner> runner() const { return runner_; }

  bool Run() {
    if (data_->cancelled.load(std::memory_order_relaxed)) return true;
    HandleScope scope(isolate_);
    SaveAndSwitchContext context(isolate_, *context_);
    v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate_));
    if (!accounted_tape_) {
      accounted_tape_ = true;
      tape_bytes_ = data_->nodes.capacity() * sizeof(BackgroundJsonNode) +
                    data_->source_ends.capacity() * sizeof(uint32_t);
      reinterpret_cast<v8::Isolate*>(isolate_)
          ->AdjustAmountOfExternalAllocatedMemory(tape_bytes_);
    }
    bool done =
        data_->is_one_byte ? BuildSlice<uint8_t>() : BuildSlice<uint16_t>();
    if (isolate_->has_exception()) {
      RejectJsonException(isolate_, promise_, &caught);
      return true;
    }
    if (!done) return false;
    Handle<Object> value = result_;
    // User JS closures cannot execute concurrently in the caller's isolate.
    if (data_->track_source &&
        !JsonParseInternalizer::Internalize(isolate_, value, reviver_, source_,
                                            result_node_)
             .ToHandle(&value)) {
      RejectJsonException(isolate_, promise_, &caught);
      return true;
    }
    if (JSPromise::Resolve(promise_, value).is_null()) {
      RejectJsonException(isolate_, promise_, &caught);
    }
    return true;
  }

  void ReleaseRoots() {
    if (!handles_) return;
    auto* jobs = GetJsonParseJobs();
    {
      base::MutexGuard lock(&jobs->mutex);
      auto it = jobs->jobs.find(isolate_);
      DCHECK(it != jobs->jobs.end());
      it->second.erase(this);
      if (it->second.empty()) jobs->jobs.erase(it);
    }
    handles_.reset();
    data_.reset();
    if (tape_bytes_) {
      reinterpret_cast<v8::Isolate*>(isolate_)
          ->AdjustAmountOfExternalAllocatedMemory(-tape_bytes_);
      tape_bytes_ = 0;
    }
    std::vector<Frame>().swap(frames_);
    std::vector<Handle<Object>>().swap(roots_);
    properties_ = {};
    elements_ = {};
    property_nodes_ = {};
    element_nodes_ = {};
  }

 private:
  struct Frame {
    bool array;
    uint32_t end;
    size_t index;
    size_t root_mark;
    uint32_t max_index = 0;
    uint32_t elements = 0;
    Handle<JSArray> direct_array{};
    uint32_t next_element = 0;
  };

  Handle<Object> Retain(Handle<Object> value) {
    if (live_roots_ == roots_.size())
      roots_.push_back(handles_->NewHandle(value));
    else
      roots_[live_roots_].PatchValue(*value);
    return roots_[live_roots_++];
  }
  void ReleaseTo(size_t mark) {
    auto undefined = *isolate_->factory()->undefined_value();
    while (live_roots_ != mark) roots_[--live_roots_].PatchValue(undefined);
  }

  template <typename Char>
  void Emit(JsonParser<Char>* parser, Handle<Object> value,
            Handle<Object> node) {
    if (frames_.empty()) {
      result_.PatchValue(*value);
      result_node_.PatchValue(*node);
    } else if (frames_.back().array) {
      Frame& parent = frames_.back();
      if (!parent.direct_array.is_null()) {
        FixedArray::cast(parent.direct_array->elements())
            ->set(static_cast<int>(parent.next_element++), *value);
      } else {
        parser->element_stack_.emplace_back(Retain(value));
        if (data_->track_source) element_nodes_.emplace_back(Retain(node));
      }
    } else {
      parser->property_stack_.back().value = Retain(value);
      if (data_->track_source) property_nodes_.back() = Retain(node);
    }
  }

  template <typename Char>
  Handle<Object> Primitive(JsonParser<Char>* parser,
                           const BackgroundJsonNode& token, int offset) {
    using Node = BackgroundJsonNode;
    switch (token.kind) {
      case Node::kString:
        return parser->MakeString(token.AsString(offset));
      case Node::kNumber:
        return isolate_->factory()->NewNumber(token.data.number);
      case Node::kTrue:
        return isolate_->factory()->true_value();
      case Node::kFalse:
        return isolate_->factory()->false_value();
      case Node::kNull:
        return isolate_->factory()->null_value();
      default:
        UNREACHABLE();
    }
  }

  template <typename Char>
  Handle<Map> ObjectFeedback(JsonParser<Char>* parser, const Frame* parent) {
    if (parent && !parent->direct_array.is_null()) {
      if (parent->next_element && parent->direct_array->HasObjectElements()) {
        auto previous = FixedArray::cast(parent->direct_array->elements())
                            ->get(static_cast<int>(parent->next_element - 1));
        if (IsJSObject(previous)) {
          auto map = JSObject::cast(previous)->map();
          if (!map->IsDetached(isolate_)) return handle(map, isolate_);
        }
      }
      return {};
    }
    if (parent && parent->array &&
        parent->index < parser->element_stack_.size() &&
        IsJSObject(*parser->element_stack_.back())) {
      auto map = JSObject::cast(*parser->element_stack_.back())->map();
      if (!map->IsDetached(isolate_)) return handle(map, isolate_);
    }
    return {};
  }

  template <typename Char>
  Handle<Object> BuildSmallValue(JsonParser<Char>* parser, int offset,
                                 Handle<Map> feedback = {}) {
    using Node = BackgroundJsonNode;
    using Continuation = typename JsonParser<Char>::JsonContinuation;
    const Node& token = data_->nodes[cursor_++];
    if (token.kind != Node::kObject && token.kind != Node::kArray)
      return Primitive(parser, token, offset);
    if (token.kind == Node::kArray) {
      HandleScope scope(isolate_);
      size_t start = parser->element_stack_.size();
      while (cursor_ < token.data.container.end) {
        Handle<Map> next_feedback;
        if (parser->element_stack_.size() > start) {
          auto previous = *parser->element_stack_.back();
          if (IsJSObject(previous)) {
            auto map = JSObject::cast(previous)->map();
            if (!map->IsDetached(isolate_))
              next_feedback = handle(map, isolate_);
          }
        }
        auto value = BuildSmallValue(parser, offset, next_feedback);
        parser->element_stack_.emplace_back(value);
      }
      auto value = parser->BuildJsonArray(start);
      parser->element_stack_.resize_no_init(start);
      return scope.CloseAndEscape(value);
    }
    size_t start = parser->property_stack_.size();
    Continuation cont(isolate_, Continuation::kObjectProperty, start);
    while (cursor_ < token.data.container.end) {
      auto key = data_->nodes[cursor_++].AsString(offset);
      if (key.is_index()) {
        ++cont.elements;
        cont.max_index = std::max(cont.max_index, key.index());
      }
      auto value = BuildSmallValue(parser, offset);
      parser->property_stack_.emplace_back(key, value);
    }
    auto value = parser->BuildJsonObject(cont, feedback);
    parser->property_stack_.resize_no_init(start);
    return cont.scope.CloseAndEscape(Handle<Object>::cast(value));
  }

  template <typename Char>
  void Close(JsonParser<Char>* parser) {
    using Continuation = typename JsonParser<Char>::JsonContinuation;
    Frame frame = frames_.back();
    Continuation cont(isolate_,
                      frame.array ? Continuation::kArrayElement
                                  : Continuation::kObjectProperty,
                      frame.index);
    cont.max_index = frame.max_index;
    cont.elements = frame.elements;
    Handle<Object> value;
    Handle<Object> node = isolate_->factory()->undefined_value();
    if (!frame.direct_array.is_null()) {
      frame.direct_array->set_length(Smi::FromInt(frame.next_element));
      value = handle(*frame.direct_array, isolate_);
    } else if (frame.array) {
      value = parser->BuildJsonArray(frame.index);
      if (data_->track_source) {
        int length =
            static_cast<int>(parser->element_stack_.size() - frame.index);
        auto snapshots = isolate_->factory()->NewFixedArray(length * 2);
        for (int i = 0; i < length; ++i) {
          snapshots->set(i * 2, *element_nodes_[frame.index + i]);
          snapshots->set(i * 2 + 1, *parser->element_stack_[frame.index + i]);
        }
        node = snapshots;
        element_nodes_.resize_no_init(frame.index);
      }
      parser->element_stack_.resize_no_init(frame.index);
    } else {
      auto feedback = ObjectFeedback(
          parser, frames_.size() > 1 ? &frames_[frames_.size() - 2] : nullptr);
      value = parser->BuildJsonObject(cont, feedback);
      if (data_->track_source) {
        int length =
            static_cast<int>(parser->property_stack_.size() - frame.index);
        auto snapshots = ObjectTwoHashTable::New(isolate_, length);
        for (int i = 0; i < length; ++i) {
          const auto& property = parser->property_stack_[frame.index + i];
          Handle<String> key =
              property.string.is_index()
                  ? isolate_->factory()->Uint32ToString(property.string.index())
                  : parser->MakeString(property.string);
          snapshots = ObjectTwoHashTable::Put(
              isolate_, snapshots, key,
              {property_nodes_[frame.index + i], property.value});
        }
        node = snapshots;
        property_nodes_.resize_no_init(frame.index);
      }
      parser->property_stack_.resize_no_init(frame.index);
    }
    ReleaseTo(frame.root_mark);
    frames_.pop_back();
    Emit(parser, value, node);
  }

  template <typename Char>
  bool BuildSlice() {
    using Node = BackgroundJsonNode;
    JsonParser<Char> parser(isolate_, source_);
    STACK_CHECK(isolate_, true);
    if (data_->failed) {
      parser.cursor_ += data_->error_position;
      if (data_->error_message) {
        parser.ReportUnexpectedToken(JsonToken::ILLEGAL, data_->error_message);
      } else {
        parser.ReportUnexpectedCharacter(parser.CurrentCharacter());
      }
      return true;
    }
    const int source_offset = parser.position();
    parser.property_stack_ = std::move(properties_);
    parser.element_stack_ = std::move(elements_);
    const auto deadline =
        base::TimeTicks::Now() + base::TimeDelta::FromMilliseconds(2);
    bool done = false;
    // Only materialization is sliced. Use JSON's fast builders instead of
    // repeated generic CreateDataProperty calls or a serialized object clone.
    for (int work = 0; work < 4096; ++work) {
      if ((work & 63) == 0) {
        STACK_CHECK(isolate_, true);
        if (work != 0 && base::TimeTicks::Now() >= deadline) break;
      }
      HandleScope iteration_scope(isolate_);
      if (!frames_.empty() && cursor_ == frames_.back().end) {
        Close(&parser);
        continue;
      }
      if (cursor_ == data_->nodes.size()) {
        DCHECK(frames_.empty());
        done = true;
        break;
      }
      const Node& token = data_->nodes[cursor_];
      if (!frames_.empty() && !frames_.back().direct_array.is_null() &&
          token.kind >= Node::kString) {
        Frame& frame = frames_.back();
        int index = static_cast<int>(frame.next_element++);
        auto array = frame.direct_array;
        if (array->HasDoubleElements()) {
          FixedDoubleArray::cast(array->elements())
              ->set(index, token.data.number);
        } else if (array->HasSmiElements()) {
          FixedArray::cast(array->elements())
              ->set(index,
                    Smi::FromInt(static_cast<int32_t>(token.data.number)));
        } else {
          auto value = Primitive(&parser, token, source_offset);
          FixedArray::cast(array->elements())->set(index, *value);
        }
        ++cursor_;
        continue;
      }
      Handle<Object> value;
      switch (token.kind) {
        case Node::kObject:
        case Node::kArray: {
          bool array = token.kind == Node::kArray;
          // Keep recursion bounded in both depth and work. Large/deep values
          // use the resumable path; small records need no persistent roots.
          if (!data_->track_source && token.depth <= 16 &&
              token.data.container.end - cursor_ <= 128 &&
              GetCurrentStackPosition() >
                  isolate_->stack_guard()->real_climit() + 64 * KB) {
            auto feedback = ObjectFeedback(
                &parser, frames_.empty() ? nullptr : &frames_.back());
            auto value = BuildSmallValue(&parser, source_offset, feedback);
            Emit(&parser, value, isolate_->factory()->undefined_value());
            continue;
          }
          frames_.push_back({array, token.data.container.end,
                             array ? parser.element_stack_.size()
                                   : parser.property_stack_.size(),
                             live_roots_});
          if (array && !data_->track_source) {
            ElementsKind kind =
                token.flags & Node::kAllSmis      ? PACKED_SMI_ELEMENTS
                : token.flags & Node::kAllNumbers ? PACKED_DOUBLE_ELEMENTS
                                                  : PACKED_ELEMENTS;
            auto result = isolate_->factory()->NewJSArray(
                kind, 0, static_cast<int>(token.data.container.count));
            frames_.back().direct_array = Handle<JSArray>::cast(Retain(result));
          }
          ++cursor_;
          continue;
        }
        case Node::kKey: {
          auto key = token.AsString(source_offset);
          if (key.is_index()) {
            ++frames_.back().elements;
            frames_.back().max_index =
                std::max(frames_.back().max_index, key.index());
          }
          parser.property_stack_.emplace_back(key);
          if (data_->track_source)
            property_nodes_.emplace_back(Handle<Object>());
          ++cursor_;
          continue;
        }
        case Node::kString:
          value = parser.MakeString(token.AsString(source_offset));
          break;
        case Node::kNumber:
          value = isolate_->factory()->NewNumber(token.data.number);
          break;
        case Node::kTrue:
          value = isolate_->factory()->true_value();
          break;
        case Node::kFalse:
          value = isolate_->factory()->false_value();
          break;
        case Node::kNull:
          value = isolate_->factory()->null_value();
          break;
      }
      Handle<Object> node = isolate_->factory()->undefined_value();
      if (data_->track_source) {
        node = isolate_->factory()->NewSubString(source_, token.start,
                                                 data_->source_ends[cursor_]);
      }
      Emit(&parser, value, node);
      ++cursor_;
    }
    properties_ = std::move(parser.property_stack_);
    elements_ = std::move(parser.element_stack_);
    return done;
  }

  Isolate* const isolate_;
  std::unique_ptr<PersistentHandles> handles_;
  Handle<NativeContext> context_;
  Handle<String> source_;
  Handle<Object> reviver_;
  Handle<JSPromise> promise_;
  Handle<Object> result_, result_node_;
  std::shared_ptr<BackgroundJsonData> data_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::vector<Frame> frames_;
  std::vector<Handle<Object>> roots_;
  size_t live_roots_ = 0, cursor_ = 0;
  int64_t tape_bytes_ = 0;
  bool accounted_tape_ = false;
  base::SmallVector<JsonProperty, 16> properties_;
  base::SmallVector<Handle<Object>, 16> elements_, property_nodes_,
      element_nodes_;
};

bool HasPendingJsonParseTasks(Isolate* isolate) {
  auto* jobs = GetJsonParseJobs();
  base::MutexGuard lock(&jobs->mutex);
  return jobs->jobs.find(isolate) != jobs->jobs.end();
}

void CancelJsonParseTasks(Isolate* isolate) {
  auto* jobs = GetJsonParseJobs();
  base::MutexGuard lock(&jobs->mutex);
  auto it = jobs->jobs.find(isolate);
  if (it != jobs->jobs.end())
    for (auto* state : it->second) state->Cancel();
}

namespace {

class JsonParseAsyncTask final : public CancelableTask {
 public:
  JsonParseAsyncTask(Isolate* isolate,
                     Handle<Managed<JsonParseAsyncState>> state)
      : CancelableTask(isolate), isolate_(isolate), state_(state) {}
  void RunInternal() override {
    v8::Isolate::Scope isolate_scope(reinterpret_cast<v8::Isolate*>(isolate_));
    if (state_->raw()->Run()) {
      state_->raw()->ReleaseRoots();
      GlobalHandles::Destroy(state_.location());
    } else {
      state_->raw()->runner()->PostNonNestableTask(
          std::make_unique<JsonParseAsyncTask>(isolate_, state_));
    }
  }

 private:
  Isolate* const isolate_;
  // Canceled tasks can outlive the isolate; no root access in destructors.
  Handle<Managed<JsonParseAsyncState>> state_;
};

class JsonParseBackgroundTask final : public CancelableTask {
 public:
  JsonParseBackgroundTask(Isolate* isolate,
                          std::shared_ptr<BackgroundJsonData> data,
                          std::shared_ptr<v8::TaskRunner> runner,
                          std::unique_ptr<v8::Task> completion)
      : CancelableTask(isolate),
        data_(std::move(data)),
        runner_(std::move(runner)),
        completion_(std::move(completion)) {}
  void RunInternal() override {
    ParseJsonInBackground(data_.get());
    runner_->PostNonNestableTask(std::move(completion_));
  }

 private:
  std::shared_ptr<BackgroundJsonData> data_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::unique_ptr<v8::Task> completion_;
};

}  // namespace

MaybeHandle<JSPromise> JsonParseAsync(Isolate* isolate, Handle<Object> source,
                                      Handle<Object> reviver) {
  Handle<JSPromise> promise = isolate->factory()->NewJSPromise();
  v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate));
  Handle<String> string;
  if (!Object::ToString(isolate, source).ToHandle(&string)) {
    if (isolate->is_execution_terminating()) {
      caught.ReThrow();
      return {};
    }
    RejectJsonException(isolate, promise, &caught);
    return promise;
  }
  auto* platform = V8::GetCurrentPlatform();
  auto runner = platform->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  if (!runner || !runner->NonNestableTasksEnabled() ||
      platform->NumberOfWorkerThreads() == 0) {
    JSPromise::Reject(
        promise, isolate->factory()->NewError(MessageTemplate::kUnsupported));
    return promise;
  }
  string = String::Flatten(isolate, string);
  auto data = std::make_shared<BackgroundJsonData>(IsCallable(*reviver));
  data->is_one_byte = String::IsOneByteRepresentationUnderneath(*string);
  int length = string->length();
  if (data->is_one_byte) {
    data->one_byte = base::OwnedVector<uint8_t>::NewForOverwrite(length);
    DisallowGarbageCollection no_gc;
    String::WriteToFlat(*string, data->one_byte.begin(), 0, length);
  } else {
    data->two_byte = base::OwnedVector<uint16_t>::NewForOverwrite(length);
    DisallowGarbageCollection no_gc;
    String::WriteToFlat(*string, data->two_byte.begin(), 0, length);
  }
  auto state = Managed<JsonParseAsyncState>::FromUniquePtr(
      isolate,
      sizeof(JsonParseAsyncState) + sizeof(BackgroundJsonData) +
          static_cast<size_t>(length) * (data->is_one_byte ? 1 : 2),
      std::make_unique<JsonParseAsyncState>(isolate, string, reviver, promise,
                                            data, runner));
  auto root = Handle<Managed<JsonParseAsyncState>>::cast(
      isolate->global_handles()->Create(*state));
  platform->CallOnWorkerThread(std::make_unique<JsonParseBackgroundTask>(
      isolate, std::move(data), runner,
      std::make_unique<JsonParseAsyncTask>(isolate, root)));
  return promise;
}

}  // namespace internal
}  // namespace v8
