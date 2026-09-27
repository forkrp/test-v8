// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run: out/json-async/d8 --expose-gc test/json/parse-async-benchmark.js -- 5

'use strict';

const runs = Number(arguments[0] || 3);
if (!Number.isInteger(runs) || runs < 1 || runs > 20) {
  throw new Error('runs must be an integer in [1, 20]');
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
    ['records', '[' + '{"id":1,"text":"value"},'.repeat(100000) + 'null]', 100001],
    ['numbers', '[' + '1.25,-0,1e20,'.repeat(40000) + '0]', 120001],
    ['strings', '[' + '"abcdefghijkl\\u1234mnop",'.repeat(50000) + 'null]', 50001],
    ['nested', '[' + '{"a":[1,2,3],"b":{"x":true,"y":"value"},"id":1},'.repeat(40000) + 'null]', 40001],
  ];
  for (const [name, text, length] of cases) {
    for (const jobs of [1, 4]) {
      const sync = [], async = [];
      for (let run = 0; run < runs; ++run) {
        gc();
        let values = [];
        let start = performance.now();
        for (let i = 0; i < jobs; ++i) values.push(JSON.parse(text));
        sync.push(performance.now() - start);
        check(values, length, jobs);
        values = null;
        gc();
        start = performance.now();
        values = await Promise.all(Array.from({length: jobs}, () => JSON.parseAsync(text)));
        async.push(performance.now() - start);
        check(values, length, jobs);
        values = null;
      }
      print(JSON.stringify({name, jobs, bytes: text.length, runs,
                            sync_ms: median(sync), async_ms: median(async)}));
    }
  }
}

main().catch(error => { print(error.stack || error); quit(1); });
