// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run: d8 --expose-gc test/json/stringify-async.js

'use strict';

function check(value, message = 'check failed') {
  if (!value) throw new Error(message);
}

async function compare(value, replacer, gap) {
  let expected, expectedError, failed = false;
  try { expected = JSON.stringify(value, replacer, gap); }
  catch (error) { expectedError = error; failed = true; }
  let promise;
  try { promise = JSON.stringifyAsync(value, replacer, gap); }
  catch (error) { throw new Error('synchronously threw: ' + error); }
  check(promise instanceof Promise);
  let actual, actualError, rejected = false;
  try { actual = await promise; } catch (error) {
    actualError = error; rejected = true;
  }
  check(failed === rejected, 'exception state differs');
  if (failed && expectedError instanceof Error) {
    check(actualError && actualError.constructor === expectedError.constructor);
    check(actualError.message === expectedError.message,
          'error differs: ' + actualError.message + ' / ' + expectedError.message);
  } else if (failed) {
    check(actualError === expectedError);
  } else {
    check(actual === expected, 'stringify result differs');
  }
}

async function main() {
  const descriptor = Object.getOwnPropertyDescriptor(JSON, 'stringifyAsync');
  check(descriptor.writable && descriptor.configurable && !descriptor.enumerable);
  check(JSON.stringifyAsync.length === 3);
  check(JSON.stringifyAsync.name === 'stringifyAsync');
  check(JSON.stringifyAsync.prototype === undefined);
  let notConstructor = false;
  try { new JSON.stringifyAsync(null); } catch (error) {
    notConstructor = error instanceof TypeError;
  }
  check(notConstructor);

  const cases = [undefined, null, true, false, 0, -0, 1.25, NaN, Infinity,
    -Infinity, 1e-7, 1e21, Number.MIN_VALUE, Number.MAX_VALUE, 9007199254740991,
    '', 'plain', '\\"\n\t\b\f\r', '漢字😀', '\ud800', '\udc00', '\ud800x',
    '\ud800\udc00', '\ud800\ud800\udc00', '\u0000', '\u00ff',
    Symbol('s'), function f() {}, 1n, Object(1n), Object(Symbol('s')),
    Object(42), Object('abc'), Object(false), [], {},
    [1, undefined, function() {}, Symbol(), NaN],
    {a: undefined, b: Symbol(), c() {}, d: null},
    {'2': 2, '1': 1, a: 3, '4294967295': 4},
    Object.defineProperty({}, '__proto__', {value: 7, enumerable: true}),
    new Uint8Array([1, 2, 3]), new Map([['x', 1]]), new Set([1, 2]),
    new Date('2020-01-02T03:04:05Z')];
  if (JSON.rawJSON) cases.push(JSON.rawJSON('12345678901234567890'));
  for (const value of cases) {
    await compare(value);
    await compare(value, undefined, 2);
  }
  for (const gap of [0, -1, 100, 2.5, NaN, Infinity, true, '', 'ab\0cd',
                     '漢😀'.repeat(6), Object('xx'), Object(3)]) {
    await compare({a: [1, {b: 'value'}], empty: {}}, null, gap);
  }
  for (const replacer of [[], ['b', 'a', 'b', 1, Object('a'), Object(2)],
                          true, {length: 2}, [null, {}, Symbol()]]) {
    await compare({a: 1, b: {a: 2, b: 3}, 1: 4, 2: 5}, replacer, 3);
  }
  const cycle = {a: {b: {}}};
  cycle.a.b.back = cycle;
  await compare(cycle);
  const cycleArray = []; cycleArray.push(cycleArray);
  await compare(cycleArray);
  const shared = {nested: {value: 7}};
  await compare([shared, shared, shared.nested]);
  const deep = {}; let cursor = deep;
  for (let i = 0; i < 80; ++i) cursor = cursor.child = {text: 'before retry'};
  await compare(deep);

  // Fresh factories are essential: callbacks mutate input, so serializing the
  // same instance twice would not be a valid differential semantics oracle.
  function observable() {
    const events = [];
    const object = {a: 1, get b() {
      events.push('get b'); this.a = 8; delete this.c; this.newKey = 9; gc();
      return {toJSON(key) { events.push('toJSON ' + key); return '漢😀'; }};
    }, c: 3};
    const proxy = new Proxy(object, {
      get(target, key, receiver) {
        events.push('get ' + String(key));
        return Reflect.get(target, key, receiver);
      },
      ownKeys(target) { events.push('keys'); return Reflect.ownKeys(target); },
      getOwnPropertyDescriptor(target, key) {
        events.push('descriptor ' + String(key));
        return Reflect.getOwnPropertyDescriptor(target, key);
      }
    });
    function replacer(key, value) {
      events.push('replace ' + key + ' ' + Object.hasOwn(this, key));
      if (key === 'a') object.c = 5;
      return value;
    }
    const gap = Object('xx');
    gap.toString = () => { events.push('gap'); return '  '; };
    return {proxy, replacer, gap, events};
  }
  const sync = observable();
  const expected = JSON.stringify(sync.proxy, sync.replacer, sync.gap);
  const async = observable();
  const promise = JSON.stringifyAsync(async.proxy, async.replacer, async.gap);
  check(JSON.stringify(async.events) === JSON.stringify(sync.events),
        'callbacks must finish once, in stringify order, before return');
  check(await promise === expected);
  check(JSON.stringify(async.events) === JSON.stringify(sync.events));

  const reason = {sentinel: 1};
  for (const value of [{get x() { throw reason; }},
                       {toJSON() { throw reason; }}]) {
    const pending = JSON.stringifyAsync(value);
    let caught;
    try { await pending; } catch (error) { caught = error; }
    check(caught === reason);
  }
  let replacerError;
  try { await JSON.stringifyAsync({}, () => { throw reason; }); }
  catch (error) { replacerError = error; }
  check(replacerError === reason);
  for (const thrown of [undefined, null, false, 0, '', reason]) {
    await compare({get value() { throw thrown; }});
  }
  const inner = [];
  const outer = JSON.stringifyAsync({toJSON() {
    inner.push(JSON.stringifyAsync({key: 'inner'}));
    gc();
    return {key: 'outer'};
  }});
  check(await outer === '{"key":"outer"}');
  check(await inner[0] === '{"key":"inner"}');
  BigInt.prototype.toJSON = function() { return String(this); };
  try { await compare({big: 12345678901234567890n}); }
  finally { delete BigInt.prototype.toJSON; }

  const revocable = Proxy.revocable({}, {}); revocable.revoke();
  await compare(revocable.proxy);
  const inherited = [1, , 3];
  const prototype = Object.create(Array.prototype);
  Object.defineProperty(prototype, '1', {get() { return 'inherited'; }});
  Object.setPrototypeOf(inherited, prototype);
  await compare(inherited);

  const chars = Array.from({length: 65536}, (_, i) => String.fromCharCode(i)).join('');
  await compare(chars);
  for (const length of [4095, 4096, 4097, 8191, 8192]) {
    await compare('x'.repeat(length) + '😀\ud800\n漢');
  }
  for (const length of [1, 15, 16, 17, 3999, 4000, 4001, 20000]) {
    await compare(Array.from({length}, (_, i) => i));
    await compare(Array.from({length}, (_, i) => (i & 1 ? -1 : 1) * 1073741823));
    await compare(Array.from({length}, (_, i) => [1.25, -0, 1e100, NaN][i & 3]));
    const holes = new Array(length); holes[length - 1] = 42;
    await compare(holes);
  }
  const records = Array.from({length: 12000}, (_, i) => ({
    id: i, ratio: i / 7, label: i & 1 ? 'Aa' : 'BB',
    ['key' + (i % 160)]: '漢' + i, nested: {id: i + 0.25}}));
  await compare(records);
  const snapshot = JSON.stringify(records);
  const pending = JSON.stringifyAsync(records);
  records[0].label = 'changed after capture';
  records.length = 0;
  gc();
  const text = await pending;
  check(text === snapshot, 'later mutations must not affect captured values');
  gc(); gc();
  check(JSON.stringify(JSON.parse(text)) === snapshot);
  check(JSON.stringify(await JSON.parseAsync(text)) === snapshot);

  // Deterministic differential input corpus, including omission and Unicode.
  let seed = 0x5973a1;
  function random() { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed; }
  function value(depth) {
    const choice = random() % (depth ? 10 : 7);
    if (choice === 0) return undefined;
    if (choice === 1) return null;
    if (choice === 2) return !!(random() & 1);
    if (choice === 3) return (random() | 0) / 11;
    if (choice === 4) return ['\ud800', '漢😀', '\0\n', 'Aa', 'BB'][random() % 5];
    if (choice === 5) return [NaN, -0, Infinity][random() % 3];
    if (choice === 6) return Symbol();
    if (choice === 7) return Array.from({length: random() % 8}, () => value(depth - 1));
    const result = {};
    for (let i = 0, count = random() % 8; i < count; ++i) {
      Object.defineProperty(result, ['a', '2', '__proto__', '漢', ''][random() % 5] + i,
        {value: value(depth - 1), enumerable: true, configurable: true});
    }
    return result;
  }
  for (let batch = 0; batch < 20; ++batch) {
    await Promise.all(Array.from({length: 32}, () => compare(value(4))));
  }
  print('PASS: JSON.stringifyAsync semantics, capture order, differential corpus, GC');
}

main().catch(error => { print(error.stack || error); quit(1); });
