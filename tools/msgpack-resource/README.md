# msgpack-resource

A native C++ command-line converter from UTF-8 JSON to bytes accepted by this
V8 fork's `MSGPACK.decodeResource`. It directly calls the engine's
`EncodeMessagePackResource` function. There is no separate codec implementation,
Node.js dependency, JavaScript execution, or external schema.

```sh
msgpack-resource config.json -o config.v8mr
msgpack-resource config.json -o config.v8mr --stats
msgpack-resource config.json -o config.msgpack --standard
cat config.json | msgpack-resource - -o - > config.v8mr
```

The default converter first creates standard MessagePack, analyzes repeated
object shapes and strings, and packs numeric arrays losslessly. It selects the
self-contained `V8MR v1` candidate only if its complete size, including tables
and header, is strictly smaller. Otherwise it writes the exact standard bytes.
Always load default output with `MSGPACK.decodeResource`: a `.v8mr` file can
contain either format. `--standard` explicitly produces ordinary MessagePack.

```js
// In the consuming V8 fork, after your application's file reader returns bytes:
const config = MSGPACK.decodeResource(resourceBytes);
```

The shipped executable embeds V8 and its startup snapshot. End users run the
binary for their OS and CPU; they do not need to install Node.js, V8, Python or
the build toolchain. Building from source requires this V8 checkout and its
normal build dependencies. The converter and decoder do not have to use the
same executable, but the decoder must implement the resource version in use.

## Options

| Option | Behavior |
| --- | --- |
| `-o FILE`, `--output FILE` | Required output; `-` writes binary bytes to stdout |
| `--standard` | Use the standard C++ encoder instead of the resource encoder |
| `--stats` | Emit one JSON statistics object to stderr after successful output |
| `--force` | Allow replacing an existing output file |
| `--` | Treat remaining arguments as filenames |
| `--help`, `-h` | Print usage |
| `--version` | Print tool, resource format and embedded V8 versions |

The input is exactly one JSON value. Use `-` for stdin. Multiple concatenated
JSON values and JSON Lines are not accepted; put multiple records in a JSON array.
Messages and statistics use stderr, so they never contaminate binary stdout.
An existing output is protected by default, including a file created after
argument validation. Input and output cannot refer to the same existing file,
even with `--force`. The output is opened only after parsing and encoding succeed.

An example statistics object:

```json
{"format":"v8mr-v1","inputBytes":10000,"standardBytes":8000,"outputBytes":5000,"bytesSaved":3000}
```

The numbers above illustrate the fields, not a benchmark. `format` is
`v8mr-v1` or `msgpack`. `--stats` computes the standard size with an additional
standard encoding when the output is compact; omit it when generation time or
peak memory matters. Savings compare raw bytes, not gzip or Brotli output.

Exit codes are `0` for success, `2` for usage/output-conflict errors, and `1`
for read, parse, encode or write failures.

## Input and numeric semantics

Input must be strict UTF-8; an initial UTF-8 BOM is accepted. The converter uses
V8's native JSON parser and the codec's existing strict string and value checks.
Unpaired surrogate escapes such as `"\ud800"` are rejected during encoding.
The codec supports at most 256 nested containers and 256 MiB of encoded bytes;
the CLI additionally limits the raw JSON input to 256 MiB. These are size limits,
not peak-memory guarantees. Encoding is an offline operation that materializes
the JSON graph and compact candidate in memory.

JSON numbers follow JavaScript `JSON.parse` semantics. Integer literals outside
the safe integer range may round before encoding; the tool does not promise
arbitrary precision for the original decimal text. Store exact large identifiers
as JSON strings. Negative zero is preserved. Numeric packing only admits
representations that reproduce the codec's numeric values exactly.

This JSON interface does not introduce a representation for BigInt, binary data,
Date, Map, Set, references or cycles. The resource format is application-specific:
ordinary MessagePack decoders cannot read a compact V8MR file. The complete wire
specification is included as `FORMAT.md` in binary distributions and is maintained
in `docs/msgpack-resource-format.md` in the source checkout.

## Build

In a configured checkout of this V8 fork, generate a monolithic release build
with an embedded snapshot and without external ICU data:

```sh
gn gen out/msgpack-resource-cli --args='is_debug=false is_component_build=false v8_monolithic=true v8_use_external_startup_data=false v8_enable_i18n_support=false'
ninja -C out/msgpack-resource-cli msgpack_resource
out/msgpack-resource-cli/msgpack-resource --version
```

Add the usual platform/compiler arguments for your environment. Set `target_cpu`
and `v8_target_cpu` when building for a CPU other than the host default. The GN
target is `msgpack_resource`; the executable is `msgpack-resource` (with `.exe`
on Windows). Existing compatible monolithic build directories can be reused.

Distribute the executable with `README.md`, `FORMAT.md`, the root V8 `LICENSE`,
and the dependency licenses included by `package_cli.py`. Different
OS/CPU combinations need separate builds. The initial distribution is tested on
macOS arm64; Linux, Windows and other CPU binaries need their own build and
verification before delivery. Normal OS C/C++ runtime libraries remain required.

## Verify changes

Build both the CLI and `d8` from the same source and configuration, then run:

```sh
ninja -C out/msgpack-resource-cli msgpack_resource d8
python3 tools/msgpack-resource/test_cli.py \
  --binary out/msgpack-resource-cli/msgpack-resource \
  --d8 out/msgpack-resource-cli/d8 \
  --output out/msgpack-resource-cli/verification.json
```

The test exercises the existing 18-workload corpus and boundary cases, validates
outputs with the independent Python resource reader, and compares every tested
CLI byte with public `MSGPACK.encodeResource` / `MSGPACK.encode` output. It also
decodes through public `MSGPACK.decodeResource` and checks numeric values,
property order, malformed inputs, buffer output, and file handling. Python and
`d8` are verification tools only; the distributed converter requires neither.

The packaging helper checks the verified executable's hash and creates a tar.gz
with documentation, licenses, verification results and SHA-256 manifests:

```sh
python3 tools/msgpack-resource/package_cli.py \
  --binary out/msgpack-resource-cli/msgpack-resource \
  --test-report out/msgpack-resource-cli/verification.json \
  --platform macos-arm64 --destination /path/to/artifacts
```

Each package directory must be new. Choose a destination outside source control
for the binary artifacts. A package records the source commit and hashes of the
CLI/codec sources, including local changes; packaging does not publish it.
