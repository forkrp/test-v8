# Android ARM32 JSON runtime dispatch qualification

Date: 2026-11-05. Branch: `12.6-jsonparse-async`. Starting commit:
`7ad236771bf1f6706e8b2c46a403193d8a92e681`. The worktree was clean before this
implementation; there were no pre-existing changes to commit.

## Implementation and integration

A single ARM32 library contains scalar and integer-NEON JSON kernels. Ordinary
engine code and rebuilt runtime dependencies use ARMv7 + VFPv3-D16. Only
`src/json/json-simd-neon.cc` enables NEON, with LTO disabled at that object
boundary. Process initialization selects the function table after CPU probing;
selection requires both the hardware capability and V8's permitted feature set.
`--no-json-simd` selects scalar JSON on a capable CPU. It cannot force NEON on an
incapable CPU. The selected table does not change after initialization.

The kernels cover string scanning for parse/parseAsync, long-string copying
across all four Latin1/UTF-16 combinations, and escape scanning for
stringify/stringifyAsync. Existing scalar escape decoding and surrogate handling
remain responsible for those semantics. ARM64 and other configurations retain
their existing implementations.

`build-android.sh arm` now selects the compatible configuration. The validated
runtime toolchain is NDK r27d (`27.3.13750724`). The helper pins upstream CRT and
compiler-rt sources, applies the relevant NDK patches, verifies source content,
and builds a separate runtime overlay without changing the installed NDK.
libc++, libc++abi and libunwind are rebuilt from the repository's pinned sources.
The monolith includes the rebuilt C++ runtime with NDK namespace `__ndk1`, ABI 1,
and libc++ configuration ABI tags enabled.

See [build and embedder instructions](../tools/json_arm32/README.md). A native
embedder must use the baseline compiler options and the supplied CRT/compiler-rt
overlay when linking its final executable or shared library. Other native app
dependencies also need a compatible baseline. The library cannot make separately
compiled NEON-only dependencies compatible.

## Failures caught before acceptance

These are observed failures retained in
`out/json-arm32-dispatch-20261105/` (including failed runs and cores under
`diagnostics/`), rather than hypothetical risks:

1. The tested NDK's `crtbegin_dynamic.o` executes NEON before `main`. A minimal
   baseline-compiled C probe received SIGILL with NEON disabled. Rebuilding the
   matching r27d Bionic CRT and Android ELF note with D16 fixed startup. An early
   attempt mixing Android 14 CRT sources with r27d's constructor protocol was
   rejected after SIGSEGV; it is not the delivered implementation.
2. Prebuilt compiler-rt `__floatdidf` accesses high VFP registers including `d17`.
   A strict D16 instruction profile received SIGILL. All 205 archive members were
   rebuilt with D16, using the exact LLVM revision and relevant NDK patches.
3. The prebuilt NDK unwinder executes NEON in `unw_init_local`. Adding a real C++
   throw/catch to the shared-library embedder smoke exposed SIGILL. Rebuilding
   libunwind with the baseline and linking with `--unwindlib=none` fixed it.
4. Chromium's disabled libc++ ABI tags allow exception-enabled and
   exception-disabled helper definitions to collide. The existing
   `MessagePackResourceAsyncRecordStreaming` exception-injection test aborted
   instead of catching `out_of_range`. This Android runtime configuration now
   preserves libc++ configuration tags. The test is retained in qualification.
5. Gitiles tarballs use request-time member timestamps. Pinning tar envelope
   hashes rejected unchanged upstream source on a second download. The helper
   instead pins a deterministic sorted file-name/content hash. Modified cached
   source and a mismatched NDK revision are both explicitly rejected.

6. The default non-NEON build passed the fixed upstream corpus but failed the
   randomized differential suite after JIT tier-up. A separate generator that
   never calls a JSON API also diverged. `--no-maglev` restored identical output
   on both no-NEON and D16 profiles; `--no-opt` alone did not. ARM Maglev uses a
   fixed D32 register set. `IsMaglevEnabled()` now requires NEON and VFP32DREGS,
   so unsupported CPUs automatically retain the interpreter/Sparkplug/TurboFan
   tiers without entering Maglev. Final qualification uses default flags, with
   no test-only `--no-maglev` workaround. Earlier divergent transcripts and the
   causal controls are retained under `diagnostics/pre-maglev-gate/`.

## Validation environment and boundaries

The build ran on macOS ARM64 using the existing local GN host-generator
adaptation, archived as `gen.py`, `gn-python.py` and `BUILD.gn` in the evidence
directory. Temporary GN host restrictions are restored after generation.
Android mksnapshot itself ran under QEMU with NEON disabled; the target snapshot
was not substituted with a host-architecture snapshot. The production build
script remains a Linux-host script and was not separately run on a Linux build
host in this session.

The native Android ELF binaries run under QEMU user mode 7.2.22, with the official
Android API 21 ARM system image's Bionic linker and system libraries. The system
image identity and extraction record are retained. Four configurations use the
same target binaries:

- `neon`: Cortex-A9, automatic selection.
- `scalar`: Cortex-A9, `--no-json-simd`.
- `no-neon`: Cortex-A9 with NEON disabled, automatic scalar selection.
- `d16`: Cortex-R5F instruction profile without NEON or D16-D31. This is a stricter
  instruction-availability check, not an Android ARMv7-A device qualification.

No physical Android device was connected. These results establish correctness
and executed instruction compatibility in the stated emulator configurations;
they do not measure device performance or constitute production app acceptance.
Full Android-device soak testing and the application's other native dependencies
remain deployment qualification work.

## Final results

All four configurations passed on the final, unchanged binaries. No
`--no-maglev` flag was supplied to the final acceptance matrix.

| Mode | Selected JSON kernel | Maglev | Native tests | Test262 executions | Upstream four-API comparisons | Additional differential comparisons |
|---|---|---|---:|---:|---:|---:|
| NEON automatic | NEON | enabled | 48/48 | 332/332 | 141,318/141,318 | 34,628/34,628 |
| NEON CPU, forced scalar JSON | scalar | enabled | 48/48 | 332/332 | 141,318/141,318 | 34,628/34,628 |
| NEON disabled | scalar | disabled | 48/48 | 332/332 | 141,318/141,318 | 34,628/34,628 |
| D16 instruction profile | scalar | disabled | 48/48 | 332/332 | 141,318/141,318 | 34,628/34,628 |

Each native run includes 35 JSON and 13 MessagePack tests. These cover scanner
and copy guard pages, all UTF-16 code units, external strings without allocation
padding, number boundaries, moving GC, worker/isolate lifetimes, termination,
cancellation and the exception-injection regression. The new native dispatch
check verifies the selected JSON table and the compiler capability gate.

Each upstream run has 36,680 parse and 36,680 parseAsync cases, plus 33,979
stringify and 33,979 stringifyAsync cases. Sources include RapidJSON, yyjson,
simdjson and nlohmann/json test data, pinned by the existing external-suite
lock. Each additional differential run has 7,970 cases per parse API and 9,344
cases per stringify API (completion-marker lines excluded). It checks numeric
bits including negative zero, UTF-16, property order/descriptors, grammar
mutations, reviver/replacer/proxy effects and forced GC. The four-API transcripts
match the frozen original V8 12.6 Mac ARM64 reference; this is an explicit
cross-architecture semantic comparison. Across all four configurations this is
703,784 JSON API comparisons, separately from Test262 and native test counts.

The same final shared object passed all four dlopen/exception/JSON/JIT/basic
Wasm/dispose/dlclose checks. `test/json/arm32-runtime-tiers.js` also passed in all
four configurations: it checks the Maglev capability decision and verifies an
actual TurboFan-optimized arithmetic function. Six no-JSON generator control
runs produced identical transcripts after the fix, including default execution
on both unsupported CPU profiles. Their shared SHA-256 is
`37aa7e3f578eacf768d78866a04910879663cf7287e528ec26405736d854c283`.

The compile audit found 1,688 ARM32 translation units using VFPv3-D16 and exactly
one optional NEON unit. No target translation unit defines `CAN_USE_NEON`.
The baseline compiler-runtime audit covers 205 archive members. The NEON object
has no `.init_array`; d8 and the shared-library smoke have no
`libc++_shared.so` dependency. ThinLTO compiler-driver validation confirms that
the optional kernel remains a native object. The acceptance build itself uses
`use_thin_lto=false`, so this is not full ThinLTO execution qualification.
GN header checks, Python syntax checks, shell syntax checks and `git diff
--check` passed. Ninja reports no work remaining for the qualified targets.

## Artifacts and reproduction

The tracked compact evidence is
[android-api21-arm32-20261105.json](../tools/json_arm32/evidence/android-api21-arm32-20261105.json).
Detailed commands, logs and transcripts are under
`out/json-arm32-dispatch-20261105/`. `frozen/manifest.json` records verified
read-only copies of ten artifacts, including d8, cctest-json, mksnapshot,
libv8_monolith.a, the shared-library smoke and the runtime overlay objects.
The smoke shared library is an integration test, not an application JNI adapter.

Final SHA-256 identities:

- `out/android-arm32-json-dispatch/d8`: `d071a873bc759a4fd2d563b32233333a70cea18d38865df42706f8af01c3db2f`
- `out/android-arm32-json-dispatch/cctest-json`: `558fa99eeed150de55e970523137020b80e7357479486215f3d340b8e908d2e1`
- `out/android-arm32-json-dispatch/obj/libv8_monolith.a`: `68858ee0ae67807528d4b7d4cf847bf5ca6b295a46679d60f1bfd49893edec6f`
- `out/json-arm32-dispatch-20261105/libv8-json-smoke.so`: `c23f268943f910a262687200ca478520e25d1be67dbfa8cd68dc459b9ae8a4bf`

The archived `validate-final.py` runs the native suite and both correctness
runners for each wrapper. `run-{neon,scalar,no-neon,d16}-*` record the exact QEMU
CPU arguments and Android sysroot. `build-final-smoke.sh`, `audit-final.py`,
`shared-smoke-final.py`, `probe-jit.py`, and `runtime-tiers-final.json` retain the
integration and causal-control commands. Evidence output directories are
intentionally not overwritten; select a fresh output directory for a new run.
The validation container is named `v8-arm32-json-validation`; restart the
Colima profile/container before reusing these local wrappers.

Failed and interrupted runs under `diagnostics/pre-abi-tag/` and
`diagnostics/pre-maglev-gate/` are excluded from acceptance. Paths recorded by
those earlier raw reports refer to their original run locations; the archived
logs/transcripts are stored inside those diagnostic directories. No result from
an earlier binary is counted as a final pass.
