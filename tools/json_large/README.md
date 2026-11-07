# Large JSON measurements on Mac ARM64

The report is [docs/json-large-mac-arm64.md](../../docs/json-large-mac-arm64.md).
The committed summary is [evidence/mac-arm64-20261026.json](evidence/mac-arm64-20261026.json).
The report records the immutable external evidence archive and exact engine commits.

The 2026-10-29 JSON.parse / JSON.parseAsync follow-up is documented in
[the final performance report](../../docs/json-parse-final-mac-arm64.md), with
[all final comparison cells](evidence/json-parse-final-mac-arm64-20261029.json)
and [PNG/SVG plots](../../docs/images/json-parse-final-mac-arm64.svg).
It compares the final build with the local `12.6` branch and two intermediate
builds, and separately rechecks async behavior against the task-start baseline.
The report identifies the retained local raw-data directories and replay commands.
Those directories contain the frozen binaries, source snapshots, calibration,
protocols, runners and every sample, including rejected candidates.
The added 5% ASCII pilot target was not fully met and is explicitly reported;
the original numeric targets and all regression gates passed.

The next follow-up targets the dictionary-construction cost at 128 named
properties. See [the wide-object report](../../docs/json-wide-dictionary-mac-arm64.md)
and [its complete results](evidence/wide-dictionary-mac-arm64-20261029.json).
It retains dictionary maps and the global map-cache size, and uses V8's existing
normalized property store for new JSON objects. `wide_objects.js` covers the
boundary, UTF-16 keys, single objects, fresh keys, changing shapes, duplicates
and indexed objects. The report records the frozen three-build comparison,
all controls, source snapshots, raw data and replay commands.

The current full-tree fingerprint uses an Int32Array for its two digest words.
This avoids boxed accumulator state during validation without changing the
fingerprint algorithm, traversal or checked values. All 18 prior references were
verified unchanged before the new timing epoch. This is outside the API timer;
validation speed is not counted as an engine improvement.

`generate.py` creates six workload families near 10/20/50 decimal MB. The three
public workloads derive from nativejson-benchmark's Canada, CITM catalog and
Twitter documents. Complete compact seed documents are repeated inside an array.
The other families use deterministic records and mixed nested data. The source
pins, upstream licenses and raw checksums are preserved with the evidence.

`benchmark.js` measures one API and one input in each process. It reads/decode
inputs before timing, performs two warmups and nine measured batches, and checks
every complete result outside the API interval. `common.js` traverses every key
and value iteratively, including numeric bits and UTF-16 string contents. The
validation stage also hashes the complete canonical UTF-8 output with SHA-256.

`run.py` alternates adjacent baseline/current processes over four pairs and
preserves all samples. Its CPU and RSS measurements cover the whole process,
including data loading, validation and explicit GC. RSS units in this recorded
experiment use Darwin's byte-valued `ru_maxrss`. Use `V8_JSON_ROOT` to select a
repository when replaying an extracted archive.

The original `12.6` build does not expose parseAsync. That API is measured only
on the extension branch. Four-job checks compare four serial parse calls with
four concurrent parseAsync calls on the 12- and 60-field record families.

## Public production data snapshots

`real-sources.json` adds four sources with different production structures:
the USGS monthly GeoJSON feed, full npm TypeScript package metadata, Taipei's
YouBike station feed, and the Kubernetes v1.34.1 OpenAPI document. These are
public service responses or a released API schema; they are not private
application traffic and do not establish coverage of every production workload.
Upstream documentation and attribution are recorded in the source configuration.

Fetch an immutable local snapshot, then expand it without changing its root:

```sh
JSON_REAL_OUT=/absolute/path/to/fresh-real-corpus
python3 tools/json_large/real_corpus.py fetch \
  --out "$JSON_REAL_OUT/originals"
python3 tools/json_large/real_corpus.py generate \
  --sources "$JSON_REAL_OUT/originals" \
  --out "$JSON_REAL_OUT/expanded" --sizes 10 20 50
python3 -m unittest discover -s tools/json_large -p test_real_corpus.py -v
```

Original files retain response bytes. The manifest records URLs, fetch time,
response headers, SHA-256, UTF-8 byte length, UTF-16 code-unit length, root kind,
value counts, maximum container widths, and ordered object-shape diversity.
Every generation checks original hashes and records the generator hash and seed.
Live responses change: replay from the saved snapshot, not from another fetch.

Generated files retain all original records and append shuffled cycles of real
records at their existing location. USGS remains an object with `features` and
metadata; npm remains an object with a `versions` dictionary; YouBike remains a
station array. Generated identifiers are unique, USGS's count is updated, and
npm's added version keys, version fields and time keys agree. Root member order,
record optional fields, nulls, Unicode and the remaining values are preserved.
JSON lexical formatting is compacted by Python and therefore differs from the
original responses. Generated records reuse non-identifier values and upstream
URLs; they are synthetic load fixtures, not new events, stations, or packages.
Do not report gains on these replicas as gains on unseen real data.

The generator skips targets below the original compact size instead of deleting
real records. Kubernetes remains an original-only control, preserving its
reference graph and the proportions of definitions and paths. In the initial
snapshot, TypeScript is already 15.90 MB and receives only 20/50 MB derivatives.
Generated derivatives and raw responses must be reported separately from the
older repeated-whole-document arrays and fully synthetic stress tests.

`benchmark.js` accepts `parse`, `parseAsync`, `stringify` and `stringifyAsync`.
Both async modes await all returned promises. Parsing checks every value and key;
serialization checks every output UTF-16 code unit. Input loading, reference
generation and result validation remain outside the timer. An original `12.6`
build supplies the correctness reference through `oracle` mode. Use complete
canonical output SHA-256 as an additional correctness check when admitting new
fixtures; a round trip alone cannot detect matching parser/stringifier bugs.

The initial device probe is retained in `out/json-real-corpus-20261107/`, with
`probe.py`, `validate.js`, `protocol.json`, original/derived manifests, and raw
timings. The [compact device evidence](evidence/real-corpus-android-20261107.json)
records 96 successful full-tree/canonical-SHA comparisons, runtime dictionary
checks, and all original-document timings. Its two-process, three-sample timing
cells are exploratory baselines
on unmodified engines. They do not establish an optimization win, tail latency,
four-job behavior, peak memory, or frame responsiveness. Optimization acceptance
still needs frozen before/after binaries, adjacent alternating runs on ARM32 and
ARM64, one/four jobs, raw and expanded sizes, API return time, completion latency,
foreground blocking, and peak native/process memory. Preserve unfavorable cells
and run the existing malformed-input and JavaScript-semantics suites as well.

## Existing benchmark replay

For a corpus-only regeneration into a fresh experiment directory:

```sh
JSON_BENCH_OUT=/absolute/path/to/fresh-experiment
python3 tools/json_large/fetch.py --out "$JSON_BENCH_OUT/downloads"
python3 tools/json_large/generate.py \
  --out "$JSON_BENCH_OUT/fixtures" \
  --sources "$JSON_BENCH_OUT/downloads" \
  --pins tools/json_large/source-pins.json
```

For a measurement replay, extract the verified archive, set `V8_JSON_ROOT`, and
use a fresh tag so the original results remain intact:

```sh
V8_JSON_ROOT=/absolute/path/to/v8-standalone \
python3 tools/json_large/run.py matrix \
  --out /absolute/path/to/extracted/json-large-20261026-n9dc_ije \
  --tag replay-branches --phase branches --pairs 4 --samples 9
```

Use `--phase async` or `--phase concurrent` with their own new tags for the
supplementary phases. `report.py` derives the frozen official tags and `plot.py`
plots the measured latency with the min/max range of process medians. These
ranges are not statistical confidence intervals. Plotting dependencies are
recorded in the evidence archive; they are installed only after timing ends.

JSONTestSuite's `y_`/`n_` cases supply additional parsing correctness checks.
Implementation-defined `i_` decisions are recorded and compared across APIs.
Inputs containing invalid UTF-8 bytes are listed separately, since the tested
JavaScript APIs consume already-decoded strings.

For the mixed nested parse follow-up, `compare.py` compares named frozen builds
in the same case block, rotating their execution order. With two builds the
four-round order is AB, BA, BA, AB. It retains every sample and checks the
binary, harness and executor hashes. A three-build run can compare a fix with
both its frozen parent and the original `12.6` build:

```sh
python3 tools/json_large/compare.py \
  --out /absolute/path/to/extracted/fix-experiment \
  --tag replay-parse --labels before metadata v12 --pairs 4 --samples 9
```

`validate.py --out EXPERIMENT --label metadata` checks complete canonical output
SHA-256 and full tree fingerprints against retained references, validates large
async results, and repeats the applicable JSONTestSuite cases. The references,
fixtures and builds belong to the verified experiment archive.
`summarize_fix.py --out EXPERIMENT --candidate metadata --tags metadata-parse-final metadata-stringify-final metadata-async4-final --extra-tags metadata-mixed8` derives the follow-up tables and gate results;
pass any retained repeat tags with `--extra-tags` when reproducing an expanded
matrix. Rejected pilots remain separate from the acceptance matrix.

The delivered matrix also pools `metadata-twitter-followup`, a single fixed
four-round verification of all three Twitter sizes after the initial 20 MB
aggregate crossed the existing 3% gate. Both the first result and the follow-up
are retained. Use `--extra-tags metadata-mixed8 metadata-twitter-followup` to
derive the delivered summary. Mixed and Twitter parse inputs, plus four-task
async record controls, have eight rounds; other parse and stringify inputs have four. Supplementary
UTF-16, Unicode-key and shape controls remain under their separate micro tag.

The marking-worklist follow-up is documented in
[docs/json-parse-async-tail-mac-arm64.md](../../docs/json-parse-async-tail-mac-arm64.md),
with committed results in
[evidence/async-tail-mac-arm64-20261027.json](evidence/async-tail-mac-arm64-20261027.json).
The verified archive retains the original head and uninstrumented markseg256
builds, all diagnostic variants, complete raw samples, and the fixed acceptance plan.
The macOS ARM64 candidate changes the object-marking worklist minimum segment
capacity from 64 to 256. The 20 MB target uses exactly eight pairs; any control
follow-up is pooled with every original sample. Rejected candidates are separate.

Replay into the extracted archive with a fresh tag:

    V8_JSON_ROOT=/absolute/path/to/v8-standalone \
    python3 tools/json_large/compare.py --out /absolute/path/to/extracted/experiment \
      --tag replay-markseg25620 --labels head markseg256 \
      --families records12 records60 --sizes 20 --modes parseAsync \
      --jobs 4 --pairs 8 --samples 9

The archive's summarize-markseg256.py derives the delivered large-input tables.
Pass every extra tag listed in results/markseg256-followup-decision.json with
--extra-tags. Separate micro controls are in results/markseg256-micro-pooled-summary.json.
Pooled batch p90 uses nearest rank over every measured batch, excluding warmups,
and is distinct from the median of process medians.

The delivered change explicitly retains one strict 3% gate exception: CITM 20 MB
parse has an eight-pair median slowdown of 3.009%. The original gate failure and
all samples remain intact. See results/markseg256-disposition.json and the report
for the engineering decision and whole-process RSS tradeoffs.


## Real-corpus parseAsync screening (2026-11-07)

`evidence/parse-async-real-phase1-20261107.json` records the frozen engine,
inputs, per-phase attribution, and all three independently screened candidates.
None met the required 5% target-group improvement, so phase 1 retains the
original engine. The two-pair pilots are exploratory screening, not formal
six-pair acceptance or statistically established regressions. No unfavorable
sample was removed.

For diagnostic builds only:

```sh
python3 tools/json_large/instrument_parse_async.py /absolute/path/to/isolated-checkout \
  --patch /absolute/path/to/diagnostic.patch
```

The tool records input allocation/copy, background parsing and input release,
foreground construction, tape reclamation, memory accounting, task waits, and
whole foreground slices. It reads the existing `LongTaskStats` counters to
include incremental GC; the counters overlap other timings and must not be
added to them. For concurrent documents, GC windows overlap: use the batch's
maximum end counter minus minimum start counter. Completion-time conclusions
must use the matching builds without instrumentation. Foreground p95/max can
be compared in identically instrumented diagnostic builds.

The original 12.6 `JSON.parse` is the correctness and error-message oracle;
the frozen current-branch engine is the performance baseline. Original 12.6
has no `JSON.parseAsync`. The artifact root in the evidence contains immutable
binaries/source patches, all raw attempts, exact commands, and input hashes.
