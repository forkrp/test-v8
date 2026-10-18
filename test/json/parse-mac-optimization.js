// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc, and again with --stress-compaction. Output is a stable
// oracle for comparison with an independently frozen baseline d8.
'use strict';

function check(condition, message) {
  if (!condition) throw Error(message);
}

function signature(value) {
  let hash = 2166136261;
  const bits = new DataView(new ArrayBuffer(8));
  function text(s) {
    for (let i = 0; i < s.length; ++i) {
      hash = Math.imul(hash ^ s.charCodeAt(i), 16777619) >>> 0;
    }
    hash = Math.imul(hash ^ 0xffff, 16777619) >>> 0;
  }
  function visit(v) {
    text(typeof v);
    if (typeof v === 'number') {
      bits.setFloat64(0, v, true);
      text(bits.getUint32(0, true).toString(16));
      text(bits.getUint32(4, true).toString(16));
    } else if (v === null || typeof v !== 'object') {
      text(String(v));
    } else {
      text(Array.isArray(v) ? 'array:' + v.length : 'object');
      const keys = Object.keys(v);
      text(String(keys.length));
      for (const key of keys) {
        text(key);
        visit(v[key]);
      }
    }
  }
  visit(value);
  return hash.toString(16);
}

let cases = 0;
async function compare(source, name) {
  const expected = JSON.parse(source);
  const actual = await JSON.parseAsync(source);
  const oracle = signature(expected);
  check(signature(actual) === oracle, name + ' async mismatch');
  print(name + ':' + oracle);
  ++cases;
  return expected;
}

async function invalid(source, name) {
  let expected;
  try { JSON.parse(source); } catch (error) {
    check(error instanceof SyntaxError, name + ' sync error type');
    expected = error.message;
  }
  check(expected !== undefined, name + ' accepted malformed input');
  let actual;
  try { await JSON.parseAsync(source); } catch (error) {
    check(error instanceof SyntaxError, name + ' async error type');
    actual = error.message;
  }
  check(actual === expected, name + ' error position or message');
  print(name + ':' + signature(expected));
  ++cases;
}

async function main() {
  const patterns = [
    ['0', '1', '-1', '1073741823', '-1073741824'],
    ['0.125', '-0', '1.25', '1e20', '-1e20'],
    ['0', '1', '2', '3.5', '4', '5'],
    ['1e400', '-1e400', '1e-400', '5e-324',
     '9007199254740993', '1.7976931348623157e308'],
  ];
  const lengths = [0, 1, 2, 15, 16, 17, 63, 64, 65, 126, 127, 128, 129, 512];
  for (let p = 0; p < patterns.length; ++p) {
    for (const length of lengths) {
      const parts = Array.from({length}, (_, i) => patterns[p][i % patterns[p].length]);
      const source = '[' + parts.join(',') + ']';
      const value = await compare(source, 'numbers-' + p + '-' + length);
      check(value.length === length, 'numeric length');
      for (let i = 0; i < length; ++i) {
        check(Object.is(value[i], Number(parts[i])), 'numeric bit pattern ' + i);
      }
      await compare('[' + source + ',' + source + ']', 'nested-' + p + '-' + length);
      for (const suffix of ['null', '"tail"', '{"x":1.25}', '[2.5,-0]']) {
        const mixed = '[' + (parts.length ? parts.join(',') + ',' : '') + suffix + ']';
        await compare(mixed, 'mixed-' + p + '-' + length + '-' + suffix);
      }
      if (length) {
        await invalid(source.slice(0, -1) + ',]', 'trailing-' + p + '-' + length);
        await invalid(source.slice(0, -1) + ',1e+]', 'exponent-' + p + '-' + length);
      }
    }
  }

  for (const length of [0, 1, 7, 8, 15, 16, 31, 32, 33, 47, 48, 63, 64, 65,
                        127, 128, 129, 4095, 4096, 4097]) {
    for (const unit of ['a', '\u00ff', '\u0100', '\u6f22', '\ud800', '\udc00', '\ud83d\ude00']) {
      const value = unit.repeat(length);
      const source = JSON.stringify(value);
      await compare(source, 'string-' + unit.charCodeAt(0) + '-' + length);
      if (unit.charCodeAt(0) >= 0xd800 && unit.charCodeAt(0) <= 0xdfff) {
        // Stringify escapes lone surrogates; also exercise literal UTF-16
        // code units in short values and property names.
        await compare('"' + value + '"',
                      'raw-string-' + unit.charCodeAt(0) + '-' + length);
        await compare('{"' + value + '":1}',
                      'raw-key-' + unit.charCodeAt(0) + '-' + length);
      }
      const sliced = ('漢'.repeat(64) + source + '後'.repeat(64)).slice(64, -64);
      await compare(sliced, 'sliced-' + unit.charCodeAt(0) + '-' + length);
    }
    await compare('"' + 'a'.repeat(length) + '\\n\\u0100\\t\\u00ff"',
                  'escape-tail-' + length);
    await invalid('"' + 'a'.repeat(length) + '\\u00G0"', 'bad-unicode-' + length);
    await invalid('"' + 'a'.repeat(length), 'unterminated-' + length);
    await invalid('"' + 'a'.repeat(length) + '\u0001"', 'control-' + length);
    // Characters beyond the closing quote must not affect the string's width.
    await compare('["' + 'a'.repeat(length) + '","漢"]', 'width-tail-' + length);
  }

  for (const source of ['[1,2.5,-0,1e400]',
                        '{"x":[1,2.5,-0],"y":{"z":9007199254740993}}']) {
    const observations = [];
    const expected = JSON.parse(source, function(key, value, context) {
      observations.push([key, context.source]);
      return value;
    });
    const asyncObservations = [];
    const actual = await JSON.parseAsync(source, function(key, value, context) {
      asyncObservations.push([key, context.source]);
      return value;
    });
    check(signature([actual, asyncObservations]) === signature([expected, observations]),
          'reviver source and order');
    print('reviver:' + signature([expected, observations]));
  }
  const source = JSON.stringify(Array.from({length: 20000}, (_, i) =>
      ({id: i, values: [i + 0.25, -0, i + 0.5]})));
  const values = await JSON.parseAsync(source);
  gc();
  values[0].values[0] = 123.75;
  check(values[1].values[0] === 1.25 && values[19999].values[2] === 19999.5,
        'independent mutable arrays after GC');
  print('parse optimization checks passed: ' + cases);
}

main().catch(error => { print(error.stack || error); quit(1); });
