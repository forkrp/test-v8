# Native binary serialization investigation

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

## Android results

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

The self-test checks 31 expression fixtures against three native decoder
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
this prototype validation.

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

To exercise moving GC separately, run the executable with
`--self-test --stress` on each ABI. To aggregate the four final batches, run
`analyze.py` with `--arm64-a`, `--arm64-b`, `--arm32-a`, `--arm32-b` pointing to
their `results.json` files and `--output` pointing to a summary file.

Production work after this investigation consists of pinning/licensing the
dependency in the build, adding the actual embedder binding and backing-store
lifetime contract, integrating the chosen API/error profile, and exercising
production assets and sanitizer/fuzz coverage. The measured codec prototype
and this evidence are reviewable inputs to that work.
