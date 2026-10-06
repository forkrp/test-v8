# Asynchronous MessagePack implementation and evidence

Goal: implement `MSGPACK.encodeAsync` and `MSGPACK.decodeAsync` like this fork's custom async JSON methods, validate correctness and ownership, and establish useful performance with representative measurements.

Measured implementation: **r11-final**, branch `codex/msgpack-async`, base `d23be36808219a08dfcc19fd55e6866e6af6df73`. Worktree: `/Users/james/.codex/worktrees/1d8b/v8-standalone`. The implementation was subsequently recorded in local commit `8dc47d839`. The performance tables below describe that pre-integration runtime source snapshot; integration with the compact resource format has separate validation evidence.

## Implementation and contract

Both APIs return Promises, have one formal argument, are non-enumerable and are not constructors. The supported value/wire profile matches synchronous MSGPACK. The complete contract, examples, rejections and resource requirements are in [msgpack-async.md](msgpack-async.md).

Encoding snapshots the admitted graph on the isolate thread. Scalar wire values and key plans stay there; packed numeric spans and deferred strings use owned native worker storage. A complete-wire capture transfers its input buffer directly. Decode copies the exact input view before return, validates/parses into a native tape on a worker, then constructs rooted V8 results on the foreground in cooperative slices. Workers do not dereference V8 heap objects.

Isolate teardown cancels jobs, releases foreground roots and waits for cancelable tasks. Output allocation ownership and external-memory accounting are checked. At most 32 jobs may be pending; captured input/metadata has a 256 MiB native budget, output has a separate 256 MiB wire limit, and nesting is bounded at 256. These are not a process-wide memory guarantee. Capture/input copying are synchronous, and the 2 ms materialization target is a soft budget rather than a hard deadline.

## Validation and completion evidence

The final requirement audit covers four source-matched linked builds, complete named test suites, complete case combinations and sample/result counters, source archives, exact binary/compiler/runtime/script/input identities, ownership and manual performance review. The saved artifact is `out/msgpack-async/requirements-audit-r11-final.json`; ownership and performance reviews are `ownership-review-r11-final.json` and `performance-review-r11-final.json`. **All 26 requirements passed, with no remaining gates. The goal is complete.**

| Configuration | Compiler inputs | Validation |
|---|---:|---|
| Normal macOS ARM64 host | 4,266 | Async/numeric/sync suites in three GC modes; seven native MSGPACK, six native JSON and three standalone JSON checks |
| Actual host heap-verifier build | 4,269 | The nine GC-mode suites and seven native MSGPACK checks with heap verification |
| Android ARM64 | 4,671 | All 23 named API, numeric, GC, lifecycle, accounting and JSON checks |
| Android ARM32 | 4,261 | All 23 corresponding checks |

Correctness coverage includes exact sync wire/value parity, numeric and unsafe 64-bit boundaries, strict UTF-8, view offsets and lengths, detachment/resizing/shared-storage rejection, mutations after snapshot, unsupported hooks/types, cycles, schema hints, realms, moving/incremental GC, retained output, 2,000 deterministic malformed inputs, large container-count regressions, tape-budget rejection/recovery, pending-job limits, termination and tasks outliving isolate disposal.

## Local integration with compact resources

The integration with resource-format commit `1b219d1ee` preserves synchronous, asynchronous and compact resource methods. Conflicts in the API documentation, builtins, global installation and codec implementation were resolved together. Resource builtins use the shared input validation and owned-output transfer helpers; the templated decoder retains compact resource decoding alongside asynchronous standard MessagePack materialization.

The combined macOS ARM64 `d8`, `cctest` and native benchmark harness linked successfully. All 38 post-integration check invocations passed: five API suites in three GC modes, seven native MSGPACK and six native JSON checks, three standalone JSON suites, four harness checks, and three focused integration reruns with long Unicode property names. The integration regression test interleaves both APIs across GC and mutations after capture. The checked-in summary is [msgpack-async-integration.json](../tools/binary_serialization/evidence/msgpack-async-integration.json); detailed logs, source identity and frozen linked binaries are retained in the original worktree's `out/msgpack-async` directory. Android correctness and performance were not rerun after integration; the Android results below remain evidence for the pre-integration measured source snapshot.

## Measurement method

Target: OnePlus 6 (`ONEPLUS A6003`), serial `885841c1`, ARM64 and ARM32 executables, big-core affinity `f0`, governor `schedutil`, NDK 27.3.13750724 runtime libraries. Reported Android release is 15; the retained build fingerprint identifies the OnePlus Android 11 vendor build. Device properties and thermal conditions are retained without inferring a ROM identity.

The full matrices cover 18 non-tiny workloads plus tiny calls, encode/decode, six public sync/async MSGPACK/JSON codecs, one/four jobs, five shuffled fresh-process passes and 31 timed samples per process. Full value restoration and exact MSGPACK wire parity are checked before timing. JSON byte variants include owned native UTF-8 transport; string-only controls remain separate. There are ten warmup batches, full GC before timing and timed GC included. Tiny samples contain 1,000 inner batches.

Reported points are medians of independent process medians. Pass ranges expose variation and are not confidence intervals. Geometric means weight each non-tiny workload equally. Four-job latency is whole-batch latency, not latency per individual job. Submission is the synchronous call portion, not all foreground CPU time including later materialization.

| Collection | Processes per ABI |
|---|---:|
| Full public API matrix | 2,280 |
| Full synchronous before/after controls | 380 |
| Unfavorable/public focused repeats, 16 workloads and three codecs | 960 |
| Ten-pass ASCII/tiny synchronous repeats | 80 |
| Matched record comparison with r8 | 80 |
| Matched encoder comparison with r10 | 160 |
| Separate tiny public repeats | 60 |
| Matched four-job tiny comparison with r8 | 20 |

The selected-source collections total **8,040 timed fresh processes**, plus synchronous calibrations and separately retained diagnostics. Full matrices are `full-{arm64,arm32}-r11-final/results.json`; all repeat and control folders follow the names above in `out/msgpack-async`. `evidence-summary-r11-final.json` retains the full aggregate and synchronous results.

## Full-corpus performance

Geometric-mean speedup versus async JSON with owned UTF-8 byte transport:

| ABI | Operation | One job | Four jobs | Median submission change versus sync MSGPACK, one/four jobs |
|---|---|---:|---:|---|
| ARM64 | encode | 1.84x | 2.06x | 3.7% higher / 35.2% lower |
| ARM64 | decode | 1.27x | 1.55x | 81.8% lower / 91.2% lower |
| ARM32 | encode | 2.60x | 2.62x | 9.9% higher / 22.7% lower |
| ARM32 | decode | 1.19x | 1.57x | 79.9% lower / 88.4% lower |

Decode submission is materially reduced. Encoding retains synchronous traversal/copying and does not generally reduce one-job submission. Both APIs have useful measured bulk performance, with the workload limitations below. Uniform latency wins or zero foreground work are not claimed.

## Workload exceptions and repeat review

Every original unfavorable case was included in the five-pass focused repeats. Persistent outcomes are retained, including the additional unfavorable cases first observed during repeats. The subset aggregate does not replace the full 18-workload aggregate.

| ABI and operation | Slower than async JSON bytes in the full matrix | Slower in the repeated subset |
|---|---|---|
| ARM64 encode, 1 job(s) | latin1_unique, ascii_unique, cjk_unique, greek_unique, text_heavy, unicode | latin1_unique, cjk_unique, ascii_unique, greek_unique, text_heavy, unicode |
| ARM64 encode, 4 job(s) | None | None |
| ARM64 decode, 1 job(s) | latin1_unique, unicode_unique, emoji_unique | latin1_unique, unicode_unique, emoji_unique, protocol_fixture |
| ARM64 decode, 4 job(s) | records_250, mixed_late | records_250, mixed_late |
| ARM32 encode, 1 job(s) | latin1_unique, ascii_unique, text_heavy, greek_unique | latin1_unique, text_heavy, ascii_unique, greek_unique |
| ARM32 encode, 4 job(s) | None | None |
| ARM32 decode, 1 job(s) | latin1_unique, unicode_unique, emoji_unique | latin1_unique, unicode_unique, emoji_unique, protocol_fixture |
| ARM32 decode, 4 job(s) | mixed_late | records_250, mixed_late |

One-job unique-string encode and Latin-1/unique Unicode decode deficits persist. Small-record and mixed four-job decode differences also remain. Encode submission increases persist for several record/shape/string workloads. All magnitudes, per-codec points and ranges are retained in `performance-review-{arm64,arm32}-r11-final-pending.json` and `repeat-review-{arm64,arm32}-r11-final-pending.json`; the filenames identify their intermediate collection stage, and the final manual assessment is `performance-review-r11-final.json`. No unfavorable case is inferred away solely because an algorithm path is unchanged or its ranges overlap.

No full synchronous control or ten-pass synchronous repeat has a measured slowdown of 5% or more. Small positive differences remain in the raw tables; overlapping ranges do not prove exactly zero cost. The historical synchronous baseline is the retained stage45r2 executable and its recorded source/build configuration. Its historical compiler binary hash was not captured, and no compiler-identity equivalence is inferred retrospectively.

## Matched optimizer comparisons

The selected scalar-eager capture/direct-transfer encoder fixes the expensive earlier record path. Against r10, record encode total changes are ARM64 -72.1%/-34.1% and ARM32 -69.5%/-26.6%; submission changes are -50.8%/-9.1% and -49.1%/-8.9%, for one/four jobs.

Control costs are retained: one-job short-decimal latency rises 14.0%/25.2% on ARM64/ARM32 against r10, with submission increases 30.4%/9.9%. ARM32 one-job CJK submission rises 18.7%. These comparisons do not redefine the full-corpus result.

Against r8, record decode changes are ARM64 -0.3%/+1.7% and ARM32 +3.8%/+1.2%. The earlier +6.6% ARM32 pilot remains in its original artifact. These small repeat changes have overlapping ranges; measured increases are retained. Four-job numeric decoding improvements from r10 selection remain documented in `matched-{arm64,arm32}-r8-r10/results.json`, while the current full matrices establish final-source performance.

## Tiny-call limits

Fresh tiny repeats and matched r8 comparisons are separate from the bulk aggregate. One-job encode process medians vary broadly: ARM64 0.038-1.288 ms, ARM32 0.035-1.291 ms. They do not support a stable low-latency claim. Tiny decode is slower than async JSON byte transport: ARM64 one/four-job totals are 1.360/0.122 ms versus 1.286/0.095 ms; ARM32 totals are 1.345/0.341 ms versus 1.281/0.099 ms. Dispatch, Promise, native staging and foreground construction overhead remain relevant at this scale; the data do not attribute the variation to one proven cause.

Four-job encode is about 6% faster than r8 on both ABIs. Matched four-job decode changes -4.1% on ARM64 and +2.9% on ARM32, with overlapping ARM32 ranges. Tiny calls are not claimed to match synchronous latency.

## Interruptions and rejected candidates

ARM64 full collection stopped after 1,403 successful processes when an adb JSON-byte decode invocation failed with empty stdout/stderr. The exact invocation passed three fresh diagnostic reruns; no cause is asserted. `resume_async.py` verified all identities, retained the original successful row prefix and order, and saved a hashed checkpoint plus new segment conditions. The completed matrix is explicitly segmented. The original failure evidence is in `failure-arm64-r11-final`.

The first tiny-only collector used an earlier loaded helper that attempted an empty non-tiny aggregate after all 60 timed processes had finished. The corrected summary was verified with 120 retained real rows, then the complete unchanged matrix was audited and the remaining comparisons continued. This was a collector failure, not a timed API failure.

The r13 worker reservation heuristic is unselected after 200 host diagnostic processes: four-job ASCII encode improves 28%, while CJK/Greek encode regress 13%/10%. Its wire/snapshot/GC/lifecycle checks pass, but no Android improvement is claimed. Earlier r6/r7/r8/r10 and r12 selection/negative evidence remains under the evidence root. The chronological development log is preserved as `development-history.md`; it is historical and its intermediate status statements are superseded by this report.

## Reproduction and evidence identity

Frozen folders `frozen-{host,verify,arm64,arm32}-r11-final` include build manifests, compiler identities, exact commands, current compiler-input hashes, saved tracked source patches, new-source archives and executable hashes. The runtime libraries, source fixture hashes, device commands and condition readings are in each raw report. The source audits and final requirement audit link those artifacts to the selected implementation.

| ABI | Selected native benchmark SHA-256 |
|---|---|
| arm64 | `db9009cbb54ef605dab5c3379dd9c7cb66019272b98c52f8b4818179618f6bfd` |
| arm32 | `a3ea457bfc650ccc6c7657f0b446b7fe46913ddd5d61a1f80b79f4e09c28e654` |

Representative correctness and audit commands:

```sh
out/msgpack-async/frozen-host-r11-final/d8 --allow-natives-syntax --expose-gc test/msgpack/async.js
out/msgpack-async/frozen-host-r11-final/d8 --expose-gc test/msgpack/async-numeric.js
out/msgpack-async/frozen-host-r11-final/cctest test-json-async/MessagePackAsyncTasksOutliveIsolate
python3 tools/binary_serialization/audit_async.py --evidence out/msgpack-async --label r11-final --ownership-review out/msgpack-async/ownership-review-r11-final.json --performance-review out/msgpack-async/performance-review-r11-final.json --output out/msgpack-async/requirements-audit-r11-final.json
```

For fresh timing runs, `run_async.py`, `run_sync_control.py` and `compare_async.py` retain their exact invocation and outputs; use a new output folder, the ABI-matched frozen executable/runtime and the same device lock. `freeze_async.py` checks current links and archives source/compiler identity; `summarize_async.py` validates raw completion before computing points; `resume_async.py` preserves and audits an interrupted prefix.

Native snapshot generation and timed matrices serialize through `/tmp/v8-msgpack-async-device.lock`. macOS-to-Android build adaptations and device snapshot commands are retained in the frozen manifests. No parallel phone benchmark is part of these evidence collections.

Performance scope is the recorded device, affinity/governor, compiler/runtime and native default-platform harness. It does not establish other-device performance, GUI frame deadlines, absence of every small regression, a hard 2 ms slice maximum, or a process-wide memory bound.
