# Asynchronous MessagePack APIs

This fork adds Promise-returning counterparts to the synchronous MessagePack
methods. They use V8's platform worker pool and non-nestable foreground tasks,
following the custom asynchronous JSON APIs.

```js
async function example(value, receivedBytes) {
  const bytes = await MSGPACK.encodeAsync(value); // independently owned Uint8Array
  const decoded = await MSGPACK.decodeAsync(receivedBytes);
  const resourceBytes = await MSGPACK.encodeResourceAsync(value);
  const resourceValue = await MSGPACK.decodeResourceAsync(resourceBytes);
  return {bytes, decoded, resourceBytes, resourceValue};
}
```

All four methods have one formal argument, are non-enumerable, and are not
constructors. The supported value and wire profile is the same as
[the synchronous API](msgpack-js-api.md): data objects, dense arrays, strict
UTF-8 strings, lossless Numbers, and signed/unsigned 64-bit integers. Safe
integer wire values decode to Number; larger values decode to BigInt. Binary
and general extension tags remain outside this profile. The resource methods
add the opt-in [V8MR v1 resource format](msgpack-resource-format.md).

`encodeResourceAsync(value)` returns exactly the same bytes as synchronous
`encodeResource(value)`: it selects the compact candidate only when the entire
file is strictly smaller, and otherwise returns the exact standard encoding.
The JS graph capture uses the existing async encoder; deferred standard byte
encoding, native tree analysis, dictionary/shape selection and numeric packing
run on the worker. The compact analysis shares the synchronous implementation.

`decodeResourceAsync(input)` accepts compact V8MR v1 and ordinary MessagePack.
On the worker, it validates the resource envelope, all table entries (including
unused entries), references and numeric blocks, then expands the resource into
owned ordinary wire bytes. The existing worker tape parser and cooperative
foreground builder then reconstruct the value. Unknown versions and malformed
V8MR-prefixed input reject; they are never retried as ordinary MessagePack.
These methods have the same input snapshots, realm ownership, cancellation and
Promise rejection behavior as `encodeAsync` and `decodeAsync` below.

## Snapshot and ownership semantics

`encodeAsync(value)` captures the admitted data graph on the calling isolate
thread before returning. The worker receives owned native bytes, numeric spans,
and copied string code units. Packed numeric arrays and long numeric prefixes
in tagged arrays use native spans. Cheap scalar numbers are emitted during
capture, avoiding a separate native-part entry for each object field. The
worker encodes the bulk numeric spans and deferred UTF-8 strings, then the
foreground thread transfers the independently owned result buffer to its
backing store. When capture contains no deferred parts, it already contains
complete wire bytes and that owned buffer transfers directly. Encoded key
caches stay on the isolate thread.
Getters, proxy traps and `toJSON` are not invoked, matching synchronous MSGPACK.

`decodeAsync(input)` accepts an ArrayBuffer or any ArrayBufferView, preserving
its exact byte offset and length. It validates storage and copies the selected
bytes before returning. A worker validates the complete wire value, UTF-8,
container/depth limits, converts safe numeric values into a native tape, and
transcodes long non-ASCII value strings into owned code units.
It never reads the caller's backing store or materializes V8 objects.

Consequently, later mutations, detachment, or resizing do not change either
operation's captured input:

```js
const source = {items: [1, 2, 3]};
const encoding = MSGPACK.encodeAsync(source);
source.items[0] = 99;
const bytes = await encoding;             // encodes [1, 2, 3]

const decoding = MSGPACK.decodeAsync(bytes);
bytes.fill(0xc1);
const value = await decoding;              // decodes the snapshot [1, 2, 3]
```

Decoded graphs, result arrays, output buffers and the Promise belong to the
method's realm. Native context and Promise roots live in foreground-owned
persistent handles. Job registration keeps `Isolate::HasPendingBackgroundTasks`
true until settlement or cancellation. Isolate teardown cancels jobs and
releases roots before waiting for cancelable tasks; workers retain only owned
native data and an opaque completion task.

## Foreground work and responsiveness

Capture is synchronous for encoding. This is required to read JS values safely;
its cost still grows with graph size. Decoding performs a synchronous byte copy.
Worker dispatch and Promise scheduling add overhead to tiny values.

Decode materialization is cooperative: large containers use rooted frames and
foreground slices, with a 2 ms time budget checked at bounded work intervals
and a 65,536-unit work-loop bound. Completing the current small subtree or
numeric batch can exceed that loop bound by a bounded chunk. Small subtrees reuse the tuned synchronous builders
with the prevalidated native tape. Consumed fixed tape blocks are released
between slices and their external-memory accounting is reduced. Numeric arrays are filled in allocation-free
128-element batches. Every unfinished array has initialized slots and a zero
length until completion. Decoder key/map handles remain valid throughout each
slice. No V8 heap address is retained in the worker data.

The time budget is cooperative rather than a hard deadline. A single allocation,
GC, or large string materialization can exceed it. The performance runner
records the submission portion and end-to-end batch latency separately so
worker gains are not confused with capture/materialization costs.

## Rejections and resource requirements

Unsupported values/storage reject with TypeError; malformed bytes or invalid
UTF-8 reject with SyntaxError; resource limits and caught native allocation
failures reject with RangeError. Execution termination follows V8's normal
termination behavior rather than settling the Promise. If the platform has no
worker threads or does not support non-nestable foreground tasks, the method
returns a rejected Promise, as the async JSON APIs do.

The existing 256 MiB wire limit and 256-container nesting limit remain. Async
jobs also require their captured native input, decoded code units and part/tape capacities to fit a
256 MiB native budget, and at most 32 jobs across all four async MessagePack
methods may be pending per isolate. Compact decode expansion accounts its
input, expanded output and native tables together before building JS objects;
expanded wire bytes also have the 256 MiB byte limit. Consequently a compact
file may fit the input limit but reject because its expansion exceeds these
limits. Resource encoding retains the synchronous codec's additional temporary
native tree, analysis tables and candidate buffers; those transient allocations
are not part of the captured-input/tape budget or an aggregate RSS bound. Its
existing 256 MiB resource metadata and candidate byte limits still apply. These
resource limits can reject a value that the synchronous API has enough memory
to process. Finished output has its separate wire-byte limit; the limits do not
constitute an aggregate process-memory guarantee. Native storage is accounted
as external memory on the foreground thread, including the completed worker
result before materialization/transfer.

Android ARM32 uses independently owned page mappings for sufficiently large
capture, input and output byte buffers, retaining the synchronous output
ownership policy. No mutable buffer pool or cross-thread realm cache is added.

## Validation and performance evidence

The implementation goal, current validation state, and performance acceptance
are tracked in [msgpack-async-goal.md](msgpack-async-goal.md). Runtime and
performance acceptance must be established using source-matched linked builds;
source checks alone do not establish either.

Repeatable tests and measurement entry points:

- `test/msgpack/async.js`: exact synchronous wire/value parity, numeric/Unicode
  boundaries, storage views, errors, snapshots, schema mutation, realms,
  retained results, moving GC, 2,000 deterministic malformed inputs, oversized
  32-bit array/map count regressions across the slice threshold, and job bound.
- `test/cctest/test-json-async.cc`, `MessagePackAsync*`: controlled worker
  execution, foreground yielding, GC, realm switching, platform support,
  termination, disposal with queued/dropped tasks, and native accounting.
- `test/msgpack/async-numeric.js`: numeric tag widths, unsafe 64-bit integers,
  tape-block boundaries, truncated numeric arrays, native tape-budget rejection
  before materialization, and successful decoding after rejection.
- `test/msgpack/async-resource.js`: exact compact/fallback wire parity, numeric
  block boundaries, Unicode tables, snapshots, storage views, malformed resource
  differential tests, expansion limits, realm ownership and the shared job bound.
- `test/mjsunit/msgpack-async-resource.js`: interleaved standard and resource
  synchronous/asynchronous APIs, long Unicode keys and shared caches.
- `test/mjsunit/msgpack.js`: existing synchronous regression suite.
- `tools/binary_serialization/run_async.py`: the 18-workload corpus plus tiny
  calls, public sync/async MessagePack and JSON, one/four jobs, shuffled fresh
  processes, warmups, timed GC, and retained submit/total samples. The native harness includes JSON UTF-8 byte transport and retains JSON
  string controls separately. Tiny calls use aggregate inner batches to avoid
  reporting ratios below clock resolution.
- `tools/binary_serialization/freeze_async.py`: current-link check, source and
  generated-header dependency closure, exact compiler commands and identity,
  GN arguments, and executable SHA-256.

`tools/binary_serialization/summarize_async.py` checks matrix completion,
process/sample counts, completed-result counters, consistent wire sizes and
ordering, then summarizes independent process medians
and their ranges. The ranges expose variability; they are not confidence
intervals. It includes every workload-specific regression and keeps tiny-call
results separate from the equally weighted corpus aggregate.

`tools/binary_serialization/resume_async.py` can continue an interrupted Android
matrix after verifying source, binary, runtime, script, input, and on-device
identities. It retains the successful row prefix and original order, records
the failure evidence and segment conditions, and preserves a hashed checkpoint.
The summary verifies that earlier samples were not replaced and includes the
interruption metadata. A resumed matrix is reported as a segmented run.

Synchronous before/after controls also cover all 19 workloads, including tiny
calls. Their existing three-call calibration retains an exact integer result
length alongside the native harness's rounded `payloadBytes` field, so the
audit can check identical byte lengths and shared iteration counts. The native
timing loop is unchanged.
