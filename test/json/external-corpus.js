// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with tools/json_correctness/external.py and its pinned fixture generator.
'use strict';

const api = arguments[0];
if (typeof JSON[api] !== 'function') throw Error('Missing API: ' + api);
const parsing = api === 'parse' || api === 'parseAsync';
const buffer = new DataView(new ArrayBuffer(8));
function hex(s) {
  const result = [];
  for (let i = 0; i < s.length; ++i) result.push(s.charCodeAt(i).toString(16).padStart(4, '0'));
  return result.join('');
}
function wire(value) {
  // Upstream adversarial cases include deep nesting. Keep the result encoder
  // off the JS call stack so its stack limit cannot masquerade as an API error.
  const out = [], tasks = [{value}];
  while (tasks.length) {
    const task = tasks.pop();
    if ('text' in task) { out.push(task.text); continue; }
    const v = task.value;
    if (v === null) { out.push('null'); continue; }
    if (typeof v === 'boolean') { out.push(v ? 'true' : 'false'); continue; }
    if (typeof v === 'string') { out.push('s' + hex(v)); continue; }
    if (typeof v === 'number') {
      buffer.setFloat64(0, v, false);
      out.push('n' + buffer.getBigUint64(0, false).toString(16).padStart(16, '0'));
      continue;
    }
    const array = Array.isArray(v);
    if (Object.getPrototypeOf(v) !== (array ? Array.prototype : Object.prototype)) {
      throw Error('Unexpected prototype');
    }
    const keys = Reflect.ownKeys(v);
    if (array && keys.length !== v.length + 1) throw Error('Array holes or extra keys');
    for (const key of keys) {
      const d = Object.getOwnPropertyDescriptor(v, key);
      if (!('value' in d) || !d.writable ||
          ((key !== 'length' || !array) && (!d.enumerable || !d.configurable))) {
        throw Error('Unexpected descriptor');
      }
    }
    const children = array ? keys.filter(k => k !== 'length') : keys;
    out.push(array ? 'a[' : 'o{');
    tasks.push({text: array ? ']' : '}'});
    for (let i = children.length - 1; i >= 0; --i) {
      const k = children[i];
      tasks.push({value: v[k]});
      if (!array) tasks.push({text: 's' + hex(k) + ':'});
      if (i !== 0) tasks.push({text: ','});
    }
  }
  return out.join('');
}
function hydrate(m) {
  switch (m[0]) {
    case 'null': return null;
    case 'b': case 's': return m[1];
    case 'n':
      buffer.setBigUint64(0, BigInt('0x' + m[1]), false);
      return buffer.getFloat64(0, false);
    case 'a': return m[1].map(hydrate);
    case 'o': {
      const value = {};
      for (const [key, child] of m[1]) {
        Object.defineProperty(value, key, {value: hydrate(child), writable: true,
            enumerable: true, configurable: true});
      }
      return value;
    }
    default: throw Error('Bad model');
  }
}
// Independent canonical string quoting, including well-formed lone surrogates.
function quote(s) {
  const out = ['"'];
  const short = {8:'\\b',9:'\\t',10:'\\n',12:'\\f',13:'\\r',34:'\\"',92:'\\\\'};
  for (let i = 0; i < s.length; ++i) {
    const c = s.charCodeAt(i);
    if (short[c]) out.push(short[c]);
    else if (c < 32) out.push('\\u' + c.toString(16).padStart(4, '0'));
    else if (c >= 0xd800 && c <= 0xdbff && i+1 < s.length &&
             s.charCodeAt(i+1) >= 0xdc00 && s.charCodeAt(i+1) <= 0xdfff) {
      out.push(s[i], s[++i]);
    } else if (c >= 0xd800 && c <= 0xdfff) out.push('\\u' + c.toString(16).padStart(4, '0'));
    else out.push(s[i]);
  }
  out.push('"');
  return out.join('');
}
async function largeCase(row) {
  const {size, seed, invalid} = row.large;
  const token = quote(seed);
  // ASCII imported seed: JS source length equals the intended UTF-8 byte size.
  if (/[^\x00-\x7f]/.test(token)) throw Error('Large seed must be ASCII');
  const count = Math.floor((size - 1) / (token.length + 1));
  const canonical = '[' + (token + ',').repeat(count - 1) + token + ']';
  if (parsing) {
    let text = canonical + ' '.repeat(size - canonical.length);
    if (invalid) {
      const at = 1 + Math.floor(count / 2) * (token.length + 1) + 2;
      text = text.slice(0, at) + '\x01' + text.slice(at + 1);
      try { await JSON[api](text); }
      catch (e) {
        if (e instanceof SyntaxError) return 'throw:SyntaxError';
        throw e;
      }
      throw Error('Accepted corrupt large input: ' + row.id);
    }
    const result = await JSON[api](text);
    if (!Array.isArray(result) || result.length !== count ||
        Object.getPrototypeOf(result) !== Array.prototype ||
        Reflect.ownKeys(result).length !== count + 1) throw Error('Large shape mismatch');
    for (let i = 0; i < count; ++i) {
      const d = Object.getOwnPropertyDescriptor(result, String(i));
      if (!d || d.value !== seed || !d.writable || !d.enumerable || !d.configurable) {
        throw Error('Large value/descriptor mismatch at ' + i);
      }
    }
  } else {
    const result = await JSON[api](Array(count).fill(seed));
    if (result !== canonical) throw Error('Large output mismatch: ' + row.id);
  }
  return 'large-ok:' + size + ':' + count;
}
async function main() {
  let count = 0;
  for (const row of externalCases) {
    if (!parsing && !row.model && (!row.large || row.large.invalid)) continue;
    let actual;
    if (row.large) {
      actual = await largeCase(row);
    } else if (parsing) {
      // Catch only the API error. A wire/assertion failure must fail the suite.
      let value;
      try { value = await JSON[api](row.text); }
      catch (error) {
        if (!(error instanceof SyntaxError)) throw error;
        actual = 'throw:SyntaxError';
      }
      if (actual === undefined) actual = wire(value);
      if (row.expected !== null && actual !== row.expected) throw Error('Parse mismatch: ' + row.id +
          ' expected=' + row.expected.slice(0, 180) + ' actual=' + actual.slice(0, 180));
    } else {
      const value = hydrate(row.model);
      if (wire(value) !== row.expected) throw Error('Fixture hydration mismatch: ' + row.id);
      const result = await JSON[api](value);
      if (typeof result !== 'string') throw Error('Missing serialized output: ' + row.id);
      if (typeof value === 'string' && result !== quote(value)) {
        throw Error('String escape mismatch: ' + row.id);
      }
      actual = 's' + hex(result);
    }
    print('CASE\t' + row.id + '\t' + actual);
    if ((++count & 1023) === 0) gc();
  }
  print('DONE\t' + count);
}
main().catch(error => { print('FAIL\t' + error.stack); quit(1); });
