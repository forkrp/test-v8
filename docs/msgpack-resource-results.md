# MessagePack resource results

Candidate r6 on rooted OnePlus 6 (`885841c1`), Android 15, CPU mask `f0`.
Acceptance: **passed**.
The fixed 18 fixtures establish these corpus results; no production asset corpus was supplied.

Every complete resource file is no larger than its standard MessagePack equivalent. The encoder returns the exact standard bytes when the full compact candidate is not smaller. Raw size and gzip size are separate; gzip is measured for comparison and is not part of the codec.

## Complete file sizes

| Workload | Minified JSON | Standard MessagePack | Resource | Raw reduction vs MessagePack | Gzip JSON | Gzip MessagePack | Gzip resource |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 31,547 | 23,331 | 12,205 | 47.69% | 3,792 | 3,639 | 3,654 |
| records_2500 | 323,733 | 239,423 | 127,047 | 46.94% | 35,725 | 32,757 | 33,624 |
| records_25000 | 3,312,808 | 2,426,103 | 1,301,227 | 46.37% | 350,682 | 316,329 | 325,560 |
| short_decimals | 612,121 | 938,165 | 242,837 | 74.12% | 5,935 | 12,907 | 5,454 |
| integers | 697,781 | 359,237 | 242,325 | 32.54% | 255,750 | 254,180 | 217,965 |
| text_heavy | 1,155,281 | 1,138,509 | 1,138,509 | 0.00% | 21,176 | 20,830 | 20,830 |
| unicode | 1,340,281 | 1,313,509 | 59,018 | 95.51% | 20,818 | 20,808 | 9,155 |
| unicode_unique | 1,506,961 | 1,480,189 | 1,457,713 | 1.52% | 31,680 | 33,016 | 32,833 |
| ascii_unique | 2,187,251 | 2,170,479 | 2,170,479 | 0.00% | 27,359 | 27,605 | 27,605 |
| latin1_unique | 2,202,251 | 2,185,479 | 2,185,479 | 0.00% | 28,100 | 27,537 | 27,537 |
| alternating_roles | 468,291 | 296,491 | 241,516 | 18.54% | 54,176 | 53,656 | 52,757 |
| mixed_late | 612,133 | 938,171 | 938,171 | 0.00% | 5,958 | 12,919 | 12,919 |
| nested_numeric | 612,133 | 938,174 | 242,846 | 74.12% | 5,963 | 12,926 | 5,465 |
| varying_shapes | 465,697 | 327,288 | 327,288 | 0.00% | 68,114 | 62,961 | 62,961 |
| protocol_fixture | 56,495 | 49,315 | 34,197 | 30.66% | 10,684 | 11,684 | 12,652 |
| cjk_unique | 3,365,281 | 3,348,509 | 3,348,509 | 0.00% | 25,626 | 26,090 | 26,090 |
| greek_unique | 3,065,281 | 3,048,509 | 3,048,509 | 0.00% | 25,673 | 26,884 | 26,884 |
| emoji_unique | 1,665,281 | 1,648,509 | 1,648,509 | 0.00% | 18,573 | 19,548 | 19,548 |
| **Total** | 23,680,607 | 22,869,390 | 18,766,384 | **17.94%** | 995,784 | 976,276 | 923,493 |

## Android decoding and standard API gates

Five shuffled fresh-process matrix rounds, shared loop counts, 20 warmed calls and a full GC before timing. Timed GC is included. First-call decoding uses separately prepared bytes and exactly one API call. Table preparation, UTF-8 validation, object construction and numeric materialization are inside decoding. Preparation, startup and file I/O are excluded. JSON gates include owned UTF-8 byte transport. All times are milliseconds per operation. Ratios above 1 favor MessagePack/resource.

### ARM64

| Workload | First standard | First resource | Warm standard | Warm resource | Warm standard/resource | Standard decode/JSON speedup | Standard encode/JSON speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.544 | 0.555 | 0.186 | 0.173 | 1.071 | 2.033 | 2.141 |
| records_2500 | 2.257 | 2.236 | 2.065 | 1.871 | 1.104 | 2.030 | 3.054 |
| records_25000 | 34.974 | 32.830 | 31.093 | 27.842 | 1.117 | 1.598 | 2.951 |
| short_decimals | 2.740 | 1.706 | 2.751 | 1.703 | 1.615 | 4.476 | 5.713 |
| integers | 3.234 | 1.224 | 2.635 | 1.120 | 2.352 | 1.949 | 4.113 |
| text_heavy | 3.461 | 3.424 | 1.825 | 1.867 | 0.977 | 2.012 | 3.226 |
| unicode | 1.739 | 1.232 | 1.234 | 0.887 | 1.390 | 11.972 | 1.690 |
| unicode_unique | 9.770 | 9.537 | 8.130 | 7.955 | 1.022 | 1.958 | 1.645 |
| ascii_unique | 6.112 | 6.404 | 3.682 | 3.694 | 0.997 | 1.874 | 2.979 |
| latin1_unique | 6.279 | 6.204 | 3.788 | 3.848 | 0.984 | 2.975 | 2.569 |
| alternating_roles | 4.689 | 4.649 | 4.562 | 4.423 | 1.032 | 1.630 | 2.963 |
| mixed_late | 14.220 | 13.315 | 7.373 | 7.438 | 0.991 | 2.063 | 4.779 |
| nested_numeric | 2.883 | 1.664 | 2.798 | 1.700 | 1.646 | 4.471 | 5.801 |
| varying_shapes | 26.096 | 25.555 | 5.768 | 5.763 | 1.001 | 1.563 | 1.928 |
| protocol_fixture | 0.729 | 0.732 | 0.274 | 0.272 | 1.009 | 1.571 | 1.520 |
| cjk_unique | 12.345 | 12.074 | 9.321 | 9.290 | 1.003 | 3.386 | 2.815 |
| greek_unique | 11.263 | 11.292 | 9.627 | 9.594 | 1.003 | 3.607 | 2.709 |
| emoji_unique | 8.500 | 8.578 | 7.339 | 7.378 | 0.995 | 2.344 | 1.725 |

60 shuffled paired control rounds cover every first-call and warmed resource workload, including standard fallbacks. Repeatable regressions of at least 5%: `[]`. The close standard gates use 60 rounds and both half-run medians. Standard 1.5x acceptance: `True`.

| Workload | First standard/resource control | Warm standard/resource control | Warm first half | Warm second half |
| --- | ---: | ---: | ---: | ---: |
| records_250 | 1.009 | 1.071 | 1.071 | 1.071 |
| records_2500 | 1.091 | 1.097 | 1.098 | 1.096 |
| records_25000 | 1.060 | 1.079 | 1.086 | 1.077 |
| short_decimals | 1.670 | 1.583 | 1.588 | 1.584 |
| integers | 2.344 | 2.288 | 2.289 | 2.287 |
| text_heavy | 0.997 | 0.996 | 1.002 | 0.992 |
| unicode | 1.381 | 1.388 | 1.385 | 1.392 |
| unicode_unique | 1.011 | 1.009 | 1.012 | 1.007 |
| ascii_unique | 0.995 | 1.006 | 1.007 | 1.004 |
| latin1_unique | 1.004 | 0.998 | 1.008 | 0.997 |
| alternating_roles | 1.031 | 1.021 | 1.025 | 1.017 |
| mixed_late | 0.993 | 0.997 | 0.994 | 0.996 |
| nested_numeric | 1.639 | 1.562 | 1.592 | 1.549 |
| varying_shapes | 0.989 | 0.997 | 0.996 | 0.999 |
| protocol_fixture | 1.018 | 1.004 | 1.005 | 1.003 |
| cjk_unique | 0.996 | 1.000 | 1.002 | 1.001 |
| greek_unique | 1.003 | 1.004 | 1.001 | 1.006 |
| emoji_unique | 1.001 | 1.000 | 0.997 | 1.000 |

| Extended standard gate | Iterations | JSON/standard | First half | Second half | Pass |
| --- | ---: | ---: | ---: | ---: | --- |
| records_25000 decode | 7 | 1.639 | 1.637 | 1.640 | True |
| unicode encode | 48 | 1.681 | 1.682 | 1.669 | True |
| unicode_unique encode | 32 | 1.675 | 1.681 | 1.653 | True |
| alternating_roles decode | 38 | 1.627 | 1.631 | 1.629 | True |
| varying_shapes decode | 17 | 1.563 | 1.564 | 1.559 | True |
| protocol_fixture decode | 540 | 1.572 | 1.574 | 1.571 | True |
| protocol_fixture encode | 1000 | 1.518 | 1.524 | 1.514 | True |
| emoji_unique encode | 37 | 1.716 | 1.729 | 1.711 | True |

### ARM32

| Workload | First standard | First resource | Warm standard | Warm resource | Warm standard/resource | Standard decode/JSON speedup | Standard encode/JSON speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.587 | 0.503 | 0.210 | 0.196 | 1.071 | 2.095 | 2.563 |
| records_2500 | 2.465 | 2.316 | 2.422 | 2.247 | 1.078 | 1.999 | 3.238 |
| records_25000 | 40.347 | 38.810 | 34.523 | 32.983 | 1.047 | 1.655 | 3.162 |
| short_decimals | 2.834 | 2.405 | 2.959 | 2.402 | 1.232 | 4.446 | 5.385 |
| integers | 3.241 | 1.475 | 2.948 | 1.383 | 2.132 | 1.978 | 4.734 |
| text_heavy | 3.157 | 3.288 | 2.046 | 2.054 | 0.996 | 2.061 | 5.088 |
| unicode | 1.749 | 1.175 | 1.416 | 0.888 | 1.594 | 10.742 | 1.913 |
| unicode_unique | 9.931 | 9.510 | 8.328 | 8.166 | 1.020 | 1.973 | 1.943 |
| ascii_unique | 5.688 | 5.737 | 3.820 | 3.822 | 0.999 | 2.050 | 4.116 |
| latin1_unique | 6.007 | 5.921 | 3.760 | 3.812 | 0.986 | 3.007 | 3.495 |
| alternating_roles | 4.812 | 4.592 | 4.906 | 4.685 | 1.047 | 1.574 | 3.187 |
| mixed_late | 17.790 | 17.933 | 9.727 | 10.131 | 0.960 | 1.870 | 5.423 |
| nested_numeric | 2.809 | 2.289 | 2.839 | 2.419 | 1.174 | 4.616 | 5.349 |
| varying_shapes | 30.455 | 29.970 | 6.838 | 6.689 | 1.022 | 1.601 | 1.955 |
| protocol_fixture | 0.776 | 0.738 | 0.318 | 0.288 | 1.103 | 1.638 | 2.399 |
| cjk_unique | 12.102 | 12.374 | 9.520 | 9.483 | 1.004 | 3.520 | 4.105 |
| greek_unique | 11.539 | 11.635 | 9.937 | 9.971 | 0.997 | 3.512 | 3.718 |
| emoji_unique | 8.945 | 9.042 | 7.664 | 7.710 | 0.994 | 2.363 | 2.739 |

60 shuffled paired control rounds cover every first-call and warmed resource workload, including standard fallbacks. Repeatable regressions of at least 5%: `[]`. The close standard gates use 60 rounds and both half-run medians. Standard 1.5x acceptance: `True`.

| Workload | First standard/resource control | Warm standard/resource control | Warm first half | Warm second half |
| --- | ---: | ---: | ---: | ---: |
| records_250 | 1.021 | 1.073 | 1.073 | 1.073 |
| records_2500 | 1.091 | 1.088 | 1.091 | 1.090 |
| records_25000 | 1.053 | 1.058 | 1.055 | 1.063 |
| short_decimals | 1.175 | 1.170 | 1.181 | 1.165 |
| integers | 2.095 | 2.101 | 2.099 | 2.098 |
| text_heavy | 1.002 | 1.004 | 1.004 | 1.001 |
| unicode | 1.479 | 1.612 | 1.616 | 1.605 |
| unicode_unique | 1.027 | 1.015 | 1.016 | 1.015 |
| ascii_unique | 0.997 | 0.996 | 0.992 | 1.000 |
| latin1_unique | 1.006 | 0.995 | 0.992 | 1.001 |
| alternating_roles | 1.035 | 1.046 | 1.045 | 1.049 |
| mixed_late | 1.004 | 0.992 | 0.991 | 0.997 |
| nested_numeric | 1.193 | 1.173 | 1.180 | 1.169 |
| varying_shapes | 1.004 | 1.000 | 1.004 | 0.996 |
| protocol_fixture | 1.048 | 1.106 | 1.103 | 1.108 |
| cjk_unique | 1.000 | 0.996 | 0.996 | 0.997 |
| greek_unique | 1.005 | 0.998 | 0.999 | 0.999 |
| emoji_unique | 1.004 | 1.000 | 0.999 | 0.998 |

| Extended standard gate | Iterations | JSON/standard | First half | Second half | Pass |
| --- | ---: | ---: | ---: | ---: | --- |
| records_25000 decode | 5 | 1.677 | 1.680 | 1.675 | True |
| alternating_roles decode | 34 | 1.585 | 1.586 | 1.582 | True |
| varying_shapes decode | 15 | 1.596 | 1.600 | 1.587 | True |
| protocol_fixture decode | 495 | 1.629 | 1.625 | 1.630 | True |
| protocol_fixture encode | 5000 | 2.178 | 2.176 | 2.182 | True |


## Offline generation

Additional offline encoding work is intentional. Times below are cold encodeResource calls. RSS is the maximum whole-process high-water mark over the measured fresh processes, including source parsing and result validation; it is not encoder-exclusive memory. CPU time is also retained in the JSON evidence. Host measurements did not reserve exclusive host CPU access.

| Workload | Host ms | Host peak MiB | ARM64 ms | ARM64 peak MiB | ARM32 ms | ARM32 peak MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 0.45 | 13.38 | 1.77 | 13.14 | 1.59 | 9.72 |
| records_2500 | 3.28 | 18.61 | 16.22 | 17.42 | 13.71 | 12.66 |
| records_25000 | 32.78 | 76.20 | 188.48 | 59.00 | 147.29 | 41.38 |
| short_decimals | 3.72 | 28.53 | 18.48 | 21.56 | 43.28 | 17.20 |
| integers | 3.63 | 25.78 | 16.61 | 19.34 | 38.92 | 15.04 |
| text_heavy | 1.88 | 32.03 | 14.89 | 28.08 | 15.75 | 23.43 |
| unicode | 3.04 | 28.59 | 15.29 | 25.32 | 18.56 | 20.71 |
| unicode_unique | 4.47 | 39.23 | 27.21 | 33.04 | 28.64 | 28.71 |
| ascii_unique | 3.10 | 50.50 | 25.14 | 41.00 | 26.69 | 34.69 |
| latin1_unique | 3.39 | 48.89 | 25.96 | 41.00 | 28.85 | 34.67 |
| alternating_roles | 4.47 | 23.05 | 23.25 | 20.23 | 22.67 | 14.81 |
| mixed_late | 2.38 | 32.50 | 14.04 | 26.00 | 14.67 | 21.33 |
| nested_numeric | 3.59 | 26.81 | 18.10 | 21.29 | 43.38 | 17.48 |
| varying_shapes | 5.88 | 25.33 | 32.40 | 22.36 | 31.95 | 16.89 |
| protocol_fixture | 0.78 | 14.12 | 2.81 | 13.55 | 2.84 | 10.30 |
| cjk_unique | 4.78 | 59.33 | 37.16 | 47.44 | 44.33 | 40.55 |
| greek_unique | 4.28 | 60.66 | 35.85 | 51.21 | 40.69 | 43.57 |
| emoji_unique | 3.08 | 41.30 | 21.73 | 33.80 | 24.18 | 28.31 |

## Cumulative feature ablations

Independent transformations of final files remove numeric blocks, then dictionary references. Each stage retains its required full tables and applies file-level standard fallback. One unchanged native decoder reads the prepared wires; identical wire stages share a measurement. These are cumulative wire controls, not timings from differently compiled decoders or separate offline encoder builds.

| Workload | Standard bytes | Shapes bytes | Shapes + strings bytes | Full bytes | ARM64 warm: shapes / strings / full ms | ARM32 warm: shapes / strings / full ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records_250 | 23,331 | 15,626 | 12,205 | 12,205 | 0.176 / 0.173 / 0.173 | 0.197 / 0.198 / 0.198 |
| records_2500 | 239,423 | 161,968 | 127,047 | 127,047 | 1.973 / 1.894 / 1.894 | 2.265 / 2.224 / 2.224 |
| records_25000 | 2,426,103 | 1,651,148 | 1,301,227 | 1,301,227 | 30.674 / 28.178 / 28.178 | 32.321 / 32.243 / 32.243 |
| short_decimals | 938,165 | 938,165 | 938,165 | 242,837 | 2.903 / 2.903 / 1.849 | 2.966 / 2.966 / 2.494 |
| integers | 359,237 | 359,237 | 359,237 | 242,325 | 2.824 / 2.824 / 1.199 | 2.986 / 2.986 / 1.426 |
| text_heavy | 1,138,509 | 1,138,509 | 1,138,509 | 1,138,509 | 1.909 / 1.909 / 1.909 | 2.090 / 2.090 / 2.090 |
| unicode | 1,313,509 | 1,291,033 | 59,018 | 59,018 | 1.128 / 0.892 / 0.892 | 1.271 / 0.890 / 0.890 |
| unicode_unique | 1,480,189 | 1,457,713 | 1,457,713 | 1,457,713 | 8.073 / 8.073 / 8.073 | 8.117 / 8.117 / 8.117 |
| ascii_unique | 2,170,479 | 2,170,479 | 2,170,479 | 2,170,479 | 3.697 / 3.697 / 3.697 | 3.797 / 3.797 / 3.797 |
| latin1_unique | 2,185,479 | 2,185,479 | 2,185,479 | 2,185,479 | 3.825 / 3.825 / 3.825 | 3.837 / 3.837 / 3.837 |
| alternating_roles | 296,491 | 241,516 | 241,516 | 241,516 | 4.441 / 4.441 / 4.441 | 4.684 / 4.684 / 4.684 |
| mixed_late | 938,171 | 938,171 | 938,171 | 938,171 | 7.268 / 7.268 / 7.268 | 9.901 / 9.901 / 9.901 |
| nested_numeric | 938,174 | 938,174 | 938,174 | 242,846 | 2.927 / 2.927 / 1.720 | 2.911 / 2.911 / 2.481 |
| varying_shapes | 327,288 | 327,288 | 327,288 | 327,288 | 6.217 / 6.217 / 6.217 | 7.492 / 7.492 / 7.492 |
| protocol_fixture | 49,315 | 41,195 | 34,197 | 34,197 | 0.284 / 0.272 / 0.272 | 0.305 / 0.288 / 0.288 |
| cjk_unique | 3,348,509 | 3,348,509 | 3,348,509 | 3,348,509 | 9.346 / 9.346 / 9.346 | 9.585 / 9.585 / 9.585 |
| greek_unique | 3,048,509 | 3,048,509 | 3,048,509 | 3,048,509 | 9.703 / 9.703 / 9.703 | 9.977 / 9.977 / 9.977 |
| emoji_unique | 1,648,509 | 1,648,509 | 1,648,509 | 1,648,509 | 7.396 / 7.396 / 7.396 | 7.696 / 7.696 / 7.696 |

| Workload | ARM64 first: shapes / strings / full ms | ARM32 first: shapes / strings / full ms |
| --- | ---: | ---: |
| records_250 | 0.529 / 0.545 / 0.545 | 0.519 / 0.504 / 0.504 |
| records_2500 | 2.127 / 2.106 / 2.106 | 2.285 / 2.360 / 2.360 |
| records_25000 | 33.811 / 33.384 / 33.384 | 39.054 / 38.334 / 38.334 |
| short_decimals | 2.665 / 2.665 / 1.640 | 2.825 / 2.825 / 2.328 |
| integers | 2.796 / 2.796 / 1.204 | 3.198 / 3.198 / 1.581 |
| text_heavy | 3.334 / 3.334 / 3.334 | 2.987 / 2.987 / 2.987 |
| unicode | 1.587 / 1.262 / 1.262 | 1.627 / 1.157 / 1.157 |
| unicode_unique | 9.610 / 9.610 / 9.610 | 9.497 / 9.497 / 9.497 |
| ascii_unique | 6.134 / 6.134 / 6.134 | 5.685 / 5.685 / 5.685 |
| latin1_unique | 6.097 / 6.097 / 6.097 | 5.835 / 5.835 / 5.835 |
| alternating_roles | 4.560 / 4.560 / 4.560 | 4.714 / 4.714 / 4.714 |
| mixed_late | 13.710 / 13.710 / 13.710 | 17.862 / 17.862 / 17.862 |
| nested_numeric | 2.696 / 2.696 / 1.631 | 2.812 / 2.812 / 2.399 |
| varying_shapes | 26.642 / 26.642 / 26.642 | 30.612 / 30.612 / 30.612 |
| protocol_fixture | 0.754 / 0.749 / 0.749 | 0.787 / 0.752 / 0.752 |
| cjk_unique | 11.911 / 11.911 / 11.911 | 11.906 / 11.906 / 11.906 |
| greek_unique | 11.163 / 11.163 / 11.163 | 11.181 / 11.181 / 11.181 |
| emoji_unique | 8.439 / 8.439 / 8.439 | 8.673 / 8.673 / 8.673 |

## Correctness, rejected experiments and identity

Standard and resource suites pass on host and both device ABIs under normal, moving and incremental GC. The host DCHECK build passes with actual heap verification enabled. An independent Python reader checks all 18 workloads for equivalent values and numeric bits and verifies cross-ABI wire identity. Coverage includes numeric boundaries, signed zero/nonfinite values, Unicode, table/shape collisions, indices, malformed/truncated blocks, unsupported versions, 256-container nesting, buffer views, ownership, realm prototypes, representation generalization, and 2,000 seeded malformed resources.

r1-r4 were rejected because the existing standard Unicode encoding gates fell below 1.5x. Separate resource packer instantiations restored standard inlining, but separate builtin entry points and 64-byte function alignment did not clear the gates. Paired frozen-baseline controls and simpleperf profiles retain the slowdown. r5 removes the second scalar pass for bounded mixed UTF-16 on ARM64 without changing standard wire bytes or Unicode validation. r5 was superseded because it excluded exact float32 nonfinite blocks; r6 applies the bit-equality selection to those values too. The original 18 corpus files remain byte-identical. Failed build/test invocations and all rejected frozen binaries, source snapshots and raw samples remain under `out/msgpack-resources`.

| Artifact | SHA-256 |
| --- | --- |
| frozen-host-r6/d8 | `b6f4c8e87657777f147c4701d2a3349f5d09382678dd737aba101ec0d00e1347` |
| frozen-host-r6/msgpack_js_benchmark | `cf057143c117918bcfc6f13dab10b5839928af9f4c2e8269c29d4b99b821b66e` |
| frozen-host-r6/build-manifest.json | `868c4cb795de65db2d3626d7f107f067da8b334437181f50e04f72cff6a20373` |
| frozen-host-verify-r6/d8 | `4351351c8483fe7a5b178778824e648f6aa5163b8529ca967edd09a9c297c22b` |
| frozen-host-verify-r6/msgpack_js_benchmark | `94139a1980024ce457b542a8659dc1ded5b41e8252f71c1eb1e7daf23302a503` |
| frozen-host-verify-r6/build-manifest.json | `36ed3eda2bcd0a5f9d19b36a173be9dd0b17c52678ba727e0f63b4a863b831fa` |
| frozen-arm64-r6/d8 | `076e2db900feb8ead6714b0982bccb58fb79583a0942615107122355dee4f9a1` |
| frozen-arm64-r6/msgpack_js_benchmark | `8845094253222326853980a61cd41a8734f9bfc8a8b479d865d18edd0532d8ef` |
| frozen-arm64-r6/exe.unstripped/msgpack_js_benchmark | `9d9d199a03ef2bd11abe47bfad1296db0e4b202563a72bd38b1487000ddc3ffe` |
| frozen-arm64-r6/build-manifest.json | `079deb5659118b514f50e61bbaa9ad5896cf6bfc47e7b0da23b9f4c21846fd45` |
| frozen-arm32-r6/d8 | `d1d847626fdbbf30965f788cf20f91a1409609e557016ba6bc2a9483b31e0e59` |
| frozen-arm32-r6/msgpack_js_benchmark | `fd6532d1fac89a23a03d1d0a7ccd4ab2422c8c9febc2a42ee44d73ab182c43ee` |
| frozen-arm32-r6/exe.unstripped/msgpack_js_benchmark | `a18104ea58ea206762088806766d54ec89706b5525c2bbe63d3f88671c525c68` |
| frozen-arm32-r6/build-manifest.json | `33cab8e4bfb4a2cd7a3b1e2f2f0b0f0721e2d80eb66c1d9a7bac9aa01abeaf70` |

Full configuration, compile-input closures, runtime/runner/fixture hashes, sample ordering, CPU/RSS/heap data and control half-runs are retained in the frozen manifests and [machine-readable evidence](../tools/binary_serialization/evidence/msgpack-resources-r6.json). The codec uses no external compression and no lossy numeric conversion. Compiled engine size is outside acceptance.
