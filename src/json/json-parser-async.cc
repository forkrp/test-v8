// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/v8-exception.h"
#include "include/v8-isolate.h"
#include "src/base/platform/time.h"
#include "src/common/message-template.h"
#include "src/execution/isolate-inl.h"
#include "src/handles/global-handles-inl.h"
#include "src/handles/persistent-handles.h"
#include "src/json/json-parser.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/objects/managed-inl.h"
#include "src/objects/objects-inl.h"
#include "src/tasks/cancelable-task.h"

namespace v8 {
namespace internal {

namespace {

// Do not turn termination into an ordinary rejected promise.
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

}  // namespace

// Only the active nesting stack is rooted separately. Completed siblings are
// owned by their parent, and handle slots are reused at each nesting depth.
// Managed owns this state so isolate teardown releases roots even when the
// embedder keeps canceled foreground tasks alive after disposing the isolate.
class JsonParseAsyncState final {
 public:
  static constexpr ExternalPointerTag kManagedTag = kJsonParseAsyncStateTag;

  JsonParseAsyncState(Isolate* isolate, Handle<String> source,
                      Handle<Object> reviver, Handle<JSPromise> promise,
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
        track_source_(IsCallable(*reviver)),
        runner_(std::move(runner)) {}

  Isolate* isolate() const { return isolate_; }
  std::shared_ptr<v8::TaskRunner> runner() const { return runner_; }

  // Returns true when finished, including failure or termination.
  bool Run() {
    HandleScope scope(isolate_);
    SaveAndSwitchContext context(isolate_, *context_);
    v8::TryCatch caught(reinterpret_cast<v8::Isolate*>(isolate_));
    if (offset_ < 0) {
      source_.PatchValue(*String::Flatten(isolate_, source_));
    }
    bool done = String::IsOneByteRepresentationUnderneath(*source_)
                    ? RunSlice<uint8_t>()
                    : RunSlice<uint16_t>();
    if (isolate_->has_exception()) {
      RejectJsonException(isolate_, promise_, &caught);
      return true;
    }
    if (!done) return false;

    Handle<Object> value = result_;
    // ponytail: user reviver code is synchronous, just as in JSON.parse. A
    // resumable internalizer is needed if reviver traversal also needs slicing.
    if (track_source_ && !JsonParseInternalizer::Internalize(
                              isolate_, value, reviver_, source_, result_node_)
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
    frames_.clear();
    handles_.reset();
  }

 private:
  enum class State {
    kValue,
    kObjectFirstKey,
    kObjectKey,
    kColon,
    kObjectNext,
    kArrayFirstValue,
    kArrayNext,
    kDone,
  };

  struct Frame {
    Handle<Object> object;
    Handle<Object> key;
    Handle<Object> node;
    uint32_t index = 0;
    bool is_array = false;
  };

  void Push(bool is_array) {
    Factory* factory = isolate_->factory();
    if (depth_ == frames_.size()) {
      frames_.push_back({handles_->NewHandle(*factory->undefined_value()),
                         handles_->NewHandle(*factory->undefined_value()),
                         handles_->NewHandle(*factory->undefined_value())});
    }
    Frame& frame = frames_[depth_++];
    frame.is_array = is_array;
    frame.index = 0;
    if (is_array) {
      frame.object.PatchValue(*factory->NewJSArray(0, PACKED_SMI_ELEMENTS));
      if (track_source_) {
        frame.node.PatchValue(*ArrayList::New(isolate_, 0));
      }
      state_ = State::kArrayFirstValue;
    } else {
      frame.object.PatchValue(
          *factory->NewJSObject(isolate_->object_function()));
      if (track_source_) {
        frame.node.PatchValue(*ObjectTwoHashTable::New(isolate_, 0));
      }
      state_ = State::kObjectFirstKey;
    }
  }

  bool Emit(Handle<Object> value, Handle<Object> node) {
    if (depth_ == 0) {
      result_.PatchValue(*value);
      result_node_.PatchValue(*node);
      state_ = State::kDone;
      return true;
    }
    Frame& frame = frames_[depth_ - 1];
    Handle<JSReceiver> object = Handle<JSReceiver>::cast(frame.object);
    PropertyKey key =
        frame.is_array ? PropertyKey(isolate_, frame.index)
                       : PropertyKey(isolate_, Handle<Name>::cast(frame.key));
    // Define an own data property: never invoke inherited setters (including
    // __proto__), even when another task mutates the realm's prototypes.
    if (JSReceiver::CreateDataProperty(isolate_, object, key, value,
                                       Just(kThrowOnError))
            .IsNothing()) {
      return false;
    }
    if (track_source_) {
      if (frame.is_array) {
        frame.node.PatchValue(*ArrayList::Add(
            isolate_, Handle<ArrayList>::cast(frame.node), node, value));
      } else {
        frame.node.PatchValue(*ObjectTwoHashTable::Put(
            isolate_, Handle<ObjectTwoHashTable>::cast(frame.node),
            Handle<String>::cast(frame.key), {node, value}));
      }
    }
    if (frame.is_array) ++frame.index;
    state_ = frame.is_array ? State::kArrayNext : State::kObjectNext;
    return true;
  }

  bool Close() {
    Frame& frame = frames_[--depth_];
    Handle<Object> value(*frame.object, isolate_);
    Handle<Object> node(*frame.node, isolate_);
    if (track_source_ && frame.is_array) {
      node = ArrayList::ToFixedArray(isolate_, Handle<ArrayList>::cast(node));
    }
    auto undefined = *isolate_->factory()->undefined_value();
    frame.object.PatchValue(undefined);
    frame.key.PatchValue(undefined);
    frame.node.PatchValue(undefined);
    return Emit(value, node);
  }

  template <typename Char>
  bool RunSlice() {
    JsonParser<Char> parser(isolate_, source_);
    if (offset_ >= 0) parser.cursor_ = parser.chars_ + offset_;
    const auto deadline =
        base::TimeTicks::Now() + base::TimeDelta::FromMilliseconds(2);
    // Count tokens AND whitespace, so long indentation also yields. The time
    // limit is cooperative, not a hard deadline: a single scalar, allocation,
    // GC, or syntax-error location calculation can take longer than the budget.
    // ponytail: giant scalars remain atomic; splitting them requires resumable
    // string/number scanners rather than merely a smaller task budget.
    constexpr int kMaxWork = 4096;
    for (int work = 0; work < kMaxWork; ++work) {
      if ((work & 63) == 0) {
        STACK_CHECK(isolate_, true);
        if (work != 0 && base::TimeTicks::Now() >= deadline) break;
      }
      base::uc32 c = parser.CurrentCharacter();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        parser.advance();
        continue;
      }
      HandleScope iteration_scope(isolate_);
      parser.SkipWhitespace();  // No whitespace remains; sets the lookahead.
      JsonToken token = parser.peek();
      switch (state_) {
        case State::kDone:
          if (token != JsonToken::EOS) {
            parser.ReportUnexpectedToken(
                token,
                MessageTemplate::kJsonParseUnexpectedNonWhiteSpaceCharacter);
          }
          return true;

        case State::kObjectFirstKey:
        case State::kObjectKey:
          if (state_ == State::kObjectFirstKey && token == JsonToken::RBRACE) {
            parser.advance();
            if (!Close()) return true;
            break;
          }
          if (token != JsonToken::STRING) {
            parser.ReportUnexpectedToken(
                token, state_ == State::kObjectFirstKey
                           ? MessageTemplate::kJsonParseExpectedPropNameOrRBrace
                           : MessageTemplate::
                                 kJsonParseExpectedDoubleQuotedPropertyName);
            return true;
          }
          parser.advance();
          {
            JsonString key = parser.ScanJsonString(true);
            if (isolate_->has_exception()) return true;
            frames_[depth_ - 1].key.PatchValue(*parser.MakeString(key));
          }
          state_ = State::kColon;
          break;

        case State::kColon:
          parser.Expect(
              JsonToken::COLON,
              MessageTemplate::kJsonParseExpectedColonAfterPropertyName);
          state_ = State::kValue;
          break;

        case State::kObjectNext:
        case State::kArrayNext: {
          bool array = state_ == State::kArrayNext;
          if (token == JsonToken::COMMA) {
            parser.advance();
            state_ = array ? State::kValue : State::kObjectKey;
          } else {
            parser.Expect(
                array ? JsonToken::RBRACK : JsonToken::RBRACE,
                array ? MessageTemplate::kJsonParseExpectedCommaOrRBrack
                      : MessageTemplate::kJsonParseExpectedCommaOrRBrace);
            if (isolate_->has_exception() || !Close()) return true;
          }
          break;
        }

        case State::kArrayFirstValue:
          if (token == JsonToken::RBRACK) {
            parser.advance();
            if (!Close()) return true;
            break;
          }
          state_ = State::kValue;
          [[fallthrough]];

        case State::kValue: {
          int start = parser.position();
          Handle<Object> value;
          switch (token) {
            case JsonToken::LBRACE:
            case JsonToken::LBRACK:
              parser.advance();
              Push(token == JsonToken::LBRACK);
              continue;
            case JsonToken::STRING: {
              parser.advance();
              JsonString string = parser.ScanJsonString(false);
              if (isolate_->has_exception()) return true;
              value = parser.MakeString(string);
              break;
            }
            case JsonToken::NUMBER:
              value = parser.ParseJsonNumber();
              break;
            case JsonToken::TRUE_LITERAL:
              parser.ScanLiteral("true");
              value = isolate_->factory()->true_value();
              break;
            case JsonToken::FALSE_LITERAL:
              parser.ScanLiteral("false");
              value = isolate_->factory()->false_value();
              break;
            case JsonToken::NULL_LITERAL:
              parser.ScanLiteral("null");
              value = isolate_->factory()->null_value();
              break;
            default:
              parser.ReportUnexpectedCharacter(c);
              return true;
          }
          if (isolate_->has_exception()) return true;
          Handle<Object> node = isolate_->factory()->undefined_value();
          if (track_source_) {
            node = isolate_->factory()->NewSubString(parser.source_, start,
                                                     parser.position());
          }
          if (!Emit(value, node)) return true;
          break;
        }
      }
      if (isolate_->has_exception()) return true;
    }
    offset_ = parser.position();
    return false;
  }

  Isolate* const isolate_;
  std::unique_ptr<PersistentHandles> handles_;
  Handle<NativeContext> context_;
  Handle<String> source_;
  Handle<Object> reviver_;
  Handle<JSPromise> promise_;
  Handle<Object> result_;
  Handle<Object> result_node_;
  const bool track_source_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::vector<Frame> frames_;
  size_t depth_ = 0;
  int offset_ = -1;
  State state_ = State::kValue;
};

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
  // A global root owned by the task chain, not by an individual task. Do NOT
  // destroy it from a task destructor: canceled tasks can outlive the isolate.
  // On cancellation, Managed's isolate teardown hook releases the parser state.
  Handle<Managed<JsonParseAsyncState>> state_;
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
  auto runner = V8::GetCurrentPlatform()->GetForegroundTaskRunner(
      reinterpret_cast<v8::Isolate*>(isolate));
  if (!runner || !runner->NonNestableTasksEnabled()) {
    // Never silently fall back to synchronous parsing or a nestable JS task.
    JSPromise::Reject(
        promise, isolate->factory()->NewError(MessageTemplate::kUnsupported));
    return promise;
  }
  auto state = Managed<JsonParseAsyncState>::FromUniquePtr(
      isolate, sizeof(JsonParseAsyncState),
      std::make_unique<JsonParseAsyncState>(isolate, string, reviver, promise,
                                            runner));
  auto root = Handle<Managed<JsonParseAsyncState>>::cast(
      isolate->global_handles()->Create(*state));
  runner->PostNonNestableTask(
      std::make_unique<JsonParseAsyncTask>(isolate, root));
  return promise;
}

}  // namespace internal
}  // namespace v8
