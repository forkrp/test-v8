# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Apply diagnostic-only async phase probes to a source-matched experiment."""
import argparse
import difflib
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('checkout', type=Path)
parser.add_argument('--patch', type=Path, required=True)
args = parser.parse_args()
changes = []


def replace(text, old, new):
    assert text.count(old) == 1, (old[:100], text.count(old))
    return text.replace(old, new, 1)


path = args.checkout / 'src/json/json-parser-background.h'
before = path.read_text()
profile = '''struct JsonAsyncPhaseProfile {
  int64_t entry_start = 0, allocated_us = 0, copied_us = 0;
  int64_t queued = 0, returned = 0, entry_gc_us = 0, gc_start_us = 0;
  int64_t worker_start = 0, worker_end = 0, completion_post = 0;
  int64_t foreground_time = 0, foreground_wait = 0, foreground_last_end = 0;
  int64_t build_us = 0, discard_us = 0, account_us = 0, entry_account_us = 0, foreground_gc_us = 0;
  size_t input_bytes = 0, tape_accounted_bytes = 0;
  uint32_t source_units = 0, nodes = 0, parts = 0;
  std::vector<int64_t> slices;
};

'''
after = replace(before, 'struct BackgroundJsonData {\n',
                profile + 'struct BackgroundJsonData {\n  JsonAsyncPhaseProfile phase_profile;\n')
path.write_text(after)
changes.append(('src/json/json-parser-background.h', before, after))

path = args.checkout / 'src/json/json-parser-async.cc'
before = path.read_text()
text = replace(before, '#include <array>\n', '#include <array>\n#include <cstdio>\n')
gc_scope = '''
// d8 does not reset LongTaskStats; these counters include incremental GC work
// that public prologue/epilogue callbacks do not cover.
int64_t JsonPhaseGCTotal(Isolate* isolate) {
  auto* stats = isolate->GetCurrentLongTaskStats();
  return stats->gc_full_atomic_wall_clock_duration_us +
         stats->gc_full_incremental_wall_clock_duration_us +
         stats->gc_young_wall_clock_duration_us;
}
class JsonPhaseGC final {
 public:
  explicit JsonPhaseGC(Isolate* isolate)
      : isolate_(isolate), before_(JsonPhaseGCTotal(isolate)) {}
  int64_t elapsed() const { return JsonPhaseGCTotal(isolate_) - before_; }
 private:
  Isolate* isolate_;
  int64_t before_;
};
'''
text = replace(text, 'DEFINE_LAZY_LEAKY_OBJECT_GETTER(JsonParseJobs, GetJsonParseJobs)\n',
               'DEFINE_LAZY_LEAKY_OBJECT_GETTER(JsonParseJobs, GetJsonParseJobs)\n' + gc_scope)
text = replace(text, '  void AccountNativeMemory() {\n',
               '  JsonAsyncPhaseProfile& PhaseProfile() { return data_->phase_profile; }\n\n'
               '  void AccountNativeMemory() {\n'
               '    const auto phase_begin = base::TimeTicks::Now().ToInternalValue();\n')
text = replace(text, '          ->AdjustAmountOfExternalAllocatedMemory(delta);\n    }\n  }',
               '          ->AdjustAmountOfExternalAllocatedMemory(delta);\n    }\n'
               '    PhaseProfile().account_us += base::TimeTicks::Now().ToInternalValue() - phase_begin;\n  }')
text = replace(text, '    bool done =\n        data_->is_one_byte ? BuildSlice<uint8_t>() : BuildSlice<uint16_t>();',
               '    const auto phase_build = base::TimeTicks::Now().ToInternalValue();\n'
               '    bool done =\n        data_->is_one_byte ? BuildSlice<uint8_t>() : BuildSlice<uint16_t>();\n'
               '    PhaseProfile().build_us += base::TimeTicks::Now().ToInternalValue() - phase_build;')
text = replace(text, '    data_->DiscardBefore(cursor_);',
               '    const auto phase_discard = base::TimeTicks::Now().ToInternalValue();\n'
               '    data_->DiscardBefore(cursor_);\n'
               '    PhaseProfile().discard_us += base::TimeTicks::Now().ToInternalValue() - phase_discard;')
old = '''    if (state_->raw()->Run()) {
      state_->raw()->ReleaseRoots();
      GlobalHandles::Destroy(state_.location());
'''
new = '''    auto& profile = state_->raw()->PhaseProfile();
    const int64_t begin = base::TimeTicks::Now().ToInternalValue();
    profile.foreground_wait += begin -
        (profile.foreground_last_end ? profile.foreground_last_end : profile.completion_post);
    bool done;
    {
      JsonPhaseGC gc(isolate_);
      done = state_->raw()->Run();
      profile.foreground_gc_us += gc.elapsed();
    }
    const int64_t end = base::TimeTicks::Now().ToInternalValue();
    profile.foreground_time += end - begin;
    profile.foreground_last_end = end;
    profile.slices.push_back(end - begin);
    if (done) {
      auto snapshot = std::move(profile);
      const int64_t release_begin = base::TimeTicks::Now().ToInternalValue();
      state_->raw()->ReleaseRoots();
      GlobalHandles::Destroy(state_.location());
      const int64_t finished = base::TimeTicks::Now().ToInternalValue();
      snapshot.slices.back() = finished - begin;
      const int64_t gc_end = JsonPhaseGCTotal(isolate_);
      std::fprintf(stderr,
          "JSON_PHASE {\\"units\\":%u,\\"nodes\\":%u,\\"parts\\":%u,"
          "\\"input_bytes\\":%zu,\\"tape_accounted_bytes\\":%zu,"
          "\\"entry_us\\":%lld,\\"allocate_us\\":%lld,\\"copy_us\\":%lld,"
          "\\"prepare_to_queue_us\\":%lld,\\"entry_gc_us\\":%lld,"
          "\\"worker_queue_us\\":%lld,\\"worker_us\\":%lld,\\"worker_free_us\\":%lld,"
          "\\"foreground_us\\":%lld,\\"foreground_wait_us\\":%lld,"
          "\\"build_us\\":%lld,\\"discard_us\\":%lld,\\"account_us\\":%lld,"
          "\\"foreground_gc_us\\":%lld,\\"gc_start_us\\":%lld,\\"gc_end_us\\":%lld,"
          "\\"gc_window_us\\":%lld,\\"release_us\\":%lld,\\"total_us\\":%lld,"
          "\\"slices_us\\":[",
          snapshot.source_units, snapshot.nodes, snapshot.parts,
          snapshot.input_bytes, snapshot.tape_accounted_bytes,
          static_cast<long long>(snapshot.returned - snapshot.entry_start),
          static_cast<long long>(snapshot.allocated_us),
          static_cast<long long>(snapshot.copied_us),
          static_cast<long long>(snapshot.queued - snapshot.entry_start),
          static_cast<long long>(snapshot.entry_gc_us),
          static_cast<long long>(snapshot.worker_start - snapshot.queued),
          static_cast<long long>(snapshot.worker_end - snapshot.worker_start),
          static_cast<long long>(snapshot.completion_post - snapshot.worker_end),
          static_cast<long long>(snapshot.foreground_time),
          static_cast<long long>(snapshot.foreground_wait),
          static_cast<long long>(snapshot.build_us),
          static_cast<long long>(snapshot.discard_us),
          static_cast<long long>(snapshot.account_us),
          static_cast<long long>(snapshot.foreground_gc_us),
          static_cast<long long>(snapshot.gc_start_us),
          static_cast<long long>(gc_end),
          static_cast<long long>(gc_end - snapshot.gc_start_us),
          static_cast<long long>(finished - release_begin),
          static_cast<long long>(finished - snapshot.entry_start));
      for (size_t i = 0; i < snapshot.slices.size(); ++i)
        std::fprintf(stderr, "%s%lld", i ? "," : "", static_cast<long long>(snapshot.slices[i]));
      std::fprintf(stderr, "]}\\n");
'''
text = replace(text, old, new)
text = replace(text, '    ParseJsonInBackground(data_.get(), V8::GetCurrentPlatform());',
               '    data_->phase_profile.worker_start = base::TimeTicks::Now().ToInternalValue();\n'
               '    ParseJsonInBackground(data_.get(), V8::GetCurrentPlatform());\n'
               '    data_->phase_profile.worker_end = base::TimeTicks::Now().ToInternalValue();\n'
               '    data_->phase_profile.nodes = static_cast<uint32_t>(data_->NodeCount());\n'
               '    data_->phase_profile.parts = static_cast<uint32_t>(data_->remaining_parts.size());\n'
               '    data_->phase_profile.tape_accounted_bytes = data_->MemoryUsage() - data_->phase_profile.input_bytes;')
text = replace(text, '    runner_->PostNonNestableTask(std::move(completion_));',
               '    data_->phase_profile.completion_post = base::TimeTicks::Now().ToInternalValue();\n'
               '    runner_->PostNonNestableTask(std::move(completion_));')
text = replace(text, '#include \"include/v8-isolate.h\"\n', '#include \"include/v8-isolate.h\"\n#include \"include/v8-metrics.h\"\n')
text = replace(text, '  Handle<JSPromise> promise = isolate->factory()->NewJSPromise();',
               '  const int64_t phase_entry_start = base::TimeTicks::Now().ToInternalValue();\n'
               '  const int64_t phase_gc_start = JsonPhaseGCTotal(isolate);\n'
               '  JsonPhaseGC phase_entry_gc(isolate);\n'
               '  Handle<JSPromise> promise = isolate->factory()->NewJSPromise();')
text = replace(text, '  int length = string->length();',
               '  int length = string->length();\n'
               '  data->phase_profile.entry_start = phase_entry_start;\n'
               '  data->phase_profile.gc_start_us = phase_gc_start;\n'
               '  data->phase_profile.source_units = length;\n'
               '  data->phase_profile.input_bytes = size_t{static_cast<size_t>(length)} * (data->is_one_byte ? 1 : 2);')
for name in ('one_byte', 'two_byte'):
    pattern = r'    data->' + name + r' = [^;]+;\n    DisallowGarbageCollection no_gc;\n    String::WriteToFlat\(\*string, data->' + name + r'\.begin\(\), 0, length\);'
    matches = list(re.finditer(pattern, text))
    assert len(matches) == 1
    old = matches[0].group()
    allocate, copy = old.split('\n', 1)
    new = ('    const auto phase_alloc = base::TimeTicks::Now().ToInternalValue();\n' + allocate + '\n'
           '    const auto phase_copy = base::TimeTicks::Now().ToInternalValue();\n' + copy + '\n'
           '    data->phase_profile.allocated_us = phase_copy - phase_alloc;\n'
           '    data->phase_profile.copied_us = base::TimeTicks::Now().ToInternalValue() - phase_copy;')
    text = replace(text, old, new)
text = replace(text, '  platform->CallOnWorkerThread(std::make_unique<JsonParseBackgroundTask>(',
               '  auto* phase_profile = &data->phase_profile;\n'
               '  phase_profile->queued = base::TimeTicks::Now().ToInternalValue();\n'
               '  platform->CallOnWorkerThread(std::make_unique<JsonParseBackgroundTask>(')
text = replace(text, '      std::make_unique<JsonParseAsyncTask>(isolate, root)));\n  return promise;',
               '      std::make_unique<JsonParseAsyncTask>(isolate, root)));\n'
               '  phase_profile->returned = base::TimeTicks::Now().ToInternalValue();\n'
               '  phase_profile->entry_gc_us = phase_entry_gc.elapsed();\n  return promise;')
# Initial input accounting happens at API entry; keep it distinct from the
# accounting inside foreground tasks rather than attributing it twice.
text = replace(text, '  root->raw()->AccountNativeMemory();',
               '  root->raw()->AccountNativeMemory();\n'
               '  root->raw()->PhaseProfile().entry_account_us = root->raw()->PhaseProfile().account_us;')
text = replace(text, r'\"account_us\":%lld,',
               r'\"account_us\":%lld,\"entry_account_us\":%lld,\"foreground_account_us\":%lld,')
text = replace(text, '          static_cast<long long>(snapshot.account_us),',
               '          static_cast<long long>(snapshot.account_us),\n'
               '          static_cast<long long>(snapshot.entry_account_us),\n'
               '          static_cast<long long>(snapshot.account_us - snapshot.entry_account_us),')

# Capture isolate scope entry/exit, Run(), final root release and re-enqueue.
# Logging and profile-vector bookkeeping remain outside the measured interval.
method_begin = text.index('  void RunInternal() override {',
                          text.index('class JsonParseAsyncTask final'))
method_end = text.index('\n private:', method_begin)
method = text[method_begin:method_end]
printing = method[method.index('      std::fprintf(stderr,'):
                  method.index('    } else {')]
replacement = '''  void RunInternal() override {
    const int64_t begin = base::TimeTicks::Now().ToInternalValue();
    JsonAsyncPhaseProfile snapshot;
    JsonAsyncPhaseProfile* record;
    int64_t release_begin = 0;
    bool done;
    {
      v8::Isolate::Scope isolate_scope(reinterpret_cast<v8::Isolate*>(isolate_));
      auto& profile = state_->raw()->PhaseProfile();
      profile.foreground_wait += begin -
          (profile.foreground_last_end ? profile.foreground_last_end : profile.completion_post);
      const int64_t gc_before = JsonPhaseGCTotal(isolate_);
      done = state_->raw()->Run();
      if (done) {
        snapshot = std::move(profile);
        record = &snapshot;
        release_begin = base::TimeTicks::Now().ToInternalValue();
        state_->raw()->ReleaseRoots();
        GlobalHandles::Destroy(state_.location());
      } else {
        record = &profile;
        state_->raw()->runner()->PostNonNestableTask(
            std::make_unique<JsonParseAsyncTask>(isolate_, state_));
      }
      record->foreground_gc_us += JsonPhaseGCTotal(isolate_) - gc_before;
    }
    const int64_t finished = base::TimeTicks::Now().ToInternalValue();
    record->foreground_time += finished - begin;
    record->foreground_last_end = finished;
    record->slices.push_back(finished - begin);
    if (done) {
      const int64_t gc_end = JsonPhaseGCTotal(isolate_);
''' + printing + '''    }
  }
'''
text = text[:method_begin] + replacement + text[method_end:]

path.write_text(text)
changes.append(('src/json/json-parser-async.cc', before, text))
args.patch.write_text(''.join(''.join(difflib.unified_diff(a.splitlines(True), b.splitlines(True),
                          fromfile='a/' + name, tofile='b/' + name)) for name, a, b in changes))
print('Instrumented', args.checkout)
