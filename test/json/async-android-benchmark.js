// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc; arguments: API CASE JOBS RUNS MODE.
'use strict';
const [api = 'parse', name = 'long_ascii', jobsArg = '1', runsArg = '9', mode = 'both'] = arguments;
const jobs = Number(jobsArg), runs = Number(runsArg);
if (!['parse', 'stringify'].includes(api) || !['sync', 'async', 'both'].includes(mode)) throw Error('options');
const creators = {
  integers: () => Array.from({length: 200000}, (_, i) => i),
  fragmented_numbers: () => Array.from({length: 10000}, () => Array.from({length: 32}, (_, i) => i / 8)),
  emoji: () => '😀'.repeat(1000000),
  emoji_mixed: () => ('漢'.repeat(4095) + '😀').repeat(400),
  emoji_lone: () => ('😀'.repeat(1024) + '\ud800').repeat(400),
  records: () => Array.from({length: 100000}, (_, i) => ({id: i, text: 'value'})),
  ten_digit_ints: () => Array.from({length: 120000}, (_, i) => [1000000000, 2147483647, 4294967295, -4294967295][i % 4]),
  numbers: () => Array.from({length: 120000}, (_, i) => [1.25, -0, 1e20][i % 3]),
  nested: () => Array.from({length: 40000}, () => ({a: [1, 2, 3], b: {x: true, y: 'value'}, id: 1})),
  varied: () => Array.from({length: 30000}, (_, i) => ({id: i, text: 'different-value-' + i, ratio: i / 8, active: !!(i & 1)})),
  long_ascii: () => 'abcdefghij'.repeat(400000),
  long_unicode: () => '文字列漢字'.repeat(400000),
  sparse_ascii: () => ('a'.repeat(511) + '\\').repeat(4000),
  sparse_unicode: () => ('漢'.repeat(511) + '\\').repeat(2000),
  dense_unicode: () => '\n\\漢'.repeat(300000),
  long_strings: () => Array.from({length: 2000}, (_, i) => 'abcdef\u1234\n'.repeat(80) + i),
  tiny: () => ({id: 1, text: 'value'}),
};
const input = creators[name]();
const text = JSON.stringify(input);
const source = api === 'parse' ? text : input;
const measured = mode === 'both' ? [false, true] : [mode === 'async'];
function median(v) { const sorted = [...v].sort((a,b)=>a-b);return sorted[sorted.length >> 1]; }
async function main() {
  const data = new Map(measured.map(async => [async, {wall: [], entry: []}]));
  for (let round = -2; round < runs; ++round) {
    for (const asynchronous of round & 1 ? [...measured].reverse() : measured) {
      gc();
      const values = [], promises = [];
      const before = performance.now();
      for (let i = 0; i < jobs; ++i) {
        if (asynchronous) promises.push(JSON[api + 'Async'](source));
        else values.push(JSON[api](source));
      }
      const entry = performance.now() - before;
      const results = asynchronous ? await Promise.all(promises) : values;
      const wall = performance.now() - before;
      if (results.length !== jobs || results.some(value => (api === 'parse' ? JSON.stringify(value) : value) !== text)) throw Error('incorrect output');
      if (round >= 0) { data.get(asynchronous).wall.push(wall); data.get(asynchronous).entry.push(entry); }
    }
  }
  for (const [asynchronous, timings] of data) {
    print(JSON.stringify({api, name, jobs, runs, mode: asynchronous ? 'async' : 'sync', code_units: text.length,
      wall_ms: median(timings.wall), entry_ms: median(timings.entry), wall_samples_ms: timings.wall, entry_samples_ms: timings.entry}));
  }
}
main().catch(error => {print(error.stack || error); quit(1);});
