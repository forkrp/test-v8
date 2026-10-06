// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Flags: --allow-natives-syntax --expose-gc
(() => {
assertEquals(1, MSGPACK.encodeResource.length);
assertEquals(1, MSGPACK.decodeResource.length);
for (const length of [63, 64, 65, 255, 256, 4095, 4096, 4097]) {
  let text = ('a中🌏é Ελληνικά').repeat(length).slice(0, length);
  if (text.charCodeAt(text.length - 1) >= 0xd800 &&
      text.charCodeAt(text.length - 1) <= 0xdbff) text = text.slice(0, -1) + 'a';
  assertEquals(text, MSGPACK.decode(MSGPACK.encode(text)));
  assertEquals(text, MSGPACK.decodeResource(MSGPACK.encodeResource(text)));
  for (const invalid of [text + '\ud800', '\udc00' + text]) {
    assertThrows(() => MSGPACK.encode(invalid), TypeError);
    assertThrows(() => MSGPACK.encodeResource(invalid), TypeError);
  }
}
function resourceRoundTrip(value) {
  const expected = MSGPACK.decode(MSGPACK.encode(value));
  const bytes = MSGPACK.encodeResource(value);
  assertTrue(bytes instanceof Uint8Array);
  assertTrue(bytes.length <= MSGPACK.encode(value).length);
  assertEquals(expected, MSGPACK.decodeResource(bytes));
  assertEquals(expected, MSGPACK.decodeResource(bytes.buffer));
  const padded = new Uint8Array(bytes.length + 17);
  padded.set(bytes, 7);
  assertEquals(expected, MSGPACK.decodeResource(new DataView(padded.buffer, 7, bytes.length)));
  assertEquals(expected, MSGPACK.decodeResource(padded.subarray(7, 7 + bytes.length)));
  return bytes;
}
function compact(bytes) { return bytes.length >= 5 && String.fromCharCode(...bytes.subarray(0, 4)) === 'V8MR'; }
for (const v of [null, true, false, -0, NaN, Infinity, -Infinity, 0.1, '', '🌏',
                 0n, -9223372036854775808n, 18446744073709551615n,
                 [], {}, [1, 'x', null], {x: [1, 2]}]) {
  const expected = MSGPACK.decode(MSGPACK.encode(v));
  const bytes = MSGPACK.encodeResource(v);
  assertTrue(bytes.length <= MSGPACK.encode(v).length);
  assertEquals(expected, MSGPACK.decodeResource(bytes));
}
const records = Array.from({length: 1000}, (_, i) => ({
  identifier: i, title: 'Repeated text 🌏'.repeat(8), active: i % 2 === 0,
  child: {horizontal: i / 10, vertical: i / 8}, tags: ['common', 'common']
}));
const wire = resourceRoundTrip(records);
assertTrue(compact(wire));
assertThrows(() => MSGPACK.decode(wire), SyntaxError);
for (let i = 0; i < wire.length; ++i) {
  // A one-byte prefix is also a valid standard fixint (86).
  if (i === 1) { assertEquals(86, MSGPACK.decodeResource(wire.subarray(0, i))); continue; }
  if (i > 150 && i % 103 !== 0) continue;
  assertThrows(() => MSGPACK.decodeResource(wire.subarray(0, i)), SyntaxError);
}
const decoded = MSGPACK.decodeResource(wire);
assertEquals(Object.keys(records[0]), Object.keys(decoded[0]));
decoded[0].child.horizontal = 77;
assertEquals(0.1, decoded[1].child.horizontal);
gc();
assertEquals(records, MSGPACK.decodeResource(wire));
for (const length of [0, 1, 15, 16, 255, 256, 257, 513, 4096]) {
  for (const make of [i => i, i => i - 300, i => i / 10, i => i / 8,
       i => (i % 7 ? -0 : 0), i => i % 3 ? NaN : Infinity,
       i => i % 2 ? Number.MAX_SAFE_INTEGER : Number.MIN_SAFE_INTEGER,
       i => i % 2 ? 2147483648 : -2147483649,
       i => i % 2 ? 0.1 : 1 / 3, i => i % 2 ? Number.MIN_VALUE : Number.MAX_VALUE]) {
    resourceRoundTrip(Array.from({length}, (_, i) => make(i)));
  }
}
resourceRoundTrip({numbers: Array.from({length: 1000}, (_, i) => (i - 500) / 10)});
// Exact float32 widening also applies to nonfinite values. A 16-element
// float64 block does not beat the complete standard file, but float32 does.
for (const number of [NaN, -NaN, Infinity, -Infinity]) {
  assertTrue(compact(resourceRoundTrip(Array(16).fill(number))));
}
resourceRoundTrip(Array.from({length: 300}, (_, i) => i / 10).concat(['tail', null, 2n]));
const indexed = Array.from({length: 100}, (_, i) => {
  const v = {'10': i, '2': i + 1, long_property_name: 'repeated'.repeat(30)};
  Object.defineProperty(v, '__proto__', {value: i, enumerable: true});
  return v;
});
const indexedResult = MSGPACK.decodeResource(resourceRoundTrip(indexed));
assertEquals(Object.keys(indexed[0]), Object.keys(indexedResult[0]));
assertTrue(Object.hasOwn(indexedResult[0], '__proto__'));
assertEquals(Object.prototype, Object.getPrototypeOf(indexedResult[0]));
assertEquals({value: 0, enumerable: true, configurable: true, writable: true},
             Object.getOwnPropertyDescriptor(indexedResult[0], '__proto__'));
const retained = [];
for (let i = 0; i < 20; ++i) {
  retained.push([records, MSGPACK.encodeResource(records)]);
  resourceRoundTrip(Array.from({length: 500 + i}, (_, j) => j / 10));
  gc();
}
for (const [v, b] of retained) assertEquals(v, MSGPACK.decodeResource(b));
// Resource envelopes/extensions assembled independently of the encoder.
const magic = [86, 56, 77, 82, 1];
function ext(type, body) {
  const header = body.length < 256 ? [0xc7, body.length] :
      [0xc8, body.length >>> 8, body.length & 255];
  return [...header, type, ...body];
}
function resource(shapes, strings, root) {
  return new Uint8Array([...magic, 0x93, ...MSGPACK.encode(shapes),
                         ...MSGPACK.encode(strings), ...root]);
}
const str = (n) => ext(0x51, Array.from(MSGPACK.encode(n)));
const similarStrings = ['x', 'y', '\0', '🌏'].map(
    s => 'same prefix'.repeat(50) + s + 'same suffix'.repeat(50));
resourceRoundTrip(Array.from({length: 200}, (_, i) => similarStrings[i % 4]));
const similarShapes = Array.from({length: 100}, (_, i) => i % 2 ?
    {long_name_ab: i, c: i + 1} : {long_name_a: i, bc: i + 1});
resourceRoundTrip(similarShapes);
assertEquals(['one', 'one'], MSGPACK.decodeResource(resource([], ['one'], [0x92, ...str(0), ...str(0)])));
assertEquals({long_property_name: 4}, MSGPACK.decodeResource(resource(
    [['long_property_name']], [], ext(0x50, [0x92, 0, 0x91, 4]))));
for (const [shapes, strings, root] of [
  [[], [], str(0)], [[], ['x'], str(1)], [[], ['x'], ext(0x51, [0, 0])],
  [[], ['x'], str(-1)], [[], ['x'], str(0.5)],
  [[], [], ext(0x53, [0])], [[], [], ext(0x50, [0x92, 0, 0x90])],
  [[['x']], [], ext(0x50, [0x92, 0, 0x90])],
  [[['x', 'x']], [], [0]], [[['x', 1]], [], [0]], [[], [1], [0]],
  [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 10, 0, 0xc4, 1, 1])],
  [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 0, 10, 0xc4, 1, 1])],
  [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 0, 0, 0xc4, 0])],
  [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 2, 0, 0xc4, 1, 1])],
  [[], [], ext(0x52, [0x92, 2, 0x91, 0x93, 0, 0, 0xc4, 1, 1])]]) {
  assertThrows(() => MSGPACK.decodeResource(resource(shapes, strings, root)), SyntaxError);
}
const unknown = new Uint8Array(wire); unknown[4] = 2;
assertThrows(() => MSGPACK.decodeResource(unknown), SyntaxError);
const trailing = new Uint8Array(wire.length + 1); trailing.set(wire);
assertThrows(() => MSGPACK.decodeResource(trailing), SyntaxError);
assertThrows(() => MSGPACK.decodeResource(resource([], [], ext(0x52,
    [0x92, 0xcd, 1, 1, 0x91, 0x93, 0, 0, 0xc5, 1, 1,
     ...Array(257).fill(1)]))), SyntaxError);
assertThrows(() => MSGPACK.decodeResource(new Uint8Array([...magic, 0x93, 0x90, 0x91, 0xa1, 0xff, 0])), SyntaxError);
for (const v of [undefined, () => {}, [, 1], new Date(), new Map(), new Set(),
                 new Uint8Array(2), 1n << 64n, '\ud800'])
  assertThrows(() => MSGPACK.encodeResource(v), TypeError);
const cycle = {}; cycle.self = cycle;
assertThrows(() => MSGPACK.encodeResource(cycle), TypeError);
let calls = 0;
const getter = {get x() { ++calls; return 1; }};
assertThrows(() => MSGPACK.encodeResource(getter), TypeError);
assertEquals(0, calls);
assertThrows(() => MSGPACK.decodeResource(new SharedArrayBuffer(8)), TypeError);
assertThrows(() => MSGPACK.decodeResource(new ArrayBuffer(256 * 1024 * 1024 + 1)), RangeError);
const detached = MSGPACK.encodeResource(records);
%ArrayBufferDetach(detached.buffer);
assertThrows(() => MSGPACK.decodeResource(detached), TypeError);
for (const v of [42, {}, 'text']) assertThrows(() => MSGPACK.decodeResource(v), TypeError);
// Deterministic malformed input must not crash or leak a pending exception.
let seed = 0x51a7;
for (let i = 0; i < 2000; ++i) {
  const a = new Uint8Array(5 + (i % 64)); a.set(magic);
  for (let j = 5; j < a.length; ++j) {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; a[j] = seed;
  }
  try { MSGPACK.decodeResource(a); } catch (e) { assertTrue(e instanceof SyntaxError || e instanceof RangeError); }
}
print('MSGPACK resource tests passed');

// Strict limits and realm ownership also apply to compact nodes.
let deep = 1;
for (let i = 0; i < 256; ++i) deep = [deep];
resourceRoundTrip(deep);
assertThrows(() => MSGPACK.encodeResource([deep]), RangeError);
const rab = new ArrayBuffer(wire.length, {maxByteLength: wire.length + 32});
new Uint8Array(rab).set(wire);
const rabView = new DataView(rab, 0, wire.length);
assertEquals(records, MSGPACK.decodeResource(rabView));
rab.resize(1);
assertThrows(() => MSGPACK.decodeResource(rabView), TypeError);
const symbols = {kept: 3};
symbols[Symbol('omit')] = 4;
Object.defineProperty(symbols, 'hidden', {value: 5});
assertEquals({kept: 3}, MSGPACK.decodeResource(MSGPACK.encodeResource(symbols)));
const proxy = new Proxy({}, {ownKeys() { ++calls; return []; }});
assertThrows(() => MSGPACK.encodeResource(proxy), TypeError);
assertEquals(0, calls);
const realm = Realm.createAllowCrossRealmAccess();
Realm.global(realm).resourceWire = wire;
const foreign = Realm.eval(realm, 'MSGPACK.decodeResource(resourceWire)');
assertEquals(Realm.eval(realm, 'Array.prototype'), Object.getPrototypeOf(foreign));
assertEquals(Realm.eval(realm, 'Object.prototype'), Object.getPrototypeOf(foreign[0]));
assertEquals(records, foreign);
Realm.dispose(realm);
print('MSGPACK resource realm/limit tests passed');
// One shape must generalize representations without sharing mutable boxes.
const generalized = Array.from({length: 700}, (_, i) => ({
  identifier: i % 5 ? i : 'id-' + i,
  number_value: [i, i / 10, -0, NaN, 'text', null, true][i % 7],
  child_object: {number_value: i % 3 ? i / 8 : 'changed', label_text: 'constant text 🌏'}
}));
const generalizedWire = resourceRoundTrip(generalized);
const generalizedResult = MSGPACK.decodeResource(generalizedWire);
generalizedResult[1].child_object.number_value = 999;
assertEquals(generalized[2].child_object.number_value, generalizedResult[2].child_object.number_value);
gc();
assertEquals(generalized, MSGPACK.decodeResource(generalizedWire));
// Unsafe packed integers are rejected even if a scale would hide their size.
assertThrows(() => MSGPACK.decodeResource(resource([], [], ext(0x52,
    [0x92, 1, 0x91, 0x93, 6, 9, 0xc4, 8, 255, 255, 255, 255, 255, 255, 255, 255]))), SyntaxError);
print('MSGPACK resource representation tests passed');

})();
