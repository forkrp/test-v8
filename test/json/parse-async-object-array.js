// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Root-object array differential cases. Run separately with the original
// JSON.parse and candidate parse/parseAsync; compare every output byte.
'use strict';
const api = arguments[0] || 'parseAsync';
const only = arguments[1] || 'all';
const Mi = 1024 * 1024;
function check(x, why) { if (!x) throw Error(why); }
function hex(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) out += s.charCodeAt(i).toString(16).padStart(4,'0');
  return out;
}
const bits = new DataView(new ArrayBuffer(8));
function encode(v) {
  if (typeof v === 'number') {
    bits.setFloat64(0, v, false);
    return 'n' + bits.getUint32(0).toString(16).padStart(8,'0') + bits.getUint32(4).toString(16).padStart(8,'0');
  }
  if (v === null) return 'null';
  if (typeof v === 'string') return 's' + hex(v);
  if (typeof v !== 'object') return typeof v + ':' + String(v);
  check(Object.getPrototypeOf(v) === (Array.isArray(v) ? Array.prototype : Object.prototype), 'prototype');
  const keys = Reflect.ownKeys(v);
  let out = Array.isArray(v) ? 'a' : 'o';
  for (const k of keys) {
    check(typeof k === 'string', 'symbol key');
    const d = Object.getOwnPropertyDescriptor(v, k);
    check('value' in d && !('get' in d), 'data descriptor');
    out += '(' + hex(k) + ':' + [+d.writable,+d.enumerable,+d.configurable].join('') + encode(d.value) + ')';
  }
  return out;
}
function array(size, wide, variant = 0) {
  const parts = Array.from({length:64}, (_, i) => {
    if (variant === 1) return i === 61 ? '-0' : i === 62 ? '1e400' : String(i);
    if (variant === 2) return i === 63 ? '"last"' : String(i);
    const key = i < 32 ? 'Aa' : 'BB';
    return '{"' + key + '":' + i + ',"value":' + (i%2 ? '-0' : '1.25') +
      ',"name":"' + (wide ? 'śŝĢŜ漢😀' : 'plain') + '\\"[,]{}\\\\",' +
      '"empty":[],"nested":{"0":1,"4294967294":2,"__proto__":3,"":4},"dup":1,"dup":2}';
  });
  const extra = size - ('[' + parts.join(',') + ']').length;
  check(extra >= 0, 'fixture too short');
  // Keep a dense prefix, then distribute whitespace to place cuts through
  // both primitive and object values while preserving a small result tree.
  for (let i = 0; i < 8; i++) parts[56 + i] += ' '.repeat(Math.floor(extra / 8) + (i < extra % 8));
  return '[' + parts.join(',') + ']';
}
let cases = 0;
async function run(id, source, reviver = false) {
  const trace = [];
  function revive(k, v, context) {
    trace.push(hex(k) + ':' + (Object.hasOwn(context, 'source') ? hex(context.source) : '-') );
    return v;
  }
  let value, error, phase = 'return';
  try { value = JSON[api](source, reviver ? revive : undefined); }
  catch(e) { error = e; phase = 'throw'; }
  if (api === 'parseAsync') {
    check(phase === 'return' && value instanceof Promise, 'Promise/rejection contract');
    if (typeof gc === 'function') gc();
    try { value = await value; } catch(e) { error = e; }
  }
  if (typeof gc === 'function') gc();
  if (error) {
    check(error instanceof SyntaxError, id + ':' + error);
    print('ERROR\t' + id + '\t' + hex(error.name) + '\t' + hex(error.message));
    check(trace.length === 0, 'reviver on rejected syntax');
  } else {
    if (id.endsWith('single-large')) {
      const text = value.data[0].x;
      check(text.length === 4*Mi, 'large string length');
      const expected = id.startsWith('wide/') ? 0x6f22 : 120;
      for (let i=0;i<text.length;i++) check(text.charCodeAt(i)===expected,'large string content');
      print('VALUE\t'+id+'\t'+text.length+':'+expected);
    } else print('VALUE\t' + id + '\t' + encode(value) + '\t' + trace.join('|'));
  }
  cases++;
}
async function main() {
  for (const wide of [false,true]) {
    const a = array(4*Mi + 13,wide);
    const pre = '"prefix":{"Aa":"first","0":-0},';
    const post = ',"suffix":{"BB":"last","4294967294":5,"__proto__":null}';
    const valid = [
      ['first','{"arbitrary":'+a+post+'}'],
      ['small-first','{"small":[0,1],"big":'+a+'}'],
      ['leading-prefix',' \n{"pad":'+' '.repeat(4*Mi)+'0,"big":'+a+'}'],
      ['middle','{'+pre+'"list":'+a+post+'}'],
      ['last','{'+pre+'"list":'+a+'}'],
      ['duplicate-before','{"list":7,"list":'+a+post+'}'],
      ['duplicate-after','{'+pre+'"list":'+a+',"list":42}'],
      ['two-large','{"first":'+a+',"second":'+a+'}'],
      ['nested-control','{"wrapper":{"list":'+a+'}}'],
      ['reviver','{'+pre+'"list":'+a+post+'}'],
    ];
    if (only !== 'errors') for (const [id,s] of valid) await run((wide?'wide/':'narrow/')+id,s,id==='reviver');
    if (only !== 'valid') {
      const head = ' \t\r\n{'+pre+'"list":';
      const tail = post+'}';
      const malformed = [
        ['prefix','{"bad":00,"list":'+a+'}'],
        ['suffix',head+a+',"bad":00}'],
        ['outer-comma',head+a+','+post+'}'],
        ['outer-key',head+a+',unquoted:1}'],
        ['trailing',head+a+tail+'true'],
        ['truncated-object',head+a+post],
        ['truncated-array',head+a.slice(0,-1)],
        ['array-end',head+a.slice(0,-1)+'}'+tail],
        ['array-first',head+'[00,'+a.slice(1)+tail],
        ['array-late',head+a.slice(0,-1)+',4e+]'+tail],
        ['array-escape',head+a.slice(0,-1)+',"\\q"]'+tail],
        ['array-control',head+a.slice(0,-1)+',"x\r\ny"]'+tail],
        ['missing-element',head+a.slice(0,-1)+',]'+tail],
        ['first-error',head+'[00,'+a.slice(1,-1)+',"\\q"]'+tail],
      ];
      for (const [id,s] of malformed) await run((wide?'wide/':'narrow/')+id,s);
    }
    if (only !== 'errors') {
      const source = '{"items":'+a+',"last":true}';
      const pending = Array.from({length:4}, () => JSON[api](source));
      if (typeof gc === 'function') gc();
      const values = await Promise.all(pending);
      for (let i=0;i<values.length;i++) {
        print('BATCH\t'+(wide?'wide/':'narrow/')+i+'\t'+encode(values[i]));
        cases++;
      }
      for (const size of [4*Mi-1,4*Mi,4*Mi+1,16*Mi-1,16*Mi,16*Mi+1]) {
        for (const variant of [1,2]) await run((wide?'wide/':'narrow/')+size+'/'+variant,
          '{"start":'+(wide?'"漢"':'0')+',"items":'+array(size,wide,variant)+',"end":true}');
      }
      await run((wide?'wide/':'narrow/')+'single-large', '{"data":[{"x":"'+(wide?'漢':'x').repeat(4*Mi)+'"}],"end":true}');
    }
  }
  print('OBJECT_ARRAY_DONE\t'+cases);
}
main().catch(e => {print(e.stack);quit(1);});
