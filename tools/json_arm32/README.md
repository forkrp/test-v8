# Android ARM32 JSON runtime dispatch

The ARM32 build contains both native-memory JSON implementations in the same
library. The ordinary engine, dependencies and dispatcher use VFPv3-D16 without
NEON. Only `src/json/json-simd-neon.cc` enables integer NEON. It is compiled to a
native object with LTO disabled, so optional instructions cannot be inlined into
the baseline dispatcher by ThinLTO.

V8's process initialization selects the function table after CPU probing and
before isolates can submit JSON worker tasks. Selection requires `base::CPU` reporting
real NEON support and V8 permitting the feature. The table is immutable afterward.
`--no-json-simd`, set before V8 initialization, selects the scalar JSON table even
on a NEON CPU. No option forces NEON on a CPU without it.

The dispatched kernels cover parse/parseAsync string scanning, validated long
string copying (all four Latin1/UTF-16 source/destination combinations), and
stringify/stringifyAsync escape scanning. Short/dense escapes and surrogate
encoding continue through the existing scalar logic. ARM64 and other targets
retain their existing compile-time implementation.

## Build

The repository's `build-android.sh arm` enables the compatible baseline. It is
a Linux-host build script. The compatible runtime sources currently require
**NDK r27d (27.3.13750724)**, Python 3.12+, CMake, Ninja and `patch`. Fetch
the libc++ and libc++abi sources pinned by `DEPS` before building; the ARM32
configuration rebuilds them instead of depending on an unaudited NEON-compiled
NDK `libc++_shared.so`. It selects libc++'s NDK namespace `__ndk1` and ABI version
1, so the switch does not silently adopt Chromium's `__Cr`/ABI 2 object layout.
It retains libc++ configuration ABI tags: the engine disables C++ exceptions,
while the MessagePack codec and embedders can enable them. Removing those tags
can merge a throwing helper with an aborting helper during static linking.
This is still a C++ runtime integration choice: an embedder must use a consistent
runtime, and other native libraries shipped by the app also need a compatible
CPU baseline.

```sh
export NDK_ROOT=/absolute/path/to/android-ndk
./build-android.sh arm
```

For a custom GN build, add these arguments to the existing Android ARM32 args:

```gn
arm_use_neon = false
arm_fpu = "vfpv3-d16"
v8_enable_json_arm32_runtime_dispatch = true
use_custom_libcxx = true
libcxx_use_ndk_abi = true
use_custom_libunwind = true
v8_android_arm32_crt_dir = "//out/android-arm32-crt"
```

Prepare the startup objects and compiler runtime before generating/building:

```sh
python3 tools/json_arm32/prepare_crt.py \
  --ndk "$NDK_ROOT" --api 21 --out out/android-arm32-crt
```

The tested NDK r27d `crtbegin_dynamic.o` uses NEON to initialize memory before
`main`, even when application sources use `-mfpu=vfpv3-d16`. The helper rebuilds
the corresponding pinned upstream startup sources with the baseline instruction
set, including the shared-library startup object and Android ELF note. It keeps
the sources, license headers, commands and hashes in the output directory and
does not modify the installed NDK. Network access is needed only to populate the
verified source cache. The rebuilt startup code retains the NDK constructor and
destructor protocol; it is not a custom replacement for Bionic initialization.

The helper also rebuilds that NDK's compiler-rt builtins with VFPv3-D16. The
prebuilt `__floatdidf`, for example, accesses `d17` and faults on a D16 CPU.
The standalone compiler-rt CMake build is deliberate: r27's Android CMake
toolchain rejects `ANDROID_ARM_NEON=OFF`. `builtins-sources.json` pins the LLVM
revision and applicable NDK patches. Gitiles archive timestamps vary per request,
so verification hashes the sorted file-name/content map, not the tar envelope.
Downloaded upstream license files remain with the cached sources.

`use_custom_libunwind=true` is required as well. The tested prebuilt unwinder
executes NEON during a C++ throw/catch on a non-NEON CPU. The monolith contains
the rebuilt libc++, libc++abi and libunwind; keep `--unwindlib=none` when linking
an embedder to prevent Clang from adding the incompatible NDK unwinder.

`v8_android_arm32_crt_dir` adds the compiler's `-B` startup search path and
`-resource-dir` compiler-runtime overlay to V8 native tools, including
mksnapshot, d8 and cctest. For an embedder executable or shared library outside
GN, compile with `--target=armv7a-linux-androideabi21 -mfpu=vfpv3-d16` and supply
these link options (replace the paths):

```sh
-B /absolute/path/to/android-arm32-crt \
-resource-dir=/absolute/path/to/android-arm32-crt/clang-resource \
-nostdlib++ --unwindlib=none
```

Link the produced `libv8_monolith.a` and the usual Android system libraries.
The static archive does not supply CRT entry points or compiler builtins, so
distribute the runtime overlay alongside it. The overlay's `include` symlink
uses the matching NDK's Clang headers; regenerate it on a different host.

The baseline applies to snapshots as well: mksnapshot calls
`CpuFeatures::Probe(true)`, so compiler-guaranteed features must not include NEON.
V8's ARM feature model groups NEON and VFPv3-D32 together; a CPU without them uses
the engine's existing conservative code-generation configuration.
The ARM32 Maglev backend assumes a fixed D32 register set. `IsMaglevEnabled()`
now requires NEON and VFP32DREGS at runtime. Without those features, the engine
uses its interpreter, Sparkplug and TurboFan tiers; the embedder does not need
to add a flag. This gate was required by a reproduced wrong-result failure in
a program that did not call JSON, and is covered by the default-mode JSON
differential suite's independent data generator.

## Validate

Use the **same** d8/cctest/library files for the following configurations:

1. A CPU with NEON, automatic selection.
2. The same CPU with `--no-json-simd`.
3. A CPU instruction model without NEON, automatic selection.
4. A stricter instruction model with only D0-D15, to catch accidental VFP D32
   dependencies. This is an instruction check, not a claim about that model
   being an Android ARMv7-A device.

Build the focused native runner with `ninja -C out/android test/cctest:cctest_json`.
Its output is `cctest-json`. Native checks include `test-json-async/JsonSimdRuntimeDispatch`,
`JsonScannerGuardPages`, `JsonNumberGuardPages`, and `JsonExternalStringGuardPages`.
The dispatch test prints the selected implementation and hardware capability;
its copy tests place both source and destination immediately before inaccessible
pages. The scanner guard test initializes V8 so it exercises the selected table.

Run the existing four-API differential suite and the pinned upstream suite in
`tools/json_correctness`. The upstream runner can use an executable wrapper for
qemu/adb; record both the wrapper hash and the actual target ELF hash. A wrapper
hash alone is not binary provenance. CPU simulation validates instruction
availability but does not provide device performance measurements.

Also run `test/json/arm32-runtime-tiers.js` with `--allow-natives-syntax
--no-concurrent-recompilation`. Pass `-- neon` for a NEON/D32 CPU and `-- scalar`
for a CPU without those features. This checks the Maglev capability gate and
forces a real TurboFan compilation with an exact arithmetic assertion. The
argument describes CPU capabilities, so a NEON CPU running `--no-json-simd`
still uses the `neon` argument here.

`embedder-smoke.cc` is intended to be compiled with **NDK C++ headers**, linked
with the rebuilt V8 monolith into one `.so`, and loaded by `load-smoke.c`. It
checks C++ exception unwinding, public C++ ABI use, V8 initialization, all four JSON APIs, async task
completion, JIT arithmetic, basic Wasm (when enabled), teardown and `dlclose`.
Use the same shared object with NEON enabled/disabled and forced scalar JSON.

Android API 21 permits the required CPU detection API; it does not by itself
prove a device supports NEON. Physical Android device qualification and app-wide
dependency integration remain separate from CPU-emulated validation.
