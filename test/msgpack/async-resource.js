// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with d8 --allow-natives-syntax --expose-gc.
'use strict';
function check(ok, message = 'check failed') { if (!ok) throw new Error(message); }
function equal(a, b) {
  if (Object.is(a, b)) return;
  check(a !== null && b !== null && typeof a === 'object' && typeof b === 'object',
        `value mismatch: ${String(a)} / ${String(b)}`);
  check(Array.isArray(a) === Array.isArray(b));
  const keys = Object.keys(b);
  check(JSON.stringify(Object.keys(a)) === JSON.stringify(keys), 'keys');
  if (Array.isArray(b)) check(a.length === b.length);
  for (const key of keys) {
    const d = Object.getOwnPropertyDescriptor(a, key);
    check(d.writable && d.enumerable && d.configurable);
    equal(a[key], b[key]);
  }
}
async function rejects(promise, type) {
  check(promise instanceof Promise);
  try { await promise; } catch (e) { check(e instanceof type, String(e)); return; }
  throw new Error('expected rejection');
}
const magic = [86, 56, 77, 82, 1];
function ext(type, body) {
  return [...(body.length < 256 ? [0xc7, body.length] :
      [0xc8, body.length >>> 8, body.length & 255]), type, ...body];
}
function resource(shapes, strings, root) {
  return new Uint8Array([...magic, 0x93, ...MSGPACK.encode(shapes),
                         ...MSGPACK.encode(strings), ...root]);
}
async function differential(bytes) {
  let expected, failure;
  try { expected = MSGPACK.decodeResource(bytes); } catch (e) { failure = e.constructor; }
  if (failure) await rejects(MSGPACK.decodeResourceAsync(bytes), failure);
  else equal(await MSGPACK.decodeResourceAsync(bytes), expected);
}
async function parity(value) {
  const sync = MSGPACK.encodeResource(value);
  const promise = MSGPACK.encodeResourceAsync(value);
  check(promise instanceof Promise);
  const bytes = await promise;
  check(bytes instanceof Uint8Array && !bytes.buffer.resizable);
  equal(Array.from(bytes), Array.from(sync));
  const expected = MSGPACK.decodeResource(sync);
  equal(await MSGPACK.decodeResourceAsync(bytes), expected);
  equal(await MSGPACK.decodeResourceAsync(bytes.buffer), expected);
  equal(await MSGPACK.decodeResourceAsync(MSGPACK.encode(value)), expected);
  const padded = new Uint8Array(bytes.length + 17);
  padded.fill(0xc1); padded.set(bytes, 7);
  equal(await MSGPACK.decodeResourceAsync(padded.subarray(7, 7 + bytes.length)), expected);
  equal(await MSGPACK.decodeResourceAsync(new DataView(padded.buffer, 7, bytes.length)), expected);
  return bytes;
}
async function main() {
  for (const name of ['encodeResourceAsync', 'decodeResourceAsync']) {
    const d = Object.getOwnPropertyDescriptor(MSGPACK, name);
    check(d.writable && d.configurable && !d.enumerable);
    check(MSGPACK[name].name === name && MSGPACK[name].length === 1);
    await rejects(MSGPACK[name](), TypeError);
    try { new MSGPACK[name](1); throw 0; } catch (e) { check(e instanceof TypeError); }
  }
  let synchronous = true;
  const pending = MSGPACK.encodeResourceAsync(1).then(() => check(!synchronous));
  synchronous = false; await pending;
  for (const value of [null, false, true, 0, -0, 0.1, NaN, -NaN, Infinity, -Infinity,
      Number.MIN_VALUE, Number.MAX_VALUE, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER,
      '', 'café', '中文🌏', [], {}, [1, null, {a: false}],
      3n, 9223372036854775807n, -9223372036854775808n, 18446744073709551615n]) {
    const bytes = await parity(value);
    equal(Array.from(bytes), Array.from(MSGPACK.encode(value))); // exact fallback
  }
  for (const count of [15, 16, 255, 256, 257, 4095, 4096, 4097]) {
    for (const make of [i => i - 300, i => i / 10, i => i / 8,
        i => i % 2 ? -0 : 0, i => i % 3 ? NaN : Infinity,
        i => i % 2 ? 2147483648 : -2147483649,
        i => i % 2 ? Number.MAX_SAFE_INTEGER : Number.MIN_SAFE_INTEGER,
        i => i % 2 ? 0.1 : 1 / 3]) await parity(Array.from({length: count}, (_, i) => make(i)));
  }
  const retained = [];
  for (const size of [63, 64, 65, 255, 256, 4095, 4096, 4097]) {
    const text = 'a中🌏é'.repeat(size);
    retained.push(await parity(Array.from({length: 300}, (_, i) => ({
      [text]: i, repeated_value: text, child: {number: i / 10},
      big: i % 2 ? 18446744073709551615n : -9223372036854775808n
    }))));
    gc();
  }
  for (const bytes of retained) await differential(bytes);
  const indexed = Array.from({length: 300}, (_, i) => {
    const v = {'10': i, '2': i + 1, long_property: 'repeated'.repeat(30)};
    Object.defineProperty(v, '__proto__', {value: i, enumerable: true});
    return v;
  });
  const own = await MSGPACK.decodeResourceAsync(await parity(indexed));
  check(Object.getPrototypeOf(own[0]) === Object.prototype && Object.hasOwn(own[0], '__proto__'));
  own[0].long_property = 'changed'; check(own[1].long_property !== 'changed');
  // Capture both JS graphs and the exact selected storage before returning.
  const original = {values: Array.from({length: 10000}, (_, i) => i / 10), text: '中'.repeat(10000)};
  const expected = MSGPACK.encodeResource(original);
  const encoding = MSGPACK.encodeResourceAsync(original);
  original.values[0] = 999; original.text = 'changed'; gc();
  equal(Array.from(await encoding), Array.from(expected));
  const storage = expected.slice();
  const decoding = MSGPACK.decodeResourceAsync(storage); storage.fill(0xc1);
  %ArrayBufferDetach(storage.buffer); gc();
  equal(await decoding, MSGPACK.decodeResource(expected));
  const rab = new ArrayBuffer(expected.length, {maxByteLength: expected.length + 32});
  new Uint8Array(rab).set(expected);
  const view = new DataView(rab, 0, expected.length);
  const resizing = MSGPACK.decodeResourceAsync(view); rab.resize(0);
  equal(await resizing, MSGPACK.decodeResource(expected));
  await rejects(MSGPACK.decodeResourceAsync(view), TypeError);
  for (const input of [undefined, null, 1, 'bytes', [], {}, new SharedArrayBuffer(8),
      new Uint8Array(new SharedArrayBuffer(8)), storage, storage.buffer])
    await rejects(MSGPACK.decodeResourceAsync(input), TypeError);
  let calls = 0;
  const cycle = {}; cycle.self = cycle;
  for (const value of [undefined, Symbol(), () => 1, cycle, [, 1], new Date(), new Map(),
      new Set(), new Uint8Array(2), 1n << 64n, '\ud800', '\udc00',
      {get x() { ++calls; return 1; }}, new Proxy({}, {ownKeys() { ++calls; return []; }})])
    await rejects(MSGPACK.encodeResourceAsync(value), TypeError);
  check(calls === 0);
  let deep = 1; for (let i = 0; i < 256; ++i) deep = [deep];
  await parity(deep);
  await rejects(MSGPACK.encodeResourceAsync([deep]), RangeError);
  await differential(resource([], [], [...Array(257).fill(0x91), 1]));
  const ref = n => ext(0x51, Array.from(MSGPACK.encode(n)));
  await differential(resource([], ['one'], [0x92, ...ref(0), ...ref(0)]));
  await differential(resource([['long_property_name']], [], ext(0x50, [0x92, 0, 0x91, 4])));
  for (const [shapes, strings, root] of [
      [[], [], ref(0)], [[], ['x'], ref(1)], [[], ['x'], ref(-1)],
      [[], ['x'], ref(0.5)], [[], ['x'], ext(0x51, [0, 0])],
      [[], [], ext(0x53, [0])], [[], [], ext(0x50, [0x92, 0, 0x90])],
      [[['x']], [], ext(0x50, [0x92, 0, 0x90])],
      [[['x']], [], ext(0x50, [0x92, 0, 0xdd, 0x40, 0, 0, 1, 0])],
      [[['x', 'x']], [], [0]], [[['x', 1]], [], [0]], [[], [1], [0]],
      [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 10, 0, 0xc4, 1, 1])],
      [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 0, 10, 0xc4, 1, 1])],
      [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 0, 0, 0xc4, 0])],
      [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 2, 0, 0xc4, 1, 1])],
      [[], [], ext(0x52, [0x92, 2, 0x91, 0x93, 0, 0, 0xc4, 1, 1])],
      [[], [], ext(0x52, [0x92, 1, 0x91, 0x93, 6, 0, 0xc4, 8, 0, 0x20, 0, 0, 0, 0, 0, 0])]])
    await differential(resource(shapes, strings, root));
  // Include empty/unused invalid tables, every extension header, and unknown versions.
  for (const bytes of [[], [0xc1], [...magic], [86, 56, 77, 82, 2],
      [...magic, 0x93, 0x90, 0x91, 0xa1, 0xff, 0],
      [...magic, 0x93, 0x91, 0x91, 0xa1, 0xff, 0x90, 0]])
    await differential(new Uint8Array(bytes));
  for (const root of [[0xd4, 0x51, 0], [0xd5, 0x51, 0xcc, 0],
      [0xc8, 0, 1, 0x51, 0], [0xc9, 0, 0, 0, 1, 0x51, 0]])
    await differential(resource([], ['one'], root));
  const compact = await parity(indexed);
  for (let i = 0; i < compact.length; ++i) {
    if (i > 150 && i % 103) continue;
    await differential(compact.subarray(0, i));
  }
  let seed = 0x51a7;
  function next() { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed >>> 0; }
  for (let i = 0; i < 2000; ++i) {
    const bytes = Uint8Array.from({length: 5 + next() % 64}, () => next() & 255);
    bytes.set(magic); await differential(bytes);
  }
  // A small resource can expand beyond the native budget; reject before JS allocation.
  await rejects(MSGPACK.decodeResourceAsync(resource([], ['x'.repeat(65536)],
      [...MSGPACK.encode(Array(4096).fill(null)).subarray(0, 3),
       ...Array.from({length: 4096}, () => ref(0)).flat()])), RangeError);
  const realm = Realm.create();
  const foreignBytes = await Realm.eval(realm, 'MSGPACK.encodeResourceAsync')(indexed);
  check(Object.getPrototypeOf(foreignBytes) === Realm.eval(realm, 'Uint8Array.prototype'));
  const foreign = await Realm.eval(realm, 'MSGPACK.decodeResourceAsync')(foreignBytes);
  check(Object.getPrototypeOf(foreign) === Realm.eval(realm, 'Array.prototype'));
  check(Object.getPrototypeOf(foreign[0]) === Realm.eval(realm, 'Object.prototype'));
  Realm.dispose(realm);
  const jobs = Array.from({length: 40}, (_, i) => i % 4 === 0 ? MSGPACK.encodeAsync(indexed) :
      i % 4 === 1 ? MSGPACK.decodeAsync(MSGPACK.encode(indexed)) :
      i % 4 === 2 ? MSGPACK.encodeResourceAsync(indexed) : MSGPACK.decodeResourceAsync(compact));
  const outcomes = await Promise.allSettled(jobs);
  check(outcomes.filter(x => x.status === 'rejected').length === 8, 'shared job bound');
  for (const out of outcomes) if (out.status === 'rejected') check(out.reason instanceof RangeError);
  await parity({after: 'limit'});
  print('PASS: resource async exact bytes, compact/fallback, snapshots, GC, realms, malformed inputs, native/job limits');
}
main().catch(e => { print(e.stack || e); quit(1); });
