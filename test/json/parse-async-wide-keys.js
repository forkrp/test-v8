// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc; repeat with --stress-compaction or --verify-heap.
'use strict';

let comparisons = 0, hash = 2166136261;
function check(value, message) { if (!value) throw Error(message); }
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  hash = Math.imul(hash ^ 0xffff, 16777619) >>> 0;
}
function equal(actual, expected) {
  if (Object.is(actual, expected)) return;
  check(actual !== null && expected !== null &&
        typeof actual === 'object' && typeof expected === 'object', 'value');
  check(Object.getPrototypeOf(actual) === Object.getPrototypeOf(expected),
        'prototype');
  const keys = Reflect.ownKeys(expected);
  check(JSON.stringify(Reflect.ownKeys(actual)) === JSON.stringify(keys),
        'key order');
  for (const key of keys) {
    const a = Object.getOwnPropertyDescriptor(actual, key);
    const b = Object.getOwnPropertyDescriptor(expected, key);
    check(a.writable === b.writable && a.enumerable === b.enumerable &&
          a.configurable === b.configurable, 'descriptor');
    equal(a.value, b.value);
  }
}
async function compare(source, useReviver = false) {
  const calls = [[], []];
  function callback(index) {
    return function(key, value, context) {
      calls[index].push([key, context.source]);
      if (key === 'drop') return undefined;
      return value;
    };
  }
  const expected = JSON.parse(source, useReviver ? callback(0) : undefined);
  const actual = await JSON.parseAsync(
      source, useReviver ? callback(1) : undefined);
  equal(actual, expected);
  equal(calls[1], calls[0]);
  record(source);
  record(JSON.stringify(actual));
  record(JSON.stringify(calls[1]));
  ++comparisons;
  return actual;
}
function fields(width, row, wide) {
  return Array.from({length:width}, (_, k) => {
    const key = (wide ? 'field漢' : 'field') + k;
    const values = [String(row), String(row + 0.25), '-0', '"same"',
                    'null', 'true'];
    return JSON.stringify(key) + ':' + values[k % values.length];
  });
}
function rows(records, count = 512) {
  return '[' + Array.from({length:count}, (_, i) =>
      records[i % records.length]).join(',') + ']';
}
async function main() {
  for (const wide of [false, true]) {
    for (const width of [63, 64, 65, 66, 127, 128, 129, 200]) {
      const records = [0, 1, 2].map(i =>
          '{' + fields(width, i, wide).join(',') + '}');
      const parsed = await compare(rows(records));
      gc();
      const key = (wide ? 'field漢' : 'field') + '1';
      parsed[0][key] = 999.5;
      check(parsed[3][key] === 0.25 && parsed[1][key] === 1.25,
            'independent mutable doubles');
      if (width === 64) await compare(rows(records, 32), true);
    }
    const tail = fields(70, 1, wide).join(',');
    // A newly populated foreground cache slot must not match an empty key.
    await compare(rows(['{"id":0,"":1,' + tail + '}',
                        '{"id":1,"Aa":2,' + tail + '}']));
    // Same depth and ordinal can describe different keys. Preserve the first
    // cached key and keep mismatches, escapes, and index keys on their fallback.
    const variants = [
      '{"alpha":1,' + tail + '}',
      '{"beta":2,' + tail + '}',
      '{"\\u0061lpha":3,' + tail + '}',
      '{"0":4,"4294967294":5,"4294967295":6,' + tail + '}',
      '{"":7,"__proto__":{"safe":8},"drop":9,' + tail + '}',
      '{"line\\nkey":10,"quote\\\"key":11,' + tail + '}',
    ];
    await compare(rows(variants));
    await compare(rows(variants, 32), true);
    // Duplicate keys straddle the small-subtree boundary and keep their first
    // enumeration position while their last value wins.
    const duplicate = '{' + tail + ',"field0":-0,"\\u0066ield0":2.25}';
    await compare(rows([duplicate, '{' + tail + '}']));
    // Reuse slots at different container depths and across nested wide records.
    await compare(rows(['{"outer":' + rows(variants, 3) + ',' + tail + '}',
                        '{"other":{' + tail + '},' + tail + '}'], 128));
  }
  // One object itself crosses the deterministic 64K tape-node slice budget.
  const large = '{' + fields(34000, 7, true).join(',') + '}';
  await compare(large);
  const base = '{' + fields(65, 0, false).join(',');
  for (const suffix of [',"bad":}', ',"bad":1e+}',
                        ',"bad":"\\u00xz"}', ',}']) {
    const source = '[' + base + suffix + ']';
    let expected, actual;
    try { JSON.parse(source); } catch (e) { expected = e.message; }
    try { await JSON.parseAsync(source); } catch (e) { actual = e.message; }
    check(expected !== undefined && actual === expected, 'error position');
    record(expected);
    ++comparisons;
  }
  print('async-wide-keys comparisons=' + comparisons +
        ' hash=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
