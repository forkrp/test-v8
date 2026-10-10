# Optimized resource async Android results

Public `MSGPACK.encodeResourceAsync` and `decodeResourceAsync` were compared with the original frozen implementation, current synchronous resource methods, standard MessagePack, and JSON with owned UTF-8 bytes.

Baseline engine: `b0bfe07be049c154f1232c2d5a6afb6843484ca9`. Optimized engine: `a856acc85e4645bc49833dcf89e6e064b204201f`.

OnePlus 6 (`885841c1`), Android 15/API 35, native ARM64 and ARM32 processes on the same ARM64 device. NDK 27.3.13750724; CPU mask `f0`, governor `schedutil`. Device policies were unchanged.

Collection: 2026-10-10T01:16:05.090767+00:00 through 2026-10-10T03:01:53.569795+00:00. 5472 fresh processes, three independent shuffled paired passes, thirteen timed samples and ten warmup batches per process. Tiny averages 1,000 batches per sample. Timed GC is included; startup, preparation and file I/O are excluded. Full values, same-version exact wire, sizes/fallback selection and completed-operation counters passed. Cross-version byte parity passed 464 fixtures on the host and both Android ABIs; all 19 workload wire outputs also matched on the host.

Before/after pairs were adjacent and their order shuffled. Summaries use the median of independent process medians. All process medians and slower cells are retained. Geometric means give eighteen bulk workloads equal weight and exclude tiny. These are descriptive device measurements rather than significance tests.

## Aggregate results

Ratios above one favor optimized async for the first column and favor async over current sync for the last column. Shared encoder improvements also accelerate synchronous resources; current sync is therefore the relevant remaining-cost comparison.

| ABI | Operation | Jobs | Old async / new async | Old sync / old async | New sync / new async |
| --- | --- | ---: | ---: | ---: | ---: |
| arm64 | encode | 1 | 1.775x | 0.807x | 0.673x |
| arm64 | encode | 4 | 2.014x | 1.790x | 1.684x |
| arm64 | decode | 1 | 1.649x | 0.264x | 0.438x |
| arm64 | decode | 4 | 1.299x | 0.582x | 0.753x |
| arm32 | encode | 1 | 2.029x | 0.813x | 0.748x |
| arm32 | encode | 4 | 2.168x | 1.811x | 1.785x |
| arm32 | decode | 1 | 1.654x | 0.250x | 0.403x |
| arm32 | decode | 4 | 1.381x | 0.521x | 0.693x |

## arm64 encode

All times are milliseconds per completed batch; four-job sync is four sequential calls and four-job async awaits four submitted calls.

| Workload | Old async 1 | New async 1 | Gain 1 | New sync/async 1 | Old async 4 | New async 4 | Gain 4 | New sync/async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 1.7759 | 1.4802 | 1.200x | 0.289x | 3.8391 | 1.2382 | 3.101x | 1.375x |
| records_2500 | 15.8721 | 7.2471 | 2.190x | 0.635x | 36.5509 | 8.5494 | 4.275x | 2.157x |
| records_25000 | 187.4482 | 62.8732 | 2.981x | 0.939x | 366.2964 | 98.1027 | 3.734x | 2.395x |
| short_decimals | 19.5032 | 5.7896 | 3.369x | 1.574x | 30.2946 | 11.0457 | 2.743x | 3.114x |
| integers | 17.1116 | 5.1302 | 3.335x | 1.597x | 25.3651 | 7.3013 | 3.474x | 4.418x |
| text_heavy | 15.5546 | 11.8104 | 1.317x | 0.444x | 28.2914 | 20.2195 | 1.399x | 0.999x |
| unicode | 18.8949 | 10.9795 | 1.721x | 0.709x | 34.5027 | 16.2196 | 2.127x | 1.792x |
| unicode_unique | 31.2098 | 19.9735 | 1.563x | 0.643x | 53.0698 | 32.4679 | 1.635x | 1.534x |
| ascii_unique | 26.7254 | 18.2893 | 1.461x | 0.451x | 47.3596 | 35.3230 | 1.341x | 1.011x |
| latin1_unique | 28.7557 | 21.3140 | 1.349x | 0.483x | 47.6004 | 37.5352 | 1.268x | 1.107x |
| alternating_roles | 22.4322 | 10.6069 | 2.115x | 0.808x | 46.5846 | 15.3779 | 3.029x | 2.229x |
| mixed_late | 14.3141 | 14.1078 | 1.015x | 0.761x | 25.1902 | 23.9322 | 1.053x | 1.775x |
| nested_numeric | 19.4219 | 11.0486 | 1.758x | 0.822x | 29.9871 | 18.7471 | 1.600x | 1.832x |
| varying_shapes | 30.4207 | 16.3450 | 1.861x | 0.870x | 63.9783 | 28.4222 | 2.251x | 2.011x |
| protocol_fixture | 4.1713 | 1.8096 | 2.305x | 0.404x | 6.4520 | 2.0972 | 3.077x | 1.405x |
| cjk_unique | 37.8573 | 25.4786 | 1.486x | 0.555x | 66.5920 | 50.9996 | 1.306x | 1.120x |
| greek_unique | 36.5647 | 25.0423 | 1.460x | 0.555x | 65.9308 | 51.2715 | 1.286x | 1.093x |
| emoji_unique | 22.6646 | 15.9966 | 1.417x | 0.772x | 38.7583 | 30.2887 | 1.280x | 1.578x |
| tiny | 0.2979 | 0.0069 | 43.071x | 0.778x | 0.1105 | 0.0228 | 4.854x | 0.782x |

## arm64 decode

All times are milliseconds per completed batch; four-job sync is four sequential calls and four-job async awaits four submitted calls.

| Workload | Old async 1 | New async 1 | Gain 1 | New sync/async 1 | Old async 4 | New async 4 | Gain 4 | New sync/async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 1.1140 | 0.4961 | 2.245x | 0.421x | 1.7884 | 1.4731 | 1.214x | 0.528x |
| records_2500 | 13.3792 | 8.1852 | 1.635x | 0.233x | 15.3193 | 10.5994 | 1.445x | 0.716x |
| records_25000 | 78.8466 | 52.3443 | 1.506x | 0.534x | 179.5982 | 134.7086 | 1.333x | 0.877x |
| short_decimals | 12.6197 | 2.4764 | 5.096x | 0.549x | 16.1210 | 7.3103 | 2.205x | 0.707x |
| integers | 12.6121 | 1.7467 | 7.220x | 0.471x | 12.9515 | 4.8480 | 2.672x | 0.767x |
| text_heavy | 3.7591 | 3.8079 | 0.987x | 0.413x | 18.3078 | 18.1656 | 1.008x | 0.696x |
| unicode | 10.0694 | 2.3233 | 4.334x | 0.394x | 14.2596 | 5.3006 | 2.690x | 0.700x |
| unicode_unique | 22.8348 | 18.1359 | 1.259x | 0.428x | 38.7987 | 29.0820 | 1.334x | 1.321x |
| ascii_unique | 6.1083 | 6.5970 | 0.926x | 0.782x | 27.5405 | 26.9114 | 1.023x | 0.691x |
| latin1_unique | 11.1191 | 13.9075 | 0.800x | 0.369x | 31.7272 | 33.2051 | 0.955x | 0.572x |
| alternating_roles | 16.9865 | 12.0767 | 1.407x | 0.369x | 32.6218 | 28.8587 | 1.130x | 0.784x |
| mixed_late | 14.6264 | 15.9369 | 0.918x | 0.615x | 65.3163 | 67.1005 | 0.973x | 0.709x |
| nested_numeric | 13.2902 | 2.1357 | 6.223x | 0.623x | 15.6107 | 7.2254 | 2.161x | 0.705x |
| varying_shapes | 10.8225 | 11.4922 | 0.942x | 0.374x | 29.2724 | 28.4976 | 1.027x | 0.578x |
| protocol_fixture | 1.1173 | 0.8898 | 1.256x | 0.321x | 2.1611 | 2.2785 | 0.948x | 0.513x |
| cjk_unique | 24.6178 | 23.9558 | 1.028x | 0.440x | 42.1543 | 41.1069 | 1.025x | 1.031x |
| greek_unique | 24.2660 | 22.9091 | 1.059x | 0.444x | 46.1910 | 48.2229 | 0.958x | 0.895x |
| emoji_unique | 17.2962 | 17.8262 | 0.970x | 0.395x | 28.2705 | 28.3539 | 0.997x | 1.222x |
| tiny | 0.3127 | 0.0084 | 37.065x | 0.713x | 0.1402 | 0.0268 | 5.223x | 0.775x |

## arm32 encode

All times are milliseconds per completed batch; four-job sync is four sequential calls and four-job async awaits four submitted calls.

| Workload | Old async 1 | New async 1 | Gain 1 | New sync/async 1 | Old async 4 | New async 4 | Gain 4 | New sync/async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 2.7716 | 1.1174 | 2.480x | 0.508x | 3.6152 | 1.7057 | 2.119x | 1.324x |
| records_2500 | 13.9119 | 7.8722 | 1.767x | 0.772x | 33.1931 | 13.1962 | 2.515x | 1.844x |
| records_25000 | 153.7322 | 84.7557 | 1.814x | 0.907x | 409.9131 | 171.9701 | 2.384x | 1.780x |
| short_decimals | 44.4417 | 9.4817 | 4.687x | 1.378x | 54.4794 | 13.3252 | 4.088x | 3.914x |
| integers | 39.1430 | 6.9167 | 5.659x | 1.356x | 47.3665 | 9.4376 | 5.019x | 3.982x |
| text_heavy | 17.4038 | 9.6749 | 1.799x | 0.644x | 34.2873 | 19.0202 | 1.803x | 1.305x |
| unicode | 21.8326 | 10.2786 | 2.124x | 0.845x | 40.2472 | 21.5138 | 1.871x | 1.619x |
| unicode_unique | 33.7161 | 20.1493 | 1.673x | 0.679x | 59.4982 | 32.3680 | 1.838x | 1.701x |
| ascii_unique | 29.0611 | 16.6127 | 1.749x | 0.584x | 60.7823 | 30.9165 | 1.966x | 1.298x |
| latin1_unique | 30.9148 | 17.0459 | 1.814x | 0.664x | 61.7261 | 34.4451 | 1.792x | 1.276x |
| alternating_roles | 22.1000 | 11.8793 | 1.860x | 0.847x | 50.8778 | 18.7461 | 2.714x | 2.154x |
| mixed_late | 14.5211 | 13.9391 | 1.042x | 0.852x | 21.4796 | 19.1583 | 1.121x | 2.427x |
| nested_numeric | 44.3447 | 15.2383 | 2.910x | 0.865x | 54.6227 | 22.5522 | 2.422x | 2.318x |
| varying_shapes | 34.6319 | 19.7089 | 1.757x | 0.858x | 64.5995 | 30.7006 | 2.104x | 2.213x |
| protocol_fixture | 3.7689 | 2.7599 | 1.366x | 0.402x | 7.2029 | 3.7972 | 1.897x | 1.174x |
| cjk_unique | 46.5008 | 24.6267 | 1.888x | 0.633x | 102.8656 | 46.6945 | 2.203x | 1.358x |
| greek_unique | 45.1698 | 24.3541 | 1.855x | 0.623x | 88.2514 | 48.3080 | 1.827x | 1.287x |
| emoji_unique | 26.7859 | 16.0045 | 1.674x | 0.668x | 49.5616 | 29.7263 | 1.667x | 1.525x |
| tiny | 0.2255 | 0.0065 | 34.547x | 0.840x | 0.0998 | 0.0222 | 4.495x | 0.791x |

## arm32 decode

All times are milliseconds per completed batch; four-job sync is four sequential calls and four-job async awaits four submitted calls.

| Workload | Old async 1 | New async 1 | Gain 1 | New sync/async 1 | Old async 4 | New async 4 | Gain 4 | New sync/async 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 2.4262 | 0.9621 | 2.522x | 0.212x | 2.3567 | 1.7869 | 1.319x | 0.462x |
| records_2500 | 13.2400 | 9.4066 | 1.408x | 0.214x | 18.8546 | 16.0349 | 1.176x | 0.504x |
| records_25000 | 101.7865 | 56.0827 | 1.815x | 0.583x | 290.0031 | 169.5331 | 1.711x | 0.790x |
| short_decimals | 14.4591 | 3.0190 | 4.789x | 0.640x | 25.8359 | 10.0873 | 2.561x | 0.740x |
| integers | 11.9542 | 2.0910 | 5.717x | 0.575x | 20.7444 | 7.3330 | 2.829x | 0.628x |
| text_heavy | 4.2116 | 4.3645 | 0.965x | 0.376x | 18.0941 | 15.7092 | 1.152x | 0.822x |
| unicode | 13.3274 | 3.0298 | 4.399x | 0.299x | 17.9792 | 5.7171 | 3.145x | 0.636x |
| unicode_unique | 25.5746 | 19.6097 | 1.304x | 0.406x | 46.1579 | 33.0743 | 1.396x | 1.111x |
| ascii_unique | 7.8123 | 8.3981 | 0.930x | 0.590x | 27.3128 | 29.8745 | 0.914x | 0.605x |
| latin1_unique | 13.8875 | 13.6926 | 1.014x | 0.364x | 36.4317 | 36.7663 | 0.991x | 0.505x |
| alternating_roles | 26.1264 | 13.7970 | 1.894x | 0.321x | 39.4209 | 31.2390 | 1.262x | 0.766x |
| mixed_late | 17.5739 | 17.3775 | 1.011x | 0.707x | 81.5641 | 91.4351 | 0.892x | 0.591x |
| nested_numeric | 15.9968 | 3.1266 | 5.116x | 0.634x | 23.7806 | 10.2574 | 2.318x | 0.759x |
| varying_shapes | 14.6503 | 13.5888 | 1.078x | 0.357x | 35.6073 | 36.6487 | 0.972x | 0.511x |
| protocol_fixture | 1.2546 | 1.3686 | 0.917x | 0.230x | 2.8676 | 2.1205 | 1.352x | 0.580x |
| cjk_unique | 24.4709 | 26.4622 | 0.925x | 0.406x | 46.0777 | 44.7064 | 1.031x | 0.918x |
| greek_unique | 24.9576 | 22.5241 | 1.108x | 0.455x | 51.6819 | 51.9766 | 0.994x | 0.870x |
| emoji_unique | 17.7602 | 19.0034 | 0.935x | 0.382x | 33.8284 | 31.3555 | 1.079x | 1.105x |
| tiny | 0.3087 | 0.0057 | 53.965x | 0.766x | 0.3330 | 0.0178 | 18.668x | 0.621x |

## Changes and limits

Compact decode validates all tables and extensions on the worker, retaining resource references and raw numeric block spans instead of expanding ordinary MessagePack and reparsing it. Shared table strings, keys and map handles remain rooted across cooperative foreground slices. Bounded ASCII records discard redundant scalar tape entries after validation. Numeric arrays retain 128-element foreground batches and the two-millisecond cooperative budget.

Encoding borrows strings from immutable owned wire bytes, uses exact shape/text memo comparisons, writes selected records directly with canonical extension headers, stops numeric scale search at the first exact narrowest candidate, and transfers compact output storage. Root numeric captures reuse the same block packer without a second native DOM. Repeated bounded long strings reuse worker-owned UTF-8 wire spans after full code-unit comparison; numeric and ordinary writers retain separate template instantiations. Captures of at most 256 bytes avoid task hops while preserving Promise microtasks and admission checks.

Native numeric predecoding and bulk foreground copying were tested and rejected: single-job latency worsened on both ABIs despite some four-job throughput gains. Variable-length per-value copies and ARM32 soft conversion calls were investigated with recorded disassembly and fixed-width/narrow experiments. Those versions, failures and pilot matrices remain diagnostic evidence and are excluded from this complete matrix. The superseded incomplete e3486 matrix is likewise excluded.

Remaining sync/async losses are retained above and in comparison.csv. Calling-thread submission alone excludes later foreground materialization and cannot establish end-to-end latency. Results apply to this device/runtime/workloads. Both native ABIs passed 28 final device correctness checks, including moving/incremental GC, realm/snapshot semantics, worker and slice behavior, rejection limits, termination, cancellation and isolate teardown. The shared native resource CLI passed 428 checks, 29 fixtures and 58 public-API byte/value comparisons on the host; this is host CLI evidence. Header dependency checks passed for the codec and native resource CLI.

Full raw results, independent medians, frozen executable/compiler/runtime hashes, recorded compile closures, commands, source snapshots and rejected experiments are kept outside source Git in the evidence archive. Reproduce with [the paired runner procedure](msgpack-resource-async-benchmarks.md#paired-optimization-measurements).

## Controls and slower cells

Of 152 resource async cells including tiny, 21 have a slower optimized median; all are decode cells. All encode cells improved. The largest observed slowdown is ARM64 single-job latin1_unique: 11.1191 to 13.9075 ms (25.1%). Two of its three paired passes were slower by at least five percent. No significance claim is made from three process medians.

Single-job bulk async completion still costs 1.485x/1.336x current sync for ARM64/ARM32 encoding, and 2.282x/2.480x for decoding. Four-job encoding favors async by 1.684x/1.785x; four-job decoding still costs 1.329x/1.443x sync. These remaining costs are part of the result, not removed from the aggregate.

The 32 bulk control aggregates (standard MessagePack and JSON byte sync/async, both ABIs, operations and job counts) span 0.947x to 1.062x before/after. Control variation does not explain or dismiss individual resource regressions. Between-process thermal_zone0 snapshots (230 including endpoints) span 33.9 to 54.0 degrees C; they are sampled every 24 processes, not continuous CPU temperature or frequency recordings.

| ABI | Workload | Jobs | Old async ms | New async ms | Slower | Paired passes at least 5% slower |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| arm64 | text_heavy | 1 | 3.7591 | 3.8079 | 1.3% | 1/3 |
| arm64 | ascii_unique | 1 | 6.1083 | 6.5970 | 8.0% | 2/3 |
| arm64 | latin1_unique | 1 | 11.1191 | 13.9075 | 25.1% | 2/3 |
| arm64 | latin1_unique | 4 | 31.7272 | 33.2051 | 4.7% | 1/3 |
| arm64 | mixed_late | 1 | 14.6264 | 15.9369 | 9.0% | 2/3 |
| arm64 | mixed_late | 4 | 65.3163 | 67.1005 | 2.7% | 1/3 |
| arm64 | varying_shapes | 1 | 10.8225 | 11.4922 | 6.2% | 2/3 |
| arm64 | protocol_fixture | 4 | 2.1611 | 2.2785 | 5.4% | 2/3 |
| arm64 | greek_unique | 4 | 46.1910 | 48.2229 | 4.4% | 2/3 |
| arm64 | emoji_unique | 1 | 17.2962 | 17.8262 | 3.1% | 2/3 |
| arm64 | emoji_unique | 4 | 28.2705 | 28.3539 | 0.3% | 0/3 |
| arm32 | text_heavy | 1 | 4.2116 | 4.3645 | 3.6% | 1/3 |
| arm32 | ascii_unique | 1 | 7.8123 | 8.3981 | 7.5% | 2/3 |
| arm32 | ascii_unique | 4 | 27.3128 | 29.8745 | 9.4% | 2/3 |
| arm32 | latin1_unique | 4 | 36.4317 | 36.7663 | 0.9% | 0/3 |
| arm32 | mixed_late | 4 | 81.5641 | 91.4351 | 12.1% | 2/3 |
| arm32 | varying_shapes | 4 | 35.6073 | 36.6487 | 2.9% | 1/3 |
| arm32 | protocol_fixture | 1 | 1.2546 | 1.3686 | 9.1% | 2/3 |
| arm32 | cjk_unique | 1 | 24.4709 | 26.4622 | 8.1% | 1/3 |
| arm32 | greek_unique | 4 | 51.6819 | 51.9766 | 0.6% | 0/3 |
| arm32 | emoji_unique | 1 | 17.7602 | 19.0034 | 7.0% | 2/3 |

## Evidence archive

Recorded local archive: `/Users/james/projects/v8/v8-standalone-worktree-docs/2026-10-10/msgpack-resource-async-optimization-a856acc85`.

`evidence/paired-accepted/results.json` is the complete accepted matrix; `evidence/paired-accepted-summary/summary.json` and `comparison.csv` retain all comparisons and individual process medians. `evidence/acceptance-review.json` records the slower cells, paired pass ratios, control aggregates and thermal range. `archive-manifest.json` verifies saved file sizes and SHA-256 hashes.

Engine source is frozen at a856acc85. The later report commit changes documentation only. The complete source-closure audit preceded local integration so the original baseline checkout could be verified in place. Frozen build manifests retain their collection-time absolute paths; use the saved source revisions in separate checkouts to reproduce.
