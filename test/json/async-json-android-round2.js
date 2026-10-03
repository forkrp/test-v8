// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc; also exercise --stress-compaction and
// --no-json-stringify-fast-path.
'use strict';

function check(condition, message) {
  if (!condition) throw new Error(message);
}

async function compare(value, message) {
  const expected = JSON.stringify(value);
  check(await JSON.stringifyAsync(value) === expected, message);
  check(JSON.stringify(await JSON.parseAsync(expected)) === expected,
        message + ' parse');
}

async function main() {
  const edges = [0, -0, NaN, Infinity, -Infinity, Number.MIN_VALUE,
    Number.MAX_VALUE, -Number.MAX_VALUE, 1e-7, 1e-6, -1e-6,
    -0.0000010000000000000002, 1e20, 1e21, 1000000000000000100,
    2147483647, -2147483648, 4294967295, -4294967295];
  const integerFields = Object.fromEntries([
    -2147483648, -1073741824, -10001, -10000, -9999, -1,
    0, 1, 9999, 10000, 10001, 1073741823, 2147483647,
  ].map((value, index) => ['k' + index, value]));
  await compare(integerFields, 'individual integer capture boundaries');
  await compare({label: '漢😀', ...integerFields},
                'individual integers widened to UTF-16');
  for (const count of [0, 1, 63, 64, 65]) {
    await compare(Array(count).fill(3.5), 'numeric memo job size ' + count);
  }
  let seed = 0x12345678;
  const words = new Uint32Array(2);
  const floating = new Float64Array(words.buffer);
  const numbers = [];
  for (let i = 0; i < 20000; ++i) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    words[0] = seed;
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    words[1] = seed;
    // Repetitions keep an adaptive cache active while distinct misses exercise
    // the formatter's bounded storage across the full binary64 exponent range.
    for (let k = 0; k < 4; ++k) numbers.push(floating[0]);
  }
  await compare([...edges, ...numbers], 'binary64 cache misses and hits');
  await compare([...numbers].reverse(), 'reversed binary64 values');
  await compare({label: '漢😀', values: [...edges, ...numbers]},
                'numeric text widened to UTF-16');
  await compare(Array.from({length: 10000}, (_, i) => i & 1 ? 3.5 : -3.5),
                'opposite signs');
  await compare([...Array(1000).fill(3.5), ...numbers.filter((_, i) => !(i % 4))],
                'repeated prefix and unique suffix');
  await compare([...numbers.filter((_, i) => !(i % 4)), ...Array(1000).fill(3.5)],
                'unique prefix and repeated suffix');

  const literals = ['-0', '1.5', '4294967295', '-2147483648', '1e400'];
  for (const count of [63, 64, 65, 127, 128, 129, 65535, 65536, 65537]) {
    const parts = Array.from({length: count}, (_, i) => literals[i % literals.length]);
    const source = '[' + parts.join(',') + ']';
    const result = await JSON.parseAsync(source);
    check(result.length === count, 'numeric array length');
    for (let i = 0; i < count; ++i) {
      check(Object.is(result[i], Number(parts[i])), 'numeric array value');
    }
    const mixed = await JSON.parseAsync('["first",' + parts.join(',') +
        ',{"key":"value"},' + source + ',"last"]');
    check(mixed[0] === 'first' && mixed[count + 1].key === 'value' &&
          mixed[count + 2].length === count && mixed[count + 3] === 'last',
          'mixed and nested numeric arrays');
    check(Object.is(mixed[1], -0), 'mixed numeric negative zero');
    let expected, actual;
    try { JSON.parse(source.slice(0, -1) + ',]'); }
    catch (error) { expected = error.name + ':' + error.message; }
    try { await JSON.parseAsync(source.slice(0, -1) + ',]'); }
    catch (error) { actual = error.name + ':' + error.message; }
    check(expected && actual === expected, 'late numeric error position');
  }

  const records = Array.from({length: 20000}, (_, i) => ({
    a: [0, 3.5, 'value', null][i % 4],
    b: [3.5, 0, {}, false][i % 4],
    c: i / 8,
  }));
  const parsed = await JSON.parseAsync(JSON.stringify(records));
  gc();
  parsed[0].c = 9.25;
  check(parsed[1].c === 0.125 && parsed[2].c === 0.25,
        'field ownership across representation changes and GC');
  check(JSON.stringify(parsed.slice(1)) === JSON.stringify(records.slice(1)),
        'cached keys and field representations');
  for (const source of ['[{"a":1,"a":2},{"a":3,"a":4}]',
      '[{"0":1,"a":2},{"a":3,"0":4}]',
      '[{"a":1},{"b":2,"a":3},{"a":4,"b":5}]']) {
    check(JSON.stringify(await JSON.parseAsync(source)) ===
          JSON.stringify(JSON.parse(source)), 'duplicate/indexed/changed keys');
  }
  const source = '[' + Array(129).fill('-0').join(',') + ']';
  let sources = 0;
  const revived = await JSON.parseAsync(source, (key, value, context) => {
    if (typeof value === 'number') {
      check(context.source === '-0', 'reviver source');
      ++sources;
    }
    return value;
  });
  check(sources === 129 && revived.every(value => Object.is(value, -0)),
        'numeric reviver values');
  print('PASS async Android round 2: binary64 formatting, numeric boundaries, ' +
        'cached keys, representations, GC, and reviver source');
}

main().catch(error => { print(error.stack || error); quit(1); });
