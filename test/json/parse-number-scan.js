// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc. Compare the complete output with a frozen reference.
'use strict';
let hash = 2166136261, errors = 0, values = 0;
function check(value, message) { if (!value) throw Error(message); }
function record(text) {
  for (let i = 0; i < text.length; ++i)
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
}
const bad = new Set(['-', '--1', '-.1', '+1', '00', '01', '-01', '0x1',
  '0o1', '0b1', '1.', '1.e2', '1e', '1e+', '1e-', '1e2.3', '1e2e3',
  '1+2', '1-2', '1_2', '1.2_3', 'NaN', 'Infinity', '-Infinity',
  '1e2x', '1e2\u0000', '1e2\u2028', '1.23\ud800']);
for (const digits of [1, 8, 9, 10, 15, 16, 19, 20, 128, 4097]) {
  const prefix = '9'.repeat(digits);
  for (const suffix of ['.', '.e1', 'e', 'e+', 'e-', 'e+1x', 'e-1.']) {
    bad.add(prefix + suffix);
    bad.add('-' + prefix + suffix);
  }
}
function contexts(text) {
  return [text, '[' + text + ']', '[0,' + text + ',1]',
          '["漢",' + text + ']', '{"漢":0,"x":' + text + '}',
          '{"x":[' + text + '],"漢":1}'];
}
async function main() {
  for (const literal of bad) {
    for (const source of contexts(literal)) {
      let expected, actual;
      try { JSON.parse(source); } catch (e) {
        check(e instanceof SyntaxError, 'sync exception type');
        expected = e.message;
      }
      check(expected !== undefined, 'malformed literal accepted');
      try { await JSON.parseAsync(source); } catch (e) {
        check(e instanceof SyntaxError, 'async exception type');
        actual = e.message;
      }
      check(expected === actual, 'error position/message');
      // Include every message in the frozen-reference comparison, not just
      // agreement between two APIs in the candidate.
      print(JSON.stringify({source, error:expected}));
      record(expected); ++errors;
    }
  }
  const literals = ['0', '-0', '-0.0', '1.0000', '999999999', '1000000000',
    '12345678901234567', '1e+00', '-0e-000', '1.23E+002', '5e-324',
    '1e400', '-1e-400', '1e000000000000000000000000000000022',
    '0.' + '0'.repeat(4097) + '1'];
  for (const wide of [false, true]) {
    const prefix = wide ? '"漢",' : '';
    const source = '[' + prefix + literals.join(',') + ']';
    for (const asynchronous of [false, true]) {
      const calls = [];
      const reviver = (key, value, context) => {
        if (typeof value === 'number') {
          const literal = literals[Number(key) - (wide ? 1 : 0)];
          check(context.source === literal, 'reviver source');
          check(Object.is(value, Number(literal)), 'reviver numeric bits');
          calls.push([key, context.source]); ++values;
        }
        return value;
      };
      if (asynchronous) await JSON.parseAsync(source, reviver);
      else JSON.parse(source, reviver);
      record(JSON.stringify(calls));
    }
  }
  // Raw JSON validation shares ParseJsonNumber but does not allow surrounding
  // whitespace or a trailing token. Preserve its exact validation behavior.
  if (typeof JSON.rawJSON === 'function') {
    for (const literal of bad) {
      let error;
      try { JSON.rawJSON(literal); } catch (e) { error = e.message; }
      check(error !== undefined, 'raw JSON malformed number accepted');
      print(JSON.stringify({raw:literal, error})); record(error); ++errors;
    }
    for (const literal of literals) {
      check(JSON.stringify(JSON.rawJSON(literal)) === literal,
            'raw JSON source preserved');
      for (const source of [' ' + literal, literal + ' ', literal + ',0']) {
        let error;
        try { JSON.rawJSON(source); } catch (e) { error = e.message; }
        check(error !== undefined, 'raw JSON trailing/prefix accepted');
        print(JSON.stringify({raw:source, error})); record(error); ++errors;
      }
    }
  }
  gc();
  print('number scan errors=' + errors + ' reviver-values=' + values +
        ' hash=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
