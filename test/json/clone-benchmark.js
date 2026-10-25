// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// d8 --expose-gc test/json/clone-benchmark.js -- method [case] [samples] [batch-ms]
'use strict';

const method = arguments[0] || 'clone';
const selected = arguments[1] || 'all';
const samples = Number(arguments[2] || 9);
const targetBatchMs = Number(arguments[3] || 0);
const operations = {
  clone: value => JSON.clone(value),
  json: value => JSON.parse(JSON.stringify(value)),
  serializer: value => d8.serializer.deserialize(d8.serializer.serialize(value))
};
const operation = operations[method];
if (!operation) throw new Error('unknown method: ' + method);

function packed(length, create) {
  const result = [];
  for (let i = 0; i < length; ++i) result.push(create(i));
  return result;
}
function record(i) {
  return {id: i, name: 'item-' + i, enabled: (i & 1) === 0,
          price: i + 0.25, tags: ['a', 'b'], nested: {x: i, y: i + 0.5}};
}

const cases = [
  {name: 'small_record', batch: 20000, create: () => record(1)},
  {name: 'records_10000', batch: 3,
   create: () => packed(10000, record)},
  {name: 'nested_1000', batch: 8,
   create: () => packed(1000, i => ({id: i, a: {b: {c: {d: {value: i}}}}}))},
  {name: 'packed_ints_100000', batch: 10,
   create: () => packed(100000, i => i)},
  {name: 'packed_doubles_100000', batch: 8,
   create: () => packed(100000, i => i + 0.25)},
  {name: 'ascii_strings_1m', batch: 8,
   create: () => packed(1000, i => ({id: i, text: 'payload-' + i + 'x'.repeat(1024)}))},
  {name: 'unicode_strings_1m', batch: 8,
   create: () => packed(1000, i => ({id: i, text: '漢字🌏'.repeat(256) + i}))},
  {name: 'mixed_shapes_10000', batch: 4,
   create: () => packed(10000, i => {
     const result = {id: i};
     for (let j = 0; j < i % 8; ++j) result['field' + j] = {number: i + 0.25};
     return result;
   })},
  {name: 'dictionary_fallback', batch: 20, create: () => {
    const result = {};
    for (let i = 0; i < 1000; ++i) result['field' + i] = {id: i};
    return result;
  }},
  {name: 'sparse_fallback', batch: 20, create: () => {
    const result = new Array(10000);
    for (let i = 0; i < result.length; i += 3) result[i] = i;
    return result;
  }},
  {name: 'late_getter_fallback', batch: 3, create: () => {
    const result = {records: packed(10000, record)};
    Object.defineProperty(result, 'last', {
      enumerable: true, get() { return 1; }
    });
    return result;
  }}
];

for (const test of cases) {
  if (selected !== 'all' && selected !== test.name) continue;
  const input = test.create();
  const expected = JSON.stringify(input);
  let last = operation(input);
  if (JSON.stringify(last) !== expected) throw new Error(test.name + ': output');
  // Keep the result observable, while retaining only one clone at a time.
  for (let warm = 0; warm < 3; ++warm) {
    for (let i = 0; i < test.batch; ++i) last = operation(input);
  }
  // Optional untimed calibration keeps very fast cases above timer granularity.
  let batch = test.batch;
  for (let attempt = 0; targetBatchMs > 0 && attempt < 3; ++attempt) {
    gc();
    const start = performance.now();
    for (let i = 0; i < batch; ++i) last = operation(input);
    const elapsed = performance.now() - start;
    if (elapsed >= targetBatchMs) break;
    batch = Math.min(1000000,
                     Math.ceil(batch * targetBatchMs / Math.max(0.25, elapsed) * 1.15));
  }
  const times = [];
  for (let sample = 0; sample < samples; ++sample) {
    gc();
    const start = performance.now();
    for (let i = 0; i < batch; ++i) last = operation(input);
    times.push((performance.now() - start) / batch);
  }
  if (JSON.stringify(last) !== expected) throw new Error(test.name + ': last output');
  print(JSON.stringify({case: test.name, method, batch, targetBatchMs,
                        jsonCodeUnits: expected.length, samplesMs: times}));
}
