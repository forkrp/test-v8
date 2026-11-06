// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with tools/json_correctness/error_parity.py.
// Compare an engine's observable JSON contract with the original 12.6 d8.
// The output never uses the JSON implementation being tested for encoding.
const [api, suite] = arguments;
const asynchronous = api.endsWith('Async');
const parsing = api.startsWith('parse');
const builtins = {SyntaxError, TypeError, RangeError};
let rows = 0;
let faults = 0;
function hex16(s) {
  let out = '';
  for (let i = 0; i < s.length; ++i) {
    out += s.charCodeAt(i).toString(16).padStart(4, '0');
  }
  return out;
}
async function check(id, args, expected, {trace = [], marker} = {}) {
  let value, phase = 'return', shape = true;
  try { value = Reflect.apply(JSON[api], JSON, args); }
  catch (e) { phase = 'throw'; value = e; }
  if (asynchronous && phase === 'return') {
    shape = Object.getPrototypeOf(value ?? {}) === Promise.prototype;
    if (shape) {
      try { value = await value; }
      catch (e) { phase = 'reject'; value = e; }
    }
  }
  let pass, observed;
  if (expected === 'marker') {
    pass = Object.is(value, marker);
    observed = 'marker:' + pass;
  } else if (Object.hasOwn(builtins, expected)) {
    pass = value != null &&
           Object.getPrototypeOf(value) === builtins[expected].prototype &&
           value.name === expected && typeof value.message === 'string';
    const nativeError = phase !== 'return' && value instanceof Error;
    observed = nativeError
        ? 'error:' + value.name + ':' + hex16(value.message)
        : 'other:' + typeof value;
  } else {
    const correct = Object.is(value, expected.value);
    pass = correct;
    observed = 'value:' + typeof value + ':' +
        (typeof value === 'string' ? hex16(value) : typeof value === 'object'
          ? (value === null ? 'null' : 'object') : String(value));
  }
  const expectedPhase = typeof expected === 'object' ? 'return' :
      (asynchronous ? 'reject' : 'throw');
  pass = pass && shape && phase === expectedPhase &&
         !trace.some(step => step.startsWith('unexpected'));
  if (!pass) ++faults;
  ++rows;
  print('CASE\t' + id + '\t' + (pass ? 'PASS' : 'FAIL-' + phase + '-' + shape) +
        '\t' + observed + '\t' + trace.join(','));
}

async function external() {
  for (const row of globalThis.rejectedCases) {
    let text = row.text;
    if (row.large) {
      const {size, seed} = row.large;
      const token = '"' + seed.replaceAll('\\', '\\\\').replaceAll('"', '\\"') + '"';
      if (/[^\x20-\x7e]/.test(token)) throw Error('Expected printable ASCII seed');
      const count = Math.floor((size - 1) / (token.length + 1));
      text = '[' + (token + ',').repeat(count - 1) + token + ']';
      text += ' '.repeat(size - text.length);
      const at = 1 + Math.floor(count / 2) * (token.length + 1) + 2;
      text = text.slice(0, at) + '\x01' + text.slice(at + 1);
      if (text.length !== size) throw Error('Wrong fixture size');
    }
    await check('corpus/' + row.id, [text], 'SyntaxError');
  }
}

async function parseEdges() {
  const malformed = [
    '', '\t \r\n', '\ufeff{}', '\u00a0[]', '\u2028[]', '\u0000[]', '+0',
    '.5', '-', '01', '-00', '0.', '1e', '1e+', '1e-', '[1e+]', 'NaN',
    'Infinity', '-Infinity', 'True', 'NULL', 'tru', 'false true', '[]]',
    '0\u0000', '1/*comment*/', '[1,]', '[,1]', '[1,,2]', '[1 2]',
    '{a:1}', "{'a':1}", '{"a"}', '{"a":}', '{"a":1,}', '{"a":1 "b":2}',
    '"', '"\\', '"\\q"', '"\\x20"', '"\\u00q0"', '"\\u123"', '"\x00"',
    '"\x1f"', '"\t"', '"\n"', '{"a":"😀",\r\n"b":-}', '{"\\uD800":}',
    '{"é": 0, "😀": 1}\r\n extra', '[[],{"x": ]}]',
    ...['undefined', 'NaN', 'Infinity', 'object Object'].map(t => ' '.repeat(32) + t)
  ];
  for (let i = 0; i < malformed.length; ++i) {
    await check('syntax/' + i, [malformed[i]], 'SyntaxError');
  }
  for (const [id, args, type] of [
    ['missing', [], 'SyntaxError'], ['undefined', [undefined], 'SyntaxError'],
    ['symbol', [Symbol('s')], 'TypeError'],
    ['object', [{}], 'SyntaxError'], ['null-prototype', [Object.create(null)], 'TypeError'],
    ['boxed-symbol', [Object(Symbol('s'))], 'TypeError'],
    ['nonprimitive', [{[Symbol.toPrimitive]() { return {}; }}], 'TypeError'],
    ['undefined-string', [{toString() { return undefined; }}], 'SyntaxError']
  ]) await check('coerce/' + id, args, type);

  const markers = [undefined, null, -0, NaN, 7n, Symbol('error'),
                   new TypeError('sentinel'), {get name() { throw 'read name'; },
                                               get message() { throw 'read message'; }}];
  for (let label = 0; label < markers.length; ++label) {
    const marker = markers[label];
    let trace = [];
    await check('coerce/thrown-' + label, [{
      get toString() { trace.push('lookup'); throw marker; },
      valueOf() { trace.push('unexpected-valueOf'); return 0; }
    }], 'marker', {trace, marker});
    trace = [];
    await check('reviver/thrown-' + label, ['{"a":1,"b":2}', function(key, value) {
      trace.push('key=' + key);
      if (key === 'a') throw marker;
      return value;
    }], 'marker', {trace, marker});
  }
  const trace = [];
  await check('reviver/not-on-syntax-error', ['[1,]', () => trace.push('unexpected')],
              'SyntaxError', {trace});
  await check('control/coercion', [{[Symbol.toPrimitive]() { return '42'; }}], {value:42});
  await check('control/non-callable-reviver', ['null', {}], {value:null});
  await check('control/lone-surrogate', ['"\\uD800"'], {value:'\uD800'});
  await check('control/negative-zero', ['-0'], {value:-0});
  await check('control/overflow', ['1e400'], {value:Infinity});
}

async function largeEdges() {
  for (const parts of [4, 8]) {
    // 4.8M or 16.8M code units and a dense prefix for the admission probe.
    const pad = ' '.repeat(parts === 4 ? 600000 : 2100000);
    const count = 8;
    for (const wide of [false, true]) {
      // These UTF-16 code units have low bytes '[', ']', '"', and '\\'.
      const prefix = (wide ? ' \t\r\n["śŝĢŜ",' : ' \t\r\n[') +
          Array.from({length:32}, (_, i) => i).join(',') + ',';
      for (const [label, at, value, suffix, reviver] of [
        ['bad-escape-early-part', 0, '"\\u12g4"', ']', false],
        ['bad-escape-late-part', count - 2, '"\\q"', ']', false],
        ['raw-control-late-part', count - 1, '"x\r\ny"', ']', false],
        ['bad-number-late-part', count - 1, '4e+', ']', false],
        ['empty-part-value', count - 1, '', ']', false],
        ['missing-comma', count - 2, '0\r\n1', ']', false],
        ['nested-trailing-comma', count - 1, '{"a":[1,]}', ']', false],
        ['truncated', count - 1, '0', '', false],
        ['junk-after-root', count - 1, '0', ']\r\n true', false],
        ['invalid-with-reviver', count - 1, '012', ']', true],
        ['first-error-wins', count - 1, '"\\q"', ']', false],
      ]) {
        const items = Array.from({length:count}, (_, i) =>
          ((label === 'first-error-wins' && i === 1) ? '00' :
           (i === at ? value : '0')) + pad);
        const source = prefix + items.join(',') + suffix;
        if (source.length < (parts === 4 ? 4 : 16) * 2 ** 20) throw Error('Short fixture');
        const trace = [];
        const args = reviver ? [source, () => trace.push('unexpected')] : [source];
        await check('large/' + parts + '/' + (wide ? 'utf16' : 'one-byte') + '/' + label,
                    args, 'SyntaxError', {trace});
      }
    }
  }
}

async function stringifyEdges() {
  const objectCycle = {};
  objectCycle.child = objectCycle;
  const arrayCycle = [1, 2];
  arrayCycle.push(arrayCycle);
  const mixed = {outer: [{nested: {}}]};
  mixed.outer[0].nested.back = mixed;
  const late = Array.from({length:2048}, () => ({x:1, y:'same'}));
  late.push(objectCycle);
  const lateBigInt = late.slice(0, -1).concat({x:1, y:7n});
  const revoked = Proxy.revocable({}, {});
  revoked.revoke();
  for (const [id, args] of [
    ['bigint', [1n]], ['bigint-boxed', [Object(1n)]],
    ['bigint-array', [[1, 2n]]], ['bigint-object', [{ok:1, bad:2n}]],
    ['bigint-from-toJSON', [{toJSON() { return 7n; }}]],
    ['bigint-from-replacer', [{x:1}, (k, v) => k === 'x' ? 7n : v]],
    ['bigint-after-fast-prefix', [lateBigInt]],
    ['cycle-object', [objectCycle]], ['cycle-array', [arrayCycle]],
    ['cycle-mixed', [mixed]], ['cycle-after-fast-prefix', [late]],
    ['cycle-via-replacer', [{x:{}}, (_, v) => v]],
    ['revoked-proxy', [revoked.proxy]], ['revoked-replacer', [{}, revoked.proxy]],
    ['duplicate-keys', [new Proxy({}, {ownKeys: () => ['a', 'a']})]],
    ['invalid-keys', [new Proxy({}, {ownKeys: () => [0]})]],
    ['invalid-descriptor', [new Proxy({x:1}, {getOwnPropertyDescriptor: () => 1})]],
    ['space-bad-primitive', [{a:1}, null, Object.assign(new Number(0),
                                          {[Symbol.toPrimitive]: () => ({})})]],
    ['replacer-bad-primitive', [{a:1}, [Object.assign(new String('a'),
                                          {[Symbol.toPrimitive]: () => ({})})]]]
  ]) {
    if (id === 'cycle-via-replacer') {
      const root = args[0];
      args[1] = (k, v) => k === 'x' ? root : v;
    }
    await check('type/' + id, args, 'TypeError');
  }
  // Proxy arrays take the slow length checks without allocating huge arrays.
  for (const length of [2 ** 32 - 1, 2 ** 32]) {
    const trace = [];
    const oversized = new Proxy([], {get(t, key, receiver) {
      if (key === 'length') {
        trace.push('length');
        return length;
      }
      if (key !== 'toJSON') trace.push('unexpected-' + String(key));
      return Reflect.get(t, key, receiver);
    }});
    await check('range/output-length-' + length, [oversized], 'RangeError', {trace});
  }
  const makers = {
    'getter': fail => [{get a() { return fail('a'); },
                        get b() { return fail('b'); }}],
    'toJSON-get': fail => [{get toJSON() { return fail('lookup'); }}],
    'toJSON-call': fail => [{a:{toJSON(k) { return fail('call=' + k); }}}],
    'replacer-call': (fail, trace) => [{a:1,b:2}, (k, v) => {
      trace.push('key=' + k);
      return k === 'a' ? fail('call') : v;
    }],
    'proxy-get': fail => [new Proxy({a:1,b:2}, {get(t, k, r) {
      return k === 'toJSON' ? Reflect.get(t,k,r) : fail('get=' + k);
    }})],
    'proxy-keys': fail => [new Proxy({}, {ownKeys() { return fail('ownKeys'); }})],
    'proxy-descriptor': fail => [new Proxy({a:1}, {getOwnPropertyDescriptor(_, k) {
      return fail('descriptor=' + k);
    }})],
    'replacer-length': fail => [{a:1}, new Proxy(['a'], {get(t, k, r) {
      return k === 'length' ? fail('length') : Reflect.get(t,k,r);
    }})],
    'replacer-item': fail => [{a:1}, [Object.assign(new String('a'),
                                 {toString() { return fail('item'); }})]],
    'space-coercion': fail => [{a:1}, null, Object.assign(new Number(1),
                                  {valueOf() { return fail('space'); }})],
  };
  for (const [kind, make] of Object.entries(makers)) {
    const markers = [undefined, null, -0, NaN, 7n, Symbol(), Error('sentinel'),
                     {get name() { throw 'read name'; }, get message() { throw 'read message'; }}];
    for (let i = 0; i < markers.length; ++i) {
      const marker = markers[i], trace = [];
      const fail = stage => { trace.push(stage); throw marker; };
      await check('hook/' + kind + '/' + i, make(fail, trace), 'marker', {trace, marker});
    }
  }
  for (const [id, args, value] of [
    ['undefined', [undefined], undefined], ['missing', [], undefined],
    ['symbol', [Symbol()], undefined], ['function', [() => {}], undefined],
    ['array-special', [[undefined, Symbol(), () => {}, NaN, Infinity, -0]], '[null,null,null,null,null,0]'],
    ['object-omission', [{a:undefined,b:Symbol(),c() {},d:1}], '{"d":1}'],
    ['escaped-surrogate', ['\ud800'], '"\\ud800"'],
    ['omit-bigint-replacer', [{a:1,b:2n}, (k, v) => k === 'b' ? undefined : v], '{"a":1}'],
    ['avoid-bigint-toJSON', [{n:1n,toJSON() { return 2; }}], '2'],
    ['avoid-cycle-replacer', [objectCycle, (k, v) => k === 'child' ? undefined : v], '{}'],
  ]) await check('control/' + id, args, {value});
}

(async () => {
  if (suite === 'external' && parsing) await external();
  else if (suite === 'large' && parsing) await largeEdges();
  else if (suite === 'edges') await (parsing ? parseEdges() : stringifyEdges());
  else throw Error('Unknown suite');
  print('DONE\t' + rows + '\t' + faults);
})().catch(e => { print('HARNESS-ERROR\t' + e.stack); quit(2); });
