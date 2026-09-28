// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Use parse-async-benchmark.py --script test/json/stringify-async-benchmark.js.

'use strict';

const runs = Number(arguments[0] || 5);
const onlyCase = arguments[1] || '';
const jobCounts = arguments[2] ? [Number(arguments[2])] : [1, 4];
const mode = arguments[3] || 'both';
if (!Number.isInteger(runs) || runs < 1 || runs > 20 ||
    jobCounts.some(n => !Number.isInteger(n) || n < 1 || n > 64) ||
    !['both', 'sync', 'async'].includes(mode)) throw new Error('invalid options');
const available = typeof JSON.stringifyAsync === 'function';
if (!available && mode === 'async') throw new Error('stringifyAsync unavailable');

function median(values) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return (sorted[(sorted.length - 1) >> 1] + sorted[sorted.length >> 1]) / 2;
}

async function main() {
  const cases = [
    ['records', () => Array.from({length: 100000}, (_, i) => ({id: i, text: 'value'}))],
    ['numbers', () => Array.from({length: 120000}, (_, i) => [1.25, -0, 1e20][i % 3])],
    ['integers', () => Array.from({length: 200000}, (_, i) => i)],
    ['strings', () => Array(50000).fill('abcdefghijkl\u1234mnop\n')],
    ['nested', () => Array.from({length: 40000}, () =>
      ({a: [1, 2, 3], b: {x: true, y: 'value'}, id: 1}))],
    ['varied', () => Array.from({length: 30000}, (_, i) =>
      ({id: i, text: 'different-value-' + i, ratio: i / 8, active: !!(i & 1)}))],
    ['long_strings', () => Array.from({length: 2000}, (_, i) =>
      'abcdef\u1234\n'.repeat(80) + i)],
    ['unicode', () => Array.from({length: 70000}, () =>
      ({id: 1, state: '成功', text: '文字列'}))],
    ['large_string', () => 'abcdefghij'.repeat(1000000)],
    ['large_unicode', () => '文字列漢字'.repeat(1000000)],
    ['large_escaped', () => '\n\\漢'.repeat(500000)],
    ['callbacks', () => Array.from({length: 20000}, (_, i) =>
      ({get id() { return i; }, toJSON() { return {id: this.id, value: i / 7}; }}))],
    ['tiny', () => ({id: 1, text: 'value'})],
  ];
  if (onlyCase && !cases.some(([name]) => name === onlyCase)) {
    throw new Error('unknown case: ' + onlyCase);
  }
  for (const [name, createInput] of cases) {
    if (onlyCase && name !== onlyCase) continue;
    const input = createInput();
    const expected = JSON.stringify(input);
    for (const jobs of jobCounts) {
      const sync = [], async = [], capture = [], maxCapture = [];
      for (let run = 0; run < runs; ++run) {
        for (const asynchronous of run & 1 ? [true, false] : [false, true]) {
          if (asynchronous ? mode === 'sync' || !available : mode === 'async') continue;
          gc();
          let values = [];
          const start = performance.now();
          if (asynchronous) {
            const promises = [];
            let longest = 0;
            for (let job = 0; job < jobs; ++job) {
              const before = performance.now();
              promises.push(JSON.stringifyAsync(input));
              longest = Math.max(longest, performance.now() - before);
            }
            capture.push(performance.now() - start);
            maxCapture.push(longest);
            values = await Promise.all(promises);
          } else {
            for (let job = 0; job < jobs; ++job) values.push(JSON.stringify(input));
          }
          (asynchronous ? async : sync).push(performance.now() - start);
          if (values.length !== jobs || values.some(value => value !== expected)) {
            throw new Error('incorrect output');
          }
          values = null;
        }
      }
      print(JSON.stringify({name, jobs, mode, runs, code_units: expected.length,
        sync_ms: median(sync), async_ms: median(async),
        capture_ms: median(capture), max_capture_ms: median(maxCapture),
        sync_samples_ms: sync, async_samples_ms: async,
        capture_samples_ms: capture, max_capture_samples_ms: maxCapture}));
    }
  }
}

main().catch(error => { print(error.stack || error); quit(1); });
