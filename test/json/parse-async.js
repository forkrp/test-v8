// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run: out/json-async/d8 --expose-gc test/json/parse-async.js

'use strict';

function check(condition, message = 'check failed') {
  if (!condition) throw new Error(message);
}

function equal(actual, expected) {
  if (Object.is(actual, expected)) return;
  check(actual !== null && expected !== null &&
        typeof actual === 'object' && typeof expected === 'object',
        `value mismatch: ${actual} / ${expected}`);
  check(Array.isArray(actual) === Array.isArray(expected));
  const keys = Object.keys(expected);
  check(JSON.stringify(Object.keys(actual)) === JSON.stringify(keys));
  if (Array.isArray(actual)) check(actual.length === expected.length);
  for (const key of keys) {
    const descriptor = Object.getOwnPropertyDescriptor(actual, key);
    check(descriptor.writable && descriptor.enumerable && descriptor.configurable);
    equal(actual[key], expected[key]);
  }
}

async function rejects(promise, constructor, reason) {
  check(promise instanceof Promise);
  try {
    await promise;
  } catch (error) {
    if (constructor) check(error instanceof constructor, String(error));
    if (reason !== undefined) check(error === reason);
    return;
  }
  throw new Error('expected rejection');
}

async function main() {
  const descriptor = Object.getOwnPropertyDescriptor(JSON, 'parseAsync');
  check(descriptor.writable && descriptor.configurable && !descriptor.enumerable);
  check(JSON.parseAsync.length === 2 && JSON.parseAsync.name === 'parseAsync');
  let synchronous = true;
  const pending = JSON.parseAsync('1').then(value => {
    check(!synchronous && value === 1);
  });
  synchronous = false;
  await pending;

  const corpus = [
    'null', 'true', 'false', '0', '-0', '-0.0', '1.25e-10', '1e400',
    '-1e400', '1e-400', '9007199254740993', '""', '"漢字😀"',
    '"\\uD800"', '"\\uDC00"', '"\\uD83D\\uDE00"',
    '"\\b\\f\\n\\r\\t\\\\\\/\\\""',
    '[]', '{}', '[1,2.5,-0,null,true,false,"a"]',
    '[1,2.5,-0,1e400,1e-400,-1e400]',
    '[1073741823,1073741824,-1073741824,-1073741825]',
    '{"4294967294":1,"10000000":2,"a":3}',
    '{"a":1,"a":2,"__proto__":{"polluted":true},"constructor":3}',
    '{"2":"two","1":"one","00":0,"4294967295":3,"a":{}}',
    '{"\\u0061":1,"a":2,"\\u0030":4,"0":5}',
    ' \t\n\r { "nested" : [ {}, [], {"x": [1]} ] } \r\n\t ',
  ];
  for (const source of corpus) {
    equal(await JSON.parseAsync(source), JSON.parse(source));
    // Exercise sliced strings, including two-byte backing stores.
    const sliced = ('前'.repeat(64) + source + '後'.repeat(64)).slice(64, -64);
    equal(await JSON.parseAsync(sliced), JSON.parse(source));
  }
  equal(await JSON.parseAsync(42), 42);
  equal(await JSON.parseAsync(null), null);
  let conversions = 0;
  const converted = JSON.parseAsync({toString() { ++conversions; return '42'; }});
  check(conversions === 1, 'ToString must run exactly once, at call time');
  equal(await converted, 42);
  await rejects(JSON.parseAsync(Symbol()), TypeError);
  await rejects(JSON.parseAsync(), SyntaxError);
  const reason = {conversion: true};
  await rejects(JSON.parseAsync({toString() { throw reason; }}), null, reason);

  const invalid = [
    '', ' ', 'undefined', 'NaN', 'Infinity', '+1', '01', '-01', '-', '.1',
    '1.', '1e', '1e+', '1e-', '[1,]', '[,1]', '{"a":1,}', '{a:1}',
    '{"a" 1}', '{"a":}', '{"a":1 "b":2}', '[1 2]', 'true false',
    'nul', 'tru', 'fals', '"unterminated', '"\\x41"', '"\\u00G0"',
    '"\\u123"', '"line\nbreak"', '\uFEFF{}', '{}x', '[', '{',
  ];
  for (const source of invalid) {
    let expected;
    try { JSON.parse(source); } catch (error) { expected = error; }
    check(expected instanceof SyntaxError);
    try {
      await JSON.parseAsync(source);
      throw new Error(`accepted invalid JSON: ${source}`);
    } catch (error) {
      check(error instanceof SyntaxError, String(error));
      check(error.message === expected.message, `${source}: ${error.message}`);
    }
  }

  // Differential reviver order, this binding, source text, deletion, mutation,
  // repeated keys, array snapshots, and precision-preserving conversion.
  function reviver(log) {
    return function(key, value, context) {
      log.push([key, context.source, Array.isArray(this)]);
      if (key === 'large') return BigInt(context.source);
      if (key === 'remove') return undefined;
      if (key === 'change') this.next = 99;
      return value;
    };
  }
  const source = '{"large":9007199254740993,"a":1,"a":2,"remove":0,' +
      '"change":0,"next":1,"items":[1,-0,{"remove":2,"x":"\\u0061"}]}';
  const syncLog = [], asyncLog = [];
  const expected = JSON.parse(source, reviver(syncLog));
  equal(await JSON.parseAsync(source, reviver(asyncLog)), expected);
  equal(asyncLog, syncLog);
  await rejects(JSON.parseAsync('[]', () => { throw reason; }), null, reason);
  check(await JSON.parseAsync('1', () => undefined) === undefined);
  equal(await JSON.parseAsync('{"a":1}', 42), {a: 1});
  check(await JSON.parseAsync('0', () => ({then(resolve) { resolve(73); }})) === 73);

  // Do not invoke prototype setters while materializing the private result.
  let setters = 0;
  Object.defineProperty(Object.prototype, 'asyncJsonTest', {
    set() { ++setters; }, configurable: true,
  });
  Object.defineProperty(Array.prototype, '0', {
    set() { ++setters; }, configurable: true,
  });
  let protectedResult;
  try {
    protectedResult = await JSON.parseAsync('{"asyncJsonTest":[1],"__proto__":2}');
  } finally {
    delete Object.prototype.asyncJsonTest;
    delete Array.prototype[0];
  }
  check(setters === 0);
  check(Object.getPrototypeOf(protectedResult) === Object.prototype);
  check(Object.hasOwn(protectedResult, '__proto__') && protectedResult.__proto__ === 2);
  equal(protectedResult.asyncJsonTest, [1]);

  // Fixed-seed randomized differential corpus; no external test dependency.
  let seed = 123456789;
  function random(n) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed % n;
  }
  function value(depth) {
    switch (random(depth ? 7 : 5)) {
      case 0: return null;
      case 1: return !!random(2);
      case 2: return random(100000) / 17 - 2000;
      case 3: return '漢😀\\\"\n\uD800'.slice(random(5));
      case 4: return random(2) ? '' : -0;
      case 5: return Array.from({length: random(5)}, () => value(depth - 1));
      default: {
        const result = {};
        for (let i = 0, n = random(5); i < n; ++i) result['k' + random(8)] = value(depth - 1);
        return result;
      }
    }
  }
  for (let i = 0; i < 400; ++i) {
    const text = JSON.stringify(value(4), null, i % 3 ? undefined : 2);
    equal(await JSON.parseAsync(text), JSON.parse(text));
  }

  // Concurrent differential checks for the native worker grammar, including
  // malformed mutations and lone UTF-16 surrogate code units.
  const mutations = ['"', '\\', '0', '-', '.', ',', ':', '[', ']', '{', '}',
                     ' ', '\n', 'e', 'u', '\u0000', '\uFEFF', '\uD800'];
  for (let batch = 0; batch < 40; ++batch) {
    await Promise.all(Array.from({length: 32}, async () => {
      let text = JSON.stringify(value(3));
      const position = random(text.length + 1);
      if (random(2)) text = text.slice(0, position) + mutations[random(mutations.length)] + text.slice(position);
      else text = text.slice(0, position) + text.slice(position + 1);
      let expected, error;
      try { expected = JSON.parse(text); } catch (caught) { error = caught; }
      let actual, rejected;
      try { actual = await JSON.parseAsync(text); } catch (caught) { rejected = caught; }
      if (error) {
        check(rejected instanceof SyntaxError, 'worker accepted invalid input: ' + text);
        check(rejected.message === error.message, 'worker error differs: ' + text);
      } else {
        check(!rejected, 'worker rejected valid input: ' + text);
        equal(actual, expected);
      }
    }));
  }

  // The small job must finish before the large job; merely deferring a full
  // synchronous parse to one task fails this test. Force GC between slices.
  const largeText = '[' + '{"id":1,"label":"漢😀"},'.repeat(60000) + 'null]';
  let largeDone = false;
  const large = JSON.parseAsync(largeText).then(result => {
    largeDone = true;
    check(result.length === 60001 && result[59999].label === '漢😀');
    return result;
  });
  await JSON.parseAsync('0');
  check(!largeDone, 'large document must yield to a later small parse');
  for (let i = 0; i < 3; ++i) {
    gc();
    await JSON.parseAsync('null');
  }
  await large;

  const deep = await JSON.parseAsync('['.repeat(12000) + '0' + ']'.repeat(12000));
  let leaf = deep;
  for (let i = 0; i < 12000; ++i) leaf = leaf[0];
  check(leaf === 0);
  const wide = {};
  for (let i = 0; i < 8000; ++i) wide['key' + i] = i;
  equal(await JSON.parseAsync(JSON.stringify(wide)), wide);
  // Same short-string hash, distinct contents; parsed strings must not alias
  // a cache slot merely because their hash or length matches.
  const collisions = Array.from({length: 20000}, (_, i) =>
    ['Aa', 'BB', 'Aa', 'ok', 'é', String(i % 200)][i % 6]);
  equal(await JSON.parseAsync(JSON.stringify(collisions)), collisions);
  equal(await JSON.parseAsync(JSON.stringify([...collisions, '成功', '文字列'])),
        [...collisions, '成功', '文字列']);
  for (const length of [4094, 4095, 4096, 4097, 8192]) {
    for (const suffix of ['\\uD800', '\\uD83D\\uDE00', '\\n', '漢']) {
      const text = '"' + 'a'.repeat(length) + suffix + '"';
      equal(await JSON.parseAsync(text), JSON.parse(text));
    }
  }
  const whitespace = JSON.parseAsync(' '.repeat(100000) + '1');
  await JSON.parseAsync('0');
  // The worker may finish a whitespace-only prefix before the small job.
  check(await whitespace === 1);

  // Large source-tracked arrays cross multiple slices too.
  const numbers = '[' + '1,'.repeat(10000) + '2]';
  equal(await JSON.parseAsync(numbers, (key, v) => typeof v === 'number' ? v + 1 : v),
        JSON.parse(numbers, (key, v) => typeof v === 'number' ? v + 1 : v));
  print('PASS: JSON.parseAsync semantics, differential corpus, GC, and yielding');
}

main().catch(error => { print(error.stack || error); quit(1); });
