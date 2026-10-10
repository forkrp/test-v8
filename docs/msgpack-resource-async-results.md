# Resource async Android performance results

The following measurements compare public synchronous/asynchronous resource
MessagePack, standard MessagePack and JSON with owned UTF-8 byte transport.
The native engine is frozen at `b0bfe07be049c154f1232c2d5a6afb6843484ca9`.
This is the original implementation baseline. See
[the subsequent paired optimization report](msgpack-resource-async-optimized-results.md)
for the optimized engine, current sync comparisons and retained regressions.
The reusable benchmark and audited measurement procedure are described in
[msgpack-resource-async-benchmarks.md](msgpack-resource-async-benchmarks.md).

Four-job resource encoding improves bulk throughput versus resource sync
by 1.783x on ARM64 and 1.807x on ARM32. One-job resource encoding and most
resource decoding cases have longer total completion time. Bulk submission
time is lower, while tiny calls increase both submission and completion cost.
Resource async encoding also costs more than standard MessagePack/JSON byte
async encoding in the bulk aggregate; wire-size savings are reported separately.

## Scope and validation

Device: OnePlus 6 `ONEPLUS_A6003`, serial `885841c1`, Android 15/API 35.
Both native ARM64 and ARM32 processes run on this same physical ARM64 device.
CPU affinity is `f0` (cores 4–7), governor `schedutil`, NDK 27.3.13750724.
No governor, frequency limit or thermal policy was changed.

Collection: 2026-10-09T11:41:33.386665+00:00 through 2026-10-09T14:05:58.377163+00:00.
All 4,560 fresh processes passed their value and result-count checks.
Every MessagePack encode also passed exact synchronous wire parity.
Nineteen inputs, six codecs, encode/decode, one/four jobs, two ABIs, five passes
and 31 timed samples per process give 141,360 timed batch samples. Each process
has ten warmup batches; tiny samples average 1,000 batches. Startup, file I/O
and input preparation are excluded. Timed GC remains included.

The summary verified the complete Cartesian matrix, frozen executable/runtime
identities, compiler identity/commands, every recorded compile input, benchmark
and runner snapshots, input hashes, and consistent wire/fallback selection.
Pilot and host smoke data are retained separately and excluded from these figures.
These measurements compare APIs within this engine version; they do not measure
an earlier engine revision or total application frame latency.

Recorded `thermal_zone0` (`aoss0-usr`) snapshots span 31.3–61.3 °C.
These are between-process snapshots, not continuous in-process CPU frequency
measurements. Temperature, frequency, memory and all process medians remain
in the raw evidence; the results apply to this device and runtime configuration.
Continuation segments: 0.

## Aggregate comparisons

Each ratio is baseline total time / resource async total time; above 1 favors
resource async. Bulk geometric means give the eighteen non-tiny inputs equal
weight. Submission reduction is the median across those inputs.

| ABI | Operation | Jobs | Vs resource sync | Vs standard async | Vs JSON bytes async | Submission reduction vs resource sync |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| arm64 | encode | 1 | 0.776x | 0.271x | 0.481x | 91.3% |
| arm64 | encode | 4 | 1.783x | 0.260x | 0.512x | 90.4% |
| arm64 | decode | 1 | 0.230x | 0.853x | 1.080x | 85.1% |
| arm64 | decode | 4 | 0.575x | 0.914x | 1.390x | 91.7% |
| arm32 | encode | 1 | 0.809x | 0.218x | 0.505x | 91.2% |
| arm32 | encode | 4 | 1.807x | 0.207x | 0.545x | 88.6% |
| arm32 | decode | 1 | 0.219x | 0.839x | 1.075x | 84.1% |
| arm32 | decode | 4 | 0.521x | 0.881x | 1.397x | 90.9% |

Submission measures API invocation on the calling thread, including graph
capture or input copying. It excludes later foreground JS materialization.
Total measures the batch through completed results. Four-job totals compare
four sequential sync calls with four submitted async calls awaited together;
amortizing those totals by four describes throughput cost, not single-call latency.

## Per-workload comparisons

Times are milliseconds per completed batch, using the median of five process
medians. All five medians and their ranges are retained in the summary JSON.
The final two columns compare resource async with standard/JSON byte async
at four jobs. The tiny object is included here and excluded from bulk aggregates.

### arm64 encode

| Workload | 1 job resource sync | 1 job resource async | Sync/async 1 | 4 jobs resource sync | 4 jobs resource async | Sync/async 4 | Standard async/resource async 4 | JSON bytes async/resource async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 1.2540 | 2.7986 | 0.448x | 5.0355 | 3.8856 | 1.296x | 0.138x | 0.360x |
| records_2500 | 14.4918 | 15.8036 | 0.917x | 58.1880 | 36.6382 | 1.588x | 0.095x | 0.295x |
| records_25000 | 183.5330 | 186.4301 | 0.984x | 738.6800 | 365.8791 | 2.019x | 0.095x | 0.279x |
| short_decimals | 17.3101 | 19.4759 | 0.889x | 69.0182 | 30.7169 | 2.247x | 0.273x | 0.481x |
| integers | 16.3355 | 17.7106 | 0.922x | 65.0922 | 26.3218 | 2.473x | 0.170x | 0.397x |
| text_heavy | 11.8923 | 15.7652 | 0.754x | 47.5673 | 28.8593 | 1.648x | 0.423x | 0.483x |
| unicode | 14.0575 | 18.8824 | 0.744x | 56.4135 | 34.5440 | 1.633x | 0.513x | 0.844x |
| unicode_unique | 26.8502 | 30.9789 | 0.867x | 106.8597 | 53.4048 | 2.001x | 0.372x | 0.646x |
| ascii_unique | 18.9672 | 26.8932 | 0.705x | 76.2136 | 46.3265 | 1.645x | 0.477x | 0.560x |
| latin1_unique | 20.3545 | 28.5038 | 0.714x | 81.7121 | 47.4152 | 1.723x | 0.492x | 0.745x |
| alternating_roles | 21.4094 | 22.8290 | 0.938x | 86.3679 | 46.0572 | 1.875x | 0.125x | 0.412x |
| mixed_late | 11.3297 | 14.3771 | 0.788x | 45.0112 | 25.2589 | 1.782x | 0.373x | 1.171x |
| nested_numeric | 17.3052 | 19.4824 | 0.888x | 68.9141 | 30.9544 | 2.226x | 0.276x | 0.481x |
| varying_shapes | 28.7135 | 30.5056 | 0.941x | 118.1754 | 64.3177 | 1.837x | 0.150x | 0.266x |
| protocol_fixture | 1.9294 | 4.3173 | 0.447x | 7.9033 | 6.2400 | 1.267x | 0.111x | 0.240x |
| cjk_unique | 27.1961 | 36.7507 | 0.740x | 108.7278 | 67.1677 | 1.619x | 0.489x | 0.759x |
| greek_unique | 26.4639 | 36.9035 | 0.717x | 106.0334 | 66.1194 | 1.604x | 0.527x | 0.857x |
| emoji_unique | 20.6651 | 23.7242 | 0.871x | 81.9462 | 38.9978 | 2.101x | 0.489x | 0.984x |
| tiny | 0.0052 | 0.2905 | 0.018x | 0.0188 | 0.1029 | 0.183x | 0.679x | 1.085x |

### arm64 decode

| Workload | 1 job resource sync | 1 job resource async | Sync/async 1 | 4 jobs resource sync | 4 jobs resource async | Sync/async 4 | Standard async/resource async 4 | JSON bytes async/resource async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.1906 | 1.9277 | 0.099x | 0.7295 | 1.7663 | 0.413x | 0.914x | 0.948x |
| records_2500 | 1.7405 | 15.7453 | 0.111x | 6.8805 | 14.8917 | 0.462x | 0.903x | 0.956x |
| records_25000 | 28.3631 | 72.4174 | 0.392x | 111.8715 | 179.7422 | 0.622x | 0.890x | 0.907x |
| short_decimals | 1.3881 | 15.0704 | 0.092x | 5.8685 | 16.1049 | 0.364x | 0.773x | 1.138x |
| integers | 0.8849 | 10.9151 | 0.081x | 3.5871 | 13.2422 | 0.271x | 0.817x | 1.268x |
| text_heavy | 1.3900 | 3.5780 | 0.388x | 12.5452 | 16.9084 | 0.742x | 1.008x | 1.043x |
| unicode | 0.9237 | 7.6535 | 0.121x | 3.5981 | 14.1671 | 0.254x | 0.774x | 4.345x |
| unicode_unique | 7.4150 | 24.2189 | 0.306x | 36.5946 | 38.6295 | 0.947x | 0.798x | 1.644x |
| ascii_unique | 5.0051 | 5.8087 | 0.862x | 17.0912 | 25.0511 | 0.682x | 1.008x | 1.064x |
| latin1_unique | 5.1642 | 16.1835 | 0.319x | 17.4203 | 31.8180 | 0.547x | 0.993x | 1.357x |
| alternating_roles | 4.0603 | 20.7437 | 0.196x | 19.7998 | 32.9875 | 0.600x | 0.909x | 1.013x |
| mixed_late | 3.8492 | 14.0815 | 0.273x | 46.4184 | 59.5022 | 0.780x | 1.029x | 0.988x |
| nested_numeric | 1.3727 | 13.7874 | 0.100x | 5.8614 | 15.7967 | 0.371x | 0.783x | 1.189x |
| varying_shapes | 4.2006 | 11.5839 | 0.363x | 16.2243 | 26.1257 | 0.621x | 1.025x | 1.269x |
| protocol_fixture | 0.2926 | 1.8309 | 0.160x | 1.1449 | 2.3231 | 0.493x | 0.901x | 0.912x |
| cjk_unique | 10.5603 | 22.8154 | 0.463x | 40.5019 | 40.3082 | 1.005x | 0.997x | 3.059x |
| greek_unique | 10.5052 | 23.5404 | 0.446x | 44.0309 | 46.0717 | 0.956x | 1.005x | 2.864x |
| emoji_unique | 6.5499 | 16.8390 | 0.389x | 33.4304 | 27.8324 | 1.201x | 1.012x | 2.425x |
| tiny | 0.0059 | 0.3250 | 0.018x | 0.0210 | 0.1193 | 0.176x | 1.061x | 0.781x |

### arm32 encode

| Workload | 1 job resource sync | 1 job resource async | Sync/async 1 | 4 jobs resource sync | 4 jobs resource async | Sync/async 4 | Standard async/resource async 4 | JSON bytes async/resource async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 1.1648 | 3.1774 | 0.367x | 4.6594 | 3.5406 | 1.316x | 0.148x | 0.523x |
| records_2500 | 12.3996 | 13.7616 | 0.901x | 49.6975 | 33.7745 | 1.471x | 0.141x | 0.458x |
| records_25000 | 149.9319 | 152.9636 | 0.980x | 603.6645 | 413.2043 | 1.461x | 0.104x | 0.419x |
| short_decimals | 42.4672 | 44.4597 | 0.955x | 169.7040 | 54.3685 | 3.121x | 0.149x | 0.426x |
| integers | 37.7339 | 39.2289 | 0.962x | 150.9575 | 47.0465 | 3.209x | 0.080x | 0.246x |
| text_heavy | 14.0793 | 17.0116 | 0.828x | 56.7590 | 34.8695 | 1.628x | 0.315x | 0.538x |
| unicode | 16.3403 | 21.8194 | 0.749x | 65.7876 | 41.6537 | 1.579x | 0.429x | 0.820x |
| unicode_unique | 28.3762 | 33.3489 | 0.851x | 113.4968 | 59.7151 | 1.901x | 0.347x | 0.641x |
| ascii_unique | 24.4792 | 29.1964 | 0.838x | 101.2792 | 60.9104 | 1.663x | 0.304x | 0.515x |
| latin1_unique | 25.7660 | 31.1109 | 0.828x | 102.5257 | 62.5045 | 1.640x | 0.336x | 0.689x |
| alternating_roles | 21.1265 | 22.1226 | 0.955x | 83.9855 | 50.7833 | 1.654x | 0.125x | 0.539x |
| mixed_late | 12.2296 | 14.7572 | 0.829x | 49.1393 | 21.7917 | 2.255x | 0.420x | 2.038x |
| nested_numeric | 42.5226 | 44.3878 | 0.958x | 170.0384 | 54.6530 | 3.111x | 0.150x | 0.425x |
| varying_shapes | 31.1087 | 34.5879 | 0.899x | 123.8434 | 64.7343 | 1.913x | 0.138x | 0.379x |
| protocol_fixture | 2.0664 | 3.7594 | 0.550x | 8.2899 | 6.7792 | 1.223x | 0.100x | 0.274x |
| cjk_unique | 38.1411 | 46.2878 | 0.824x | 152.0698 | 100.7673 | 1.509x | 0.276x | 0.584x |
| greek_unique | 35.5099 | 44.2642 | 0.802x | 141.0292 | 89.7270 | 1.572x | 0.349x | 0.666x |
| emoji_unique | 22.0669 | 26.7815 | 0.824x | 87.5708 | 50.6652 | 1.728x | 0.362x | 0.901x |
| tiny | 0.0056 | 0.2574 | 0.022x | 0.0199 | 0.0982 | 0.202x | 0.749x | 1.147x |

### arm32 decode

| Workload | 1 job resource sync | 1 job resource async | Sync/async 1 | 4 jobs resource sync | 4 jobs resource async | Sync/async 4 | Standard async/resource async 4 | JSON bytes async/resource async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.2068 | 2.4902 | 0.083x | 0.7941 | 2.4688 | 0.322x | 0.834x | 0.843x |
| records_2500 | 1.9558 | 16.1732 | 0.121x | 7.6442 | 19.0757 | 0.401x | 0.888x | 0.913x |
| records_25000 | 30.8161 | 96.2669 | 0.320x | 133.9606 | 293.0883 | 0.457x | 0.895x | 0.921x |
| short_decimals | 2.1105 | 15.5456 | 0.136x | 8.6562 | 24.1526 | 0.358x | 0.751x | 1.312x |
| integers | 1.2525 | 14.7735 | 0.085x | 5.0656 | 21.1610 | 0.239x | 0.751x | 1.259x |
| text_heavy | 1.5454 | 3.9755 | 0.389x | 13.3906 | 17.6422 | 0.759x | 0.940x | 1.186x |
| unicode | 0.9089 | 11.7006 | 0.078x | 3.6208 | 18.6159 | 0.194x | 0.668x | 3.512x |
| unicode_unique | 7.6636 | 26.6123 | 0.288x | 37.1653 | 44.7469 | 0.831x | 0.770x | 1.547x |
| ascii_unique | 4.9200 | 6.6076 | 0.745x | 19.0579 | 26.8707 | 0.709x | 1.004x | 1.276x |
| latin1_unique | 4.9090 | 16.3857 | 0.300x | 18.7955 | 36.0065 | 0.522x | 0.989x | 1.330x |
| alternating_roles | 4.2366 | 23.8910 | 0.177x | 21.0722 | 38.7308 | 0.544x | 0.910x | 1.002x |
| mixed_late | 4.1646 | 14.3532 | 0.290x | 64.8120 | 74.9664 | 0.865x | 0.997x | 1.130x |
| nested_numeric | 2.1337 | 16.1349 | 0.132x | 8.7326 | 24.8075 | 0.352x | 0.745x | 1.266x |
| varying_shapes | 4.9135 | 15.5714 | 0.316x | 18.6455 | 32.7559 | 0.569x | 1.011x | 1.259x |
| protocol_fixture | 0.3119 | 2.7442 | 0.114x | 1.2383 | 2.8074 | 0.441x | 0.834x | 0.892x |
| cjk_unique | 10.3912 | 24.2980 | 0.428x | 41.0207 | 45.6629 | 0.898x | 1.003x | 3.036x |
| greek_unique | 10.8517 | 21.4261 | 0.506x | 44.3144 | 50.9754 | 0.869x | 1.010x | 2.868x |
| emoji_unique | 7.0729 | 20.1304 | 0.351x | 35.0127 | 33.0057 | 1.061x | 0.986x | 2.301x |
| tiny | 0.0041 | 0.3055 | 0.013x | 0.0134 | 0.3270 | 0.041x | 0.995x | 0.301x |

## Wire sizes and exact fallback

| Workload | Standard MessagePack bytes | Resource bytes | Resource raw reduction | Compact resource | JSON UTF-8 bytes |
| --- | ---: | ---: | ---: | --- | ---: |
| records_250 | 23,331 | 12,205 | 47.69% | yes | 31,547 |
| records_2500 | 239,423 | 127,047 | 46.94% | yes | 323,733 |
| records_25000 | 2,426,103 | 1,301,227 | 46.37% | yes | 3,312,808 |
| short_decimals | 938,165 | 242,837 | 74.12% | yes | 612,121 |
| integers | 359,237 | 242,325 | 32.54% | yes | 697,781 |
| text_heavy | 1,138,509 | 1,138,509 | 0.00% | standard fallback | 1,155,281 |
| unicode | 1,313,509 | 59,018 | 95.51% | yes | 1,340,281 |
| unicode_unique | 1,480,189 | 1,457,713 | 1.52% | yes | 1,506,961 |
| ascii_unique | 2,170,479 | 2,170,479 | 0.00% | standard fallback | 2,187,251 |
| latin1_unique | 2,185,479 | 2,185,479 | 0.00% | standard fallback | 2,202,251 |
| alternating_roles | 296,491 | 241,516 | 18.54% | yes | 468,291 |
| mixed_late | 938,171 | 938,171 | 0.00% | standard fallback | 612,133 |
| nested_numeric | 938,174 | 242,846 | 74.12% | yes | 612,133 |
| varying_shapes | 327,288 | 327,288 | 0.00% | standard fallback | 465,697 |
| protocol_fixture | 49,315 | 34,197 | 30.66% | yes | 56,495 |
| cjk_unique | 3,348,509 | 3,348,509 | 0.00% | standard fallback | 3,365,281 |
| greek_unique | 3,048,509 | 3,048,509 | 0.00% | standard fallback | 3,065,281 |
| emoji_unique | 1,648,509 | 1,648,509 | 0.00% | standard fallback | 1,665,281 |
| tiny | 20 | 20 | 0.00% | standard fallback | 34 |

## Slower cases and limitations

Every ratio below one is preserved in `full-summary.json`, including tiny
calls and near-parity cases. Paired-pass ratios and counts slower by at least
five percent are descriptive repeat evidence, not significance tests.

- arm64 encode, 1 job: slower than resource sync on `alternating_roles`, `ascii_unique`, `cjk_unique`, `emoji_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.
- arm64 encode, 4 jobs: no bulk median slower than resource sync.
- arm64 decode, 1 job: slower than resource sync on `alternating_roles`, `ascii_unique`, `cjk_unique`, `emoji_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.
- arm64 decode, 4 jobs: slower than resource sync on `alternating_roles`, `ascii_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.
- arm32 encode, 1 job: slower than resource sync on `alternating_roles`, `ascii_unique`, `cjk_unique`, `emoji_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.
- arm32 encode, 4 jobs: no bulk median slower than resource sync.
- arm32 decode, 1 job: slower than resource sync on `alternating_roles`, `ascii_unique`, `cjk_unique`, `emoji_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.
- arm32 decode, 4 jobs: slower than resource sync on `alternating_roles`, `ascii_unique`, `cjk_unique`, `greek_unique`, `integers`, `latin1_unique`, `mixed_late`, `nested_numeric`, `protocol_fixture`, `records_250`, `records_2500`, `records_25000`, `short_decimals`, `text_heavy`, `unicode`, `unicode_unique`, `varying_shapes`.

The resource async decoder validates and expands compact bytes on a worker,
then parses the standard representation into a native tape and materializes
JS values in foreground slices. This adds stages compared with the direct
synchronous resource decoder. Resource async encoding performs graph capture
synchronously, then standard encoding and compact-candidate analysis on a
worker. Those stages explain structural costs; this timing matrix does not
attribute an exact fraction of elapsed time to each stage.

Reduced submission time must be evaluated separately from total latency and
throughput. Unique strings can add resource analysis work while providing
little wire reduction. Tiny calls expose dispatch and Promise overhead, and
their independent process variation is retained. No universal speedup or
hard foreground scheduling deadline is established by these measurements.

## Evidence and executable identity

The external archive is
`/Users/james/projects/v8/v8-standalone-worktree-docs/2026-10-09/msgpack-resource-async-performance-b0bfe07be`.
It retains `full/results.json`, `full-summary.json`, `comparison.csv`,
inputs, script snapshots, frozen native harnesses/runtimes, build manifests,
logs, host smoke checks, excluded pilot results and the archive hash manifest.

| ABI | Native public-JS harness SHA-256 |
| --- | --- |
| arm64 | `26d57936a656f3d4600f71d6369546fd36a53b553fda9094f55a8254d6568e67` |
| arm32 | `c173bf2a6b633c7027bb5d14bac1273a0b679cf8bc0c9d15afb3c97021030e7f` |
