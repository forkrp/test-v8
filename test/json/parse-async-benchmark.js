// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run: out/json-async/d8 --expose-gc test/json/parse-async-benchmark.js -- 5

'use strict';

const runs = Number(arguments[0] || 3);
const onlyCase = arguments[1] || '';
const jobCounts = arguments[2] ? [Number(arguments[2])] : [1, 4];
const mode = arguments[3] || 'both';
if (!['both', 'sync', 'async'].includes(mode)) {
  throw new Error('mode must be both, sync, or async');
}
if (!Number.isInteger(runs) || runs < 1 || runs > 20) {
  throw new Error('runs must be an integer in [1, 20]');
}
if (jobCounts.some(n => !Number.isInteger(n) || n < 1 || n > 64)) {
  throw new Error('jobs must be an integer in [1, 64]');
}
function median(values) {
  values.sort((a, b) => a - b);
  return (values[(values.length - 1) >> 1] + values[values.length >> 1]) / 2;
}
function check(values, length, jobs) {
  if (values.length !== jobs || values.some(value => value.length !== length)) {
    throw new Error('benchmark result length mismatch');
  }
}

async function main() {
  const cases = [
    ['records', () => '[' + '{"id":1,"text":"value"},'.repeat(100000) + 'null]', 100001],
    ['numbers', () => '[' + '1.25,-0,1e20,'.repeat(40000) + '0]', 120001],
    ['strings', () => '[' + '"abcdefghijkl\\u1234mnop",'.repeat(50000) + 'null]', 50001],
    ['nested', () => '[' + '{"a":[1,2,3],"b":{"x":true,"y":"value"},"id":1},'.repeat(40000) + 'null]', 40001],
    ['varied', () => Array.from({length: 4}, (_, job) => JSON.stringify(
      Array.from({length: 30000}, (_, i) => ({id: i + job * 30000,
        text: 'different-value-' + job + '-' + i, ratio: i / 8,
        active: !!(i & 1)})))), 30000],
    ['long_strings', () => '[' + ('"' + 'abcdef\\u1234\\n'.repeat(80) + '",').repeat(2000) + 'null]', 2001],
    ['unicode', () => '[' + '{"id":1,"state":"成功","text":"文字列"},'.repeat(70000) + 'null]', 70001],
    ['key_churn', () => JSON.stringify(Array.from({length: 30000}, (_, i) =>
      ({['key' + i]: i, tail: i & 1 ? 'value' : 0}))), 30000],
    ['wide_objects', () => JSON.stringify(Array.from({length: 16}, (_, row) =>
      Object.fromEntries(Array.from({length: 2048}, (_, i) =>
        ['property' + i, i + row])))), 16],
    ['key_collisions', () => '[' +
      '{"Aa":1,"BB":2,"AaAa":3,"BBBB":4,"AaBB":5,"BBAa":6},'.repeat(30000) +
      'null]', 30001],
  ];
  if (onlyCase && !cases.some(([name]) => name === onlyCase)) {
    throw new Error('unknown benchmark case: ' + onlyCase);
  }
  for (const [name, createInput, length] of cases) {
    if (onlyCase && name !== onlyCase) continue;
    const input = createInput();
    const texts = Array.isArray(input) ? input : [input];
    for (const jobs of jobCounts) {
      const sync = [], async = [];
      for (let run = 0; run < runs; ++run) {
        // Alternate order: never always warm maps/strings with JSON.parse first.
        for (const asynchronous of run & 1 ? [true, false] : [false, true]) {
          if (mode === 'sync' && asynchronous) continue;
          if (mode === 'async' && !asynchronous) continue;
          gc();
          let values = [];
          const start = performance.now();
          if (asynchronous) {
            values = await Promise.all(Array.from({length: jobs}, (_, i) =>
              JSON.parseAsync(texts[i % texts.length])));
          } else {
            for (let i = 0; i < jobs; ++i) {
              values.push(JSON.parse(texts[i % texts.length]));
            }
          }
          (asynchronous ? async : sync).push(performance.now() - start);
          check(values, length, jobs);
          values = null;
        }
      }
      print(JSON.stringify({name, jobs, mode, code_units: texts[0].length, runs,
                            sync_ms: sync.length ? median([...sync]) : null,
                            async_ms: async.length ? median([...async]) : null,
                            sync_samples_ms: sync, async_samples_ms: async}));
    }
  }
}

main().catch(error => { print(error.stack || error); quit(1); });
