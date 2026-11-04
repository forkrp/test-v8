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
