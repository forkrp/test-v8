# Resource async performance comparisons

The resource counterparts are `MSGPACK.encodeResourceAsync(value)` and
`MSGPACK.decodeResourceAsync(bytes)`. Their functional contract is described in
[msgpack-async.md](msgpack-async.md). Performance comparisons must use the same
linked engine for both synchronous and asynchronous public APIs.
The completed Android comparison is in
[msgpack-resource-async-results.md](msgpack-resource-async-results.md).

## Measurement procedure

`tools/binary_serialization/run_resource_async.py` runs the native
`msgpack_js_benchmark --async` harness on a physical Android device. The harness
invokes public JavaScript APIs and pumps foreground tasks and microtasks until
the benchmark Promise finishes. It supplies native UTF-8 conversion helpers
and a high-resolution steady clock; Node.js is not required.

The default matrix contains:

- Android ARM64 and ARM32, each with its matching frozen executable and C++
  shared runtime.
- Eighteen bulk inputs plus one tiny object, including record counts from 250
  to 25,000, integer/decimal arrays, changing object shapes, the protocol fixture,
  repeated text, and unique ASCII, Latin-1, mixed Unicode, CJK, Greek and emoji.
- Encoding and decoding; one job and four jobs per batch. Synchronous four-job
  batches call the API four times sequentially. Asynchronous batches submit four
  jobs, then await all four results.
- Synchronous and asynchronous resource MessagePack, standard MessagePack, and
  JSON with owned UTF-8 byte transport. JSON encode includes conversion of the
  resulting string to bytes; JSON decode includes conversion of input bytes to
  a string. Both JSON conversions run on the calling thread.
- Five shuffled fresh-process passes, ten warmup batches and 31 timed samples
  per process. Each tiny sample averages 1,000 batches. Full GC runs immediately
  before timed samples; GC that occurs during a sample remains in its timing.

This gives 4,560 independent processes and 141,360 timed batch samples. Input
preparation, process startup and file I/O are excluded from API timing. Each
codec encodes the same logical value and decodes its corresponding wire format
into fully materialized values; the raw wire sizes are reported separately.
Every process checks complete value restoration before timing; MessagePack encodes
also check exact synchronous wire parity. Completed-operation and result-length
counters are audited. Resource size and compact/standard fallback selection
must match across codecs, passes and ABIs.

`submissionMs` measures the calling-thread API calls. For encoding this includes
the synchronous graph capture; for decoding it includes copying the selected
input bytes. It excludes subsequent foreground result materialization.
`totalMs` measures the whole batch through completed results and their
consumption. For four jobs, dividing total time by four describes amortized
throughput cost; it does not measure each job's response latency or all
foreground work. Reduced submission time alone does not establish improved
end-to-end latency.

The summary takes each process's sample median, then the median across five
independent processes. It retains all five process medians and their range;
ranges are not confidence intervals. Bulk geometric means give each non-tiny
workload equal weight. Tiny results are reported separately. Ratios are
baseline total time divided by resource async total time, so values above one
favor resource async. Every ratio below one is retained, together with paired
pass ratios and the number of passes slower by at least five percent. These
are descriptive comparisons, not significance tests.

## Reproduction

Build and freeze both Android ABIs from the intended engine source state. A
frozen directory must include `msgpack_js_benchmark`, `build-manifest.json`,
`compiler-identity.json`, `commands.txt`, and the ABI-matched
`libc++_shared.so`. `freeze_async.py` checks that the linked targets are current
and records compiler commands and the compile dependency closure. The runtime
is copied separately from the matching Android NDK sysroot.

```sh
python3 tools/binary_serialization/freeze_async.py \
  --build out/json-review-arm64 --output out/resource-async-frozen-arm64
python3 tools/binary_serialization/freeze_async.py \
  --build out/json-review-arm32 --output out/resource-async-frozen-arm32
# Copy each ABI's libc++_shared.so into its frozen directory before measuring.

python3 tools/binary_serialization/run_resource_async.py \
  --arm64 out/resource-async-frozen-arm64 \
  --arm32 out/resource-async-frozen-arm32 \
  --serial 885841c1 --passes 5 --samples 31 --seed 20261009 \
  --output out/msgpack-resource-async-performance/full

python3 tools/binary_serialization/summarize_resource_async.py \
  --matrix out/msgpack-resource-async-performance/full/results.json \
  --output out/msgpack-resource-async-performance/full-summary.json
```

Use a new output directory for a new run. `--workloads`, `--codecs`, `--jobs`,
`--passes` and `--samples` can select a diagnostic subset; do not combine those
samples with a completed full matrix. The runner uses CPU affinity `f0` and
records the governor, thermal sensor, frequency and available memory without
changing device policy. Measurement and native snapshot generation share
`/tmp/v8-msgpack-async-device.lock`. Detected overlapping engine jobs are
excluded, retained in `excluded-overlaps.json`, and retried in a fresh process.

If an invocation fails, the runner saves the command and error in
`failure-INDEX.json`. Repeat the exact command with `--resume` to retain its
successful prefix and original randomized order. Resume verifies configuration,
source, binary, runtime and device asset hashes, and saves a hashed checkpoint.
The summary verifies complete Cartesian coverage, sample/counter invariants,
unchanged checkpoint prefixes, input hashes, wire consistency and frozen build
provenance, including every recorded compile input. Report any continuation as
a segmented collection.

Keep raw samples, inputs, frozen binaries and runtime/build identities in the
external evidence archive. Source Git should contain the reusable benchmark
and report rather than multi-megabyte inputs or binaries.

## Paired optimization measurements

`tools/binary_serialization/compare_resource_async.py` compares two frozen full
GN builds in adjacent before/after fresh processes. It shuffles both version
order and ABI/workload/operation/codec/job blocks for each independent pass.
All six codecs remain as controls, including current synchronous resource
performance. It validates completed values, exact same-version wire, payload
sizes, fallback selection and operation counters, and retains failed or
overlapping samples separately. Resume requires unchanged identities, inputs,
runner dependencies and device assets.

```sh
python3 tools/binary_serialization/compare_resource_async.py \
  --before-arm64 /path/to/baseline-arm64 --before-arm32 /path/to/baseline-arm32 \
  --after-arm64 /path/to/candidate-arm64 --after-arm32 /path/to/candidate-arm32 \
  --output out/resource-async-paired --passes 3 --samples 13
python3 tools/binary_serialization/summarize_resource_comparison.py \
  out/resource-async-paired/results.json --output out/resource-async-paired-summary
```

The summary audits the complete Cartesian matrix, frozen binaries/runtimes and
recorded compile input closures. Preserve the baseline checkout and candidate
worktree until that audit completes. The baseline/optimized ratio measures the
change in each API; current sync/async measures the remaining cost relative to
the optimized synchronous implementation. Geometric means exclude tiny input
and give each bulk workload equal weight. Keep all process medians and losses.
Diagnostic relinks, instrumented stage timings and short pilot matrices must
remain separate from acceptance data.
