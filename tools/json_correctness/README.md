# JSON correctness regression checks

This runner compares `JSON.parse`, `JSON.parseAsync`, `JSON.stringify` and
`JSON.stringifyAsync` with a separately built, unmodified V8 12.6 executable.
The reference only needs the synchronous APIs. Use a fresh output directory
for each invocation; existing evidence is never overwritten by the runner.

```sh
python3 tools/json_correctness/run.py \
  --reference /absolute/path/to/original-12.6/d8 \
  --candidate out/host/d8 \
  --out out/json-correctness-new-release
```

The JS test does not use the modified JSON stringifier to compare parsed values.
It encodes binary64 bits, UTF-16 code units, key order, property descriptors,
array holes and standard/null/other prototype categories directly. Parser input
comes from a separate simple encoder. Stringifier output is compared exactly,
including exception names/messages and observable callback traces. Expected
positive cases are also checked for accidental harness exceptions.

Each run stores gzip-compressed transcripts, stderr, commands, source and binary
SHA-256, and `results.json`. A successful report requires both `completed` and
`all_passed` to be true. Comparisons are performed line by line; digest equality
is not the only check. The caller must supply the genuine original reference;
using another optimized build would weaken the oracle.

Repeat the same corpus in a DCHECK build, retaining the original references:

```sh
python3 tools/json_correctness/run.py \
  --reference /absolute/path/to/original-12.6/d8 \
  --candidate /absolute/path/to/dcheck/d8 \
  --out out/json-correctness-new-dcheck \
  --label dcheck \
  --reference-results out/json-correctness-new-release/results.json \
  --flag=--stress-compaction --flag=--verify-heap
```

Useful additional configurations:

- An ASan d8 with `--flag=--stress-compaction`.
- `--flag=--no-json-stringify-fast-path` to exercise the generic serializer.
- `--group fuzz --seed 0x13579bdf --count 10000` for a separate deterministic
  corpus; a reference-results file must match the seed, count, group and script.
- `--group large` for exact 4 MiB threshold neighbors and a 16 MiB threshold
  crossing, escaped structural punctuation, leading whitespace, UTF-16 source,
  and malformed input near prospective partition boundaries. It also checks
  the corresponding large serialization workload. Large fixture construction
  uses bounded temporary chunks so GC stress exercises the API without creating
  millions of irrelevant temporary concatenation nodes.

The native checks use inaccessible pages to detect overreads, including at the
end of external V8 strings:

```sh
out/host/cctest test-json-async/JsonScannerGuardPages
out/host/cctest test-json-async/JsonNumberGuardPages
out/host/cctest test-json-async/JsonExternalStringGuardPages
```

Run these in Release, DCHECK and ASan builds. They cover input alignment and
length boundaries, every UTF-16 code unit in SIMD lanes, width classification,
numeric conversion, external strings, truncated escapes and error reporting.
The existing `test-json-async` tests cover task cancellation, dropped completion,
isolate teardown, realms and GC; the new differential suite supplements them.

For ARM32, distinguish a portable code path from a portable complete binary.
The current default ARM32 configuration enables NEON. `__ARM_NEON` is a compile
time condition, and V8 CPU feature probing incorporates compiler-implied
features. A check inside JSON cannot make a globally NEON-compiled V8 safe on a
CPU lacking NEON.

The relevant settings for an ARM32 baseline without NEON are:

```gn
arm_use_neon = false
arm_fpu = "vfpv3-d16"
```

These settings must apply to the complete build and be audited with its native
dependencies, snapshot generation and target-device execution. Separate NEON
kernels and runtime dispatch would be additional work. Merely setting Android
API level 21 does not perform hardware detection.

The 2026-11-05 validation compiled eight JSON/numeric translation units with
NEON disabled, and exercised the portable string implementation on the host.
That is not a full non-NEON ARM32 binary or device qualification. See the
[validation report](../../docs/json-correctness-validation-20261105.md) for exact
builds, results and remaining limits.

## Pinned upstream suites

`external.py` adds upstream tests without making a C/C++ library's behavior the
definition of ECMAScript JSON. It uses only the Python standard library. The
locked files and their SHA-256 hashes are in `external-sources.json`; source
downloads, generated fixtures and raw logs stay under the chosen output directory.

```sh
python3 tools/json_correctness/external.py fetch \
  --cache out/json-external/sources
python3 tools/json_correctness/external.py prepare \
  --cache out/json-external/sources --out out/json-external/prepared
python3 tools/json_correctness/external.py run \
  --prepared out/json-external/prepared \
  --binary /absolute/path/to/original-12.6/d8 --label reference \
  --out out/json-external/reference
python3 tools/json_correctness/external.py run \
  --prepared out/json-external/prepared \
  --binary out/host/d8 --label release \
  --reference-results out/json-external/reference/results.json \
  --out out/json-external/release
```

Only `fetch` accesses the network. It verifies existing cache entries and refuses
modified files; `prepare` and `run` work offline. Output directories must be new.
Without `--reference-results`, `run` executes only the synchronous APIs and
produces the original-reference report. Supply an actual unmodified reference,
not another optimized executable. Candidate runs require a successful reference
with the same fixture manifest and JS driver, then compare all four API outputs
line by line. Both `completed` and `all_passed` must be true.

The pinned scope is:

- All 166 Test262 JSON files, including raw JSON and source access, each run in
  its own d8 process in both sloppy and strict modes (332 executions). The
  upstream assertions and required harness files are used unchanged. The small
  `$262.createRealm` adapter uses d8's cross-realm-access interface. These are
  synchronous ECMAScript tests; this is not a run of all Test262 or an automatic
  conversion of its assertions to asynchronous APIs.
- RapidJSON Reader/Writer literal macro inputs, including adjacent C literals,
  numeric errors, UTF-8 string cases and `ScanWriteUnescapedString`. Nonliteral
  C++ loops, transcoding arrays and the rest of the C++ unit suite are outside
  this importer. Literal inputs inside inactive preprocessor branches are kept
  as additional cases; this does not claim RapidJSON runs them upstream.
- Every line of yyjson's 22 number `.txt` files, classified under strict JSON
  grammar and binary64 rather than yyjson's optional extensions or integer types.
- simdjson's pinned `simdjson-data/jsonchecker` JSON files, including adversarial
  regressions and minefield cases; nlohmann's roundtrip, json_tests and Big List
  of Naughty Strings data. Overlapping inputs are deduplicated by UTF-16 content,
  retaining every origin. This is not deduplication against all older local tests.
- Derived string offsets around 16/32/64 code-unit boundaries, truncated imported
  inputs and imported escaped strings repeated into arrays at 4 MiB minus/plus
  32 bytes and 16 MiB plus 32 bytes. Each large size has a control character
  injected inside a string near the midpoint. Large successful parses check all
  elements and their descriptors; large serialization compares the entire output.

Ordinary input expectations come from Python's independent strict JSON decoder,
binary64 conversion and an explicit ECMAScript key-order adapter. The wire format
retains numeric bits (including negative zero), UTF-16 code units and object/array
structure. The d8 driver additionally checks prototypes, property descriptors and
holes. Serializer inputs are built from tagged values, without calling a parser;
string escaping also has a small independent quoting oracle. Other serialized
outputs are compared exactly with original V8, not with C/C++ writer spelling.
Error comparisons use `SyntaxError`, not engine-specific error messages.

There are 36,680 unique cases in this snapshot: 33,979 serializer inputs, 2,653
independently classified syntax errors and 48 deep inputs checked only by parse
differential. The latter exceed the Python oracle's recursion limit and have no
independent semantic verdict. The JS result encoder is iterative. Another 28
files contain invalid UTF-8 bytes and are explicitly excluded because these JSON
APIs receive UTF-16 strings; no replacement-character decoding is performed.
`manifest.json` retains the exact exclusions, adaptations and provenance.

Repeat candidate runs using a DCHECK binary with
`--flag=--stress-compaction --flag=--verify-heap`, an ASan binary with
`--flag=--stress-compaction`, a qualified portable-path binary, and the Release
binary with `--flag=--no-json-stringify-fast-path`. Host portable-path testing
does not qualify an ARM32 CPU without NEON. Existing native guard-page and async
lifecycle tests remain complementary to these external inputs.

The importer/driver control checks include deliberately incorrect expectations:

```sh
JSON_TEST_D8="$PWD/out/host/d8" python3 tools/json_correctness/test_external.py
```

Upstream sources are downloaded at the pinned commits, not redistributed here.
The cache retains the fetched license files and attribution READMEs. RapidJSON
tests and yyjson are MIT; Test262 test content is BSD, and the cached Big List of
Naughty Strings includes its MIT license. simdjson's code license must not be
assumed to cover every separately collected data file, and nlohmann's aggregated
data has multiple origins. Consult those origins before redistributing fixtures.
The lock keeps the simdjson build dependency declaration that identifies the
chosen simdjson-data revision.

The [2026-11-05 upstream validation report](../../docs/json-external-correctness-20261105.md)
records the five candidate configurations, source/binary identities and explicit
coverage limits.
