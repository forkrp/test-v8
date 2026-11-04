// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with --expose-gc; also exercise --stress-compaction --verify-heap.
'use strict';

let checks = 0;
let digest = 2166136261;
function record(value) {
  const text = JSON.stringify(value);
  for (let i = 0; i < text.length; ++i) {
    digest = Math.imul(digest ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  ++checks;
}
function asciiJSON(value) {
  return JSON.stringify(value).replace(/[\u0100-\uffff]/g, c =>
      '\\u' + c.charCodeAt(0).toString(16).padStart(4, '0'));
}
async function verify(text, expected) {
  const canonical = JSON.stringify(expected);
  for (const parse of [JSON.parse, JSON.parseAsync]) {
    const actual = await parse(text);
    if (JSON.stringify(actual) !== canonical) throw Error('Decoded value');
    record(actual);
    const revived = await parse(text, (key, value, context) => {
      if (typeof value === 'string' &&
          (typeof context.source !== 'string' ||
           !text.includes(context.source) ||
           JSON.parse(context.source) !== value)) {
        throw Error('Reviver source');
      }
      return value;
    });
    if (JSON.stringify(revived) !== canonical) throw Error('Revived value');
    record(revived);
  }
}

(async function() {
  const lengths = [0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
                   95, 96, 97, 127, 128, 129, 143, 144, 145, 255, 256,
                   257, 511, 512, 513, 4096];
  const dense = '\n\t"\\/\b\f\r';
  for (const length of lengths) {
    for (const escape of ['\n', '\\', '"', '\u00ff', '\u1234', '\ud800',
                          '\udfff', '\ud83d\ude00']) {
      const values = [
        'x'.repeat(length) + escape + 'y'.repeat(513),
        escape + 'x'.repeat(length) + '\t' + 'z'.repeat(129),
        'x'.repeat(length) + dense.repeat(80),
        dense.repeat(80) + 'y'.repeat(length)
      ];
      for (const value of values) {
        // ASCII source with both one-byte and two-byte decoded results.
        await verify(asciiJSON(value), value);
        // Force a two-byte source while retaining a narrow first result when
        // its contents permit it. This also covers output block remainders.
        await verify('[' + asciiJSON(value) + ',"漢"]', [value, '漢']);
      }
    }
  }
  // Escaped keys pass through the same decoder and must still internalize and
  // overwrite duplicates correctly, including across a moving collection.
  for (const length of [127, 128, 129, 256, 4096]) {
    const key = 'k'.repeat(length) + '\n漢';
    const encoded = asciiJSON(key);
    await verify('{' + encoded + ':1,' + encoded + ':2}', {[key]: 2});
    gc();
  }
  print('PASS string-decode', checks, digest.toString(16));
})().catch(error => { print(error.stack || error); quit(1); });
