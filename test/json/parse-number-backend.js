// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc; repeat with --stress-compaction and --verify-heap.
'use strict';

function check(ok, message) { if (!ok) throw Error(message); }
const view = new DataView(new ArrayBuffer(8));
function fromBits(bits) {
  view.setBigUint64(0, bits, true);
  return view.getFloat64(0, true);
}
const cases = [];
function add(text, expected = Number(text)) {
  cases.push({text, expected});
}

// Construct exact decimal midpoints using integer arithmetic, independently of
// both number conversion implementations. Check either side and ties to even.
const lowerBits = [0n, 1n, 2n, 0xffffffffffffen, 0xfffffffffffffn,
                   0x10000000000000n, 0x3fefffffffffffffn,
                   0x3ff0000000000000n, 0x3ff0000000000001n,
                   0x7feffffffffffffen, 0x7fefffffffffffffn];
for (const exponent of [1, 2, 20, 500, 1000, 1023, 1100, 1500, 2000, 2046]) {
  for (const fraction of [0n, 1n, 0x123456789abcdn, 0xfffffffffffffn]) {
    lowerBits.push((BigInt(exponent) << 52n) | fraction);
  }
}
let exactChecks = 0;
for (const bits of lowerBits) {
  const encodedExponent = Number(bits >> 52n);
  const significand = (bits & 0xfffffffffffffn) +
      (encodedExponent === 0 ? 0n : 0x10000000000000n);
  const exponent = (encodedExponent === 0 ? -1074 :
      encodedExponent - 1023 - 52) - 1;
  let midpoint = 2n * significand + 1n, suffix = '';
  if (exponent < 0) {
    midpoint *= 5n ** BigInt(-exponent);
    suffix = 'e' + exponent;
  } else {
    midpoint <<= BigInt(exponent);
  }
  for (const delta of [-1n, 0n, 1n]) {
    const expectedBits = delta < 0n ? bits : delta > 0n ? bits + 1n :
        (bits & 1n) === 0n ? bits : bits + 1n;
    const text = String(midpoint + delta) + suffix;
    const expected = fromBits(expectedBits);
    add(text, expected);
    add('-' + text, -expected);
    exactChecks += 2;
  }
}

for (const text of ['-0', '-0.0', '-0e999999999999999999999',
                   '0e-999999999999999999999', '1e999999999999999999999',
                   '-1e-999999999999999999999', '5e-324',
                   '2.2250738585072012e-308', '1.7976931348623158e308',
                   '9007199254740993', '18446744073709551615',
                   '1.' + '0'.repeat(8192) + '1',
                   '0.' + '0'.repeat(8192) + '1',
                   '9'.repeat(8192) + 'e-8192']) add(text);

let seed = 0x51a8432d;
function random() {
  seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
  return seed >>> 0;
}
const lengths = [1, 9, 15, 16, 17, 18, 19, 20, 38, 64, 128, 768, 1024];
for (let i = 0; i < 20000; ++i) {
  const length = lengths[random() % lengths.length];
  let digits = String(1 + random() % 9);
  while (digits.length < length) digits += String(random() % 10);
  const split = random() % (length + 1);
  let text = split === 0 ? '0.' + digits : split === length ? digits :
      digits.slice(0, split) + '.' + digits.slice(split);
  if (random() & 1) text = '-' + text;
  if (random() & 1) text += 'e' + ((random() % 2401) - 1200);
  add(text);
}

let hash = 2166136261, checks = 0;
function verify(actual, expected, text) {
  check(Object.is(actual, expected), 'numeric mismatch: ' + text);
  view.setFloat64(0, actual, true);
  hash = Math.imul(hash ^ view.getUint32(0, true), 16777619) >>> 0;
  hash = Math.imul(hash ^ view.getUint32(4, true), 16777619) >>> 0;
  ++checks;
}
async function main() {
  for (const {text, expected} of cases) {
    verify(JSON.parse(text), expected, text);
  }
  for (let start = 0; start < cases.length; start += 127) {
    const group = cases.slice(start, start + 127);
    for (const wide of [false, true]) {
      const text = '[' + (wide ? '"漢",' : '') +
          group.map(c => c.text).join(',') + ']';
      const sync = JSON.parse(text), async = await JSON.parseAsync(text);
      for (let i = 0; i < group.length; ++i) {
        const at = i + (wide ? 1 : 0), c = group[i];
        verify(sync[at], c.expected, c.text);
        verify(async[at], c.expected, c.text);
      }
    }
  }
  gc();
  print('number backend cases=' + cases.length + ' exact=' + exactChecks +
        ' checks=' + checks + ' bits=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
