// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
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
  check(promise instanceof Promise, 'must return a Promise');
  try { await promise; } catch (e) {
    check(e instanceof type, `${e} must be ${type.name}`); return;
  }
  throw new Error('expected rejection');
}
async function parity(value) {
  const sync = MSGPACK.encode(value);
  const promise = MSGPACK.encodeAsync(value);
  check(promise instanceof Promise);
  const async = await promise;
  check(async instanceof Uint8Array && !async.buffer.resizable);
  equal(Array.from(async), Array.from(sync));
  const expected = MSGPACK.decode(sync);
  equal(await MSGPACK.decodeAsync(async), expected);
  equal(await MSGPACK.decodeAsync(async.buffer), expected);
  const padded = new Uint8Array(sync.length + 17);
  padded.fill(0xc1); padded.set(sync, 7);
  equal(await MSGPACK.decodeAsync(padded.subarray(7, 7 + sync.length)), expected);
  equal(await MSGPACK.decodeAsync(new DataView(padded.buffer, 7, sync.length)), expected);
  return async;
}
async function main() {
  for (const name of ['encodeAsync', 'decodeAsync']) {
    const d = Object.getOwnPropertyDescriptor(MSGPACK, name);
    check(d.writable && d.configurable && !d.enumerable);
    check(MSGPACK[name].name === name && MSGPACK[name].length === 1);
    const missing = MSGPACK[name]();
    check(missing instanceof Promise);
    missing.catch(() => {});
    // Consume the intentionally rejected missing-argument promise.
    MSGPACK[name]().catch(() => {});
    try { new MSGPACK[name](1); throw 0; } catch (e) { check(e instanceof TypeError); }
  }
  let synchronous = true;
  const pending = MSGPACK.encodeAsync(1).then(() => check(!synchronous));
  synchronous = false; await pending;
  for (const value of [null, false, true, 0, -0, 127, 128, 255, 256, 65535,
      65536, -32, -33, -128, -129, -32768, -32769, 2147483647, 2147483648,
      -2147483649, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER,
      Number.MIN_VALUE, Number.MAX_VALUE, 0.5, 0.1, NaN, Infinity, -Infinity,
      Math.fround(1 / 3), '', 'ascii', 'café', '中文', 'Ελληνικά', '🌏😀',
      [], {}, [1, null, false, {a: 'nested'}],
      {'0': 'zero', '10': 'ten', a: 'named'}, 3n,
      9223372036854775807n, -9223372036854775808n, 18446744073709551615n]) await parity(value);
  for (const size of [1, 31, 32, 64, 65, 255, 256, 4095, 4096, 4097, 65535, 65536]) {
    for (const text of ['x\0'.repeat(size), 'é'.repeat(size),
                       '中'.repeat(size), 'x'.repeat(size) + '😀' + 'y'.repeat(size)])
      await parity({text});
  }
  // Allocation hints share only weak map/count keys. Keep the root shape
  // stable while native byte/part requirements grow, shrink and change kind.
  const changing = {text: '', values: []};
  for (const [text, values] of [
      ['x'.repeat(65536), Array.from({length: 4096}, (_, i) => i / 7)],
      ['', [1]],
      ['é'.repeat(65536), Array.from({length: 4096}, (_, i) => ({n: i / 7}))],
      ['中😀'.repeat(65536), Array.from({length: 4096}, (_, i) => i)],
      ['short', []]]) {
    changing.text = text; changing.values = values;
    await parity(changing); gc(); await parity(changing);
  }
  changing.values = [changing];
  await rejects(MSGPACK.encodeAsync(changing), TypeError);
  changing.values = [0.1, -0, NaN, Infinity];
  await parity(changing);
  for (const count of [0, 1, 7, 8, 9, 15, 16, 63, 64, 65, 257, 4096, 65536]) {
    for (const make of [i => i % 65536, i => i / 8, i => i % 2 ? -0 : 0.1,
                       i => i % 2 ? NaN : Infinity]) {
      const numbers = Array.from({length: count}, (_, i) => make(i));
      await parity(numbers); await parity({numbers});
    }
  }
  for (const count of [63, 64, 65, 256, 257, 4096]) {
    for (const make of [i => i, i => i % 2 ? i / 10 : -0,
                       i => i % 2 ? NaN : Infinity]) {
      const mixed = [];
      for (let i = 0; i < count; ++i) mixed.push(make(i));
      mixed.push({tail: ['mixed', null]}, 'end');
      await parity(mixed); await parity({mixed});
    }
  }
  const cyclicPrefix = [];
  for (let i = 0; i < 128; ++i) cyclicPrefix.push(i / 10);
  cyclicPrefix.push(cyclicPrefix);
  await rejects(MSGPACK.encodeAsync(cyclicPrefix), TypeError);
  const records = Array.from({length: 4000}, (_, i) => ({id: i, score: i / 10,
    child: {x: i / 8, value: 'child-' + i}, tags: ['a', 'b']}));
  for (let i = 0; i < records.length; i += 31) {
    records[i].score = 'changed'; delete records[i].tags; records[i].other = -0;
  }
  const retained = [];
  for (let pass = 0; pass < 6; ++pass) {
    const promise = MSGPACK.encodeAsync(records); gc();
    const bytes = await promise; retained.push(bytes);
    const decode = MSGPACK.decodeAsync(bytes); gc();
    equal(await decode, records);
  }
  gc(); for (const bytes of retained) equal(await MSGPACK.decodeAsync(bytes), records);
  const largeMap = {};
  for (let i = 0; i < 3000; ++i) largeMap['key' + i] = {value: i / 8};
  largeMap[''] = {values: records}; await parity(largeMap);
  let deep = 1; for (let i = 0; i < 256; ++i) deep = [deep];
  await parity(deep);
  await rejects(MSGPACK.encodeAsync([deep]), RangeError);
  await rejects(MSGPACK.decodeAsync(new Uint8Array([...Array(257).fill(0x91), 1])), RangeError);
  const protoBytes = new Uint8Array([0x81, 0xa9, ...Array.from('__proto__', c => c.charCodeAt(0)), 7]);
  const proto = await MSGPACK.decodeAsync(protoBytes);
  check(Object.getPrototypeOf(proto) === Object.prototype && Object.hasOwn(proto, '__proto__') && proto.__proto__ === 7);
  equal(await MSGPACK.decodeAsync(new Uint8Array([0x82, 0xa1, 120, 1, 0xa1, 120, 2])), {x: 2});
  const layoutValue = {x: 0.1, child: {v: 3}, label: 'data'};
  const layoutBytes = MSGPACK.encode(layoutValue);
  for (const change of [o => {o.x = 'changed'; o.child = [];}, o => Object.freeze(o),
      o => Object.defineProperty(o, 'x', {get() {throw new Error('must not invoke');}}),
      o => {delete o.label;}, o => Object.setPrototypeOf(o, null),
      o => {for (let i = 0; i < 300; ++i) o['added' + i] = i;}]) {
    const previous = await MSGPACK.decodeAsync(layoutBytes);
    change(previous); gc();
    const next = await MSGPACK.decodeAsync(layoutBytes);
    equal(next, layoutValue);
    check(Object.getPrototypeOf(next) === Object.prototype);
  }
  await parity([{id: 1, text: 'ordinary'},
    {id: 2, text: 'prefix'.repeat(200) + '🌏' + 'suffix'.repeat(200)}, {id: 3, text: 'tail'}]);
  let calls = 0;
  const accessor = {get x() { ++calls; return 1; }};
  const proxy = new Proxy({}, {ownKeys() { ++calls; return []; }});
  const cycle = {}; cycle.self = cycle;
  for (const value of [undefined, Symbol('x'), () => 1, accessor, proxy, cycle,
      [1, , 3], new Date(), new Map(), new Set(), /x/, new Uint8Array(3),
      18446744073709551616n, -9223372036854775809n, '\ud800', '\udc00',
      {nested: '\ud800'}, 'x'.repeat(4095) + '\ud800', 'x'.repeat(4096) + '\udc00'])
    await rejects(MSGPACK.encodeAsync(value), TypeError);
  check(calls === 0);
  for (const bytes of [[], [0xc1], [1, 2], [0x91], [0xd9, 3, 65],
      [0xa2, 0xc0, 0xaf], [0xa3, 0xed, 0xa0, 0x80], [0xa4, 0xf4, 0x90, 0x80, 0x80],
      [0x81, 1, 2], [0xc4, 1, 0], [0xd4, 0, 0]])
    await rejects(MSGPACK.decodeAsync(new Uint8Array(bytes)), SyntaxError);
  // High wire count bits must be validated before the compact tape stores
  // its bounded 30-bit count. Include enough children to select sliced builds.
  for (const tag of [0xdd, 0xdf]) {
    for (const high of [0x40, 0x80, 0xc0]) {
      for (const count of [0, 1, 256, 257]) {
        const body = tag === 0xdd ? Array(count).fill(0xc0)
            : Array.from({length: count}, () => [0xa1, 0x78, 0xc0]).flat();
        const bytes = new Uint8Array([tag, high, 0, count >>> 8, count & 255, ...body]);
        let error;
        try { MSGPACK.decode(bytes); } catch (e) { error = e; }
        check(error instanceof SyntaxError, 'oversized sync container count');
        await rejects(MSGPACK.decodeAsync(bytes), SyntaxError);
      }
    }
  }
  for (const input of [undefined, null, 1, 'bytes', [1], {}, new SharedArrayBuffer(8),
                       new Uint8Array(new SharedArrayBuffer(8))])
    await rejects(MSGPACK.decodeAsync(input), TypeError);
  const detached = new ArrayBuffer(8), view = new Uint8Array(detached);
  %ArrayBufferDetach(detached);
  await rejects(MSGPACK.decodeAsync(detached), TypeError);
  await rejects(MSGPACK.decodeAsync(view), TypeError);
  // Both API inputs are snapshotted before dispatch.
  const original = {text: '中'.repeat(20000), numbers: [1, 0.1, -0]};
  const encoding = MSGPACK.encodeAsync(original);
  original.text = 'mutated'; original.numbers[0] = 999;
  equal(MSGPACK.decode(await encoding), {text: '中'.repeat(20000), numbers: [1, 0.1, -0]});
  let storage = MSGPACK.encode(records);
  const decoding = MSGPACK.decodeAsync(storage); storage.fill(0xc1);
  %ArrayBufferDetach(storage.buffer); equal(await decoding, records);
  const rab = new ArrayBuffer(16, {maxByteLength: 32});
  new Uint8Array(rab).set(MSGPACK.encode({a: 1}), 4);
  const rabDecode = MSGPACK.decodeAsync(new DataView(rab, 4, 4)); rab.resize(0);
  equal(await rabDecode, {a: 1});
  await rejects(MSGPACK.decodeAsync(new DataView(rab, 0, 0)), SyntaxError);
  rab.resize(16); const oob = new Uint8Array(rab, 12, 4), oobData = new DataView(rab, 12, 4);
  rab.resize(8);
  await rejects(MSGPACK.decodeAsync(oob), TypeError);
  await rejects(MSGPACK.decodeAsync(oobData), TypeError);
  // Deterministic malformed differential corpus covers every byte tag.
  let seed = 0x12873456;
  function next() { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return seed >>> 0; }
  for (let i = 0; i < 2000; ++i) {
    const bytes = Uint8Array.from({length: next() % 40}, () => next() & 255);
    let expected, failure;
    try { expected = MSGPACK.decode(bytes); } catch (e) { failure = e.constructor; }
    if (failure) await rejects(MSGPACK.decodeAsync(bytes), failure);
    else equal(await MSGPACK.decodeAsync(bytes), expected);
  }
  const realm = Realm.create();
  const foreign = Realm.eval(realm, 'MSGPACK.decodeAsync');
  const foreignPromise = foreign(MSGPACK.encode(records));
  const foreignValue = await foreignPromise;
  check(Object.getPrototypeOf(foreignValue) === Realm.eval(realm, 'Array.prototype'));
  check(Object.getPrototypeOf(foreignValue[0]) === Realm.eval(realm, 'Object.prototype'));
  const foreignBytes = await Realm.eval(realm, 'MSGPACK.encodeAsync')({a: 1});
  check(Object.getPrototypeOf(foreignBytes) === Realm.eval(realm, 'Uint8Array.prototype'));
  Realm.dispose(realm);
  const jobs = Array.from({length: 40}, () => MSGPACK.decodeAsync(MSGPACK.encode(records)));
  const outcomes = await Promise.allSettled(jobs);
  check(outcomes.filter(x => x.status === 'rejected').length === 8, 'pending job bound');
  for (const out of outcomes) {
    if (out.status === 'rejected') check(out.reason instanceof RangeError);
    else equal(out.value, records);
  }
  equal(await MSGPACK.decodeAsync(await MSGPACK.encodeAsync({after: 'limit'})), {after: 'limit'});
  print('PASS: MessagePack async wire/value parity, errors, snapshots, GC, realms, 2000 malformed inputs, job bound');
}
main().catch(e => { print(e.stack || e); quit(1); });
