// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Flags: --allow-natives-syntax --expose-gc

assertEquals(1, MSGPACK.encode.length);
assertEquals(1, MSGPACK.decode.length);
assertEquals('[object MSGPACK]', Object.prototype.toString.call(MSGPACK));
assertFalse(Object.keys(globalThis).includes('MSGPACK'));
const jsonKeysBefore = Reflect.ownKeys(JSON);
Object.freeze(JSON);

function roundTrip(value) {
  const bytes = MSGPACK.encode(value);
  assertTrue(bytes instanceof Uint8Array);
  assertFalse(bytes.buffer.resizable);
  assertEquals(value, MSGPACK.decode(bytes));
  assertEquals(value, MSGPACK.decode(bytes.buffer));
  const padded = new Uint8Array(bytes.length + 17);
  padded.fill(0xc1);
  padded.set(bytes, 7);
  assertEquals(value, MSGPACK.decode(padded.subarray(7, 7 + bytes.length)));
  assertEquals(value, MSGPACK.decode(new DataView(padded.buffer, 7, bytes.length)));
  return bytes;
}

for (const value of [null, true, false, 0, -0, 127, 128, 255, 256, 65535,
                     65536, -32, -33, -128, -129, -32768, -32769,
                     2147483647, 2147483648, -2147483649,
                     Number.MAX_SAFE_INTEGER, Number.MIN_SAFE_INTEGER,
                     0.1, Number.MIN_VALUE, Number.MAX_VALUE, Infinity,
                     -Infinity, NaN, '', 'ascii', 'café', '中文', 'Ελληνικά',
                     '🌏😀', [], {}, [1, null, false, {a: 'nested'}],
                     {'0': 'zero', '10': 'ten', a: 'named'},
                     9223372036854775807n, -9223372036854775808n,
                     18446744073709551615n]) roundTrip(value);
assertTrue(Object.is(-0, MSGPACK.decode(MSGPACK.encode(-0))));
assertEquals(3, MSGPACK.decode(MSGPACK.encode(3n)));
assertEquals([0xca, 0x3f, 0, 0, 0], Array.from(MSGPACK.encode(0.5)));
assertEquals([0xca, 0x80, 0, 0, 0], Array.from(MSGPACK.encode(-0)));
assertEquals(0xcb, MSGPACK.encode(0.1)[0]);
assertEquals(0xca, MSGPACK.encode(Math.fround(1 / 3))[0]);
assertEquals(0xcb, MSGPACK.encode(Math.fround(1 / 3) + Number.EPSILON)[0]);

// Standard string header boundaries and embedded NUL bytes must agree with
// independent MessagePack readers, including direct and nested ASCII paths.
for (const [length, header] of [
    [0, [0xa0]], [31, [0xbf]], [32, [0xd9, 32]], [255, [0xd9, 255]],
    [256, [0xda, 1, 0]], [65535, [0xda, 255, 255]],
    [65536, [0xdb, 0, 1, 0, 0]]]) {
  const text = ('x\0'.repeat(Math.ceil(length / 2))).slice(0, length);
  const bytes = roundTrip(text);
  assertEquals(header, Array.from(bytes.subarray(0, header.length)));
  assertEquals(length + header.length, bytes.length);
  roundTrip({text});
  roundTrip([text, text]);
}

// Direct fixed-container tags and the standard larger-header fallback agree
// at each transition. These dense arrays also exercise mapped output growth.
for (const [length, header] of [
    [0, [0x90]], [15, [0x9f]], [16, [0xdc, 0, 16]],
    [65535, [0xdc, 255, 255]], [65536, [0xdd, 0, 1, 0, 0]]]) {
  const values = [];
  for (let i = 0; i < length; ++i) values.push(0);
  const bytes = roundTrip(values);
  assertEquals(header, Array.from(bytes.subarray(0, header.length)));
  assertEquals(length + header.length, bytes.length);
}
for (const [length, header] of [[0, [0x80]], [15, [0x8f]], [16, [0xde, 0, 16]]]) {
  const value = {};
  for (let i = 0; i < length; ++i) value['key' + i] = i;
  const bytes = roundTrip(value);
  assertEquals(header, Array.from(bytes.subarray(0, header.length)));
}

// Every short-string allocation size must remain valid through later child
// allocations and moving GC. Unicode at each tail uses the strict fallback.
const shortStrings = [];
for (let length = 2; length <= 64; ++length) {
  const ascii = ('ab\0'.repeat(Math.ceil(length / 3))).slice(0, length);
  shortStrings.push(ascii, ascii.slice(0, -1) + 'é');
}
const decodedShortStrings = MSGPACK.decode(MSGPACK.encode(shortStrings));
gc();
assertEquals(shortStrings, decodedShortStrings);
roundTrip({values: shortStrings});

// Reusing a decoded schema must allocate fresh numeric field boxes, including
// representation changes and indexed/duplicate-key fallback construction.
const numericRecords = [];
for (const value of [1, 0.1, -0, NaN, Infinity, 2147483648,
                     Number.MIN_SAFE_INTEGER, 'changed', null, 3]) {
  const expected = {id: value, nested: {number: value}};
  const restored = MSGPACK.decode(MSGPACK.encode(expected));
  assertEquals(expected, restored);
  numericRecords.push([expected, restored]);
  roundTrip({...expected, '0': value});
  gc();
}
for (const [expected, restored] of numericRecords) assertEquals(expected, restored);

// Repeated layouts may be filled while child values allocate. Every partial
// object must remain valid during GC, and a late key/type mismatch must rewind
// the whole object without exposing or reusing its partially initialized boxes.
const streamingRecords = [];
for (let i = 0; i < 300; ++i) {
  const value = {id: i, number: i / 8,
                 child: {value: 'child-' + i}, tail: i / 10};
  if (i % 31 === 30) value.number = 'changed-' + i;
  if (i % 37 === 36) {
    delete value.tail;
    value.other = -0;
  }
  streamingRecords.push(value);
}
const streamingBytes = MSGPACK.encode(streamingRecords);
const streamingRestored = MSGPACK.decode(streamingBytes);
assertEquals(streamingRecords, streamingRestored);
gc();
assertEquals(streamingRecords, streamingRestored);
assertEquals(streamingRecords, MSGPACK.decode(streamingBytes));
const repeatedPair = [0x82, 0xa1, 120, 1, 0xa1, 121, 2];
for (const tail of [
    [0x82, 0xa1, 120, 3, 0xa1, 122, 4],
    [0x82, 0xa1, 120, 3, 0xa1, 120, 4],
    [0x82, 0xa1, 120, 0xc0, 0xa1, 121, 0xa1, 122]]) {
  const wire = new Uint8Array([0x94, ...repeatedPair, ...repeatedPair,
                             ...repeatedPair, ...tail]);
  assertEquals([{x: 1, y: 2}, {x: 1, y: 2}, {x: 1, y: 2},
                tail[5] === 122 ? {x: 3, z: 4} :
                tail[5] === 120 ? {x: 4} : {x: null, y: 'z'}],
               MSGPACK.decode(wire));
}
for (const tail of [[0x82, 0xa1, 120, 3, 0xa1, 121],
                    [0x82, 0xa1, 120, 3, 0xc0, 4]]) {
  assertThrows(() => MSGPACK.decode(new Uint8Array([
      0x94, ...repeatedPair, ...repeatedPair, ...repeatedPair, ...tail])), SyntaxError);
}
const numberKey = [0xa1, 120];
assertEquals({x: -0}, MSGPACK.decode(new Uint8Array([
    0x82, ...numberKey, 1, ...numberKey, 0xcb, 0x80, 0, 0, 0, 0, 0, 0, 0])));

// Cached raw traversal must rewind a partly written subtree when a late
// unflattened string needs the rooted path. Empty double arrays are valid.
const emptyDoubles = [0.1];
emptyDoubles.pop();
roundTrip(emptyDoubles);
const rope = 'prefix'.repeat(200) + '🌏' + 'suffix'.repeat(200);
roundTrip([{id: 1, text: 'ordinary'}, {id: 2, text: rope}, {id: 3, text: 'tail'}]);

// Large numeric arrays exercise both architecture-specific Smi ranges,
// signed zero, float64 boundaries, and late transitions to mixed/BigInt data.
for (const length of [1, 2, 3, 7, 8, 9, 63, 64, 65, 257, 4096]) {
  for (const make of [i => i % 65536, i => i - 100,
                     i => i / 8, i => i % 3 ? -0 : 0.1,
                     i => i % 2 ? 2147483648 : -2147483649,
                     i => i % 2 ? NaN : Infinity]) {
    const numbers = Array.from({length}, (_, i) => make(i));
    roundTrip(numbers);
    roundTrip({numbers});
    roundTrip(numbers.concat(['tail', null]));
    roundTrip(numbers.concat([18446744073709551615n]));
  }
}

// A size hint is capacity feedback only. Equal root shapes/lengths can carry
// very different byte sizes, and retained outputs must remain independent.
const changingSizes = [];
for (const length of [1, 100000, 3, 1000, 2]) {
  const value = [{text: '🌏x'.repeat(length)}];
  changingSizes.push([value, roundTrip(value)]);
}
gc();
for (const [value, bytes] of changingSizes) assertEquals(value, MSGPACK.decode(bytes));

const integerView = new Uint32Array(1);
new Uint8Array(integerView.buffer).set(MSGPACK.encode({a: 1}));
assertEquals({a: 1}, MSGPACK.decode(integerView));
assertEquals([1, 2], MSGPACK.decode(new Uint8Array([0x92, 1, 2])));

const protoBytes = new Uint8Array([0x81, 0xa9, ...Array.from('__proto__', c => c.charCodeAt(0)), 7]);
const proto = MSGPACK.decode(protoBytes);
assertTrue(Object.is(Object.prototype, Object.getPrototypeOf(proto)));
assertTrue(Object.hasOwn(proto, '__proto__'));
assertEquals(7, proto.__proto__);
assertEquals({x: 2}, MSGPACK.decode(new Uint8Array([0x82, 0xa1, 120, 1, 0xa1, 120, 2])));
for (const header of [[0xa1], [0xd9, 1], [0xda, 0, 1], [0xdb, 0, 0, 0, 1]]) {
  assertEquals({'1': 2}, MSGPACK.decode(new Uint8Array([0x81, ...header, 49, 2])));
}
roundTrip({'01': 1, '1中文': 2, '4294967295': 3, '': 4});

// Cached field layout must survive representation generalization and avoid
// reusing maps reconfigured by JS (attributes, accessors, prototypes, dicts).
const layoutValue = {x: 0.1, child: {v: 3}, label: 'data'};
const layoutBytes = MSGPACK.encode(layoutValue);
for (let i = 0; i < 4; ++i) assertEquals(layoutValue, MSGPACK.decode(layoutBytes));
let layoutGetterCalls = 0;
for (const change of [
    o => { o.x = 'changed'; o.child = []; },
    o => Object.freeze(o),
    o => Object.defineProperty(o, 'x', {get() { ++layoutGetterCalls; return 99; }}),
    o => { delete o.label; },
    o => Object.setPrototypeOf(o, null),
    o => Object.create(o),
    o => { for (let i = 0; i < 300; ++i) o['added' + i] = i; }]) {
  const previous = MSGPACK.decode(layoutBytes);
  change(previous);
  gc();
  const next = MSGPACK.decode(layoutBytes);
  assertEquals(layoutValue, next);
  assertTrue(Object.is(Object.prototype, Object.getPrototypeOf(next)));
  const descriptor = Object.getOwnPropertyDescriptor(next, 'x');
  assertTrue(descriptor.writable && descriptor.enumerable && descriptor.configurable);
}
assertEquals(0, layoutGetterCalls);

// Exact short-span comparisons must include overlapping tails and the middle.
for (let size = 1; size <= 32; ++size) {
  const first = 'a'.repeat(size);
  const last = first.slice(0, -1) + 'b';
  const middle = first.slice(0, size >> 1) + 'c' + first.slice((size >> 1) + 1);
  roundTrip([{[first]: first}, {[last]: last}, {[middle]: middle}, {[first]: first}]);
}

let calls = 0;
const accessor = {get x() { ++calls; return 1; }};
const proxy = new Proxy({}, {ownKeys() { ++calls; return []; }});
for (const value of [undefined, Symbol('x'), () => 1, accessor, proxy,
                     [1, , 3], new Date(), new Map(), new Set(), /x/,
                     new Uint8Array(3), 18446744073709551616n,
                     -9223372036854775809n, '\ud800', '\udc00',
                     {nested: '\ud800'}]) {
  assertThrows(() => MSGPACK.encode(value), TypeError);
}
assertEquals(0, calls);
const cycle = {}; cycle.self = cycle;
assertThrows(() => MSGPACK.encode(cycle), TypeError);
const inheritedHook = Object.create({toJSON() { ++calls; return 1; }});
inheritedHook.x = 2;
roundTrip({x: 2});
assertEquals({x: 2}, MSGPACK.decode(MSGPACK.encode(inheritedHook)));
assertEquals(0, calls);

for (const bytes of [[], [0xc1], [1, 2], [0x91], [0xd9, 3, 65],
                     [0xa2, 0xc0, 0xaf], [0xa3, 0xed, 0xa0, 0x80],
                     [0xa4, 0xf4, 0x90, 0x80, 0x80], [0x81, 1, 2],
                     [0xc4, 1, 0], [0xd4, 0, 0]]) {
  assertThrows(() => MSGPACK.decode(new Uint8Array(bytes)), SyntaxError);
}
for (const input of [undefined, null, 1, 'bytes', [1], {},
                     new SharedArrayBuffer(8),
                     new Uint8Array(new SharedArrayBuffer(8))]) {
  assertThrows(() => MSGPACK.decode(input), TypeError);
}
const detached = new ArrayBuffer(8);
const detachedView = new Uint8Array(detached);
%ArrayBufferDetach(detached);
assertThrows(() => MSGPACK.decode(detached), TypeError);
assertThrows(() => MSGPACK.decode(detachedView), TypeError);

const rab = new ArrayBuffer(16, {maxByteLength: 32});
new Uint8Array(rab).set(MSGPACK.encode({a: 1}), 4);
assertEquals({a: 1}, MSGPACK.decode(new Uint8Array(rab, 4, 4)));
assertEquals({a: 1}, MSGPACK.decode(new DataView(rab, 4, 4)));
const outOfBounds = new Uint8Array(rab, 12, 4);
const outOfBoundsData = new DataView(rab, 12, 4);
rab.resize(8);
assertThrows(() => MSGPACK.decode(outOfBounds), TypeError);
assertThrows(() => MSGPACK.decode(outOfBoundsData), TypeError);

let deep = 1;
for (let i = 0; i < 256; ++i) deep = [deep];
roundTrip(deep);
assertThrows(() => MSGPACK.encode([deep]), RangeError);
assertThrows(() => MSGPACK.decode(new Uint8Array([...Array(257).fill(0x91), 1])), RangeError);

// Rooted metadata and native input/output must survive repeated moving GC.
const retained = [];
for (let pass = 0; pass < 30; ++pass) {
  const values = Array.from({length: 250}, (_, i) =>
      ({id: i, name: 'item-' + i, child: {x: i / 8}, tags: ['a', 'b']}));
  const bytes = roundTrip(values);
  retained.push(bytes);
  gc();
  assertEquals(values, MSGPACK.decode(bytes));
}
for (const bytes of retained) assertEquals(250, MSGPACK.decode(bytes).length);
assertEquals(jsonKeysBefore, Reflect.ownKeys(JSON));
const realm = Realm.create();
const foreignDecode = Realm.eval(realm, 'MSGPACK.decode');
const foreignObjectPrototype = Realm.eval(realm, 'Object.prototype');
const foreign = foreignDecode(MSGPACK.encode({a: 1}));
assertTrue(Object.is(foreignObjectPrototype, Object.getPrototypeOf(foreign)));
assertTrue(Object.is(Object.prototype, Object.getPrototypeOf(MSGPACK.decode(MSGPACK.encode({a: 1})))));
Realm.dispose(realm);
