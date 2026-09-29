// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc. Modes numbers/oracle print deterministic baseline data.
'use strict';

function check(condition, message = 'upstream JSON backport check failed') {
  if (!condition) throw new Error(message);
}

function numbers() {
  const buffer = new ArrayBuffer(8);
  const words = new Uint32Array(buffer), value = new Float64Array(buffer);
  let seed = 0x71cfb347;
  function next() {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
    return seed >>> 0;
  }
  const edges = [0, -0, NaN, Infinity, -Infinity, Number.MIN_VALUE,
    Number.MAX_VALUE, Number.MIN_SAFE_INTEGER, Number.MAX_SAFE_INTEGER,
    1e-7, 1e-6, 1e20, 1e21, 0.1, 1.005, 1000000000000000100,
    -2147483648, 2147483647, 2147483648, 2 ** -1022, 2 ** -1022 - Number.MIN_VALUE];
  for (let i = 0; i < edges.length + 200000; ++i) {
    words[0] = next(); words[1] = next();
    const n = i < edges.length ? edges[i] : value[0];
    const text = String(n);
    if (Number.isFinite(n) && n !== 0) check(Object.is(Number(text), n));
    print(text + '|' + JSON.stringify(n));
    if (i < 1024 && Number.isFinite(n)) {
      print(n.toFixed(6) + '|' + n.toExponential() + '|' + n.toPrecision(17));
    }
  }
}

const texts = [
  '[{"a":1,"b":2},{"a":3,"b":4}]',
  '[{"a":1,"b":2},{"b":3,"a":4}]',
  '[{"a":1,"b":2},{"a":3},{"a":4,"b":5,"c":6}]',
  '[{"a":1},{"0":2,"a":3},{"a":4,"0":5}]',
  '[{"a":1},{"a":2,"a":3},{"a":4,"\\u0061":5}]',
  '[{"x\\n":1},{"x\\n":2}]', '[{"漢":1},{"漢":2}]',
  '[{"__proto__":1},{"__proto__":2}]',
  '[{"a":1},{"a":', '[{"a":1},{"a" 2}]', '[{"a":1},{"a":2,}]',
  '[{"a":1},{"a":2,"b"}]', '[{"a":1},{"a":2} trailing]',
  '[{"a":1},{"a":2,"a\\u00":3}]',
];

function oracle() {
  for (const text of texts) {
    try { print(JSON.stringify(JSON.parse(text))); }
    catch (error) { print(error.name + ': ' + error.message); }
  }
  const chars = Array.from({length: 65536}, (_, i) => String.fromCharCode(i)).join('');
  for (const value of [chars, [chars, '\ud800', '\udc00', '😀'],
      {empty: '', number: -0, missing: undefined}, new Number(1.25),
      new String('漢😀'), Object(Symbol('s'))]) {
    for (const gap of [undefined, 2, 'ab\0cd', '漢 ']) {
      print(JSON.stringify(value, undefined, gap));
    }
  }
}

function* randomValues() {
  let seed = 0x193fc87;
  const next = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0);
  const strings = ['', 'a', '漢😀', '\ud800', '\udc00', '"\n\\', '__proto__', '01'];
  function value(depth) {
    switch (next() % (depth ? 7 : 4)) {
      case 0: return next() / 7;
      case 1: return strings[next() % strings.length];
      case 2: return !!(next() & 1);
      case 3: return null;
      case 4: return Array.from({length: next() % 8}, () => value(depth - 1));
      default: {
        const result = {};
        for (let i = 0, n = next() % 8; i < n; ++i) {
          const key = next() & 1 ? strings[next() % strings.length] : String(next() % 100);
          Object.defineProperty(result, key, {value: value(depth - 1),
            enumerable: true, configurable: true});
        }
        return result;
      }
    }
  }
  for (let i = 0; i < 3000; ++i) yield value(4);
}

async function differential() {
  let index = 0;
  for (const value of randomValues()) {
    const text = JSON.stringify(value);
    print(text);
    check(await JSON.stringifyAsync(value) === text);
    check(JSON.stringify(JSON.parse(text)) === text);
    check(JSON.stringify(await JSON.parseAsync(text)) === text);
    if (++index <= 500) {
      for (const malformed of [text.slice(0, -1), text + 'x', text.replace(':', ' ')]) {
        let expected, failure;
        try { expected = JSON.stringify(JSON.parse(malformed)); }
        catch (e) { failure = e.name + ': ' + e.message; }
        print(failure || expected);
        let actual, asyncFailure;
        try { actual = JSON.stringify(await JSON.parseAsync(malformed)); }
        catch (e) { asyncFailure = e.name + ': ' + e.message; }
        check(actual === expected && asyncFailure === failure);
      }
    }
  }
}

async function main() {
  if (arguments.length) throw new Error('unexpected main arguments');
  const chars = Array.from({length: 65536}, (_, i) => String.fromCharCode(i)).join('');
  for (const value of [chars, {漢: [1, 2, '😀'], ascii: chars}, [chars, chars]]) {
    check(await JSON.stringifyAsync(value) === JSON.stringify(value));
  }
  for (const length of [0, 7, 8, 15, 16, 31, 32, 4095, 4096, 4097]) {
    for (const tail of ['x', '\n', '\\', '"', '\ud800', '\udc00', '😀']) {
      const value = '漢'.repeat(length) + tail + 'x';
      check(await JSON.stringifyAsync(value) === JSON.stringify(value));
    }
  }
  const objects = Array.from({length: 3000}, (_, i) => ({a: i, b: i / 7, c: 'x'}));
  const baseline = JSON.stringify(objects);
  check(await JSON.stringifyAsync(objects) === baseline);
  gc();
  Object.defineProperty(objects[0], 'b', {enumerable: false});
  objects[1][Symbol('hidden')] = 9;
  objects[2].b = 'changed';
  delete objects[3].a;
  let calls = 0;
  Object.defineProperty(objects[4], 'a', {get() { ++calls; gc(); return 42; }});
  const text = JSON.stringify(objects);
  check(calls === 1, 'fast-path bailout replayed a getter');
  check(await JSON.stringifyAsync(objects) === text);
  check(calls === 2);
  check(JSON.parse(text)[0].b === undefined);
  check(JSON.parse(text)[2].b === 'changed');
  for (let i = 0; i < 100; ++i) {
    const object = {};
    for (let j = 0; j < 100; ++j) object['key' + j] = j + i / 8;
    const value = [object, {...object}];
    check(await JSON.stringifyAsync(value) === JSON.stringify(value));
    if ((i & 7) === 0) gc();
  }
  const proto = {toJSON(key) { ++calls; return 'custom:' + key; }};
  const inherited = Object.assign(Object.create(proto), {x: 1});
  calls = 0;
  check(JSON.stringify({a: 1, b: inherited}) === '{"a":1,"b":"custom:b"}');
  check(calls === 1);
  check(await JSON.stringifyAsync({a: 1, b: inherited}) === '{"a":1,"b":"custom:b"}');
  check(calls === 2, 'async fast fallback replayed toJSON');
  Object.prototype.toJSON = function() { return 23; };
  try { check(JSON.stringify({x: 1}) === '23'); }
  finally { delete Object.prototype.toJSON; }
  Array.prototype.toJSON = function() { return 24; };
  try { check(JSON.stringify([1, 2]) === '24'); }
  finally { delete Array.prototype.toJSON; }
  for (const text of texts) {
    let expected, error;
    try { expected = JSON.stringify(JSON.parse(text)); } catch (e) { error = e; }
    try {
      const actual = await JSON.parseAsync(text);
      check(!error && JSON.stringify(actual) === expected);
    } catch (e) { check(error && e.name === error.name && e.message === error.message); }
  }
  const cycle = {}; cycle.self = cycle;
  let threw = false;
  try { JSON.stringify(cycle); } catch (e) { threw = e instanceof TypeError; }
  check(threw);
  const deep = {}; let node = deep;
  for (let i = 0; i < 1000; ++i) node = node.next = {value: i};
  check(await JSON.stringifyAsync(deep) === JSON.stringify(deep));
  print('PASS: upstream JSON fast-path fallback, metadata, Unicode, GC, async composition');
}

if (arguments[0] === 'numbers') numbers();
else if (arguments[0] === 'oracle') oracle();
else if (arguments[0] === 'differential') {
  differential().catch(error => { print(error.stack || error); quit(1); });
}
else main().catch(error => { print(error.stack || error); quit(1); });
