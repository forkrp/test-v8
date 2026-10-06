// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// d8 --expose-gc async-benchmark.js -- source.json operation codec jobs samples
'use strict';
const [sourcePath, operation, codec, jobsArg, samplesArg] = arguments;
const jobs = +jobsArg, samples = +samplesArg;
const original = JSON.parse(typeof __source === 'undefined' ? read(sourcePath) : __source);
let input = operation === 'encode' ? original : codec.startsWith('msgpack')
    ? MSGPACK.encode(original) : codec.includes('bytes')
      ? __utf8Encode(JSON.stringify(original)) : JSON.stringify(original);
const async = codec.includes('async');
let api = codec.startsWith('msgpack') ? MSGPACK[operation + (async ? 'Async' : '')]
    : JSON[(operation === 'encode' ? 'stringify' : 'parse') + (async ? 'Async' : '')];
if (codec.includes('bytes')) {
  if (operation === 'encode') api = async ? value => JSON.stringifyAsync(value).then(__utf8Encode)
                                         : value => __utf8Encode(JSON.stringify(value));
  else api = async ? value => JSON.parseAsync(__utf8Decode(value))
                   : value => JSON.parse(__utf8Decode(value));
}
async function main() {
  const validation = await api(input);
  const restored = operation === 'decode' ? validation : codec.startsWith('msgpack')
      ? MSGPACK.decode(validation) : JSON.parse(codec.includes('bytes') ? __utf8Decode(validation) : validation);
  if (JSON.stringify(restored) !== JSON.stringify(original)) throw new Error('value mismatch');
  if (operation === 'encode' && codec.startsWith('msgpack')) {
    const expected = MSGPACK.encode(original);
    if (expected.length !== validation.length || expected.some((v,i) => v !== validation[i]))
      throw new Error('wire mismatch');
  }
  const elapsed = [], submit = [];
  const inner = sourcePath.includes('tiny.json') ? 1000 : 1;
  let sink = 0;
  for (let i = -10; i < samples; ++i) {
    if (i === 0) gc();
    const start = performance.now();
    let submission = 0;
    for (let batch = 0; batch < inner; ++batch) {
      const begin = performance.now();
      const results = [];
      for (let j = 0; j < jobs; ++j) results.push(api(input));
      submission += performance.now() - begin;
      const values = async ? await Promise.all(results) : results;
      for (const value of values) sink += operation === 'encode' ? value.length
          : Array.isArray(value) ? value.length : Object.keys(value).length;
    }
    const end = performance.now();
    if (i >= 0) { elapsed.push((end - start) / inner); submit.push(submission / inner); }
  }
  print(JSON.stringify({operation, codec, jobs, innerIterations: inner, elapsed, submit, sink,
    msgpackBytes: MSGPACK.encode(original).length, jsonCodeUnits: JSON.stringify(original).length}));
}
main().catch(e => { print(e.stack || e); quit(1); });
