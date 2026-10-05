# Native binary serialization investigation

The encoder and decoder now include the 2026-10-05 native NEON pass described
under **SIMD string processing** below, following the earlier decoder pass.
The original nine-workload measurements are preserved under **Original Android
results (frozen baseline)**; they describe commit `fe97eae01`, before the
decoder optimizations. The earlier paired decoder evidence compares that exact
native executable with `e6f93a891` on the same device and core objects. The SIMD
comparison starts from that optimized implementation and includes a scalar
control for the new encoder and decoder code.

This experiment decodes caller-owned native bytes directly into ordinary V8
objects and arrays. It also encodes those graphs into owned native byte buffers.
There is no third-party JavaScript codec in either measured path. The target is
fully materialized data: application code can access the resulting values with
ordinary JavaScript property and array operations.

The recommended MessagePack implementation is `msgpack-c` **`cpp_master` using
the visitor API**, rather than unpacking a native tree before constructing V8
values. This is a workload-dependent choice. The device results do not justify
replacing all JSON with MessagePack, or calling MessagePack the fastest format
for every graph.

## Implementation and dependency selection

`src/msgpack/messagepack.cc` implements three decoder paths with the same V8
object builder:

| Decoder | Dependency/API | Intermediate representation |
| --- | --- | --- |
| `msgpack` / `msgpack32` | msgpack-c C++ template visitor | Rooted V8 handles and per-container staging; no native object tree |
| `msgpack_tree` | msgpack-c C++ `unpack` | Native zone-backed tree, followed by V8 construction |
| `mpack` | MPack C reader | Reader tags followed directly by V8 construction |

The visitor is header-only and compiled with `MSGPACK_NO_BOOST`. Its callbacks
allow the adapter to construct V8 values without a second traversal over a
native DOM. The C branch's public unpacking API returns `msgpack_object` plus
zone storage. Its internal macro template could be adapted to callbacks, but
that would add maintenance of a lower-level dependency interface. This
investigation does not claim to have timed a custom C-branch callback decoder.

The C branch is used independently to unpack and re-encode every generated
MessagePack payload. The checker requires byte-identical output for these
fixtures. C versus C++ changes the implementation, not the MessagePack wire
format or its inherent size.

Pinned dependency commits:

- msgpack-c `cpp_master`: `36631b24d14cd5e135d240b0b59ba2a7fb7eeab7` (C++ 9.0.0).
- msgpack-c `c_master`: `dbc7bfbbff2a54c0b41a78187f92667e0ca6413f` (C 7.0.3).
- MPack: `c9d1820ecfdbc3954c60a218492251e457cc1f86`.
- V8 reference source: `0028c72db97031eacb3852fab4e8f6096f56f02e`, version
  12.6.228.49 in the checked-out source.

The adapter shares V8's `JSDataObjectBuilder` with JSON. The existing builder
was moved from `src/json/json-parser.cc` into
`src/objects/js-data-object-builder.h`, preserving its implementation. It
supports fast property layouts, array element specialization, shape feedback,
key reuse, and short-value string sharing. All staged values and feedback maps
are indirect handles, so allocations and moving GC can update them safely.
The decoder keeps the input in stable native memory for the synchronous call.

The encoder reads fast property descriptors and packed array storage directly,
with fallback handling for indexed/dictionary properties. ASCII strings avoid
an intermediate UTF-8 buffer. The adapter translation unit enables C++
exceptions and catches dependency exceptions at its boundary; other V8
translation units retain their existing exception configuration. A production
integration must account for that build-system requirement.

## Decoder optimization pass

The optimized decoder validates UTF-8 and computes UTF-16 length and the
one-byte/two-byte representation in a combined preflight. It then allocates the
final V8 string and decodes complete code points directly into it. ASCII word
scanning and bulk copying of the ASCII prefix are safe on unaligned ARM32 and
ARM64 input. This removes the separate validation pass followed by V8's lossy
DFA preflight and conversion, while retaining strict malformed-input rejection.

Per-decode caches contain 256 key entries and 128 value entries, with a maximum
of 4,096 UTF-8 bytes per cached string. The sampled hash selects a slot; length
and every byte must match a previously validated input before a cached V8
string can be reused. Input references stay within caller-owned immutable bytes
for the synchronous call, and all V8 references remain rooted handles. Keys are
internalized; equal long primitive string values can share storage without
changing their JavaScript value semantics. Unique-string controls separate
transcoding gains from gains due to repeated values.

Key cache entries also retain array-index classification, eliminating repeated
classification when an object closes. Expected-map feedback is scoped by
nesting depth and the surrounding property role, with 64 bounded entries.
This distinguishes different object roles at the same depth. Arrays did not
write maps into the original feedback table; the optimization addresses sharing
between object roles, not array-map pollution.

An experimental unboxed numeric staging path was excluded after mixed-array
fallback and additional probing produced regressions. The final implementation
retains the established numeric-array construction path. The late-mixed and
nested-numeric controls remain in the corpus to guard that decision.

`compare_decode.py` compares the frozen and updated native executables using
identical payloads, V8 core object fingerprints, CPU affinity, and iteration
counts per codec. Shared counts avoid comparing different GC averaging windows.
Each ABI uses two independently shuffled five-round batches. The extended
corpus has 15 workloads: the original nine plus unique Unicode, unique ASCII,
unique Latin1 with an ASCII prefix, alternating nested roles, a late mixed
array, and a numeric array nested inside an object. The independent C checker
and all three native decoder backends validate every prepared corpus.

The current optimized correctness suite has 43 expression fixtures, 18
malformed-input cases, incomplete prefixes, and 2,000 deterministic random byte
inputs. It additionally covers all non-surrogate BMP code points, Latin1,
supplementary code-point samples and boundaries, and cache-fingerprint
collisions with both valid and invalid strings.

## SIMD string processing

`src/msgpack/messagepack-string.h` contains the native string kernels. They use
intrinsics shared by AArch64 and ARMv7 NEON; other targets retain scalar paths.
The adapter does not depend on a third-party JavaScript library or add a Unicode
library dependency. MessagePack token dispatch and V8 object construction remain
scalar. SIMD changes neither the MessagePack format nor the resulting graph.

For strings of at least 64 UTF-8 bytes, the decoder scans the ASCII prefix in
64-byte blocks, then validates general UTF-8 in 16-byte blocks. Vector masks
check byte classes, required continuation positions, overlong encodings,
surrogate encodings, and the maximum code point. State crosses block boundaries.
The same pass counts UTF-16 code units and selects V8's one-byte or two-byte
representation. A final incomplete code point is rewound and validated by the
scalar tail; it is never silently accepted. Conversion handles ASCII and
regular two/three-byte runs with SIMD, with scalar handling for mixed widths,
supplementary characters, and tails. Every load and store stays within its span.
Short ASCII strings keep a compact inline word scan rather than entering the
larger outlined vector validator.

The encoder retains its direct one-byte ASCII path. Other strings are validated
and sized before UTF-8 is written into the final native output buffer. This
removes `ToCString`'s intermediate buffer and its subsequent copy. SIMD sizing
and interleaved UTF-8 stores handle regular character-width runs. Two bounded
eight-unit samples select that strategy for strings of at least 64 code units;
Short strings and strings whose samples contain mixed widths or surrogates use
scalar conversion.
The samples only choose a strategy: both paths validate the entire string,
including every surrogate pair. Failed SIMD conversion attempts fall back for
their window rather than probing each overlapping mixed-width window.

Flat V8 string pointers are used only inside `DisallowGarbageCollection`, after
flattening the rooted string. The output buffer uses native allocations;
conversion performs no V8 allocations. All existing decode caches and
object-builder roots are retained. Unicode conversion is outlined to avoid
vector-register saves on the common ASCII encoder path. Scalar and SIMD writer
functions are compiled separately so the scalar fallback keeps the same code
generation as the scalar control.

`build.py --disable-msgpack-simd` builds an otherwise identical scalar control.
It disables explicit adapter NEON, while retaining compiler vectorization and
existing V8 copy helpers. The new scalar encoder also writes directly to the
output, so the control separates conversion/copy improvements from explicit
SIMD improvements. `compare_simd.py` compares that control, the SIMD variant,
and the preceding optimized implementation. It checks source and binary
fingerprints, shared V8 core objects, identical wire bytes, independent C wire
round trips, and shared iteration counts per operation and codec.

The expanded correctness suite has 48 graph fixtures across both float policies
and all three decoder backends. Normal and moving-GC runs pass on the host and
on both Android ABIs. `string_simd_test.cc` checks 106,400 string inputs against
the retained scalar kernels, including 20,000 random byte strings, all SIMD
boundary offsets, incomplete prefixes, malformed UTF-8, and 160 unpaired
surrogate placements. It passes with host AddressSanitizer/UndefinedBehaviorSanitizer
and on both Android ABIs. `evidence/simd-validation.json` records fingerprints
and disassembly examples confirming NEON in the validator and converters.

## SIMD Android results

The final native code is `eb331d2c2`, following the decoder-only source
`e6f93a891`. These results are medians of ten fresh-process samples per
variant, operation, and workload: two independently shuffled five-round
batches on physical OnePlus 6 serial `885841c1`, Android API 35, cores 4–7.
The V8 core and JSON parser object files are identical between variants.
All workers use 20 warmups, GC before timing, and shared iteration counts;
GC during timing is included. File I/O, decompression, and cold startup are
excluded. Decode creates complete ordinary JS graphs; encode returns an owned
native buffer. `SIMD` denotes the enabled native adapter; `scalar` denotes
the new direct writer with explicit adapter NEON disabled.

`evidence/simd-summary.json` contains all medians and fingerprints. Only the
four completed `measured-a` / `measured-b` reports contribute. The aggregator
verified all prepared payloads, independent C checks, and standard wire
identity before/after, against the scalar control, and across both ABIs.

### Android ARM64

| Workload | Decode before | Decode SIMD | Decode scalar | Encode before | Encode SIMD | Encode scalar |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.324 | 0.320 | 0.323 | 0.265 | 0.262 | 0.260 |
| records_2500 | 3.257 | 3.217 | 3.247 | 2.652 | 2.680 | 2.669 |
| records_25000 | 47.902 | 48.072 | 48.077 | 29.381 | 29.725 | 29.783 |
| short_decimals | 8.011 | 7.923 | 8.065 | 3.068 | 3.092 | 3.056 |
| integers | 3.811 | 3.928 | 3.955 | 3.903 | 3.944 | 3.901 |
| text_heavy | 1.922 | 1.837 | 1.940 | 2.542 | 2.443 | 2.510 |
| unicode | 1.748 | 1.750 | 1.764 | 13.183 | 8.064 | 7.995 |
| unicode_unique | 9.864 | 8.325 | 9.471 | 14.929 | 9.746 | 9.615 |
| ascii_unique | 3.427 | 3.207 | 3.430 | 5.316 | 5.204 | 5.292 |
| latin1_unique | 3.483 | 3.310 | 3.510 | 20.385 | 6.428 | 8.633 |
| alternating_roles | 6.765 | 6.763 | 6.729 | 4.199 | 4.298 | 4.242 |
| mixed_late | 7.467 | 7.473 | 7.506 | 4.590 | 4.601 | 4.671 |
| nested_numeric | 8.070 | 8.000 | 7.956 | 2.938 | 2.987 | 2.904 |
| varying_shapes | 12.000 | 11.998 | 12.008 | 3.751 | 3.785 | 3.754 |
| protocol_fixture | 0.382 | 0.378 | 0.386 | 0.282 | 0.286 | 0.280 |
| cjk_unique | 18.973 | 9.596 | 18.970 | 19.453 | 9.376 | 13.948 |
| greek_unique | 20.149 | 8.633 | 20.234 | 21.199 | 9.080 | 14.198 |
| emoji_unique | 8.968 | 7.132 | 8.884 | 13.576 | 6.974 | 6.859 |

All times are milliseconds per complete operation.

### Android ARM32 / ARMv7 NEON

| Workload | Decode before | Decode SIMD | Decode scalar | Encode before | Encode SIMD | Encode scalar |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.332 | 0.333 | 0.338 | 0.337 | 0.329 | 0.329 |
| records_2500 | 3.689 | 3.707 | 3.782 | 3.531 | 3.447 | 3.469 |
| records_25000 | 56.416 | 55.871 | 56.701 | 38.294 | 37.401 | 37.402 |
| short_decimals | 7.869 | 7.979 | 7.926 | 4.604 | 4.648 | 4.650 |
| integers | 4.309 | 4.269 | 4.366 | 6.189 | 6.167 | 6.177 |
| text_heavy | 2.107 | 1.860 | 2.166 | 3.114 | 3.056 | 3.065 |
| unicode | 2.111 | 2.131 | 2.164 | 13.731 | 7.357 | 7.163 |
| unicode_unique | 9.990 | 8.725 | 9.895 | 15.937 | 8.390 | 8.327 |
| ascii_unique | 3.701 | 3.235 | 3.760 | 5.971 | 5.925 | 6.023 |
| latin1_unique | 3.814 | 3.331 | 3.903 | 24.424 | 7.037 | 9.715 |
| alternating_roles | 7.666 | 7.754 | 7.726 | 5.607 | 5.429 | 5.439 |
| mixed_late | 7.325 | 7.429 | 7.440 | 6.072 | 6.078 | 6.091 |
| nested_numeric | 7.858 | 7.905 | 7.884 | 4.516 | 4.562 | 4.550 |
| varying_shapes | 15.102 | 15.409 | 15.231 | 5.007 | 4.831 | 4.841 |
| protocol_fixture | 0.410 | 0.406 | 0.409 | 0.357 | 0.349 | 0.349 |
| cjk_unique | 20.383 | 9.522 | 20.143 | 22.136 | 9.333 | 13.829 |
| greek_unique | 22.435 | 8.741 | 22.067 | 23.549 | 9.482 | 14.002 |
| emoji_unique | 9.124 | 7.167 | 9.045 | 14.762 | 7.218 | 7.194 |

All times are milliseconds per complete operation.

### Explicit SIMD contribution

These ratios compare the SIMD variant with the new scalar control, separating
NEON from the encoder's removal of intermediate conversion buffers. A ratio
above 1 is faster. The scalar writer functions have identical disassembly in
both binaries on each ABI.

| Workload | ARM64 decode | ARM32 decode | ARM64 encode | ARM32 encode |
| --- | ---: | ---: | ---: | ---: |
| ascii_unique | 1.07× | 1.16× | 1.02× | 1.02× |
| unicode_unique | 1.14× | 1.13× | 0.99× | 0.99× |
| latin1_unique | 1.06× | 1.17× | 1.34× | 1.38× |
| cjk_unique | 1.98× | 2.12× | 1.49× | 1.48× |
| greek_unique | 2.34× | 2.52× | 1.56× | 1.48× |
| emoji_unique | 1.25× | 1.26× | 0.98× | 1.00× |

Long Chinese and Greek decoding improves 1.98–2.57× over the preceding
implementation. Encoding those workloads improves 2.07–2.48× overall, with
explicit SIMD contributing 1.48–1.56× relative to the new scalar writer.
Long Latin1 encoding improves 3.17× on ARM64 and 3.47× on ARM32 overall;
its explicit SIMD contribution is 1.34× and 1.38×, respectively.

Mixed Unicode and emoji encoding primarily benefit from the direct native
writer; their sampled scalar conversion remains close to the scalar control.
Cached repeated Unicode decoding is essentially unchanged because the earlier
cache already avoids most conversion. Numeric and shape-heavy workloads show
no broad improvement. Across the ten-sample medians, JSON control differences
are at most 1.87% / 2.16% for decode and 3.25% / 3.64% for encode on ARM64 /
ARM32. Small changes in controls should not be treated as strong performance
claims. These synthetic results do not replace production-asset measurements.

Standard MessagePack and optional float32 payload sizes are unchanged. The
linked benchmark grows by 17,840 bytes on ARM64 and 5,520 bytes on ARM32;
that includes expanded self-tests and is not a stripped production code-size
measurement. The SIMD pass keeps the existing cache-sharing policy and final
graph representation; the encoder removes its intermediate UTF-8 buffer.
Measured retained heap per decoded graph is identical before and after for
all 18 workloads on both ABIs.

## Data contract

The implemented profile supports null, booleans, numbers, strings, dense
arrays, and objects with string keys. Map keys become own data properties;
`__proto__` does not invoke a prototype setter. Duplicate string keys use the
last value, and indexed string keys follow JavaScript enumeration semantics.
Strings must be valid UTF-8 on decode; unpaired UTF-16 surrogates are rejected
on encode.

Wire integers within JavaScript's safe integer range become Number. Larger
signed/unsigned 64-bit values become BigInt. A small BigInt encoded as an
integer will decode as Number: standard MessagePack does not preserve the
original JavaScript Number-versus-BigInt distinction. Negative zero is encoded
as a float and preserved. NaN and infinities are supported by the native
profile; timing corpora are restricted to JSON-representable values.

`msgpack32` is standard MessagePack with an optional exact float32 selection
policy. It emits float32 only when converting the Number to float32 and back
reproduces the value exactly; otherwise it emits float64. It does not round
decimal values or introduce an application-specific extension.

Binary/ext values, non-string map keys, functions, accessors, sparse arrays,
cycles, and BigInts outside the 64-bit wire range are outside this prototype's
contract. The adapter limits encoded/input bytes to 256 MiB and container
nesting to 256. That byte limit is not a limit on the size of the expanded JS
heap. This is an experimental internal API linked into an embedding
executable; no production GN codec target or public JS binding has been added.

## Measurements and interpretation

The primary device is a physical OnePlus 6, Android API 35, serial `885841c1`.
ARM64 and ARM32 use separate native executables and architecture-specific V8
core objects, with NDK 27.3.13750724 and the reference build's `-O2` flags.
ARM32 is ARMv7/NEON, Android's softfp ABI; ARM64 uses pointer compression.
Both batches pin each worker to performance cores 4–7 (`taskset f0`). Device
batches run sequentially. CPU governor and thermal behavior remain under the
device's normal policy; post-workload telemetry is recorded, not a continuous
measurement of active CPU frequency.

There are nine corpora: 250/2,500/25,000 repeated records, 120,000 short decimal
numbers, 120,000 integers, text-heavy records, Unicode-heavy records, varying
object shapes, and the checked-in V8 protocol JSON fixture. The first eight
are deterministic synthetic workloads. They do not substitute for a specific
production asset corpus.

Each ABI has two independently shuffled five-round batches (seeds 20261004
and 20261005). Each sample uses a fresh process, 20 untimed warmup operations,
full GC before timing, and calibrated iteration counts. GC inside the timed
loop is included. The aggregate takes the median of all ten raw samples for
each operation and workload. File I/O, decompression, and cold startup are
outside the timer. `json` includes native UTF-8 bytes becoming a V8 string;
`json_string` separately measures parsing an already-retained source string.
All encoders return owned native buffers; relevant conversion/copy work is
included.

Retained heap is the incremental heap delta after retaining five decoded
graphs and forcing GC, divided by five. It reflects graph layout and sharing
in that experiment. A smaller file does not imply a smaller final JS graph.
Peak RSS includes bootstrap, warmup, timing, and five retained graphs; it
must not be interpreted as the peak memory of a single load.

Payload sizes use minified JSON. `gzipBytes` is gzip level 6 size only. No
compressed-format load-time comparison was run. Compression can remove or
reverse MessagePack's size advantage, especially on repeated keys and short
decimal numbers.

`evidence/android-summary.json` contains the validated aggregate. Raw batch
files and build manifests remain under `out/binary-serialization/`. The
aggregator verifies source, builder, binary, runner, manifest, device-binary,
payload, and independent C checker results before emitting the summary.

## Earlier decoder optimization results (before SIMD)

These are medians of ten samples from the validated paired batches. The before
executable is frozen at `fe97eae01`; the optimized native source is
`e6f93a891`. Both use the same V8 core objects and JSON builder. All timings
are milliseconds per complete byte-input decode, including GC in the timed loop.

| Workload | ARM64 before | ARM64 optimized | ARM64 JSON | ARM32 before | ARM32 optimized | ARM32 JSON |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.406 | 0.324 | 0.386 | 0.434 | 0.331 | 0.432 |
| records_2500 | 4.093 | 3.264 | 4.123 | 4.461 | 3.638 | 4.588 |
| records_25000 | 57.951 | 47.888 | 51.418 | 63.562 | 55.346 | 57.760 |
| short_decimals | 7.698 | 7.697 | 13.768 | 7.705 | 7.636 | 15.956 |
| integers | 3.882 | 3.833 | 4.917 | 4.307 | 4.314 | 5.712 |
| text_heavy | 3.079 | 1.887 | 2.997 | 3.337 | 2.095 | 3.780 |
| unicode | 16.573 | 1.739 | 13.836 | 16.564 | 2.089 | 14.584 |
| unicode_unique | 17.610 | 9.848 | 14.732 | 17.399 | 9.884 | 15.307 |
| ascii_unique | 5.280 | 3.360 | 5.076 | 5.868 | 3.654 | 6.283 |
| latin1_unique | 5.417 | 3.444 | 9.674 | 6.039 | 3.742 | 10.086 |
| alternating_roles | 9.465 | 6.658 | 7.391 | 10.459 | 7.433 | 7.471 |
| mixed_late | 7.287 | 7.147 | 13.180 | 7.120 | 7.078 | 14.657 |
| nested_numeric | 7.745 | 7.588 | 13.892 | 7.798 | 7.672 | 15.966 |
| varying_shapes | 13.426 | 11.821 | 9.037 | 15.187 | 14.512 | 11.557 |
| protocol_fixture | 0.531 | 0.380 | 0.423 | 0.569 | 0.410 | 0.502 |

Unique Unicode strings improve by about 1.8x on both ABIs. The repeated
Unicode case improves by 9.5x on ARM64 and 7.9x on ARM32 because it also
shares repeated long strings. These are distinct effects; the repeated case
must not be used as the general Unicode throughput claim. Highly varying
object shapes still decode faster with JSON. Numeric and late-mixed controls
remain close to the original native decoder.

| Retained heap per graph | Before native | Optimized native | JSON |
| --- | ---: | ---: | ---: |
| records_25000 | 3,419,370 B | 3,419,370 B | 3,707,370 B |
| unicode | 1,466,860 B | 127,121 B | 1,486,030 B |
| unicode_unique | 1,800,220 B | 1,800,220 B | 1,819,390 B |
| short_decimals | 960,024 B | 960,024 B | 960,024 B |
| integers | 480,024 B | 480,024 B | 480,024 B |

The repeated-Unicode retained heap falls from 1,466,860 to 127,121 bytes per
graph, approximately 91.3%, with the same values and ordinary JS access.
The unique-string and repeated-record retained heaps do not materially
change. The table uses the five-graph incremental retention measurement;
it does not measure peak memory of one load.

All serialized JSON, MessagePack, exact-float32 MessagePack, and V8 payload
bytes are identical before and after this optimization. Standard payloads
also match across both ABIs. This pass improves decoding and string sharing;
it introduces no new wire profile or file-size reduction.

The full aggregate, fingerprints, batch references, heap and peak-RSS metrics
are in `evidence/optimization-summary.json`. Final normal/moving-GC results
and independent C checker provenance are in `evidence/optimization-validation.json`.

## Original Android results (frozen baseline)

All latency values below are milliseconds per operation, medians of ten samples.
MessagePack means the C++ visitor path. The JSON timing starts with native UTF-8
bytes, including conversion to a V8 source string. These are warm operations.

| Workload | ARM64 JSON decode | ARM64 MessagePack decode | ARM32 JSON decode | ARM32 MessagePack decode |
| --- | ---: | ---: | ---: | ---: |
| records_250 | 0.386 | 0.406 | 0.430 | 0.435 |
| records_2500 | 4.111 | 4.122 | 4.593 | 4.476 |
| records_25000 | 51.295 | 58.318 | 58.401 | 63.833 |
| short_decimals | 13.774 | 7.694 | 16.244 | 7.645 |
| integers | 4.984 | 3.821 | 5.753 | 4.313 |
| text_heavy | 2.950 | 3.175 | 3.734 | 3.420 |
| unicode | 13.810 | 17.503 | 14.702 | 17.257 |
| varying_shapes | 9.136 | 13.397 | 11.878 | 15.295 |
| protocol_fixture | 0.423 | 0.532 | 0.504 | 0.567 |

| Workload | Minified JSON bytes | MessagePack bytes | MessagePack exact-float32 bytes |
| --- | ---: | ---: | ---: |
| records_250 | 31,547 | 25,047 | 23,331 |
| records_2500 | 323,733 | 256,595 | 239,423 |
| records_25000 | 3,312,808 | 2,597,831 | 2,426,103 |
| short_decimals | 612,121 | 986,165 | 938,165 |
| integers | 697,781 | 359,237 | 359,237 |
| text_heavy | 1,155,281 | 1,138,509 | 1,138,509 |
| unicode | 1,340,281 | 1,313,509 | 1,313,509 |
| varying_shapes | 465,697 | 327,288 | 327,288 |
| protocol_fixture | 56,495 | 49,315 | 49,315 |

| Operation | ARM64 JSON | ARM64 MessagePack | ARM32 JSON | ARM32 MessagePack |
| --- | ---: | ---: | ---: | ---: |
| Encode records_25000 | 40.388 | 29.215 | 49.663 | 37.692 |
| Encode short_decimals | 10.786 | 3.086 | 17.869 | 4.633 |
| Encode text_heavy | 7.091 | 2.553 | 8.100 | 3.181 |
| Encode unicode | 11.511 | 13.170 | 12.062 | 13.725 |

For 25,000 records, the C++ visitor / MPack C reader / C++ native tree
decode medians are 58.318 / 61.198 / 70.684 ms on ARM64 and
63.833 / 69.618 / 86.981 ms on ARM32. The small protocol fixture does not establish a
clear visitor-versus-MPack ranking across both ABIs.

The repeated-record retained-heap delta per graph is 3,707,370 bytes for JSON
and 3,419,370 bytes for MessagePack, a 7.8% reduction, on both ABIs.
The integer-array graph is 480,024 bytes with either decoder; the decimal-array
graph is 960,024 bytes with either decoder. These are the five-graph incremental
retention measurements described above, not standalone peak-load measurements.

| Workload | gzip JSON bytes | gzip MessagePack bytes | gzip exact-float32 MessagePack bytes |
| --- | ---: | ---: | ---: |
| records_25000 | 350,682 | 355,179 | 316,329 |
| short_decimals | 5,935 | 14,808 | 12,907 |
| protocol_fixture | 10,684 | 11,684 | 11,684 |

On Unicode, V8 ValueDeserializer takes 3.642 ms on ARM64 and 3.894 ms on ARM32.
For context, parsing the already-retained JSON source string takes 2.637 ms on
ARM64; UTF-8 decoding/materialization is a substantial part of the byte-input
JSON timing. Treat the byte-input and retained-string cases as different input
contracts.

## Other formats and fit

These are format/design comparisons, not native throughput measurements of
Protobuf or FlatBuffers in this experiment:

| Option | Fit for this requirement |
| --- | --- |
| Native MessagePack visitor | Good candidate for evolving JSON-like data with ordinary JS objects, particularly integer arrays and repeated records whose payload size matters. Adoption should follow per-asset measurements. |
| Native MPack C reader | Direct construction without a DOM and no C++ exception requirement in the dependency; usually slower than the visitor on the measured corpus, with a small-fixture result close enough that no general winner is established there. A credible alternative where dependency/build constraints dominate. |
| V8 ValueSerializer/ValueDeserializer | Already native; a strong candidate for controlled V8-produced caches. The measured Unicode decoder is substantially faster, while repeated-record decoding and retained heap are worse. Its header carries a wire version; the V8 API documents backward-compatible storage, but future-writer/older-reader and cross-runtime interoperability still need a deliberate contract. |
| Protocol Buffers | A concrete schema can replace repeated field-name bytes with field numbers and pack numeric sequences. Native decoding still needs a V8 construction pass unless a direct schema-specific adapter is generated. A generic dynamic-value wrapper does not establish the same size benefit. |
| FlatBuffers | A strong fit when consumers read selected fields directly from the buffer using generated accessors. Requiring a complete ordinary JS graph adds traversal/string/object allocation; the direct-buffer access advantage cannot be assumed for this use case. |

Protocol Buffers' schema and field-number behavior is documented in its
[encoding guide](https://protobuf.dev/programming-guides/encoding/).
FlatBuffers documents generated schemas and direct buffer access in its
[tutorial](https://flatbuffers.dev/tutorial/). The full object-allocation
implication above is an inference from the required JS representation.
MessagePack's float/integer/string layout is defined by the
[specification](https://github.com/msgpack/msgpack/blob/master/spec.md).
MPack's independent reader and node APIs are described in its
[documentation](https://ludocode.github.io/mpack/).

## Validation and reproduction

The original self-test checked 31 expression fixtures against three native decoder
backends and both float profiles. It checks full values, property order,
prototypes, safe/unsafe integers, BigInt boundaries, negative zero, Unicode,
mixed/dense arrays, duplicate keys, changing shapes, ten malformed-input
classes, incomplete prefixes, nesting limits, and 2,000 deterministic random
byte inputs with differential acceptance checks. Normal and moving-GC stress
runs passed on the desktop and both physical Android ABIs. Every timed corpus
is round-tripped before measurement; both MessagePack profiles pass the
independent C-branch wire checker.

The final six normal/stress runs, executable fingerprints, and independent
C checker provenance are saved in `evidence/validation.json`. The full V8
test suite and a production GN build of a published codec target are outside
this prototype validation. That file records the original investigation; the
optimization pass has separate evidence and a larger correctness suite.

The build script intentionally reuses fingerprinted V8 core objects. It
recompiles the relocated JSON parser, native adapter, and harness, and links
them with the corresponding core objects. It does not invoke Ninja or mutate
the reference V8 checkout. A build manifest's checkout commit records HEAD
at build time; the per-source SHA-256 values identify the actual experiment
sources, including any then-uncommitted optimization.

From this worktree, the ARM64 reproduction commands are:

```sh
python3 tools/binary_serialization/build.py \
  --platform android-arm64 \
  --v8-build /Users/james/projects/v8/v8-standalone/out/json-review-arm64 \
  --msgpack /Users/james/projects/serialization/msgpack-c \
  --output out/binary-serialization/android-arm64-reproduction

python3 tools/binary_serialization/build_c_peer.py \
  --msgpack /Users/james/projects/serialization/msgpack-c

python3 tools/binary_serialization/run.py \
  --binary out/binary-serialization/android-arm64-reproduction/native-binary-benchmark \
  --output out/binary-serialization/android-arm64-reproduction/batch-a \
  --rounds 5 --seed 20261004 --target-ms 120 \
  --c-peer out/binary-serialization/c-peer \
  --adb-serial 885841c1 \
  --remote-dir /data/local/tmp/codex-binary-arm64 --cpu-mask f0
```

MPack must be present at the pinned commit under
`out/binary-serialization/mpack-source`, or passed with `build.py --mpack`.
The supplied msgpack-c include tree must match the pinned C++ commit. Repeat
with seed 20261005 and a fresh `batch-b` output directory. For ARM32, use
`--platform android-arm32`, the `out/json-review-arm32` V8 core directory,
and distinct ARM32 output/device directories. Run device batches sequentially.
The scripts do not reconstruct a full V8 core build from an arbitrary source
checkout; the supplied core objects and build flags must match the source.

The following paired decoder commands describe the earlier pass. Reproducing
or validating its source fingerprints requires the `e6f93a891` source state.
For that comparison, keep the original executable and its
manifest at the frozen source commit. Build the updated executable into a new
directory, then run:

```sh
python3 tools/binary_serialization/compare_decode.py \
  --before out/binary-serialization/android-arm64-optimized/native-binary-benchmark \
  --after out/binary-serialization/optimization-final-arm64/native-binary-benchmark \
  --output out/binary-serialization/optimization-final-arm64/paired-a \
  --rounds 5 --seed 20261005 --target-ms 120 \
  --c-peer out/binary-serialization/c-peer \
  --adb-serial 885841c1 \
  --remote-dir /data/local/tmp/codex-msgpack-opt-arm64
```

Repeat with seed 20261006 and a fresh `paired-b` directory. Run equivalent
ARM32 batches sequentially. `analyze_decode.py` consumes the four results with
`--arm64-a`, `--arm64-b`, `--arm32-a`, `--arm32-b`, and `--output`. It requires
two completed five-round, 15-workload batches per ABI, identical source/binary
fingerprints, unchanged standard payloads across both ABIs, and shared before/
after iteration counts. It verifies all prepared payload files and the C-peer
results before reporting combined ten-sample medians. The build script also
rejects a build if experiment source or headers change while compiling/linking.

For the current SIMD comparison, build both new variants from the same frozen
source. ARM64 uses `--platform android-arm64` and
`--v8-build /Users/james/projects/v8/v8-standalone/out/json-review-arm64`:

```sh
python3 tools/binary_serialization/build.py \
  --platform android-arm64 \
  --v8-build /Users/james/projects/v8/v8-standalone/out/json-review-arm64 \
  --msgpack /Users/james/projects/serialization/msgpack-c \
  --output out/binary-serialization/simd-arm64

python3 tools/binary_serialization/build.py \
  --platform android-arm64 --disable-msgpack-simd \
  --v8-build /Users/james/projects/v8/v8-standalone/out/json-review-arm64 \
  --msgpack /Users/james/projects/serialization/msgpack-c \
  --output out/binary-serialization/simd-scalar-arm64

python3 tools/binary_serialization/compare_simd.py \
  --before out/binary-serialization/optimization-final-arm64/native-binary-benchmark \
  --after out/binary-serialization/simd-arm64/native-binary-benchmark \
  --scalar out/binary-serialization/simd-scalar-arm64/native-binary-benchmark \
  --output out/binary-serialization/simd-arm64/reproduction-a \
  --rounds 5 --seed 20261005 --target-ms 100 \
  --c-peer out/binary-serialization/c-peer \
  --adb-serial 885841c1 \
  --remote-dir /data/local/tmp/msgpack-simd-reproduction-arm64-a
```

The before executable is the preceding optimized decoder, with native source
`e6f93a891`; the driver's baseline source check uses `7f8f26f07`, whose native
sources are identical. Repeat with seed 20261006 and fresh local/device
directories. Repeat sequentially for ARM32 using its core build and distinct
SIMD/scalar output directories. `analyze_simd.py` accepts the same four batch
arguments as `analyze_decode.py`, and requires two completed five-round,
18-workload batches per ABI. Only the final source-matched `measured-a` and
`measured-b` batches contribute to the SIMD report; pilot and superseded runs
are excluded.

To exercise moving GC separately, run the executable with
`--self-test --stress` on each ABI. To aggregate the four final batches, run
`analyze.py` with `--arm64-a`, `--arm64-b`, `--arm32-a`, `--arm32-b` pointing to
their `results.json` files and `--output` pointing to a summary file.

Production work after this investigation consists of pinning/licensing the
dependency in the build, adding the actual embedder binding and backing-store
lifetime contract, integrating the chosen API/error profile, and exercising
production assets with broader binding-level sanitizer/fuzz coverage. The measured codec prototype
and this evidence are reviewable inputs to that work.
