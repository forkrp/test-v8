// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc, and again with --stress-compaction. Compare the
// deterministic output with an independently frozen baseline d8.
'use strict';

function check(ok, message) { if (!ok) throw Error(message); }
let hash = 2166136261, comparisons = 0;
const bits = new DataView(new ArrayBuffer(8));
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  hash = Math.imul(hash ^ 0xffff, 16777619) >>> 0;
}
function fingerprint(value) {
  record(typeof value);
  if (typeof value === 'number') {
    bits.setFloat64(0, value, true);
    record(bits.getUint32(0, true).toString(16));
    record(bits.getUint32(4, true).toString(16));
  } else if (value === null || typeof value !== 'object') {
    record(String(value));
  } else {
    const array = Array.isArray(value);
    check(Object.getPrototypeOf(value) ===
          (array ? Array.prototype : Object.prototype), 'prototype');
    record(array ? 'array:' + value.length : 'object');
    const keys = Object.keys(value);
    record(String(keys.length));
    for (const key of keys) {
      const desc = Object.getOwnPropertyDescriptor(value, key);
      check(desc.enumerable && desc.configurable && desc.writable &&
            Object.hasOwn(desc, 'value'), 'data property ' + key);
      record(key);
      fingerprint(desc.value);
    }
  }
}
function signature(value) {
  const saved = hash;
  hash = 2166136261;
  fingerprint(value);
  const result = hash;
  hash = saved;
  return result;
}
async function compare(source) {
  const sync = JSON.parse(source);
  const async = await JSON.parseAsync(source);
  check(signature(sync) === signature(async), 'sync/async mismatch');
  fingerprint(sync);
  ++comparisons;
  return [sync, async];
}
function repeated(records, count = 64) {
  return '[' + Array.from({length: count}, (_, i) =>
      records[i % records.length]).join(',') + ']';
}
async function main() {
  // Raw text also covers discovering the shape without JSON.stringify first
  // marking its descriptors as fast iterable.
  const layouts = [
    ['{"a":1,"b":2}', '{"a":3,"b":4}'],
    ['{"a":1.25,"b":2.5}', '{"a":3,"b":-0}'],
    ['{"a":1,"b":2}', '{"a":1.25,"b":2.5}'],
    ['{"a":1,"b":2}', '{"a":"x","b":null}',
     '{"a":{},"b":[]}', '{"a":true,"b":false}'],
    ['{"a":{"x":1},"b":1}', '{"a":{"y":2},"b":1.5}'],
    ['{"a":1,"b":{"a":1.5,"b":2}}',
     '{"a":1.5,"b":{"a":"x","b":2}}'],
    ['{"a":1,"b":2}', '{"b":3,"a":4}'],
    // A newly assigned cache slot is still uninitialized on the foreground
    // thread. Its empty-string sentinel must not match the previous empty key.
    ['{"id":0,"":1}', '{"id":1,"Aa":2}'],
    ['{"a":1,"b":2}', '{"a":3}', '{"a":4,"b":5,"c":6}'],
    ['{"a":1,"a":2}', '{"a":3,"b":4}', '{"a":5,"b":6,"a":7}'],
    ['{"a":1,"b":2}', '{"a":3,"\\u0061":4}'],
    ['{"a":1,"b":2}', '{"a":3,"0":4,"b":5}'],
    ['{"a":1,"0":2}', '{"a":3,"4294967294":4}',
     '{"a":5,"4294967295":6}'],
    ['{"":1,"__proto__":2}', '{"":3,"__proto__":{"a":4}}'],
    ['{"constructor":1,"prototype":2,"toString":3}'],
    ['{"漢":1.5,"é":2}', '{"漢":3,"é":4.5}'],
    ['{"a\\nb":1,"a\\\"b":2}', '{"a\\nb":3.5,"a\\\"b":4}'],
    ['[]', '{"length":1}', '[1,2]', '{"length":2,"x":3}'],
    ['{"a":1e400,"b":-0}', '{"a":5e-324,"b":1.7976931348623157e308}'],
  ];
  for (const records of layouts) {
    for (const prefix of ['', '"漢",']) {
      const text = repeated(records);
      await compare('[' + prefix + text.slice(1));
    }
  }
  // Shared descriptor arrays may contain more keys than the feedback map owns.
  // Cover the small-subtree and 64-slot cache boundaries, plus wide fallback.
  for (const width of [1, 2, 12, 60, 63, 64, 65, 66, 128, 200]) {
    const fields = Array.from({length: width}, (_, i) => '"p' + i + '":' + i);
    const normal = '{' + fields.join(',') + '}';
    const wide = '{' + fields.concat('"extra":1.25').join(',') + '}';
    const reversed = '{' + [...fields].reverse().join(',') + '}';
    for (const rows of [[normal], [wide, normal], [normal, wide],
                        [normal, reversed]]) {
      await compare(repeated(rows, 32));
    }
  }
  // A double field must get independent mutable storage for every record,
  // including when a later Smi fits an already-generalized double field.
  for (const field of ['a', '漢', '__proto__']) {
    const key = JSON.stringify(field);
    const source = repeated(['{' + key + ':1.25,"other":0}',
                             '{' + key + ':2,"other":0}'], 20000);
    const values = await compare(source);
    gc();
    for (const parsed of values) {
      parsed[0][field] = 999.5;
      parsed[1][field] = 888.25;
      check(parsed[2][field] === 1.25 && parsed[3][field] === 2,
            'mutable number alias');
      check(Object.getPrototypeOf(parsed[0]) === Object.prototype,
            'prototype after mutation');
      fingerprint(parsed.slice(0, 4));
    }
  }
  // Vary keys and value types to exercise map changes and fallback after a
  // matching prefix; deterministic output is also checked against the baseline.
  let seed = 0x2b9c1357;
  function random() {
    seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
    return seed;
  }
  const values = ['0', '-0', '1.25', '"x"', 'null', 'true', '{}', '[]'];
  for (let round = 0; round < 100; ++round) {
    const rows = [];
    for (let row = 0; row < 48; ++row) {
      const fields = [];
      const width = 1 + random() % 8;
      for (let field = 0; field < width; ++field) {
        fields.push('"k' + (random() % 7) + '":' +
                    values[random() % values.length]);
      }
      rows.push('{' + fields.join(',') + '}');
    }
    await compare('[' + rows.join(',') + ']');
  }
  const reviverText = '[{"x":1.25,"y":2},{"x":3,"y":4},' +
                      '{"x":-0,"y":5},{"x":6.5,"y":7}]';
  async function withReviver(asynchronous) {
    const calls = [];
    const reviver = function(key, value, context) {
      calls.push([key, context.source]);
      if (key === 'x') this.y = 9.25;
      if (key === 'y') return undefined;
      return value;
    };
    const parsed = asynchronous ? await JSON.parseAsync(reviverText, reviver) :
        JSON.parse(reviverText, reviver);
    return [parsed, calls];
  }
  const reviverSync = await withReviver(false);
  const reviverAsync = await withReviver(true);
  check(signature(reviverSync) === signature(reviverAsync), 'reviver');
  fingerprint(reviverSync);
  for (const suffix of ['{"a":}', '{"a":1,}', '{"a":1 "b":2}',
                        '{"a":1,"b":2,', '{"a":1,"b":2e+}',
                        '{"a":1,"b":"\\u00xz"}']) {
    const text = '[{"a":1,"b":2},{"a":3,"b":4},' + suffix + ']';
    let expected, actual;
    try { JSON.parse(text); } catch (e) {
      check(e instanceof SyntaxError, 'sync SyntaxError'); expected = e.message;
    }
    try { await JSON.parseAsync(text); } catch (e) {
      check(e instanceof SyntaxError, 'async SyntaxError'); actual = e.message;
    }
    check(expected !== undefined && expected === actual, 'error position');
    record(expected);
    ++comparisons;
  }
  print('object-build comparisons=' + comparisons + ' hash=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
