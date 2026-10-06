# MessagePack resource format, version 1

`MSGPACK.encodeResource(value)` produces an independently owned Uint8Array.
`MSGPACK.decodeResource(ArrayBuffer | ArrayBufferView)` returns ordinary values.
Both methods are synchronous and use the standard API's supported-value,
strict Unicode, error, buffer ownership and 256 MiB byte/256-container depth
profile. Safe integer BigInts decode as Numbers, as in standard MessagePack.

Existing `encode`/`decode` retain their contract. Ordinary MessagePack peers do
not interpret compact resources. `decodeResource` also accepts standard bytes.
Encoding first captures standard MessagePack, analyzes native data offline,
and generates a compact candidate. It returns the candidate only if its total
byte length is strictly smaller; otherwise it returns the exact standard bytes.

## File and node representation

A compact file starts with five bytes `56 38 4d 52 01` (ASCII V8MR and version 1).
This prefix cannot collide with a valid complete standard MessagePack value:
its first byte is a fixint, which cannot have trailing bytes. Unknown resource
versions fail with SyntaxError. After the prefix, exactly one MessagePack array
contains `[shapeTable, stringTable, root]`; trailing bytes are forbidden.

Shapes are arrays of unique, strict UTF-8 string keys in ordinary JS property
order. Strings are strict UTF-8 value strings. The file owns its tables; it
requires no process-global state, previous file, or external schema. IDs are
zero-based MessagePack unsigned integers. Ordinary scalar, array and string-key
map nodes retain standard tags. Three application extensions apply only within
the compact root:

| Extension type | Payload, itself exactly one MessagePack value |
| --- | --- |
| 0x50 | `[shapeIndex, fieldValues]`, where fieldValues has exactly the shape's field count |
| 0x51 | Unsigned string-table index |
| 0x52 | `[elementCount, blocks]`, where each block is `[numericKind, decimalScale, binaryBytes]` |

Extension headers use the normal shortest fixext/ext8/ext16/ext32 forms. Shape
and string references are bounds checked. A shape's keys are internalized once
per decode; its values may contain any admitted ordinary or extension node.
Dictionary value strings are validated and allocated once per decode. Repeated
objects remain independently mutable; primitive string sharing preserves value
semantics. Indexed keys and `__proto__` use ordinary own-data-property behavior.

## Numeric blocks

All multibyte bodies are big-endian, independent of CPU and alignment.

| Kind | Body |
| --- | --- |
| 0, 1 | uint8, int8 |
| 2, 3 | uint16, int16 |
| 4, 5 | uint32, int32 |
| 6, 7 | uint64, int64 |
| 8, 9 | IEEE float32, IEEE float64 |

A block is nonempty, has at most 256 elements, and its byte length is exactly
an integer multiple of the kind's width. The sum of block element counts must
match elementCount. Signed integers use two's complement. Integer bodies must
be within the JS safe integer range. Their result is binary64(integer) divided
by `10^decimalScale`, for scales 0 through 9. Float kinds require scale zero.

The offline encoder admits integer/scaled candidates only if every reconstructed
binary64 bit pattern equals the input. Float32 likewise requires exact widening.
Negative zero and nonfinite values use float representations, including
float32 when widening reproduces the binary64 bits. Float64 preserves
nonrepresentable values. Each block selects the narrowest admitted width;
equal widths prefer the first integer candidate, then float32, then float64.
Arrays smaller than 16, mixed arrays, and arrays containing integer wire nodes
outside the safe range retain ordinary nodes. Large Numbers can still use exact
floating representations. Safe BigInts already normalize to
Numbers in the standard wire tree and can participate in numeric packing. A
numeric extension is emitted only if its full size beats the ordinary array.

## Cost selection and ownership

Shapes and dictionary strings use deterministic first-occurrence ordering.
Dictionary selection charges definitions and actual reference lengths. Shape
selection charges definitions and conservative reference overhead; every record
then compares its actual extension size with the ordinary map size. File-level
selection includes all tables and the five-byte header. This protects raw size;
gzip size is reported separately and need not improve.

Analysis uses an owned standard wire value and a native MessagePack tree. This
is an offline-generation tradeoff: generation time and whole-process peak RSS
are reported separately from decoding. Runtime decoding uses rooted V8 handles, the existing
checked object-construction paths, and direct numeric-array construction.
Unsupported/malformed extensions fail; no getter, proxy trap, or toJSON hook is
invoked. Input backing storage stays alive throughout synchronous decoding.

Table bookkeeping has an additional ABI-independent 256 MiB accounting limit:
48 bytes per shape plus 16 per key and dictionary string. The decoder checks
this charge before reserving tables. An encoder candidate exceeding this limit
falls back to the validated standard bytes. This is a bound on bookkeeping,
not a claim that process peak memory is limited to 256 MiB.
