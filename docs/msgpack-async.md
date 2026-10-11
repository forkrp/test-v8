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
An owned root numeric span reuses the same block packer directly, avoiding a
second numeric DOM. Native strings borrow immutable owned bytes during analysis;
selected output storage transfers to the result backing store without a copy.
Bounded repeated long strings reuse worker-owned encoded spans after exact
code-unit comparison; cache offsets remain valid across output relocation.

`decodeResourceAsync(input)` accepts compact V8MR v1 and ordinary MessagePack.
On the worker, it validates the resource envelope, all table entries (including
unused entries), references and numeric blocks. It retains compact references
and numeric block spans in an owned native tape instead of expanding ordinary
wire bytes. The cooperative foreground builder restores shared dictionary
strings and record keys once per operation, and fills packed numeric arrays
directly from validated blocks. Bounded ASCII records can discard their scalar
tape entries after validation and reuse the direct object builder. Unknown versions and malformed
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
The resource methods execute owned captures of at most 256 bytes inline to
avoid worker dispatch and foreground task hops. Their returned Promise may
already be settled; `.then` and `await` continuations still run through normal
microtasks. The same platform and pending-job admission checks run first.
The byte threshold bounds native capture size, rather than a mutable graph
estimate; large operations continue to use workers and cooperative slices.

Compact inputs of at most 64 KiB can also use the direct decoder after a bounded
structural cost probe. The probe caps tables at 64 shapes, 512 keys and 256
strings, nesting at 32, and estimated work at 32,768 units (object and array
allocations carry extra weight). It skips UTF-8 bodies and numeric conversion;
the direct decoder performs full validation and construction once. This
reduces dispatch and duplicate parsing for small resources. The returned
Promise can be settled before return, with normal microtask continuations.
Such calls spend more of their small completion cost on the caller; the work
estimate and byte cap are bounds rather than a hard time guarantee.

A flat packed-number root with empty shape/string tables can use the same
direct route up to 256 KiB, 131,072 elements and 1,024 blocks. The probe reads
block headers and skips numeric bytes; full validation remains in the decoder.
Packed 64-bit integers keep the worker path. These bounded arrays avoid task
hops when the remaining worker validation is cheaper than dispatch itself.
The owned inline snapshot is charged as external memory until completion.

Decode materialization is cooperative: large containers use rooted frames and
foreground slices, with a 2 ms time budget checked at bounded work intervals. Resource decode uses
this time budget without forcing another task solely at 65,536 work units.
Standard decode retains the additional work-loop bound. Small subtrees reuse the tuned synchronous builders
with the prevalidated native tape. Consumed fixed tape blocks are released
between slices and their external-memory accounting is reduced. Numeric arrays are filled in allocation-free
128-element batches. Every unfinished array has initialized slots and a zero
length until completion. Decoder key/map handles remain valid throughout each
slice. No V8 heap address is retained in the worker data.

When resource output falls back to standard MessagePack, bounded ASCII maps
also use a validated descriptor rather than a full scalar tape. The scan limits
each subtree to 256 logical values and depth 16; other subtrees use the normal
tape. Container descriptors retain logical work independently of physical tape
size, preserving cooperative frame decisions after child maps collapse.

Compact root arrays can overlap native validation with foreground construction.
The stream admits 512–20,000 records in inputs up to 1 MiB, with at most 64
shapes, 512 keys, 256 dictionary entries, ASCII tables and extension payloads
of at most 512 bytes per record.
The worker publishes validated batches through release/acquire counters. The
foreground reads only immutable input and those published spans; its scratch
and errors are separate from the worker's mutable tape and Unicode buffers.
Results remain rooted, hole initialized and logically length zero until every
record and the complete input have passed validation. Late errors and
cancellation discard the partial result. The two-millisecond foreground budget
continues to apply, including bounded batches of array stores.

For an otherwise idle MessagePack job queue, inputs of 65–256 KiB containing
1,024–4,096 such records can prepare the native table/layout description and a
small validated prefix on the calling thread. Preparation admits at most 512
records and 32 KiB of record wire, checking the 16,384 logical work-unit
budget between complete records; table characters are capped at 64 KiB. It builds no V8 result objects. This allows a foreground
task to start immediately while a real worker validates the remaining records.
Concurrent batches prepare only their first admitted job, avoiding repeated
submission costs. Total latency and the increased calling-thread submission
time must both be reported. Other inputs retain the existing worker or bounded
inline routes. Scratch capacities are allocated before publication and stay
charged as external memory without concurrent reads of worker vectors.

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
methods may be pending per isolate. Compact decode accounts owned input,
tape capacities, decoded code units, numeric block metadata and native tables
before building JS objects. Logical standard-wire expansion remains bounded
at 256 MiB without allocating it; packed numeric arrays conservatively charge
up to nine logical wire bytes per value. Consequently a compact
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
ownership policy. Resource decode can retain one input buffer and one decoded-character buffer
per isolate, each capped at 8 MiB. Retained capacity stays charged to V8 external
memory; borrowing transfers that charge to the pending job. Cancellation and
isolate disposal release the cache. Workers use exclusively owned snapshots,
and realm handles stay on the foreground thread.

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
- `tools/binary_serialization/run_resource_async.py`: matched resource/standard
  MessagePack and JSON byte comparisons on both Android ABIs. It retains value,
  exact wire, completed-result and fallback-selection checks; see
  [the resource async measurement procedure](msgpack-resource-async-benchmarks.md).
  The [Android resource async results](msgpack-resource-async-results.md) retain
  both throughput gains and slower completion/submission cases.
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
