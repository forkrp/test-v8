// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
'use strict';
const [specArg, mode = 'parse', jobsArg = '1', samplesArg = '9',
       repeatsArg = '1', action = 'bench'] = arguments;
const spec = JSON.parse(specArg);
const jobs = Number(jobsArg), samples = Number(samplesArg);
const repeats = Number(repeatsArg), width = spec.width;
const rows = spec.kind === 'single' ? 1 : Math.floor(524288 / width);
const prefix = spec.charset === 'utf16' ? '漢' : '';
const cold = spec.kind === 'cold';
if (cold && repeats !== 1) throw Error('cold inputs cannot repeat');

function keyAt(field, row, round) {
  const shape = cold ? 'cold' + round + '_' + row + '_' :
      spec.kind === 'churn' ? 'shape' + row % 8 + '_' : '';
  return prefix + shape + 'k' + String(field).padStart(4, '0');
}
function rawIndex(order, row) {
  return (spec.kind === 'reverse' || spec.kind === 'churn') && (row & 1) ?
      width - order - 1 : order;
}
function valueAt(field, row) {
  if (spec.pattern === 'smi') return field * 17 + 13;
  const kind = (field + (spec.kind === 'churn' ? row % 4 : 0)) % 7;
  switch (kind) {
    case 0: return field * 17 + 13;
    case 1: return field + 0.125;
    case 2: return -0;
    case 3: return 'value';
    case 4: return true;
    case 5: return null;
    case 6: return 123456789012;
  }
}
function valueText(field, row) {
  const value = valueAt(field, row);
  return Object.is(value, -0) ? '-0' : typeof value === 'string' ? '"value"' :
      String(value);
}
function makeRecord(row, round) {
  const fields = [];
  if (spec.kind === 'indexed') fields.push('"0":1');
  for (let order = 0; order < width; ++order) {
    const raw = rawIndex(order, row);
    const field = spec.kind === 'duplicates' ? raw % (width / 2) : raw;
    fields.push('"' + keyAt(field, row, round) + '":' + valueText(raw, row));
  }
  if (spec.kind === 'indexed')
    fields.push('"4294967294":2', '"2":3', '"4294967295":4', '"0":5');
  return '{' + fields.join(',') + '}';
}
function makeText(round) {
  if (spec.kind === 'single') return makeRecord(0, round);
  if (spec.kind === 'same' || spec.kind === 'duplicates' || spec.kind === 'indexed')
    return '[' + Array(rows).fill(makeRecord(0, round)).join(',') + ']';
  return '[' + Array.from({length: rows}, (_, row) => makeRecord(row, round)).join(',') + ']';
}
const inputs = cold ? Array.from({length: samples + 2}, (_, i) => makeText(i)) :
    [makeText(0)];
if (action === 'fixtures') {
  print(JSON.stringify({spec, rows, inputs: inputs.length}));
  for (const text of inputs) print(text);
  quit(0);
}
const namedCount = spec.kind === 'duplicates' ? width / 2 : width;
const expectedKeys = cold ? null : Array.from({length: 8}, (_, row) =>
    Array.from({length: namedCount}, (_, order) => keyAt(
        spec.kind === 'duplicates' ? order : rawIndex(order, row), row, 0)));
function validate(result, round) {
  const values = spec.kind === 'single' ? [result] : result;
  if (!Array.isArray(values) || values.length !== rows) throw Error('rows');
  const count = namedCount;
  const indexed = spec.kind === 'indexed';
  for (let row = 0; row < rows; ++row) {
    const object = values[row], keys = Object.keys(object);
    if (Object.getPrototypeOf(object) !== Object.prototype ||
        keys.length !== count + (indexed ? 4 : 0)) throw Error('object');
    if (indexed && (keys[0] !== '0' || keys[1] !== '2' ||
        keys[2] !== '4294967294' || keys.at(-1) !== '4294967295' ||
        object[0] !== 5 || object[2] !== 3 || object[4294967294] !== 2 ||
        object[4294967295] !== 4)) throw Error('indexed order/value');
    for (let order = 0; order < count; ++order) {
      const field = spec.kind === 'duplicates' ? order : rawIndex(order, row);
      const expectedKey = cold ? keyAt(field, row, round) : expectedKeys[row % 8][order];
      if (keys[order + (indexed ? 3 : 0)] !== expectedKey ||
          !Object.is(object[expectedKey], valueAt(
              spec.kind === 'duplicates' ? field + count : field, row)))
        throw Error('key order/value');
    }
  }
}
function median(v) {
  const a = [...v].sort((a, b) => a - b), n = a.length;
  return n & 1 ? a[n >> 1] : (a[n / 2 - 1] + a[n / 2]) / 2;
}
(async function() {
  const warming = [], measured = [];
  for (let round = -2; round < samples; ++round) {
    const inputIndex = cold ? round + 2 : 0;
    const text = inputs[inputIndex];
    gc();
    const results = [];
    const start = performance.now();
    for (let repetition = 0; repetition < repeats; ++repetition) {
      if (mode === 'parseAsync') {
        const pending = [];
        for (let job = 0; job < jobs; ++job) pending.push(JSON.parseAsync(text));
        results.push(...await Promise.all(pending));
      } else {
        for (let job = 0; job < jobs; ++job) results.push(JSON.parse(text));
      }
    }
    const batch_ms = performance.now() - start;
    if (results.length !== repeats * jobs) throw Error('jobs');
    for (const result of results) validate(result, inputIndex);
    (round < 0 ? warming : measured).push({batch_ms, elapsed_ms: batch_ms / repeats});
    results.length = 0;
  }
  print(JSON.stringify({spec, mode, jobs, repeats, rows,
    raw_named_fields_per_call: rows * width, input_code_units: inputs.map(x => x.length),
    wall_ms: median(measured.map(x => x.elapsed_ms)),
    median_batch_ms: median(measured.map(x => x.batch_ms)),
    warming, measured, validated_all_results: true,
    validation: 'all keys/order/prototypes/values against independent generator values'}));
})().catch(error => { print(error.stack); quit(1); });
