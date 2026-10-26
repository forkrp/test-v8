# Large JSON measurements on Mac ARM64

The report is [docs/json-large-mac-arm64.md](../../docs/json-large-mac-arm64.md).
The committed summary is [evidence/mac-arm64-20261026.json](evidence/mac-arm64-20261026.json).
The report records the immutable external evidence archive and exact engine commits.

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
