// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with --expose-gc, optionally --stress-compaction / --verify-heap.
// Compare the deterministic result with the frozen pre-change binary.
'use strict';

function check(ok, reason) { if (!ok) throw Error(reason); }
let hash = 2166136261, comparisons = 0;
const bits = new DataView(new ArrayBuffer(8));
function record(text) {
  for (let i = 0; i < text.length; ++i)
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
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
    const keys = Object.keys(value);
    record((array ? 'array' : 'object') + keys.length);
    for (const key of keys) {
      const d = Object.getOwnPropertyDescriptor(value, key);
      check(d.writable && d.enumerable && d.configurable &&
            Object.hasOwn(d, 'value'), 'data attributes');
      record(key);
      fingerprint(d.value);
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
function fields(width, wide, mode = 0) {
  return Array.from({length: width}, (_, i) => {
    const key = (wide ? '漢' : 'f') + i;
    const value = mode === 1 ? String(i + 0.25) : mode === 2 ? String(i) :
        [String(i), String(i + 0.25), '-0', 'true', 'null',
         '"text"', '1e400', '{"child":[1,2.5]}'][i % 8];
    return JSON.stringify(key) + ':' + value;
  });
}
function object(parts) { return '{' + parts.join(',') + '}'; }
function rows(variants, count = 8) {
  return '[' + Array.from({length: count}, (_, i) =>
      variants[i % variants.length]).join(',') + ']';
}

(async function() {
  async function compare(text, reviver) {
    const a = JSON.parse(text, reviver);
    const b = await JSON.parseAsync(text, reviver);
    check(signature(a) === signature(b), 'sync/async');
    fingerprint(a);
    ++comparisons;
    return [a, b];
  }
  for (const width of [127, 128, 129, 192, 252, 253, 256, 512, 1024]) {
    for (const wide of [false, true]) {
      const f = fields(width, wide), key = (wide ? '漢' : 'f') + '0';
      const normal = object(f);
      const duplicate = object([...f, JSON.stringify(key) + ':9.25',
                                 (wide ? '"\\u6f220"' : '"\\u00660"') + ':-0']);
      const indexed = object(['"0":1', ...f, '"3":2',
                              '"4294967294":3', '"4294967295":4', '"0":5']);
      const special = object([...f, '"":1', '"__proto__":{"own":true}',
          '"constructor":2', '"prototype":3', '"toString":4',
          '"toJSON":null', '"quote\\\"key":5', '"line\\nkey":6']);
      for (const variants of [[normal], [normal, duplicate],
          [object([...f].reverse()), normal], [indexed, special],
          [normal, object([...f, '"extra":1.5'])],
          [object(fields(width, wide, 1)), object(fields(width, wide, 2)), normal]]) {
        await compare(rows(variants));
      }
      await compare(normal);
      // Dictionary values must remain independent when a later assignment
      // changes a double or reconfigures a property after construction.
      const values = await compare(rows([object(fields(width, wide, 1))], 32));
      gc();
      for (const parsed of values) {
        parsed[0][key] = 99.5;
        Object.defineProperty(parsed[1], key, {value: 88.25});
        check(parsed[2][key] === 0.25, 'number alias');
        check(delete parsed[0][key], 'delete');
        parsed[0][key] = -0;
        check(Object.keys(parsed[0]).at(-1) === key, 'reinsert order');
        fingerprint(parsed.slice(0, 3));
      }
    }
  }
  // Raw counts straddle 128 even though duplicates leave fewer unique names.
  for (const width of [63, 64, 65, 127, 128, 192]) {
    const f = fields(width, false, 2);
    await compare(rows([object(f), object([...f, ...f, '"f0":-0'])]));
  }
  await compare(object(fields(4096, false)));
  const reviverText = rows([object(fields(192, true))], 4);
  async function revive(asynchronous) {
    const calls = [];
    function reviver(key, value, context) {
      calls.push([key, context.source]);
      if (key === '漢0') this['漢1'] = 77.5;
      if (key === '漢3') return undefined;
      return value;
    }
    const value = asynchronous ? await JSON.parseAsync(reviverText, reviver) :
        JSON.parse(reviverText, reviver);
    return [value, calls];
  }
  const revived = await revive(false);
  check(signature(revived) === signature(await revive(true)), 'reviver source');
  fingerprint(revived);
  for (const width of [128, 192, 512]) {
    const prefix = '{' + fields(width, false).join(',');
    for (const tail of [',"bad":}', ',"bad":1e+}', ',"bad":"\\u00xz"}', ',}']) {
      const text = '[' + prefix + tail + ']';
      let message;
      try { JSON.parse(text); } catch (error) {
        check(error instanceof SyntaxError, 'syntax type'); message = error.message;
      }
      try { await JSON.parseAsync(text); throw Error('async accepted'); }
      catch (error) { check(error.message === message, 'error position'); }
      check(message !== undefined, 'sync accepted');
      record(message);
      ++comparisons;
    }
  }
  print('wide-dictionary comparisons=' + comparisons + ' hash=' + hash.toString(16));
})().catch(error => { print(error.stack); quit(1); });
