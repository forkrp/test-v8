// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// d8 --expose-gc async-benchmark.js -- source.json operation codec jobs samples
'use strict';
const [sourcePath, operation, codec, jobsArg, samplesArg] = arguments;
const jobs = +jobsArg, samples = +samplesArg;
if (!['encode', 'decode'].includes(operation) ||
    !['msgpack', 'msgpack_async', 'msgpack_resource', 'msgpack_resource_async',
      'json', 'json_async', 'json_bytes', 'json_async_bytes'].includes(codec) ||
    !Number.isInteger(jobs) || jobs < 1 || jobs > 32 ||
    !Number.isInteger(samples) || samples < 1) throw new Error('invalid benchmark case');
const original = JSON.parse(typeof __source === 'undefined' ? read(sourcePath) : __source);
const resource = codec.startsWith('msgpack_resource');
const standardBytes = MSGPACK.encode(original);
const resourceBytes = typeof MSGPACK.encodeResource === 'function'
    ? MSGPACK.encodeResource(original) : null;
const jsonText = JSON.stringify(original);
const jsonBytes = typeof __utf8Encode === 'function' ? __utf8Encode(jsonText) : null;
if (resource && resourceBytes === null) throw new Error('resource API is unavailable');
let input = operation === 'encode' ? original : codec.startsWith('msgpack')
    ? resource ? resourceBytes : standardBytes : codec.includes('bytes')
      ? jsonBytes : jsonText;
const async = codec.includes('async');
let api = codec.startsWith('msgpack')
    ? MSGPACK[operation + (resource ? 'Resource' : '') + (async ? 'Async' : '')]
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
      ? MSGPACK[resource ? 'decodeResource' : 'decode'](validation)
      : JSON.parse(codec.includes('bytes') ? __utf8Decode(validation) : validation);
  if (JSON.stringify(restored) !== jsonText) throw new Error('value mismatch');
  if (operation === 'encode' && codec.startsWith('msgpack')) {
    const expected = resource ? resourceBytes : standardBytes;
    if (expected.length !== validation.length || expected.some((v,i) => v !== validation[i]))
      throw new Error('wire mismatch');
  }
  const elapsed = [], submit = [];
  const inner = sourcePath.includes('tiny.json') ? 1000 : 1;
  let sink = 0, completedOperations = 0;
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
      completedOperations += values.length;
    }
    const end = performance.now();
    if (i >= 0) { elapsed.push((end - start) / inner); submit.push(submission / inner); }
  }
  print(JSON.stringify({operation, codec, jobs, innerIterations: inner, elapsed, submit, sink,
    completedOperations, warmupBatches: 10, valueValidated: true,
    wireValidated: operation === 'encode' && codec.startsWith('msgpack'),
    msgpackBytes: standardBytes.length, resourceBytes: resourceBytes?.length ?? null,
    resourceCompressed: resourceBytes !== null && resourceBytes.length >= 5 &&
        resourceBytes[0] === 86 && resourceBytes[1] === 56 && resourceBytes[2] === 77 &&
        resourceBytes[3] === 82 && resourceBytes[4] === 1,
    jsonCodeUnits: jsonText.length, jsonBytes: jsonBytes?.length ?? null}));
}
main().catch(e => { print(e.stack || e); quit(1); });
