# Reproducing resource measurements

The [format](msgpack-resource-format.md), [results](msgpack-resource-results.md)
and checked-in JSON evidence describe candidate r6. Raw samples, prepared files,
rejected candidates, profiles and frozen builds live in `out/msgpack-resources`.
Output directories must be new: runners refuse to overwrite evidence.

Build `d8` and `msgpack_js_benchmark` with `v8_monolithic=true` and
`v8_use_external_startup_data=false`. Freeze only a fully linked, current build:

```sh
ninja -C out/resource-build d8 msgpack_js_benchmark
python3 tools/binary_serialization/freeze_gn.py \
  --build out/resource-build --output out/resource-evidence/frozen-arm64
```

The frozen r6 folders contain exact `args.gn`, compiler/link commands and
SHA-256 dependency closures for each ABI. On this Mac, Android builds used
`configure_resource_build.py` for temporary GN host adaptations and
`gn_mac_android.py` to run matching Android generators on the phone. Adapted
configuration files are restored. Use the matching NDK `libc++_shared.so`;
the runners verify its device hash.

This ARM64 example uses the frozen r6 executable. Change the binary and runtime
together for ARM32 (`arm-linux-androideabi`). Choose a new output root and unique
remote directories for each experiment.

```sh
resource_binary=out/msgpack-resources/frozen-arm64-r6/msgpack_js_benchmark
resource_output=out/resource-repeat-arm64
resource_serial=885841c1
resource_remote=/data/local/tmp/resource-repeat-arm64
resource_runtime=/Users/james/software/android/sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so

python3 tools/binary_serialization/run_js.py \
  --binary "$resource_binary" --output "$resource_output/cold" \
  --adb-serial "$resource_serial" --remote-dir "$resource_remote/cold" \
  --runtime-library "$resource_runtime" --include-resource --cold --rounds 5

python3 tools/binary_serialization/run_js.py \
  --binary "$resource_binary" --output "$resource_output/warm" \
  --adb-serial "$resource_serial" --remote-dir "$resource_remote/warm" \
  --runtime-library "$resource_runtime" --include-resource --rounds 5 \
  --target-ms 200 --min-iterations 5

python3 tools/binary_serialization/controls_resource.py \
  --binary "$resource_binary" \
  --cold-matrix "$resource_output/cold/results.json" \
  --warm-matrix "$resource_output/warm/results.json" \
  --output "$resource_output/resource-controls" \
  --adb-serial "$resource_serial" --remote-dir "$resource_remote/resource-controls" \
  --runtime-library "$resource_runtime" --rounds 60

python3 tools/binary_serialization/controls_js.py \
  --binary "$resource_binary" --matrix "$resource_output/warm/results.json" \
  --output "$resource_output/standard-controls" \
  --adb-serial "$resource_serial" --remote-dir "$resource_remote/standard-controls" \
  --runtime-library "$resource_runtime" --rounds 60

python3 tools/binary_serialization/analyze_resource.py \
  --cold "$resource_output/cold/results.json" \
  --warm "$resource_output/warm/results.json" \
  --output "$resource_output/independent.json"

python3 tools/binary_serialization/run_resource_stages.py \
  --binary "$resource_binary" \
  --cold-matrix "$resource_output/cold/results.json" \
  --warm-matrix "$resource_output/warm/results.json" \
  --output "$resource_output/stages" \
  --adb-serial "$resource_serial" --remote-dir "$resource_remote/stages" \
  --runtime-library "$resource_runtime" --rounds 5
```

For a host cold matrix, omit Android arguments and use its frozen benchmark.
Run `check_resource_api.py` against each frozen `d8`; device tests also require
the serial, remote directory and runtime arguments above. The host DCHECK build
uses `--verify-heap`. ARM32's standard controls additionally used
`--sustained-protocol-encode` to check 5,000 iterations for the closest encoding
fixture. Both Android ABIs are required for acceptance.

`run_js.py` selects all 18 fixed workloads unless a pilot subset is requested.
Cold decoding consumes separately prepared bytes and times exactly one first
API call. Complete result validation follows timing. Warmed measurements use
shared calibrated loop counts, 20 warmups and a full GC before timing; timed
GC is included. Resource controls load prepared bytes and include table
preparation, validation and object/array construction. File I/O and startup
are outside these codec timers.

`analyze_resource.py` independently checks decoded values and numeric bits,
complete sizes, standard fallback identity and cross-ABI wires. It prepares
cumulative shape/string/numeric ablations. Gzip uses level 6 and mtime zero.
`run_resource_stages.py` measures the prepared stages through one unchanged
resource decoder; identical wires share a measurement.

Device work serializes through `/tmp/v8-msgpack-async-device.lock`. The guard
also records and retries samples overlapping foreign V8 device jobs. Keep
excluded samples and rejected experiments in their original directories.

`report_resource.py` requires completed host/ARM64/ARM32 matrices, all four
passing API suites, matching source closures, 60 resource control rounds and
60 close standard control rounds. Audit the four frozen builds and generate
the r6 report with:

```sh
python3 tools/binary_serialization/audit_resource.py \
  --root out/msgpack-resources --label r6

python3 tools/binary_serialization/report_resource.py \
  --root out/msgpack-resources --label r6 \
  --document docs/msgpack-resource-results.md \
  --evidence tools/binary_serialization/evidence/msgpack-resources-r6.json
```
