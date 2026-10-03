// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc; also exercise --stress-compaction.
'use strict';
function check(condition, message) { if (!condition) throw new Error(message); }
async function main() {
  const positions = [0, 7, 8, 15, 16, 31, 32, 4094, 4095, 4096, 4097, 8191];
  const specials = ['\0', '\n', '\t', '"', '\\', '\ud7ff', '\ud800', '\udbff',
                    '\udc00', '\udfff', '\ue000', '😀'];
  let strings = 0;
  for (const fill of ['a', '漢']) for (const at of positions) {
    for (const special of specials) {
      const value = fill.repeat(at) + special + 'abc漢😀xyz';
      for (const input of [value, [value, 'a'], {[value]: value}]) {
        const expected = JSON.stringify(input);
        check(await JSON.stringifyAsync(input) === expected, 'string boundary');
        check(JSON.stringify(await JSON.parseAsync(expected)) === expected,
              'parse boundary');
        ++strings;
      }
      for (const control of ['\0', '\n', '\t']) {
        const text = '"' + fill.repeat(at) + control + '"';
        let expected, actual;
        try { JSON.parse(text); } catch (e) { expected = e.name + ':' + e.message; }
        try { await JSON.parseAsync(text); }
        catch (e) { actual = e.name + ':' + e.message; }
        check(expected && actual === expected, 'error position/message');
      }
    }
  }
  const parts = ['-0', '999999999', '1000000000', '1073741823', '1073741824',
    '2147483647', '2147483648', '4294967295', '4294967296', '9999999999',
    '10000000000', '-1000000000', '-2147483648', '-4294967295', '-4294967296',
    '1000000000.125', '4294967295e1', '4294967295e-1', '1e20', '1e400'];
  let seed = 0x12345678;
  for (let i = 0; i < 3000; ++i) {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    parts.push(String(seed), '-' + seed);
  }
  const text = '[' + parts.join(',') + ']';
  // Use Number() as an independent oracle for both JSON numeric parsers.
  const expected = parts.map(Number), synchronous = JSON.parse(text);
  const actual = await JSON.parseAsync(text);
  check(actual.length === expected.length, 'number count');
  for (let i = 0; i < actual.length; ++i) {
    check(Object.is(actual[i], expected[i]), 'async integer boundary/rounding');
    check(Object.is(synchronous[i], expected[i]), 'sync integer boundary/rounding');
  }
  for (const source of parts.slice(0, 20)) {
    const seen = [];
    await JSON.parseAsync(source, (key, value, context) => {
      seen.push(context.source); return value;
    });
    check(seen.length === 1 && seen[0] === source, 'reviver source');
  }
  // Double fields must remain uniquely owned across repeated numeric values.
  const records = await JSON.parseAsync('[{"n":3.5,"id":1},' +
      '{"n":0,"id":1},'.repeat(3000) + '{"n":0,"id":1}]');
  gc();
  records[1].n = 7.25;
  records[1].id = 31;
  check(records[0].n === 3.5 && records[2].n === 0 && records[2].id === 1,
        'independent fields after GC');
  const doubles = await JSON.parseAsync('[' +
      '{"n":3.5},'.repeat(3000) + '{"n":3.5}]');
  doubles[0].n = 8.5;
  check(doubles[1].n === 3.5, 'unique mutable double fields');
  for (const input of [Array.from({length: 20000}, (_, i) => i),
      Array.from({length: 20000}, (_, i) => i % 7 === 0 ? NaN : i / 8),
      Array.from({length: 1000}, () => [1, 2, -3, 4.5, -0])]) {
    check(await JSON.stringifyAsync(input) === JSON.stringify(input),
          'numeric spans');
  }
  print('PASS async Android boundaries: ' + strings + ' strings, ' +
        parts.length + ' numbers, mutable-field independence, numeric spans');
}
main().catch(error => { print(error.stack || error); quit(1); });
