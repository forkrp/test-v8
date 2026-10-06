# MessagePack JavaScript API

This V8 fork provides a synchronous `MSGPACK` global. Its standard methods use
ordinary MessagePack:

```js
const bytes = MSGPACK.encode({id: 42, name: 'example', position: [0.5, -0]});
// bytes is an owned Uint8Array. Its ArrayBuffer is fixed length.
const value = MSGPACK.decode(bytes);
const sameValue = MSGPACK.decode(bytes.buffer);

// Any ArrayBufferView is accepted, using precisely its offset and byte length.
const storage = new Uint8Array(bytes.length + 16);
storage.set(bytes, 8);
const fromView = MSGPACK.decode(new DataView(storage.buffer, 8, bytes.length));
```

`encode(value)` returns independent bytes for one MessagePack value. The finished
native buffer becomes the result's backing store. Output growth and trimming
may copy bytes; transferring the finished buffer to JavaScript does not.
Retained results remain independent of subsequent calls.

On Android ARM32, buffers whose allocation capacity reaches 32 KiB use
independently owned native page mappings to avoid expensive Scudo allocator
release scans during backing-store sweeping. The backing store releases its mapping when its final owner dies;
there is no buffer pool. The exposed byte length is the exact payload length.
Trimming releases whole tail pages without copying the payload, leaving less
than one native page of physical tail space when the release succeeds.
Other architectures use the normal malloc ownership path. Measured heap,
external-byte and process RSS costs are recorded with the performance evidence.

`decode(input)` accepts an `ArrayBuffer` or an `ArrayBufferView`, including a
typed array or `DataView`. It returns ordinary objects and arrays from the
calling realm. Input storage stays alive throughout the synchronous call.
The input must contain exactly one complete value with no trailing bytes.

## Value profile

The wire format uses standard MessagePack tags with this deliberately limited
JavaScript data profile:

| JavaScript input or wire value | Behavior |
| --- | --- |
| `null`, booleans | Preserved |
| `Number` | Preserves the numeric value, including signed zero, NaN, and infinities |
| Exact integer `Number` within the safe integer range | Uses a compact integer tag |
| Exactly representable finite float32 `Number` | Uses float32; widening reproduces the original Number exactly |
| Other noninteger `Number` | Uses float64 |
| `BigInt` between -2^63 and 2^64 - 1 | Uses an integer tag; decoded safe integers become Number, larger integers become BigInt |
| Strings | Strict UTF-8; unpaired UTF-16 surrogates are rejected |
| Dense arrays | Encode their indexed values and decode to ordinary dense arrays |
| Data objects | Encode own enumerable string-keyed data properties and decode to ordinary objects |
| Map tags | Require string keys; duplicate keys use the last value |

The format carries values rather than object identity, prototypes, or property
attributes. Repeated references decode to independent values. `__proto__` is an
ordinary own data property when present in the wire map. Indexed property names
follow JavaScript's normal property ordering.

## Compact resources

For assets generated offline and loaded by this fork, use:

```js
const resourceBytes = MSGPACK.encodeResource(value);
const loadedValue = MSGPACK.decodeResource(resourceBytes);
```

`encodeResource` analyzes a validated standard encoding and constructs a
self-contained compact candidate with shared object shapes, repeated value
strings, and lossless numeric blocks. It returns compact bytes only when the
entire file is strictly smaller than the standard encoding. Otherwise it
returns the standard bytes. Each result owns its storage independently.

`decodeResource` accepts either format and the same buffer/view arguments as
`decode`. It reconstructs the same supported values in the calling realm,
with ordinary property ordering and own `__proto__` data properties. Compact
files require this resource decoder; standard `encode` and `decode` keep their
existing wire format. See [the version 1 format](msgpack-resource-format.md)
for extension layouts, numeric representations, validation, and limits.

Unsupported values include `undefined`, functions, proxies, cyclic graphs,
sparse arrays, enumerable accessors, BigInts outside the 64-bit range, and
special objects such as Date, Map, Set, and typed arrays as encoded values.
The encoder does not call getters, proxy traps, or `toJSON` hooks. Symbol-keyed
and nonenumerable properties are omitted. MessagePack binary and extension
tags, including timestamp extensions, are outside this profile.

## Errors and limits

| Condition | Exception |
| --- | --- |
| Unsupported encoded value or unsupported decode argument | `TypeError` |
| Shared, detached, or out-of-bounds input storage | `TypeError` |
| Malformed, truncated, invalid UTF-8, unsupported wire tags, empty input, or trailing bytes | `SyntaxError` |
| Byte/container/nesting limit exceeded or native allocation failure | `RangeError` |

Encoded output and decoded input are limited to 256 MiB; nesting is limited to
256 containers. Additional V8 string and array allocation limits also apply.
The synchronous methods run on the isolate thread and may trigger GC.

## Performance evidence

The Android optimization goal and fixed workload set are recorded in
[msgpack-js-android-goal.md](msgpack-js-android-goal.md). Final candidate
stage45r2 clears both warmed 1.5x median gates on all 18 workloads on rooted
Android ARM64 and ARM32, including 60-round controls for close cases. The
comparison includes owned native UTF-8 byte transport for JSON. String-only
JSON and first-call timing are reported separately; the first call does not
meet both gates on every workload.

Minified JSON size, gzip size and memory costs are also recorded separately.
Binary encoding does not guarantee a smaller payload for every number
distribution, and gzip can favor JSON even when raw MessagePack is smaller.
