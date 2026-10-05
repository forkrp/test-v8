# MessagePack JS integration and Android performance goal

Acceptance achieved on the recorded device with final candidate **stage45r2**,
2026-10-05. All 18 fixed workloads clear the 1.5x encode and decode median gates
on both ARM64 and ARM32. Extended controls also pass all whole-run and half-run
median gates. This claim applies to warmed binary transport on the tested
configuration. First-call, string-only JSON and size limits are reported below.

Baseline: `d9c5ab6a9e5abe6920a1ffd828ad8c685a87bbb7`, isolated branch
`codex/msgpack-js-android`. Device: rooted OnePlus 6, serial `885841c1`, CPU mask
`f0`. Android ARM64 and ARMv7/NEON runs were serialized. NDK
27.3.13750724 supplies the separately fingerprinted libc++ runtime for each ABI.
The primary checkout is preserved; no remote publication is part of this goal.

## Contract and measurement

The synchronous API is `MSGPACK.encode(value)` -> independently owned Uint8Array
and `MSGPACK.decode(ArrayBuffer | ArrayBufferView)` -> ordinary JS values.
The complete supported-value, error and ownership contract is in
[msgpack-js-api.md](msgpack-js-api.md).

Acceptance compares actual public MSGPACK builtins with current
`JSON.stringify` plus native UTF-8 encoding into an owned Uint8Array, and native
UTF-8 decoding plus `JSON.parse`. JSON with a JS string input/output is measured
separately; its speedups do not satisfy the all-workload binary transport gate.
No third-party JS codec is used. All 18 workloads remain in the set, including
all three record counts, short decimals, integers, alternating roles, varied
shapes, protocol data, and repeated/unique ASCII and Unicode text.

Each operation/codec has five shuffled fresh-process samples. The same loop
count is shared across codecs for an operation, calibrated for 200 ms with at
least five iterations. Each process validates the complete prepared value,
warms the loop 20 times, and performs full GC before timing. GC during timing is
included. Decode preparation retains only the required input, rather than the
original decoded graph. Startup, input preparation and I/O are excluded.
The ratio is median JSON-byte time divided by median MSGPACK time; both
operations must independently reach 1.5x on each architecture.

## Final full corpus

| Workload | ARM64 decode | ARM64 encode | ARM32 decode | ARM32 encode |
| --- | ---: | ---: | ---: | ---: |
| records_250 | 2.011x | 2.144x | 2.129x | 2.467x |
| records_2500 | 1.938x | 2.915x | 1.997x | 3.174x |
| records_25000 | 1.659x | 2.927x | 1.665x | 3.181x |
| short_decimals | 4.386x | 5.602x | 4.435x | 5.361x |
| integers | 1.942x | 3.843x | 1.961x | 4.633x |
| text_heavy | 1.898x | 2.854x | 2.018x | 5.050x |
| unicode | 12.062x | 1.642x | 10.448x | 1.901x |
| unicode_unique | 1.983x | 1.729x | 1.944x | 1.973x |
| ascii_unique | 1.704x | 2.952x | 1.952x | 4.412x |
| latin1_unique | 2.877x | 2.444x | 2.925x | 3.610x |
| alternating_roles | 1.603x | 2.860x | 1.585x | 3.022x |
| mixed_late | 2.125x | 4.814x | 1.868x | 5.615x |
| nested_numeric | 4.384x | 5.567x | 4.505x | 5.358x |
| varying_shapes | 1.511x | 1.688x | 1.564x | 1.889x |
| protocol_fixture | 1.567x | 1.514x | 1.628x | 2.263x |
| cjk_unique | 3.478x | 2.536x | 3.513x | 3.982x |
| greek_unique | 3.555x | 2.590x | 3.544x | 3.704x |
| emoji_unique | 2.361x | 2.343x | 2.311x | 2.765x |

All 72 encode/decode/ABI median gates pass. Raw samples, counts, execution
orders, JSON string controls and memory counters are in
`out/msgpack-js-android/js-matrix-{arm64,arm32}-stage45r2/results.json`.

## Extended repeatability controls

Every operation at or below 1.8x in the full matrix receives 60 additional
fresh-process samples per codec, using its original shared count. ARM32
protocol encoding also receives 60 samples at 5,000 iterations to check the
previous allocator cliff. Case order is shuffled and codec order is reversed
using the recorded round/position schedule. First-codec counts are recorded;
the schedule is randomized rather than exactly 30/30 for every case.

| ABI and operation | Whole run | First 30 rounds | Last 30 rounds |
| --- | ---: | ---: | ---: |
| arm64 records_25000 decode | 1.571x | 1.566x | 1.611x |
| arm64 unicode encode | 1.667x | 1.667x | 1.671x |
| arm64 unicode_unique encode | 1.711x | 1.708x | 1.715x |
| arm64 ascii_unique decode | 1.752x | 1.742x | 1.743x |
| arm64 alternating_roles decode | 1.616x | 1.611x | 1.618x |
| arm64 varying_shapes decode | 1.518x | 1.521x | 1.503x |
| arm64 varying_shapes encode | 1.743x | 1.753x | 1.741x |
| arm64 protocol_fixture decode | 1.564x | 1.563x | 1.564x |
| arm64 protocol_fixture encode | 1.520x | 1.517x | 1.524x |
| arm32 records_25000 decode | 1.678x | 1.685x | 1.684x |
| arm32 alternating_roles decode | 1.577x | 1.576x | 1.576x |
| arm32 varying_shapes decode | 1.576x | 1.580x | 1.570x |
| arm32 protocol_fixture decode | 1.636x | 1.633x | 1.636x |
| arm32 protocol_fixture encode (5,000 iterations) | 2.097x | 2.097x | 2.094x |

All whole-run medians and both 30-round medians pass. A descriptive paired-round
bootstrap (5,000 resamples, seed 20261005) puts the narrowest 95% interval,
ARM64 varying-shape decode, at 1.501x to 1.529x. Its overall ratio is 1.518x;
the margin remains small and does not guarantee 1.5x on other devices or builds.
The interval calculation and first-codec counts accompany the compact evidence.
The controls retain all 1,680 fresh-process measurements in
`js-controls-{arm64,arm32}-stage45r2/results.json` under the evidence directory.

## Implementation

The direct reader stages numbers in native storage, validates complete key and
field layouts, and uses checked ordinary young-space allocation to initialize
objects and their uniquely owned numeric boxes before publication. ARM64 also
uses guarded allocations for short ASCII value strings and small tagged
arrays. Unsupported layouts retain the factory path. Native references do not
cross GC; V8 objects remain rooted. No heap data moves to workers.

The encoder traverses admitted data-only layouts without per-property roots in
no-GC spans, with rooted rewind fallbacks for unsupported layouts or strings.
Realm caches hold weak maps, immutable encoded key metadata and scalar sizing
feedback. They retain neither input graphs nor encoded output. Every layout,
key, representation and field type is checked before using a hint. The encoded
key metadata cap is 8 MiB; size hints reserve at most 8 MiB. Major GC triggers
re-bucketing of the encoder's address-derived weak map table.

Bounded ASCII/UTF-8 and numeric reads avoid repeated tiny helper calls. Short
native byte copies are enabled only on ARM32 after ARM64 regressions. Android
release codec translation uses `-O3`; the JSON engine retains its default
configuration. No fast-math or lossy conversion is enabled. Public encoding
uses float32 only if widening reproduces the original Number exactly; the
private default continues to use float64 for noninteger Numbers.

Public Android ARM32 buffers reaching 32 KiB use independent native page
mappings. Backing-store final ownership frees the exact mapping extent. Tail
pages are released without copying; a failed trim retains a correctly freed
original mapping. Other targets and the default private API use malloc/free.
Legacy `Release()` callers always receive free-compatible memory.

## Source-matched validation and build identity

The final candidate passes:

- The public API suite on both production Android ABIs in normal, moving-GC
  (`--stress-compaction --gc-interval=37`) and incremental-marking modes.
- A dedicated host DCHECK/heap-verifier build in all three modes with actual
  `--verify-heap`, including malformed input, strict Unicode, numeric and tag
  boundaries, view offsets, detached/shared/out-of-bounds buffers, lifetime,
  retained-output growth, map mutation and realm ownership.
- Fresh native host and Android builds, normal and compaction stress. Each run
  covers 48 fixtures across four decoding backends, 18 malformed cases and
  2,000 deterministic differential byte inputs, plus 24 native buffer ownership
  cases spanning growth, trimming, mapping transfer and legacy free ownership.
- Independent C MessagePack decode/re-encode checks for all 18 public payloads
  from each ABI. Wire bytes and lengths match exactly across architectures.
- Four focused JSON C++ API cctests, invoked individually: default gap,
  explicit gap, getter/toJSON semantics and access checks.

Production GN compiles the codec and new builtins against the complete updated
builtin list. Exceptions are enabled only for the codec boundary and benchmark;
dependency exceptions are caught before returning to V8. Vendored msgpack-c
revision `36631b24d14cd5e135d240b0b59ba2a7fb7eeab7` uses the Boost Software
License 1.0; MPack revision `c9d1820ecfdbc3954c60a218492251e457cc1f86` uses MIT.
The license/notice files are included. All 735 msgpack files and 14 MPack source/
license files match their pinned upstream revision; local GN/README files are
identified separately.

Frozen GN manifests bind source, compiler dependencies, SDK/generated headers,
GN args, exact commands, and linked production/profiling binary SHA-256. The
final audit checks 4,265 host, 4,101 verifier, 4,121 ARM64 and 4,096 ARM32 inputs
against current files, with zero mismatches. Test, runner, fixture, executable
and ABI runtime hashes bind each report to its actual inputs. The native
reference adapter uses the original builtin-definition overlay to match the
reused core object's IsolateData layout; its timings are not JS acceptance.

The compact tracked report is
[the final evidence index](../tools/binary_serialization/evidence/msgpack-js-android-stage45r2.json).
Full reports, binaries, profiles, rejected sources and measurements remain in
`out/msgpack-js-android`. The index contains SHA-256 values for 19 final reports
and all completion requirements; frozen manifests include exact build closure.

## First call, size and memory

Three shuffled samples per operation/codec measure exactly the first call in a
fresh process and realm, with zero codec warmups. Decode bytes are prepared in
separate processes. Complete value validation follows timing. First calls do
not clear both 1.5x gates on every workload: 3/18 clear both on ARM64 and 10/18
on ARM32. Varying-shape first-call decode/encode ratios are 0.694x/0.496x on
ARM64 and 0.752x/0.566x on ARM32. Protocol first-call encode is 0.801x/0.958x.
These misses remain visible rather than being counted as warmed wins.

The wire is standard MessagePack with the documented lossless profile. Gzip
uses level 6, deterministic timestamp zero; the following sizes match both ABIs.

| Workload | MessagePack bytes | JSON bytes | MessagePack gzip | JSON gzip |
| --- | ---: | ---: | ---: | ---: |
| records_250 | 23,331 | 31,547 | 3,639 | 3,792 |
| records_2500 | 239,423 | 323,733 | 32,757 | 35,725 |
| records_25000 | 2,426,103 | 3,312,808 | 316,329 | 350,682 |
| short_decimals | 938,165 | 612,121 | 12,907 | 5,935 |
| integers | 359,237 | 697,781 | 254,180 | 255,750 |
| text_heavy | 1,138,509 | 1,155,281 | 20,830 | 21,176 |
| unicode | 1,313,509 | 1,340,281 | 20,808 | 20,818 |
| unicode_unique | 1,480,189 | 1,506,961 | 33,016 | 31,680 |
| ascii_unique | 2,170,479 | 2,187,251 | 27,605 | 27,359 |
| latin1_unique | 2,185,479 | 2,202,251 | 27,537 | 28,100 |
| alternating_roles | 296,491 | 468,291 | 53,656 | 54,176 |
| mixed_late | 938,171 | 612,133 | 12,919 | 5,958 |
| nested_numeric | 938,174 | 612,133 | 12,926 | 5,963 |
| varying_shapes | 327,288 | 465,697 | 62,961 | 68,114 |
| protocol_fixture | 49,315 | 56,495 | 11,684 | 10,684 |
| cjk_unique | 3,348,509 | 3,365,281 | 26,090 | 25,626 |
| greek_unique | 3,048,509 | 3,065,281 | 26,884 | 25,673 |
| emoji_unique | 1,648,509 | 1,665,281 | 19,548 | 18,573 |

Large records save 26.8% raw bytes; short decimal text is smaller than lossless
binary floats, so that fixture grows 53.3%. The protocol fixture saves 12.7%
raw but grows 9.4% after gzip. Standard MessagePack cannot promise a size win
for every number distribution or compression policy.

The fixed realm weak tables cost approximately 192 KiB before encoded-key
metadata. The larger transient decoder tables add approximately 27.5 KiB of
native stack space on ARM64 and 14 KiB on ARM32. Page-backed ARM32 output has
less than one page of tail space after successful trimming. V8's external-byte
counter reports the payload length, so it excludes allocator/page slack.

On the large-record first-call samples, retained heap after encode is about
7.51 MB for MessagePack versus 7.31 MB for JSON on both ABIs, including the input
graph and cache. Retained external bytes are exactly 2,426,103 versus 3,312,808.
Median process peak RSS after encode is 46,132/47,628 KiB on ARM64 and
41,772/43,320 KiB on ARM32 (MessagePack/JSON). Varied-shape encoding retains
about 0.635 MB more heap than JSON due to feedback metadata. Memory is a
tradeoff, not a universal reduction. Heap/external measurements are taken
before timing, after timing, and after GC retaining the final graph/output;
raw process peak RSS also includes setup and validation and is not a codec-only
peak. Every workload's counters remain in the compact index and cold raw files.

## Baseline correction and retained negative experiments

The original native `v8::JSON::Stringify` benchmark used an absent gap that this
fork converted to empty string. The optimized JSON path expects undefined, so
those old encoding ratios compared with a slow JSON path. The absent-gap C++
API now passes undefined, while explicit gaps preserve their semantics.
Historical native ratios are excluded from claims over current fast JSON.

Earlier full matrices cleared only 13/18 (stage16) and 16/18 (stage28) on each
ABI. These remain intermediate results; the complete final matrix supersedes
them. Negative/rejected experiments are retained under their original stage:

- Eager per-map rooting in stage2 worsened varying-shape encode 3.79 -> 17.08 ms.
  The scoped-root stage17 experiment also failed to justify startup overhead.
- Stage19's fused ByteArray/number allocation worsened relevant records/roles
  timing. It was removed before the separately validated ordinary allocation.
- Stage23 streaming object construction regressed ARM64 large-record decode
  34.77 -> 37.46 ms and roles 5.76 -> 6.18 ms; removed.
- Stage25 full descriptor hashing per object regressed varying-shape encode
  1.65 -> 1.98 ms ARM64 and 2.62 -> 3.07 ms ARM32; replaced by re-bucketing
  only after major GC.
- Stage29r2 malloc protocol encode reached 2.047x at 100 iterations but only
  0.864x at 5,000. Paired profiles show Scudo release scans in backing-store
  sweeping taking more than half sampled CPU. Stage31r2 independent mappings
  reached 1.867x in the seven-round sustained control; final 60-round control
  reaches 2.097x. No GC or buffer freeing is disabled.
- Stage34 short copies helped ARM32 but regressed several ARM64 encodes ~6%.
  Stage39 short ASCII allocation also regressed ARM32. Each optimization is
  retained only on the architecture where it was beneficial.
- Stage40 signature-plane/NEON lookup passed correctness but regressed varied
  shapes to 6.516 ms versus 5.776 on ARM64 and 7.912 versus 7.082 on ARM32;
  removed in favor of the weak Map/Smi layout.
- An initial builtin-ID adapter mismatch and stage31's private-OS API compile
  failure produced invalid builds. Tests accidentally using an older binary
  are explicitly excluded. The rejected single-generation flag failed startup
  because it is compiled readonlyfalse; it is not a successful validation.
  The final fast allocations guard that configuration and retain factory fallbacks.

Profiles include startup/warmup/GC and establish hypotheses; their instrumented
latencies are excluded from acceptance. Test suites exposed and fixed a
read-only empty-array write-barrier query and debug NaN boxing before the final
source was frozen.

## Reproduction

Build standard GN `d8` and `msgpack_js_benchmark` targets using the appropriate
host/Android configuration, then freeze only successfully linked current
outputs. The exact tested args and commands are saved under
`gn-frozen-{host,host-verify,arm64,arm32}-stage45r2`. This Mac-host Android setup
uses local cross-toolchain wrappers and executes mksnapshot on the device;
Android linking/snapshot generation and all device tests must run serially.

```sh
python3 tools/binary_serialization/freeze_gn.py \
  --build out/msgpack-js-android/gn-arm64 --output out/msgpack-js-android/recheck-frozen-arm64
python3 tools/binary_serialization/run_js.py \
  --binary out/msgpack-js-android/recheck-frozen-arm64/msgpack_js_benchmark \
  --output out/msgpack-js-android/recheck-matrix-arm64 \
  --adb-serial 885841c1 --remote-dir /data/local/tmp/msgpack-recheck-arm64 \
  --rounds 5 --target-ms 200 --min-iterations 5 --warmups 20
python3 tools/binary_serialization/controls_js.py \
  --binary out/msgpack-js-android/recheck-frozen-arm64/msgpack_js_benchmark \
  --matrix out/msgpack-js-android/recheck-matrix-arm64/results.json \
  --output out/msgpack-js-android/recheck-controls-arm64 \
  --adb-serial 885841c1 --remote-dir /data/local/tmp/msgpack-controls-recheck-arm64 \
  --runtime-library /path/to/arm64/libc++_shared.so --rounds 60
```

Run ARM32 afterward with its own frozen output/runtime and
`--sustained-protocol-encode`. For the first-call/size/memory matrix, run
`run_js.py` with `--cold --rounds 3` and a fresh output directory. The runner
prepares and fingerprints bytes independently. Build the C peer with
`build_c_peer.py` and check each prepared `.msgpack` file. Normal API validation
uses `d8 --allow-natives-syntax --expose-gc test/intl/assert.js
test/mjsunit/msgpack.js`; add the recorded moving/incremental flags for those
modes. Use the dedicated verifier build for `--verify-heap`.
