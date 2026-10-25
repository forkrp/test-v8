// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
'use strict';
const [suitePath, filePath, mode = 'parse', jobsArg = '1',
       samplesArg = '9', expectedPath = ''] = arguments;
load(suitePath + '/common.js');
const jobs = Number(jobsArg), samples = Number(samplesArg), warmups = 2;
const name = filePath.split('/').pop().replace(/\.json$/, '');
const text = read(filePath);
async function main() {
  if (mode === 'oracle') {
    const value = JSON.parse(text);
    const tree = largeJson.fingerprint(value);
    const canonical = JSON.stringify(value);
    print(JSON.stringify({name, code_units: text.length, tree,
                          canonical: largeJson.textDigest(canonical)}));
    write(canonical);
    return;
  }
  if (!['parse', 'parseAsync', 'stringify'].includes(mode)) throw Error('mode');
  if (typeof JSON[mode] !== 'function') throw Error('API unavailable: ' + mode);
  const expected = JSON.parse(read(expectedPath))[name];
  if (!expected) throw Error('Missing oracle: ' + name);
  if (text.length !== expected.code_units) throw Error('Input code-unit length');
  let sourceValue = null;
  if (mode === 'stringify') {
    sourceValue = JSON.parse(text);
    largeJson.same(largeJson.fingerprint(sourceValue), expected.tree, 'stringify input');
  }
  const measured = [], warming = [];
  for (let round = -warmups; round < samples; ++round) {
    gc();
    const values = [], pending = [];
    const before = performance.now();
    for (let job = 0; job < jobs; ++job) {
      if (mode === 'parseAsync') pending.push(JSON.parseAsync(text));
      else if (mode === 'parse') values.push(JSON.parse(text));
      else values.push(JSON.stringify(sourceValue));
    }
    const entry = performance.now() - before;
    const results = mode === 'parseAsync' ? await Promise.all(pending) : values;
    const wall = performance.now() - before;
    if (results.length !== jobs) throw Error('Job count');
    for (const result of results) {
      if (mode === 'stringify') {
        largeJson.same(largeJson.textDigest(result), expected.canonical, 'stringify output');
      } else {
        largeJson.same(largeJson.fingerprint(result), expected.tree, mode + ' output');
      }
    }
    (round < 0 ? warming : measured).push({round, wall_ms: wall, entry_ms: entry});
    results.length = 0;
    values.length = 0;
    pending.length = 0;
  }
  print(JSON.stringify({name, mode, jobs, samples, warmups, code_units: text.length,
    wall_ms: largeJson.median(measured.map(x => x.wall_ms)),
    entry_ms: largeJson.median(measured.map(x => x.entry_ms)),
    measurements: measured, warming, validated_all_results: true}));
}
main().catch(error => { print(error.stack || error); quit(1); });
