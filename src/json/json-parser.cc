// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/json/json-parser.h"

#include "src/base/strings.h"
#include "src/builtins/builtins.h"
#include "src/common/assert-scope.h"
#include "src/common/globals.h"
#include "src/common/message-template.h"
#include "src/debug/debug.h"
#include "src/execution/frames-inl.h"
#include "src/heap/factory.h"
#include "src/json/json-scanner.h"
#include "src/numbers/conversions.h"
#include "src/numbers/hash-seed-inl.h"
#include "src/objects/elements-kind.h"
#include "src/objects/js-data-object-builder.h"
#include "src/objects/field-type.h"
#include "src/objects/hash-table-inl.h"
#include "src/objects/map-updater.h"
#include "src/objects/objects-inl.h"
#include "src/objects/property-descriptor.h"
#include "src/objects/property-details.h"
#include "src/roots/roots.h"
#include "src/strings/char-predicates-inl.h"
#include "src/strings/string-hasher.h"
#include "src/utils/boxed-float.h"

namespace v8 {
namespace internal {

namespace {

constexpr JsonToken GetOneCharJsonToken(uint8_t c) {
  // clang-format off
  return
     c == '"' ? JsonToken::STRING :
     IsDecimalDigit(c) ?  JsonToken::NUMBER :
     c == '-' ? JsonToken::NUMBER :
     c == '[' ? JsonToken::LBRACK :
     c == '{' ? JsonToken::LBRACE :
     c == ']' ? JsonToken::RBRACK :
     c == '}' ? JsonToken::RBRACE :
     c == 't' ? JsonToken::TRUE_LITERAL :
     c == 'f' ? JsonToken::FALSE_LITERAL :
     c == 'n' ? JsonToken::NULL_LITERAL :
     c == ' ' ? JsonToken::WHITESPACE :
     c == '\t' ? JsonToken::WHITESPACE :
     c == '\r' ? JsonToken::WHITESPACE :
     c == '\n' ? JsonToken::WHITESPACE :
     c == ':' ? JsonToken::COLON :
     c == ',' ? JsonToken::COMMA :
     JsonToken::ILLEGAL;
  // clang-format on
}

// Table of one-character tokens, by character (0x00..0xFF only).
static const constexpr JsonToken one_char_json_tokens[256] = {
#define CALL_GET_SCAN_FLAGS(N) GetOneCharJsonToken(N),
    INT_0_TO_127_LIST(CALL_GET_SCAN_FLAGS)
#undef CALL_GET_SCAN_FLAGS
#define CALL_GET_SCAN_FLAGS(N) GetOneCharJsonToken(128 + N),
        INT_0_TO_127_LIST(CALL_GET_SCAN_FLAGS)
#undef CALL_GET_SCAN_FLAGS
};

}  // namespace

MaybeHandle<Object> JsonParseInternalizer::Internalize(
    Isolate* isolate, Handle<Object> result, Handle<Object> reviver,
    Handle<String> source, MaybeHandle<Object> val_node) {
  DCHECK(IsCallable(*reviver));
  JsonParseInternalizer internalizer(isolate, Handle<JSReceiver>::cast(reviver),
                                     source);
  Handle<JSObject> holder =
      isolate->factory()->NewJSObject(isolate->object_function());
  Handle<String> name = isolate->factory()->empty_string();
  JSObject::AddProperty(isolate, holder, name, result, NONE);
  return internalizer.InternalizeJsonProperty<kWithSource>(
      holder, name, val_node.ToHandleChecked(), result);
}

template <JsonParseInternalizer::WithOrWithoutSource with_source>
MaybeHandle<Object> JsonParseInternalizer::InternalizeJsonProperty(
    Handle<JSReceiver> holder, Handle<String> name, Handle<Object> val_node,
    Handle<Object> snapshot) {
  DCHECK_EQ(with_source == kWithSource,
            !val_node.is_null() && !snapshot.is_null());
  DCHECK(IsCallable(*reviver_));
  HandleScope outer_scope(isolate_);
  Handle<Object> value;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate_, value, Object::GetPropertyOrElement(isolate_, holder, name),
      Object);

  // When with_source == kWithSource, the source text is passed to the reviver
  // if the reviver has not mucked with the originally parsed value.
  //
  // When with_source == kWithoutSource, this is unused.
  bool pass_source_to_reviver =
      with_source == kWithSource && Object::SameValue(*value, *snapshot);

  if (IsJSReceiver(*value)) {
    Handle<JSReceiver> object = Handle<JSReceiver>::cast(value);
    Maybe<bool> is_array = Object::IsArray(object);
    if (is_array.IsNothing()) return MaybeHandle<Object>();
    if (is_array.FromJust()) {
      Handle<Object> length_object;
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate_, length_object,
          Object::GetLengthFromArrayLike(isolate_, object), Object);
      double length = Object::Number(*length_object);
      if (pass_source_to_reviver) {
        Handle<FixedArray> val_nodes_and_snapshots =
            Handle<FixedArray>::cast(val_node);
        int snapshot_length = val_nodes_and_snapshots->length() / 2;
        for (int i = 0; i < length; i++) {
          HandleScope inner_scope(isolate_);
          Handle<Object> index = isolate_->factory()->NewNumber(i);
          Handle<String> index_name =
              isolate_->factory()->NumberToString(index);
          // Even if the array pointer snapshot matched, it's possible the
          // array had new elements added that are not in the snapshotted
          // elements.
          const bool rv =
              i < snapshot_length
                  ? RecurseAndApply<kWithSource>(
                        object, index_name,
                        handle(val_nodes_and_snapshots->get(i * 2), isolate_),
                        handle(val_nodes_and_snapshots->get(i * 2 + 1),
                               isolate_))
                  : RecurseAndApply<kWithoutSource>(
                        object, index_name, Handle<Object>(), Handle<Object>());
          if (!rv) {
            return MaybeHandle<Object>();
          }
        }
      } else {
        for (int i = 0; i < length; i++) {
          HandleScope inner_scope(isolate_);
          Handle<Object> index = isolate_->factory()->NewNumber(i);
          Handle<String> index_name =
              isolate_->factory()->NumberToString(index);
          if (!RecurseAndApply<kWithoutSource>(
                  object, index_name, Handle<Object>(), Handle<Object>())) {
            return MaybeHandle<Object>();
          }
        }
      }
    } else {
      Handle<FixedArray> contents;
      ASSIGN_RETURN_ON_EXCEPTION(
          isolate_, contents,
          KeyAccumulator::GetKeys(isolate_, object, KeyCollectionMode::kOwnOnly,
                                  ENUMERABLE_STRINGS,
                                  GetKeysConversion::kConvertToString),
          Object);
      if (pass_source_to_reviver) {
        Handle<ObjectTwoHashTable> val_nodes_and_snapshots =
            Handle<ObjectTwoHashTable>::cast(val_node);
        for (int i = 0; i < contents->length(); i++) {
          HandleScope inner_scope(isolate_);
          Handle<String> key_name(String::cast(contents->get(i)), isolate_);
          auto property_val_node_and_snapshot =
              val_nodes_and_snapshots->Lookup(isolate_, key_name);
          Handle<Object> property_val_node(property_val_node_and_snapshot[0],
                                           isolate_);
          Handle<Object> property_snapshot(property_val_node_and_snapshot[1],
                                           isolate_);
          // Even if the object pointer snapshot matched, it's possible the
          // object had new properties added that are not in the snapshotted
          // contents.
          const bool rv =
              !IsTheHole(*property_snapshot)
                  ? RecurseAndApply<kWithSource>(
                        object, key_name, property_val_node, property_snapshot)
                  : RecurseAndApply<kWithoutSource>(
                        object, key_name, Handle<Object>(), Handle<Object>());
          if (!rv) {
            return MaybeHandle<Object>();
          }
        }
      } else {
        for (int i = 0; i < contents->length(); i++) {
          HandleScope inner_scope(isolate_);
          Handle<String> key_name(String::cast(contents->get(i)), isolate_);
          if (!RecurseAndApply<kWithoutSource>(
                  object, key_name, Handle<Object>(), Handle<Object>())) {
            return MaybeHandle<Object>();
          }
        }
      }
    }
  }

  Handle<JSObject> context =
      isolate_->factory()->NewJSObject(isolate_->object_function());
  if (pass_source_to_reviver && IsString(*val_node)) {
    JSReceiver::CreateDataProperty(isolate_, context,
                                   isolate_->factory()->source_string(),
                                   val_node, Just(kThrowOnError))
        .Check();
  }
  Handle<Object> argv[] = {name, value, context};
  Handle<Object> result;
  ASSIGN_RETURN_ON_EXCEPTION(
      isolate_, result, Execution::Call(isolate_, reviver_, holder, 3, argv),
      Object);
  return outer_scope.CloseAndEscape(result);
}

template <JsonParseInternalizer::WithOrWithoutSource with_source>
bool JsonParseInternalizer::RecurseAndApply(Handle<JSReceiver> holder,
                                            Handle<String> name,
                                            Handle<Object> val_node,
                                            Handle<Object> snapshot) {
  STACK_CHECK(isolate_, false);
  DCHECK(IsCallable(*reviver_));
  Handle<Object> result;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate_, result,
      InternalizeJsonProperty<with_source>(holder, name, val_node, snapshot),
      false);
  Maybe<bool> change_result = Nothing<bool>();
  if (IsUndefined(*result, isolate_)) {
    change_result = JSReceiver::DeletePropertyOrElement(holder, name,
                                                        LanguageMode::kSloppy);
  } else {
    PropertyDescriptor desc;
    desc.set_value(result);
    desc.set_configurable(true);
    desc.set_enumerable(true);
    desc.set_writable(true);
    change_result = JSReceiver::DefineOwnProperty(isolate_, holder, name, &desc,
                                                  Just(kDontThrow));
  }
  MAYBE_RETURN(change_result, false);
  return true;
}

template <typename Char>
JsonParser<Char>::JsonParser(Isolate* isolate, Handle<String> source)
    : isolate_(isolate),
      hash_seed_(HashSeed(isolate)),
      object_constructor_(isolate_->object_function()),
      original_source_(source) {
  size_t start = 0;
  size_t length = source->length();
  PtrComprCageBase cage_base(isolate);
  if (IsSlicedString(*source, cage_base)) {
    Tagged<SlicedString> string = SlicedString::cast(*source);
    start = string->offset();
    Tagged<String> parent = string->parent();
    if (IsThinString(parent, cage_base))
      parent = ThinString::cast(parent)->actual();
    source_ = handle(parent, isolate);
  } else {
    source_ = String::Flatten(isolate, source);
  }

  if (StringShape(*source_, cage_base).IsExternal()) {
    chars_ =
        static_cast<const Char*>(SeqExternalString::cast(*source_)->GetChars());
    chars_may_relocate_ = false;
  } else {
    DisallowGarbageCollection no_gc;
    isolate->main_thread_local_heap()->AddGCEpilogueCallback(
        UpdatePointersCallback, this);
    chars_ = SeqString::cast(*source_)->GetChars(no_gc);
    chars_may_relocate_ = true;
  }
  cursor_ = chars_ + start;
  end_ = cursor_ + length;
}

template <typename Char>
bool JsonParser<Char>::IsSpecialString() {
  // The special cases are undefined, NaN, Infinity, and {} being passed to the
  // parse method
  int offset = IsSlicedString(*original_source_)
                   ? SlicedString::cast(*original_source_)->offset()
                   : 0;
  size_t length = original_source_->length();
#define CASES(V)       \
  V("[object Object]") \
  V("undefined")       \
  V("Infinity")        \
  V("NaN")
  switch (length) {
#define CASE(n)          \
  case arraysize(n) - 1: \
    return CompareCharsEqual(chars_ + offset, n, arraysize(n) - 1);
    CASES(CASE)
    default:
      return false;
  }
#undef CASE
#undef CASES
}

template <typename Char>
MessageTemplate JsonParser<Char>::GetErrorMessageWithEllipses(
    Handle<Object>& arg, Handle<Object>& arg2, int pos) {
  MessageTemplate message;
  Factory* factory = this->factory();
  arg = factory->LookupSingleCharacterStringFromCode(*cursor_);
  int origin_source_length = original_source_->length();
  // only provide context for strings with at least
  // kMinOriginalSourceLengthForContext charcacters in length
  if (origin_source_length >= kMinOriginalSourceLengthForContext) {
    int substring_start = 0;
    int substring_end = origin_source_length;
    if (pos < kMaxContextCharacters) {
      message =
          MessageTemplate::kJsonParseUnexpectedTokenStartStringWithContext;
      // Output the string followed by elipses
      substring_end = pos + kMaxContextCharacters;
    } else if (pos >= kMaxContextCharacters &&
               pos < origin_source_length - kMaxContextCharacters) {
      message =
          MessageTemplate::kJsonParseUnexpectedTokenSurroundStringWithContext;
      // Add context before and after position of bad token surrounded by
      // elipses
      substring_start = pos - kMaxContextCharacters;
      substring_end = pos + kMaxContextCharacters;
    } else {
      message = MessageTemplate::kJsonParseUnexpectedTokenEndStringWithContext;
      // Add ellipses followed by some context before bad token
      substring_start = pos - kMaxContextCharacters;
    }
    arg2 =
        factory->NewSubString(original_source_, substring_start, substring_end);
  } else {
    arg2 = original_source_;
    // Output the entire string without ellipses but provide the token which
    // was unexpected
    message = MessageTemplate::kJsonParseUnexpectedTokenShortString;
  }
  return message;
}

template <typename Char>
MessageTemplate JsonParser<Char>::LookUpErrorMessageForJsonToken(
    JsonToken token, Handle<Object>& arg, Handle<Object>& arg2, int pos) {
  MessageTemplate message;
  switch (token) {
    case JsonToken::EOS:
      message = MessageTemplate::kJsonParseUnexpectedEOS;
      break;
    case JsonToken::NUMBER:
      message = MessageTemplate::kJsonParseUnexpectedTokenNumber;
      break;
    case JsonToken::STRING:
      message = MessageTemplate::kJsonParseUnexpectedTokenString;
      break;
    default:
      // Output entire string without ellipses and don't provide the token
      // that was unexpected because it makes the error messages more confusing
      if (IsSpecialString()) {
        arg = original_source_;
        message = MessageTemplate::kJsonParseShortString;
      } else {
        message = GetErrorMessageWithEllipses(arg, arg2, pos);
      }
  }
  return message;
}

template <typename Char>
void JsonParser<Char>::CalculateFileLocation(Handle<Object>& line,
                                             Handle<Object>& column) {
  // JSON allows only \r and \n as line terminators.
  // (See https://www.json.org/json-en.html - "whitespace")
  int line_number = 1;
  const Char* start =
      chars_ + (IsSlicedString(*original_source_)
                    ? SlicedString::cast(*original_source_)->offset()
                    : 0);
  const Char* last_line_break = start;
  const Char* cursor = start;
  const Char* end = cursor_;  // cursor_ points to the position of the error.
  for (; cursor < end; ++cursor) {
    if (*cursor == '\r' && cursor < end - 1 && cursor[1] == '\n') {
      // \r\n counts as a single line terminator, as of
      // https://tc39.es/ecma262/#sec-line-terminators. JSON itself does not
      // have a notion of lines or line terminators.
      ++cursor;
    }
    if (*cursor == '\r' || *cursor == '\n') {
      ++line_number;
      last_line_break = cursor + 1;
    }
  }
  int column_number = 1 + static_cast<int>(cursor - last_line_break);
  line = handle(Smi::FromInt(line_number), isolate());
  column = handle(Smi::FromInt(column_number), isolate());
}

template <typename Char>
void JsonParser<Char>::ReportUnexpectedToken(
    JsonToken token, base::Optional<MessageTemplate> errorMessage) {
  // Some exception (for example stack overflow) was already thrown.
  if (isolate_->has_exception()) return;

  // Parse failed. Current character is the unexpected token.
  Factory* factory = this->factory();
  int offset = IsSlicedString(*original_source_)
                   ? SlicedString::cast(*original_source_)->offset()
                   : 0;
  int pos = position() - offset;
  Handle<Object> arg(Smi::FromInt(pos), isolate());
  Handle<Object> arg2;
  Handle<Object> arg3;
  CalculateFileLocation(arg2, arg3);

  MessageTemplate message =
      errorMessage ? errorMessage.value()
                   : LookUpErrorMessageForJsonToken(token, arg, arg2, pos);

  Handle<Script> script(factory->NewScript(original_source_));
  DCHECK_IMPLIES(isolate_->NeedsSourcePositions(), script->has_line_ends());
  DebuggableStackFrameIterator it(isolate_);
  if (!it.done() && it.is_javascript()) {
    FrameSummary summary = it.GetTopValidFrame();
    script->set_eval_from_shared(summary.AsJavaScript().function()->shared());
    if (IsScript(*summary.script())) {
      script->set_origin_options(
          Script::cast(*summary.script())->origin_options());
    }
  }

  // We should sent compile error event because we compile JSON object in
  // separated source file.
  isolate()->debug()->OnCompileError(script);
  MessageLocation location(script, pos, pos + 1);
  isolate()->ThrowAt(factory->NewSyntaxError(message, arg, arg2, arg3),
                     &location);

  // Move the cursor to the end so we won't be able to proceed parsing.
  cursor_ = end_;
}

template <typename Char>
void JsonParser<Char>::ReportUnexpectedCharacter(base::uc32 c) {
  JsonToken token = JsonToken::ILLEGAL;
  if (c == kEndOfString) {
    token = JsonToken::EOS;
  } else if (c <= unibrow::Latin1::kMaxChar) {
    token = one_char_json_tokens[c];
  }
  return ReportUnexpectedToken(token);
}

template <typename Char>
JsonParser<Char>::~JsonParser() {
  if (StringShape(*source_).IsExternal()) {
    // Check that the string shape hasn't changed. Otherwise our GC hooks are
    // broken.
    SeqExternalString::cast(*source_);
  } else {
    // Check that the string shape hasn't changed. Otherwise our GC hooks are
    // broken.
    SeqString::cast(*source_);
    isolate()->main_thread_local_heap()->RemoveGCEpilogueCallback(
        UpdatePointersCallback, this);
  }
}

template <typename Char>
MaybeHandle<Object> JsonParser<Char>::ParseJson(Handle<Object> reviver) {
  Handle<Object> result;
  // Only record the val node when reviver is callable.
  bool reviver_is_callable = IsCallable(*reviver);
  bool should_track_json_source = reviver_is_callable;
  if (V8_UNLIKELY(should_track_json_source)) {
    ASSIGN_RETURN_ON_EXCEPTION(isolate(), result, ParseJsonValue<true>(),
                               Object);
  } else {
    ASSIGN_RETURN_ON_EXCEPTION(isolate(), result, ParseJsonValueRecursive(),
                               Object);
  }

  if (!Check(JsonToken::EOS)) {
    ReportUnexpectedToken(
        peek(), MessageTemplate::kJsonParseUnexpectedNonWhiteSpaceCharacter);
    return MaybeHandle<Object>();
  }
  if (isolate_->has_exception()) {
    return MaybeHandle<Object>();
  }
  return result;
}

MaybeHandle<Object> InternalizeJsonProperty(Handle<JSObject> holder,
                                            Handle<String> key);

namespace {
template <typename Char>
JsonToken GetTokenForCharacter(Char c) {
  return V8_LIKELY(c <= unibrow::Latin1::kMaxChar) ? one_char_json_tokens[c]
                                                   : JsonToken::ILLEGAL;
}
}  // namespace

template <typename Char>
void JsonParser<Char>::SkipWhitespace() {
  JsonToken local_next = JsonToken::EOS;

  cursor_ = std::find_if(cursor_, end_, [&](Char c) {
    JsonToken current = GetTokenForCharacter(c);
    bool result = current != JsonToken::WHITESPACE;
    if (V8_LIKELY(result)) local_next = current;
    return result;
  });

  next_ = local_next;
}

template <typename Char>
base::uc32 JsonParser<Char>::ScanUnicodeCharacter() {
  base::uc32 value = 0;
  for (int i = 0; i < 4; i++) {
    int digit = base::HexValue(NextCharacter());
    if (V8_UNLIKELY(digit < 0)) return kInvalidUnicodeCharacter;
    value = value * 16 + digit;
  }
  return value;
}

// Parse any JSON value.
template <typename Char>
JsonString JsonParser<Char>::ScanJsonPropertyKey(JsonContinuation* cont) {
  {
    DisallowGarbageCollection no_gc;
    const Char* start = cursor_;
    base::uc32 first = CurrentCharacter();
    if (first == '\\' && NextCharacter() == 'u') first = ScanUnicodeCharacter();
    if (IsDecimalDigit(first)) {
      if (first == '0') {
        if (NextCharacter() == '"') {
          advance();
          // Record element information.
          cont->elements++;
          DCHECK_LE(0, cont->max_index);
          return JsonString(0);
        }
      } else {
        uint32_t index = first - '0';
        while (true) {
          cursor_ = std::find_if(cursor_ + 1, end_, [&index](Char c) {
            return !TryAddArrayIndexChar(&index, c);
          });

          if (CurrentCharacter() == '"') {
            advance();
            // Record element information.
            cont->elements++;
            cont->max_index = std::max(cont->max_index, index);
            return JsonString(index);
          }

          if (CurrentCharacter() == '\\' && NextCharacter() == 'u') {
            if (TryAddArrayIndexChar(&index, ScanUnicodeCharacter())) continue;
          }

          break;
        }
      }
    }
    // Reset cursor_ to start if the key is not an index.
    cursor_ = start;
  }
  return ScanJsonString(true);
}

class NamedPropertyValueIterator {
 public:
  NamedPropertyValueIterator(const JsonProperty* it, const JsonProperty* end)
      : it_(it), end_(end) {
    DCHECK_LE(it_, end_);
    DCHECK_IMPLIES(it_ != end_, !it_->string.is_index());
  }

  NamedPropertyValueIterator& operator++() {
    DCHECK_LT(it_, end_);
    do {
      it_++;
    } while (it_ != end_ && it_->string.is_index());
    return *this;
  }

  Handle<Object> operator*() { return it_->value; }

  bool operator!=(const NamedPropertyValueIterator& other) const {
    return it_ != other.it_;
  }

 private:
  // We need to store both the current iterator and the iterator end, since we
  // don't want to iterate past the end on operator++ if the last property is an
  // index property.
  const JsonProperty* it_;
  const JsonProperty* end_;
};

template <typename Char>
class JsonParser<Char>::NamedPropertyIterator {
 public:
  NamedPropertyIterator(JsonParser<Char>& parser, const JsonProperty* it,
                        const JsonProperty* end)
      : parser_(parser), it_(it), end_(end) {
    DCHECK_LE(it_, end_);
    while (it_ != end_ && it_->string.is_index()) {
      it_++;
    }
    start_ = it_;
  }

  void Advance() {
    DCHECK_LT(it_, end_);
    do {
      it_++;
    } while (it_ != end_ && it_->string.is_index());
  }

  bool Done() const {
    DCHECK_LE(it_, end_);
    return it_ == end_;
  }

  Handle<String> GetKey(Handle<String> expected_key_hint) {
    return parser_.MakeString(it_->string, expected_key_hint);
  }
  Handle<Object> GetValue(bool will_revisit_value) {
    // Revisiting values is free, so we don't need to cache the value anywhere.
    return it_->value;
  }
  NamedPropertyValueIterator RevisitValues() {
    return NamedPropertyValueIterator(start_, it_);
  }

  const JsonProperty* current() const { return it_; }

 private:
  JsonParser<Char>& parser_;

  const JsonProperty* start_;
  const JsonProperty* it_;
  const JsonProperty* end_;
};

template <typename Char>
class JsonParser<Char>::CachedNamedPropertyIterator
    : public JsonParser<Char>::NamedPropertyIterator {
 public:
  using Base = typename JsonParser<Char>::NamedPropertyIterator;
  CachedNamedPropertyIterator(JsonParser<Char>* parser,
                              const JsonProperty* begin,
                              const JsonProperty* end,
                              base::Vector<const Handle<String>> keys)
      : Base(*parser, begin, end), keys_(keys) {
    DCHECK_EQ(JsonString::kNoCacheSlot, keys.length());
  }

  Handle<String> GetKnownKey() {
    uint8_t slot = this->current()->string.cache_slot();
    if (slot == JsonString::kNoCacheSlot) return {};
    auto key = keys_[slot];
    // Empty names are not cached; an empty slot still needs GetKey's hint.
    return key->length() ? key : Handle<String>();
  }

  Handle<String> GetKey(Handle<String> expected_key_hint) {
    uint8_t slot = this->current()->string.cache_slot();
    if (slot == JsonString::kNoCacheSlot) {
      return Base::GetKey(expected_key_hint);
    }
    auto key = keys_[slot];
    DCHECK(!key.is_null());
    // Empty names are never cached. Patch the persistent slot, not a local
    // handle, so a later slice can reuse the internalized string across GC.
    if (key->length() == 0) key.PatchValue(*Base::GetKey(expected_key_hint));
    return key;
  }

 private:
  base::Vector<const Handle<String>> keys_;
};

template <typename Char>
Handle<JSObject> JsonParser<Char>::BuildJsonObject(
    const JsonContinuation& cont, Handle<Map> feedback) {
  return BuildJsonObjectImpl<false>(cont, feedback, {});
}

template <typename Char>
Handle<JSObject> JsonParser<Char>::BuildJsonObjectWithCachedKeys(
    const JsonContinuation& cont, Handle<Map> feedback,
    base::Vector<const Handle<String>> keys) {
  return BuildJsonObjectImpl<true>(cont, feedback, keys);
}

// Share the construction logic, not a runtime cache branch in JSON.parse.
template <typename Char>
template <bool cached_keys>
Handle<JSObject> JsonParser<Char>::BuildJsonObjectImpl(
    const JsonContinuation& cont, Handle<Map> feedback,
    base::Vector<const Handle<String>> keys) {
  if (!feedback.is_null() && feedback->is_deprecated()) {
    feedback = Map::Update(isolate_, feedback);
  }
  size_t start = cont.index;
  DCHECK_LE(start, property_stack_.size());
  int length = static_cast<int>(property_stack_.size() - start);
  int named_length = length - cont.elements;
  DCHECK_LE(0, named_length);

  Handle<FixedArrayBase> elements;
  ElementsKind elements_kind = HOLEY_ELEMENTS;

  // First store the elements.
  if (cont.elements > 0) {
    // Store as dictionary elements if that would use less memory.
    if (ShouldConvertToSlowElements(cont.elements, cont.max_index + 1)) {
      Handle<NumberDictionary> elms =
          NumberDictionary::New(isolate_, cont.elements);
      for (int i = 0; i < length; i++) {
        const JsonProperty& property = property_stack_[start + i];
        if (!property.string.is_index()) continue;
        uint32_t index = property.string.index();
        Handle<Object> value = property.value;
        NumberDictionary::UncheckedSet(isolate_, elms, index, value);
      }
      elms->SetInitialNumberOfElements(length);
      elms->UpdateMaxNumberKey(cont.max_index, Handle<JSObject>::null());
      elements_kind = DICTIONARY_ELEMENTS;
      elements = elms;
    } else {
      Handle<FixedArray> elms =
          factory()->NewFixedArrayWithHoles(cont.max_index + 1);
      DisallowGarbageCollection no_gc;
      Tagged<FixedArray> raw_elements = *elms;
      WriteBarrierMode mode = raw_elements->GetWriteBarrierMode(no_gc);

      for (int i = 0; i < length; i++) {
        const JsonProperty& property = property_stack_[start + i];
        if (!property.string.is_index()) continue;
        uint32_t index = property.string.index();
        Handle<Object> value = property.value;
        raw_elements->set(static_cast<int>(index), *value, mode);
      }
      elements = elms;
    }
  } else {
    elements = factory()->empty_fixed_array();
  }

  JSDataObjectBuilder js_data_object_builder(
      isolate_, elements_kind, named_length, feedback,
      JSDataObjectBuilder::kHeapNumbersGuaranteedUniquelyOwned);

  if constexpr (cached_keys) {
    CachedNamedPropertyIterator it(this, property_stack_.begin() + start,
                                   property_stack_.end(), keys);
    return js_data_object_builder.BuildFromIterator(it, elements);
  } else {
    NamedPropertyIterator it(*this, property_stack_.begin() + start,
                             property_stack_.end());
    return js_data_object_builder.BuildFromIterator(it, elements);
  }
}

template <typename Char>
Handle<Object> JsonParser<Char>::BuildJsonArray(size_t start) {
  int length = static_cast<int>(element_stack_.size() - start);

  ElementsKind kind = PACKED_SMI_ELEMENTS;
  for (size_t i = start; i < element_stack_.size(); i++) {
    Tagged<Object> value = *element_stack_[i];
    if (IsHeapObject(value)) {
      if (IsHeapNumber(HeapObject::cast(value))) {
        kind = PACKED_DOUBLE_ELEMENTS;
      } else {
        kind = PACKED_ELEMENTS;
        break;
      }
    }
  }

  Handle<JSArray> array = factory()->NewJSArray(kind, length, length);
  if (kind == PACKED_DOUBLE_ELEMENTS) {
    DisallowGarbageCollection no_gc;
    Tagged<FixedDoubleArray> elements =
        FixedDoubleArray::cast(array->elements());
    for (int i = 0; i < length; i++) {
      elements->set(i, Object::Number(*element_stack_[start + i]));
    }
  } else {
    DisallowGarbageCollection no_gc;
    Tagged<FixedArray> elements = FixedArray::cast(array->elements());
    WriteBarrierMode mode = kind == PACKED_SMI_ELEMENTS
                                ? SKIP_WRITE_BARRIER
                                : elements->GetWriteBarrierMode(no_gc);
    for (int i = 0; i < length; i++) {
      elements->set(i, *element_stack_[start + i], mode);
    }
  }
  return array;
}

// Parse rawJSON value.
template <typename Char>
bool JsonParser<Char>::ParseRawJson() {
  if (end_ == cursor_) {
    isolate_->Throw(*isolate_->factory()->NewSyntaxError(
        MessageTemplate::kInvalidRawJsonValue));
    return false;
  }
  next_ = GetTokenForCharacter(*cursor_);
  switch (peek()) {
    case JsonToken::STRING:
      Consume(JsonToken::STRING);
      ScanJsonString(false);
      break;

    case JsonToken::NUMBER:
      ParseJsonNumber();
      break;

    case JsonToken::TRUE_LITERAL:
      ScanLiteral("true");
      break;

    case JsonToken::FALSE_LITERAL:
      ScanLiteral("false");
      break;

    case JsonToken::NULL_LITERAL:
      ScanLiteral("null");
      break;

    default:
      ReportUnexpectedCharacter(CurrentCharacter());
      return false;
  }
  if (isolate_->has_exception()) return false;
  if (cursor_ != end_) {
    isolate_->Throw(*isolate_->factory()->NewSyntaxError(
        MessageTemplate::kInvalidRawJsonValue));
    return false;
  }
  return true;
}

template <typename Char>
V8_INLINE MaybeHandle<Object> JsonParser<Char>::ParseJsonValueRecursive(
    Handle<Map> feedback) {
  SkipWhitespace();
  switch (peek()) {
    case JsonToken::NUMBER:
      return ParseJsonNumber();
    case JsonToken::STRING:
      Consume(JsonToken::STRING);
      return MakeString(ScanJsonString(false));

    case JsonToken::TRUE_LITERAL:
      ScanLiteral("true");
      return factory()->true_value();
    case JsonToken::FALSE_LITERAL:
      ScanLiteral("false");
      return factory()->false_value();
    case JsonToken::NULL_LITERAL:
      ScanLiteral("null");
      return factory()->null_value();

    case JsonToken::LBRACE:
      return ParseJsonObject(feedback);
    case JsonToken::LBRACK:
      return ParseJsonArray();

    case JsonToken::COLON:
    case JsonToken::COMMA:
    case JsonToken::ILLEGAL:
    case JsonToken::RBRACE:
    case JsonToken::RBRACK:
    case JsonToken::EOS:
      ReportUnexpectedCharacter(CurrentCharacter());
      // Pop the continuation stack to correctly tear down handle scopes.
      return MaybeHandle<Object>();

    case JsonToken::WHITESPACE:
      UNREACHABLE();
  }
}

// 12.6 token helpers return void; preserve their failure position/messages
// while adapting the upstream boolean-returning property parser.
#define JSON_EXPECT_NEXT_OR_RETURN(token, message, result) \
  do {                                                     \
    if (V8_UNLIKELY(!Check(token))) {                      \
      ReportUnexpectedToken(peek(), message);              \
      return result;                                       \
    }                                                      \
  } while (false)
#define JSON_EXPECT_OR_RETURN(token, message, result) \
  do {                                                \
    if (V8_UNLIKELY(peek() != token)) {               \
      ReportUnexpectedToken(peek(), message);         \
      return result;                                  \
    }                                                 \
    advance();                                        \
  } while (false)

template <typename Char>
bool JsonParser<Char>::FastKeyMatch(const uint8_t* key_chars,
                                    uint32_t key_length) {
  return key_length < static_cast<size_t>(end_ - cursor_) &&
         *(cursor_ + key_length) == '"' &&
         CompareCharsEqual(key_chars, cursor_, key_length);
}

template <typename Char>
bool JsonParser<Char>::FastKeyMatch(const uint8_t* key_chars,
                                    uint32_t key_length,
                                    JsonString scanned_key) {
  return key_length == static_cast<uint32_t>(scanned_key.length()) &&
         CompareCharsEqual(key_chars, chars_ + scanned_key.start(), key_length);
}

template <typename Char>
bool JsonParser<Char>::ParseJsonPropertyValue(const JsonString& key) {
  JSON_EXPECT_NEXT_OR_RETURN(
      JsonToken::COLON,
      MessageTemplate::kJsonParseExpectedColonAfterPropertyName, false);
  Handle<Object> value;
  if (V8_UNLIKELY(!ParseJsonValueRecursive().ToHandle(&value))) return false;
  property_stack_.emplace_back(key, value);
  return true;
}

namespace {

const uint8_t* GetFastKeyChars(Isolate* isolate, Tagged<String> key,
                               Tagged<Map> map,
                               const DisallowGarbageCollection& no_gc) {
  DCHECK(key->IsOneByteRepresentation());
#if V8_STATIC_ROOTS_BOOL
  ReadOnlyRoots roots(isolate);
  if (map == roots.internalized_one_byte_string_map()) {
    return SeqOneByteString::cast(key)->GetChars(no_gc);
  } else {
    DCHECK(map == roots.external_internalized_one_byte_string_map() ||
           map == roots.uncached_external_internalized_one_byte_string_map());
    return ExternalOneByteString::cast(key)->GetChars();
  }
#else
  InstanceType instance_type = map->instance_type();
  switch (instance_type) {
    case INTERNALIZED_ONE_BYTE_STRING_TYPE:
      return SeqOneByteString::cast(key)->GetChars(no_gc);
    case EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
    case UNCACHED_EXTERNAL_INTERNALIZED_ONE_BYTE_STRING_TYPE:
      return ExternalOneByteString::cast(key)->GetChars();
    default:
      UNREACHABLE();
  }
#endif
}

}  // namespace

template <typename Char>
template <DescriptorArray::FastIterableState fast_iterable_state>
bool JsonParser<Char>::ParseJsonObjectProperties(
    JsonContinuation* cont, MessageTemplate first_token_msg,
    Handle<DescriptorArray> descriptors) {
  using FastIterableState = DescriptorArray::FastIterableState;
  if constexpr (fast_iterable_state == FastIterableState::kJsonSlow) {
    do {
      JSON_EXPECT_NEXT_OR_RETURN(JsonToken::STRING, first_token_msg, false);
      first_token_msg =
          MessageTemplate::kJsonParseExpectedDoubleQuotedPropertyName;
      JsonString key = ScanJsonPropertyKey(cont);
      if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;
    } while (Check(JsonToken::COMMA));
  } else {
    DCHECK_GT(descriptors->number_of_descriptors(), 0);
    InternalIndex idx{0};
    do {
      JSON_EXPECT_NEXT_OR_RETURN(JsonToken::STRING, first_token_msg, false);
      first_token_msg =
          MessageTemplate::kJsonParseExpectedDoubleQuotedPropertyName;
      bool key_match;
      if constexpr (fast_iterable_state == FastIterableState::kJsonFast) {
        uint32_t key_length;
        {
          DisallowGarbageCollection no_gc;
          Tagged<String> expected_key = String::cast(descriptors->GetKey(idx));
          Tagged<Map> key_map = expected_key->map();
          // Fast iterable keys are guaranteed to be 1-byte.
          const uint8_t* expected_chars =
              GetFastKeyChars(isolate_, expected_key, key_map, no_gc);
          key_length = expected_key->length();
          key_match = FastKeyMatch(expected_chars, key_length);
        }
        if (V8_LIKELY(key_match)) {
          JsonString key =
              JsonString(position(), key_length, false, true, false);
          ++idx;
          cursor_ += key_length + 1 /* double quote */;
          if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;
        } else {
          JsonString key = ScanJsonPropertyKey(cont);
          if (!key.is_index()) {
            // Feedback doesn't match. Finish processing the current property
            // and continue in slow-path if we have more properties.
            if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;
            if (Check(JsonToken::COMMA)) {
              return ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
                  cont, first_token_msg, {});
            }
            return true;
          }
          if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;
        }
      } else {
        DCHECK_EQ(fast_iterable_state, FastIterableState::kUnknown);
        JsonString key = ScanJsonPropertyKey(cont);
        // Indices don't participate in fast iterable key checks.
        if (key.is_index()) {
          if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;
          continue;
        }
        // Before accessing the descriptor array, make sure that it wasn't
        // shrunk during a potential GC after the previous range check.
        if (V8_UNLIKELY(idx.as_int() >= descriptors->number_of_descriptors())) {
          break;
        }
        // Check if the key is fast iterable.
        // Some of the checks below are not relevant for the parser, but are
        // requirements for fast iterable keys in general (e.g. for
        // JSON.stringify).
        Tagged<Name> property_name = descriptors->GetKey(idx);
        bool is_slow = key.has_escape();
        // Check that the property is enumerable and located in field.
        PropertyDetails details = descriptors->GetDetails(idx);
        if (V8_UNLIKELY(details.IsDontEnum() ||
                        details.location() != PropertyLocation::kField)) {
          is_slow = true;
        }
        // Symbol property keys are slow.
        if (V8_UNLIKELY(IsSymbol(property_name))) {
          is_slow = true;
        }

        key_match = false;
        if (V8_LIKELY(!is_slow)) {
          DisallowGarbageCollection no_gc;
          // Property key is known to be fast so far, so it is guaranteed to
          // be a string.
          Tagged<String> expected_key = String::cast(property_name);
          Tagged<Map> key_map = expected_key->map();
          if (!expected_key->IsOneByteRepresentation()) {
            // Two-byte keys are slow.
            is_slow = true;
          } else {
            const uint8_t* expected_chars =
                GetFastKeyChars(isolate_, expected_key, key_map, no_gc);
            const uint32_t key_length = expected_key->length();
            key_match = FastKeyMatch(expected_chars, key_length, key);
          }
        }

        if (V8_UNLIKELY(is_slow)) {
          // The key is not fast iterable. Mark it as slow in the descriptor
          // array.
          descriptors->set_fast_iterable(FastIterableState::kJsonSlow);
        }

        // Finish parsing the property.
        if (V8_UNLIKELY(!ParseJsonPropertyValue(key))) return false;

        // If key is not fast iterable or doesn't match the feedback, we
        // continue on the slow-path.
        if (V8_UNLIKELY(is_slow || !key_match)) {
          if (Check(JsonToken::COMMA)) {
            return ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
                cont, first_token_msg, {});
          }
          // No more properties to scan, we are done.
          return true;
        }
        ++idx;
      }
    } while (idx < InternalIndex(descriptors->number_of_descriptors()) &&
             Check(JsonToken::COMMA));
    if constexpr (fast_iterable_state == FastIterableState::kUnknown) {
      if (idx == InternalIndex(descriptors->number_of_descriptors())) {
        descriptors->set_fast_iterable_if(FastIterableState::kJsonFast,
                                          FastIterableState::kUnknown);
      }
    }
    // Additional, unknown properties. Scan them slow.
    if (Check(JsonToken::COMMA)) {
      return ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
          cont, first_token_msg, descriptors);
    }
  }

  return true;
}

template <typename Char>
MaybeHandle<Object> JsonParser<Char>::ParseJsonObject(Handle<Map> feedback) {
  {
    StackLimitCheck check(isolate_);
    if (V8_UNLIKELY(check.HasOverflowed())) {
      return ParseJsonValue<false>();
    }
  }

  Consume(JsonToken::LBRACE);
  if (Check(JsonToken::RBRACE)) {
    return factory()->NewJSObject(object_constructor_);
  }

  JsonContinuation cont(isolate_, JsonContinuation::kObjectProperty,
                        property_stack_.size());
  bool success;
  using FastIterableState = DescriptorArray::FastIterableState;
  const MessageTemplate first_token_msg =
      MessageTemplate::kJsonParseExpectedPropNameOrRBrace;
  if (!feedback.is_null()) {
    Handle<DescriptorArray> descriptors =
        handle(feedback->instance_descriptors(), isolate_);
    if (*descriptors == ReadOnlyRoots(isolate_).empty_descriptor_array()) {
      success = ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
          &cont, first_token_msg, {});
    } else {
      switch (descriptors->fast_iterable()) {
        case FastIterableState::kJsonFast:
          success = ParseJsonObjectProperties<FastIterableState::kJsonFast>(
              &cont, first_token_msg, descriptors);
          break;
        case FastIterableState::kJsonSlow:
          success = ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
              &cont, first_token_msg, {});
          break;
        case FastIterableState::kUnknown:
          success = ParseJsonObjectProperties<FastIterableState::kUnknown>(
              &cont, first_token_msg, descriptors);
          break;
      }
    }
  } else {
    success = ParseJsonObjectProperties<FastIterableState::kJsonSlow>(
        &cont, first_token_msg, {});
  }
  if (V8_UNLIKELY(!success)) {
    return {};
  }

  JSON_EXPECT_OR_RETURN(JsonToken::RBRACE,
                        MessageTemplate::kJsonParseExpectedCommaOrRBrace, {});
  Handle<Object> result = BuildJsonObject(cont, feedback);
  property_stack_.resize_no_init(cont.index);
  return cont.scope.CloseAndEscape(result);
}

#undef JSON_EXPECT_OR_RETURN
#undef JSON_EXPECT_NEXT_OR_RETURN

template <typename Char>
MaybeHandle<Object> JsonParser<Char>::ParseJsonArray() {
  {
    StackLimitCheck check(isolate_);
    if (V8_UNLIKELY(check.HasOverflowed())) {
      return ParseJsonValue<false>();
    }
  }

  Consume(JsonToken::LBRACK);
  if (Check(JsonToken::RBRACK)) {
    return factory()->NewJSArray(0, PACKED_SMI_ELEMENTS);
  }

  HandleScope handle_scope(isolate_);
  size_t start = element_stack_.size();
  Handle<Object> value;
  if (V8_UNLIKELY(!ParseJsonValueRecursive().ToHandle(&value))) return {};
  element_stack_.emplace_back(value);
  while (Check(JsonToken::COMMA)) {
    Handle<Map> feedback;
    if (IsJSObject(*value)) {
      Tagged<Map> maybe_feedback = JSObject::cast(*value)->map();
      // Don't consume feedback from objects with a map that's detached
      // from the transition tree.
      if (!maybe_feedback->IsDetached(isolate_)) {
        feedback = handle(maybe_feedback, isolate_);
      }
    }
    if (V8_UNLIKELY(!ParseJsonValueRecursive(feedback).ToHandle(&value))) {
      return {};
    }
    element_stack_.emplace_back(value);
  }

  Expect(JsonToken::RBRACK, MessageTemplate::kJsonParseExpectedCommaOrRBrack);
  Handle<Object> result = BuildJsonArray(start);
  element_stack_.resize_no_init(start);
  return handle_scope.CloseAndEscape(result);
}

// Parse any JSON value.
template <typename Char>
template <bool should_track_json_source>
MaybeHandle<Object> JsonParser<Char>::ParseJsonValue() {
  std::vector<JsonContinuation> cont_stack;

  cont_stack.reserve(16);

  JsonContinuation cont(isolate_, JsonContinuation::kReturn, 0);

  Handle<Object> value;

  // When should_track_json_source is true, we use val_node to record current
  // JSON value's parse node.
  //
  // For primitive values, the val_node is the source string of the JSON value.
  //
  // For JSObject values, the val_node is an ObjectHashTable in which the key is
  // the property name and the first value is the property value's parse
  // node. The order in which properties are defined may be different from the
  // order in which properties are enumerated when calling
  // InternalizeJSONProperty for the JSObject value. E.g., the JSON source
  // string is '{"a": 1, "1": 2}', and the properties enumerate order is ["1",
  // "a"]. Moreover, properties may be defined repeatedly in the JSON string.
  // E.g., the JSON string is '{"a": 1, "a": 1}', and the properties enumerate
  // order is ["a"]. So we cannot use the FixedArray to record the properties's
  // parse node by the order in which properties are defined and we use a
  // ObjectHashTable here to record the property name and the property's parse
  // node. We then look up the property's parse node by the property name when
  // calling InternalizeJSONProperty. The second value associated with the key
  // is the property value's snapshot.
  //
  // For JSArray values, the val_node is a FixedArray containing the parse nodes
  // and snapshots of the elements.
  //
  // For information about snapshotting, see below.
  Handle<Object> val_node;
  // Record the start position and end position for the primitive values.
  int start_position;
  int end_position;

  // Workaround for -Wunused-but-set-variable on old gcc versions (version < 8).
  USE(start_position);
  USE(end_position);

  // element_val_node_stack is used to track all the elements's
  // parse nodes. And we use this to construct the JSArray's
  // parse node and value snapshot.
  SmallVector<Handle<Object>> element_val_node_stack;
  // property_val_node_stack is used to track all the property
  // value's parse nodes. And we use this to construct the
  // JSObject's parse node and value snapshot.
  SmallVector<Handle<Object>> property_val_node_stack;
  while (true) {
    // Produce a json value.
    //
    // Iterate until a value is produced. Starting but not immediately finishing
    // objects and arrays will cause the loop to continue until a first member
    // is completed.
    while (true) {
      SkipWhitespace();
      // The switch is immediately followed by 'break' so we can use 'break' to
      // break out of the loop, and 'continue' to continue the loop.

      if constexpr (should_track_json_source) {
        start_position = position();
      }
      switch (peek()) {
        case JsonToken::STRING:
          Consume(JsonToken::STRING);
          value = MakeString(ScanJsonString(false));
          if constexpr (should_track_json_source) {
            end_position = position();
            val_node = isolate_->factory()->NewSubString(
                source_, start_position, end_position);
          }
          break;

        case JsonToken::NUMBER:
          value = ParseJsonNumber();
          if constexpr (should_track_json_source) {
            end_position = position();
            val_node = isolate_->factory()->NewSubString(
                source_, start_position, end_position);
          }
          break;

        case JsonToken::LBRACE: {
          Consume(JsonToken::LBRACE);
          if (Check(JsonToken::RBRACE)) {
            // TODO(verwaest): Directly use the map instead.
            value = factory()->NewJSObject(object_constructor_);
            if constexpr (should_track_json_source) {
              val_node = ObjectTwoHashTable::New(isolate_, 0);
            }
            break;
          }

          // Start parsing an object with properties.
          cont_stack.emplace_back(std::move(cont));
          cont = JsonContinuation(isolate_, JsonContinuation::kObjectProperty,
                                  property_stack_.size());

          // Parse the property key.
          ExpectNext(JsonToken::STRING,
                     MessageTemplate::kJsonParseExpectedPropNameOrRBrace);
          property_stack_.emplace_back(ScanJsonPropertyKey(&cont));
          if constexpr (should_track_json_source) {
            property_val_node_stack.emplace_back(Handle<Object>());
          }

          ExpectNext(JsonToken::COLON,
                     MessageTemplate::kJsonParseExpectedColonAfterPropertyName);

          // Continue to start producing the first property value.
          continue;
        }

        case JsonToken::LBRACK:
          Consume(JsonToken::LBRACK);
          if (Check(JsonToken::RBRACK)) {
            value = factory()->NewJSArray(0, PACKED_SMI_ELEMENTS);
            if constexpr (should_track_json_source) {
              val_node = factory()->NewFixedArray(0);
            }
            break;
          }

          // Start parsing an array with elements.
          cont_stack.emplace_back(std::move(cont));
          cont = JsonContinuation(isolate_, JsonContinuation::kArrayElement,
                                  element_stack_.size());

          // Continue to start producing the first array element.
          continue;

        case JsonToken::TRUE_LITERAL:
          ScanLiteral("true");
          value = factory()->true_value();
          if constexpr (should_track_json_source) {
            val_node = isolate_->factory()->true_string();
          }
          break;

        case JsonToken::FALSE_LITERAL:
          ScanLiteral("false");
          value = factory()->false_value();
          if constexpr (should_track_json_source) {
            val_node = isolate_->factory()->false_string();
          }
          break;

        case JsonToken::NULL_LITERAL:
          ScanLiteral("null");
          value = factory()->null_value();
          if constexpr (should_track_json_source) {
            val_node = isolate_->factory()->null_string();
          }
          break;

        case JsonToken::COLON:
        case JsonToken::COMMA:
        case JsonToken::ILLEGAL:
        case JsonToken::RBRACE:
        case JsonToken::RBRACK:
        case JsonToken::EOS:
          ReportUnexpectedCharacter(CurrentCharacter());
          // Pop the continuation stack to correctly tear down handle scopes.
          while (!cont_stack.empty()) {
            cont = std::move(cont_stack.back());
            cont_stack.pop_back();
          }
          return MaybeHandle<Object>();

        case JsonToken::WHITESPACE:
          UNREACHABLE();
      }
      // Done producing a value, consume it.
      break;
    }

    // Consume a produced json value.
    //
    // Iterate as long as values are produced (arrays or object literals are
    // finished).
    while (true) {
      // The switch is immediately followed by 'break' so we can use 'break' to
      // break out of the loop, and 'continue' to continue the loop.
      switch (cont.type()) {
        case JsonContinuation::kReturn:
          if constexpr (should_track_json_source) {
            DCHECK(!val_node.is_null());
            Tagged<Object> raw_value = *value;
            parsed_val_node_ = cont.scope.CloseAndEscape(val_node);
            return cont.scope.CloseAndEscape(handle(raw_value, isolate_));
          } else {
            return cont.scope.CloseAndEscape(value);
          }

        case JsonContinuation::kObjectProperty: {
          // Store the previous property value into its property info.
          property_stack_.back().value = value;
          if constexpr (should_track_json_source) {
            property_val_node_stack.back() = val_node;
          }

          if (V8_LIKELY(Check(JsonToken::COMMA))) {
            // Parse the property key.
            ExpectNext(
                JsonToken::STRING,
                MessageTemplate::kJsonParseExpectedDoubleQuotedPropertyName);

            property_stack_.emplace_back(ScanJsonPropertyKey(&cont));
            if constexpr (should_track_json_source) {
              property_val_node_stack.emplace_back(Handle<Object>());
            }
            ExpectNext(
                JsonToken::COLON,
                MessageTemplate::kJsonParseExpectedColonAfterPropertyName);

            // Break to start producing the subsequent property value.
            break;
          }

          Handle<Map> feedback;
          if (cont_stack.size() > 0 &&
              cont_stack.back().type() == JsonContinuation::kArrayElement &&
              cont_stack.back().index < element_stack_.size() &&
              IsJSObject(*element_stack_.back())) {
            Tagged<Map> maybe_feedback =
                JSObject::cast(*element_stack_.back())->map();
            // Don't consume feedback from objects with a map that's detached
            // from the transition tree.
            if (!maybe_feedback->IsDetached(isolate_)) {
              feedback = handle(maybe_feedback, isolate_);
            }
          }
          value = BuildJsonObject(cont, feedback);
          Expect(JsonToken::RBRACE,
                 MessageTemplate::kJsonParseExpectedCommaOrRBrace);
          // Return the object.
          if constexpr (should_track_json_source) {
            size_t start = cont.index;
            int num_properties =
                static_cast<int>(property_stack_.size() - start);
            Handle<ObjectTwoHashTable> table =
                ObjectTwoHashTable::New(isolate(), num_properties);
            for (int i = 0; i < num_properties; i++) {
              const JsonProperty& property = property_stack_[start + i];
              Handle<Object> property_val_node =
                  property_val_node_stack[start + i];
              Handle<Object> property_snapshot = property.value;
              Handle<String> key;
              if (property.string.is_index()) {
                key = factory()->Uint32ToString(property.string.index());
              } else {
                key = MakeString(property.string);
              }
              table = ObjectTwoHashTable::Put(
                  isolate(), table, key,
                  {property_val_node, property_snapshot});
            }
            property_val_node_stack.resize_no_init(cont.index);
            DisallowGarbageCollection no_gc;
            Tagged<ObjectTwoHashTable> raw_table = *table;
            value = cont.scope.CloseAndEscape(value);
            val_node = cont.scope.CloseAndEscape(handle(raw_table, isolate_));
          } else {
            value = cont.scope.CloseAndEscape(value);
          }
          property_stack_.resize_no_init(cont.index);

          // Pop the continuation.
          cont = std::move(cont_stack.back());
          cont_stack.pop_back();
          // Consume to produced object.
          continue;
        }

        case JsonContinuation::kArrayElement: {
          // Store the previous element on the stack.
          element_stack_.emplace_back(value);
          if constexpr (should_track_json_source) {
            element_val_node_stack.emplace_back(val_node);
          }
          // Break to start producing the subsequent element value.
          if (V8_LIKELY(Check(JsonToken::COMMA))) break;

          value = BuildJsonArray(cont.index);
          Expect(JsonToken::RBRACK,
                 MessageTemplate::kJsonParseExpectedCommaOrRBrack);
          // Return the array.
          if constexpr (should_track_json_source) {
            size_t start = cont.index;
            int num_elements = static_cast<int>(element_stack_.size() - start);
            Handle<FixedArray> val_node_and_snapshot_array =
                factory()->NewFixedArray(num_elements * 2);
            DisallowGarbageCollection no_gc;
            Tagged<FixedArray> raw_val_node_and_snapshot_array =
                *val_node_and_snapshot_array;
            for (int i = 0; i < num_elements; i++) {
              raw_val_node_and_snapshot_array->set(
                  i * 2, *element_val_node_stack[start + i]);
              raw_val_node_and_snapshot_array->set(i * 2 + 1,
                                                   *element_stack_[start + i]);
            }
            element_val_node_stack.resize_no_init(cont.index);
            value = cont.scope.CloseAndEscape(value);
            val_node = cont.scope.CloseAndEscape(
                handle(raw_val_node_and_snapshot_array, isolate_));
          } else {
            value = cont.scope.CloseAndEscape(value);
          }
          element_stack_.resize_no_init(cont.index);
          // Pop the continuation.
          cont = std::move(cont_stack.back());
          cont_stack.pop_back();
          // Consume the produced array.
          continue;
        }
      }

      // Done consuming a value. Produce next value.
      break;
    }
  }
}

template <typename Char>
void JsonParser<Char>::AdvanceToNonDecimal() {
  cursor_ =
      std::find_if(cursor_, end_, [](Char c) { return !IsDecimalDigit(c); });
}

template <typename Char>
Handle<Object> JsonParser<Char>::ParseJsonNumber() {
  double number;
  int sign = 1;

  {
    const Char* start = cursor_;
    DisallowGarbageCollection no_gc;

    base::uc32 c = *cursor_;
    if (c == '-') {
      sign = -1;
      c = NextCharacter();
    }

    if (c == '0') {
      // Prefix zero is only allowed if it's the only digit before
      // a decimal point or exponent.
      c = NextCharacter();
      if (base::IsInRange(c, 0,
                          static_cast<int32_t>(unibrow::Latin1::kMaxChar)) &&
          IsNumberPart(character_json_scan_flags[c])) {
        if (V8_UNLIKELY(IsDecimalDigit(c))) {
          AllowGarbageCollection allow_before_exception;
          ReportUnexpectedToken(JsonToken::NUMBER);
          return handle(Smi::FromInt(0), isolate_);
        }
      } else if (sign > 0) {
        return handle(Smi::FromInt(0), isolate_);
      }
    } else {
      const Char* smi_start = cursor_;
      static_assert(Smi::IsValid(-999999999));
      static_assert(Smi::IsValid(999999999));
      const int kMaxSmiLength = 9;
      int32_t i = 0;
      const Char* stop = cursor_ + kMaxSmiLength;
      if (stop > end_) stop = end_;
      while (cursor_ < stop && IsDecimalDigit(*cursor_)) {
        i = (i * 10) + ((*cursor_) - '0');
        cursor_++;
      }
      if (V8_UNLIKELY(smi_start == cursor_)) {
        AllowGarbageCollection allow_before_exception;
        ReportUnexpectedToken(
            JsonToken::ILLEGAL,
            MessageTemplate::kJsonParseNoNumberAfterMinusSign);
        return handle(Smi::FromInt(0), isolate_);
      }
      c = CurrentCharacter();
      if (!base::IsInRange(c, 0,
                           static_cast<int32_t>(unibrow::Latin1::kMaxChar)) ||
          !IsNumberPart(character_json_scan_flags[c])) {
        // Smi.
        // TODO(verwaest): Cache?
        return handle(Smi::FromInt(i * sign), isolate_);
      }
      AdvanceToNonDecimal();
    }

    if (CurrentCharacter() == '.') {
      c = NextCharacter();
      if (!IsDecimalDigit(c)) {
        AllowGarbageCollection allow_before_exception;
        ReportUnexpectedToken(
            JsonToken::ILLEGAL,
            MessageTemplate::kJsonParseUnterminatedFractionalNumber);
        return handle(Smi::FromInt(0), isolate_);
      }
      AdvanceToNonDecimal();
    }

    if (AsciiAlphaToLower(CurrentCharacter()) == 'e') {
      c = NextCharacter();
      if (c == '-' || c == '+') c = NextCharacter();
      if (!IsDecimalDigit(c)) {
        AllowGarbageCollection allow_before_exception;
        ReportUnexpectedToken(
            JsonToken::ILLEGAL,
            MessageTemplate::kJsonParseExponentPartMissingNumber);
        return handle(Smi::FromInt(0), isolate_);
      }
      AdvanceToNonDecimal();
    }

    base::Vector<const Char> chars(start, cursor_ - start);
    number =
        StringToDouble(chars,
                       NO_CONVERSION_FLAGS,  // Hex, octal or trailing junk.
                       std::numeric_limits<double>::quiet_NaN());

    DCHECK(!std::isnan(number));
  }

  return factory()->NewNumber(number);
}

namespace {

template <typename Char>
bool Matches(base::Vector<const Char> chars, Handle<String> string) {
  DCHECK(!string.is_null());
  return string->IsEqualTo(chars);
}

}  // namespace

template <typename Char>
template <typename SinkSeqString>
Handle<String> JsonParser<Char>::DecodeString(
    const JsonString& string, Handle<SinkSeqString> intermediate,
    Handle<String> hint) {
  using SinkChar = typename SinkSeqString::Char;
  {
    DisallowGarbageCollection no_gc;
    SinkChar* dest = intermediate->GetChars(no_gc);
    if (!string.has_escape()) {
      DCHECK(!string.internalize());
      CopyChars(dest, chars_ + string.start(), string.length());
      return intermediate;
    }
    DecodeString(dest, string.start(), string.length());

    if (!string.internalize()) return intermediate;

    base::Vector<const SinkChar> data(dest, string.length());
    if (!hint.is_null() && Matches(data, hint)) return hint;
  }

  return factory()->InternalizeString(intermediate, 0, string.length());
}

template <typename Char>
Handle<String> JsonParser<Char>::MakeString(const JsonString& string,
                                            Handle<String> hint) {
  if (string.length() == 0) return factory()->empty_string();

  if (string.internalize() && !string.has_escape()) {
    if (!hint.is_null()) {
      base::Vector<const Char> data(chars_ + string.start(), string.length());
      if (Matches(data, hint)) return hint;
    }
    if (chars_may_relocate_) {
      return factory()->InternalizeString(Handle<SeqString>::cast(source_),
                                          string.start(), string.length(),
                                          string.needs_conversion());
    }
    base::Vector<const Char> chars(chars_ + string.start(), string.length());
    return factory()->InternalizeString(chars, string.needs_conversion());
  }

  if (sizeof(Char) == 1 ? V8_LIKELY(!string.needs_conversion())
                        : string.needs_conversion()) {
    Handle<SeqOneByteString> intermediate =
        factory()->NewRawOneByteString(string.length()).ToHandleChecked();
    return DecodeString(string, intermediate, hint);
  }

  Handle<SeqTwoByteString> intermediate =
      factory()->NewRawTwoByteString(string.length()).ToHandleChecked();
  return DecodeString(string, intermediate, hint);
}

template <typename Char>
template <typename SinkChar>
void JsonParser<Char>::DecodeString(SinkChar* sink, int start, int length) {
  SinkChar* sink_start = sink;
  const Char* cursor = chars_ + start;
  while (true) {
    const Char* end = cursor + length - (sink - sink_start);
    cursor = std::find_if(cursor, end, [&sink](Char c) {
      if (c == '\\') return true;
      *sink++ = c;
      return false;
    });

    if (cursor == end) return;

    cursor++;

    switch (GetEscapeKind(character_json_scan_flags[*cursor])) {
      case EscapeKind::kSelf:
        *sink++ = *cursor;
        break;

      case EscapeKind::kBackspace:
        *sink++ = '\x08';
        break;

      case EscapeKind::kTab:
        *sink++ = '\x09';
        break;

      case EscapeKind::kNewLine:
        *sink++ = '\x0A';
        break;

      case EscapeKind::kFormFeed:
        *sink++ = '\x0C';
        break;

      case EscapeKind::kCarriageReturn:
        *sink++ = '\x0D';
        break;

      case EscapeKind::kUnicode: {
        base::uc32 value = 0;
        for (int i = 0; i < 4; i++) {
          value = value * 16 + base::HexValue(*++cursor);
        }
        if (value <=
            static_cast<base::uc32>(unibrow::Utf16::kMaxNonSurrogateCharCode)) {
          *sink++ = value;
        } else {
          *sink++ = unibrow::Utf16::LeadSurrogate(value);
          *sink++ = unibrow::Utf16::TrailSurrogate(value);
        }
        break;
      }

      case EscapeKind::kIllegal:
        UNREACHABLE();
    }
    cursor++;
  }
}

template <typename Char>
JsonString JsonParser<Char>::ScanJsonString(bool needs_internalization) {
  DisallowGarbageCollection no_gc;
  int start = position();
  int offset = start;
  bool has_escape = false;
  base::uc32 bits = 0;

  while (true) {
    cursor_ = ScanJsonStringCharacters(cursor_, end_, &bits);

    if (V8_UNLIKELY(is_at_end())) {
      AllowGarbageCollection allow_before_exception;
      ReportUnexpectedToken(JsonToken::ILLEGAL,
                            MessageTemplate::kJsonParseUnterminatedString);
      break;
    }

    if (*cursor_ == '"') {
      int end = position();
      advance();
      int length = end - offset;
      bool convert = sizeof(Char) == 1 ? bits > unibrow::Latin1::kMaxChar
                                       : bits <= unibrow::Latin1::kMaxChar;
      constexpr int kMaxInternalizedStringValueLength = 10;
      bool internalize =
          needs_internalization ||
          (sizeof(Char) == 1 && length < kMaxInternalizedStringValueLength);
      return JsonString(start, length, convert, internalize, has_escape);
    }

    if (*cursor_ == '\\') {
      has_escape = true;
      base::uc32 c = NextCharacter();
      if (V8_UNLIKELY(!base::IsInRange(
              c, 0, static_cast<int32_t>(unibrow::Latin1::kMaxChar)))) {
        AllowGarbageCollection allow_before_exception;
        ReportUnexpectedCharacter(c);
        break;
      }

      switch (GetEscapeKind(character_json_scan_flags[c])) {
        case EscapeKind::kSelf:
        case EscapeKind::kBackspace:
        case EscapeKind::kTab:
        case EscapeKind::kNewLine:
        case EscapeKind::kFormFeed:
        case EscapeKind::kCarriageReturn:
          offset += 1;
          break;

        case EscapeKind::kUnicode: {
          base::uc32 value = ScanUnicodeCharacter();
          if (value == kInvalidUnicodeCharacter) {
            AllowGarbageCollection allow_before_exception;
            ReportUnexpectedToken(JsonToken::ILLEGAL,
                                  MessageTemplate::kJsonParseBadUnicodeEscape);
            return JsonString();
          }
          bits |= value;
          // \uXXXX results in either 1 or 2 Utf16 characters, depending on
          // whether the decoded value requires a surrogate pair.
          offset += 5 - (value > static_cast<base::uc32>(
                                     unibrow::Utf16::kMaxNonSurrogateCharCode));
          break;
        }

        case EscapeKind::kIllegal:
          AllowGarbageCollection allow_before_exception;
          ReportUnexpectedToken(JsonToken::ILLEGAL,
                                MessageTemplate::kJsonParseBadEscapedCharacter);
          return JsonString();
      }

      advance();
      continue;
    }

    DCHECK_LT(*cursor_, 0x20);
    AllowGarbageCollection allow_before_exception;
    ReportUnexpectedToken(JsonToken::ILLEGAL,
                          MessageTemplate::kJsonParseBadControlCharacter);
    break;
  }

  return JsonString();
}

// Explicit instantiation.
template class JsonParser<uint8_t>;
template class JsonParser<uint16_t>;

}  // namespace internal
}  // namespace v8
