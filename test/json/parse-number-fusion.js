// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc, and again with --stress-compaction.
'use strict';

function check(ok, message) { if (!ok) throw Error(message); }
const literals = new Set([
  '0', '-0', '-0.0', '-0e22', '-0e-22', '1e400', '-1e400',
  '1e-400', '-1e-400', '5e-324', '2.2250738585072014e-308',
  '1.7976931348623157e308', '1.7976931348623159e308',
  '9007199254740991', '9007199254740992', '9007199254740993',
  '1e000000000000000000000000000000000000022',
  '1e-000000000000000000000000000000000000022',
  '0.' + '0'.repeat(4096) + '1',
  '1' + '0'.repeat(4096), '1.' + '0'.repeat(4096),
]);
for (const digits of ['1', '9', '10', '999999999', '1000000000',
                      '1073741823', '1073741824', '2147483647',
                      '99999999999999', '100000000000000',
                      '999999999999999', '1000000000000000',
                      '9999999999999999', '12345678901234567']) {
  for (let split = 0; split <= digits.length; ++split) {
    const base = split === 0 ? '0.' + digits : split === digits.length ? digits :
        digits.slice(0, split) + '.' + digits.slice(split);
    for (const exponent of ['', 'e0', 'e-0', 'E+1', 'e-1', 'e22', 'e-22',
                            'e23', 'e-23', 'e37', 'e-37', 'e308', 'e-324',
                            'e00022', 'e+999', 'e-999']) {
      literals.add(base + exponent);
      literals.add('-' + base + exponent);
    }
  }
}
let seed = 0x518e34a9;
function random() { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed; }
for (let i = 0; i < 12000; ++i) {
  const length = 1 + random() % 22;
  let digits = String(1 + random() % 9);
  while (digits.length < length) digits += String(random() % 10);
  const split = random() % (length + 1);
  let text = split === 0 ? '0.' + digits : split === length ? digits :
      digits.slice(0, split) + '.' + digits.slice(split);
  if (random() & 1) text = '-' + text;
  if (random() & 1) text += 'e' + ((random() % 801) - 400);
  literals.add(text);
}

let hash = 2166136261;
const bits = new DataView(new ArrayBuffer(8));
function record(value) {
  bits.setFloat64(0, value, true);
  hash = Math.imul(hash ^ bits.getUint32(0, true), 16777619) >>> 0;
  hash = Math.imul(hash ^ bits.getUint32(4, true), 16777619) >>> 0;
}

async function main() {
  const texts = [...literals];
  for (const text of texts) {
    const actual = JSON.parse(text);
    check(Object.is(actual, Number(text)), 'sync numeric bits: ' + text);
    record(actual);
  }
  for (let start = 0; start < texts.length; start += 127) {
    const group = texts.slice(start, start + 127);
    for (const tail of ['', ',"漢"']) {
      const input = '[' + group.join(',') + tail + ']';
      const sync = JSON.parse(input);
      const async = await JSON.parseAsync(input);
      for (let i = 0; i < group.length; ++i) {
        check(Object.is(sync[i], Number(group[i])), 'sync array numeric bits');
        check(Object.is(async[i], sync[i]), 'async array numeric bits');
        record(async[i]);
      }
    }
  }
  let errors = 0;
  for (const text of ['00', '-00', '01', '+1', '1.', '1.e2', '1e', '1e+',
                      '1e-', '1e1.2', '1.2.3', '1e2x', '1e2\u0000',
                      '999999999999999.', '999999999999999e+',
                      '1.' + '0'.repeat(4096) + 'e-']) {
    for (const source of [text, '[' + text + ']', '["漢",' + text + ']']) {
      let expected;
      try { JSON.parse(source); } catch (e) { expected = e.message; }
      check(expected !== undefined, 'accepted malformed number');
      let actual;
      try { await JSON.parseAsync(source); } catch (e) { actual = e.message; }
      check(actual === expected, 'async numeric error offset');
      print('error:' + expected);
      ++errors;
    }
  }
  const source = '{"x":[-0,123456789012345e-22,1.2345e22,1e400],"漢":0.125}';
  const calls = [];
  const sync = JSON.parse(source, (key, value, context) => {
    calls.push([key, context.source]); return value;
  });
  const asyncCalls = [];
  const async = await JSON.parseAsync(source, (key, value, context) => {
    asyncCalls.push([key, context.source]); return value;
  });
  check(JSON.stringify(calls) === JSON.stringify(asyncCalls), 'reviver source/order');
  for (let i = 0; i < sync.x.length; ++i) check(Object.is(sync.x[i], async.x[i]), 'reviver value');
  gc();
  print('number fusion checks passed: ' + texts.length + ' numbers, ' + errors +
        ' errors; bits=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
