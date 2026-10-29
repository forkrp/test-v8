// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run with d8 --expose-gc. Compare the final digest with the frozen baseline.
'use strict';

let checks = 0;
let digest = 2166136261;
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    digest = Math.imul(digest ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  ++checks;
}

(async function() {
  async function valid(text, expected) {
    for (const parse of [JSON.parse, JSON.parseAsync]) {
      const result = await parse(text);
      const canonical = JSON.stringify(result);
      if (canonical !== JSON.stringify(expected)) throw Error('string value');
      record(canonical);
    }
  }
  const lengths = [0, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 95, 96,
                   97, 127, 128, 129, 159, 160, 191, 192, 193, 255, 256,
                   257, 319, 320, 321, 1023, 1024, 4096];
  for (const length of lengths) {
    for (const unit of ['a', '\u00ff', '\u0100', '\uffff']) {
      const value = unit.repeat(length);
      await valid(JSON.stringify(value), value);
      await valid(JSON.stringify([value, '\u6f22']), [value, '\u6f22']);
      await valid(JSON.stringify({[value]: value}), {[value]: value});
    }
  }
  for (let offset = 0; offset < 160; ++offset) {
    const prefix = 'a'.repeat(32 + offset);
    // Wide characters must be accounted for even when their entire block is
    // accepted at once. Exercise each lane and blocks after the first one.
    for (const middle of ['\u0100', '\uffff', '\ud800', '\udfff', '\u{1f600}']) {
      const value = prefix + middle + 'z'.repeat(160);
      await valid('"' + value + '"', value);
    }
    for (const [escape, decoded] of [['\\u0100', '\u0100'], ['\\n', '\n'],
                                     ['\\"', '"'], ['\\\\', '\\']]) {
      await valid('"' + prefix + escape + 'tail"', prefix + decoded + 'tail');
    }
    // A wide character after a closing quote must not be read as part of the
    // preceding value. Also exercise an escaped quote at every block offset.
    await valid('["' + prefix + '","\u6f22"]', [prefix, '\u6f22']);
    for (const bad of [0, 9, 10, 31]) {
      for (const wide of [false, true]) {
        const text = '["' + prefix + String.fromCharCode(bad) +
                     'tail",' + (wide ? '"\u6f22"' : '0') + ']';
        let syncMessage;
        try { JSON.parse(text); throw Error('accepted invalid control'); }
        catch (error) {
          if (!(error instanceof SyntaxError)) throw error;
          syncMessage = error.message;
        }
        try { await JSON.parseAsync(text); throw Error('async accepted control'); }
        catch (error) {
          if (!(error instanceof SyntaxError) || error.message !== syncMessage)
            throw error;
        }
        record(syncMessage);
      }
    }
  }
  gc();
  print('PASS string-blocks', checks, digest.toString(16));
})().catch(error => { print(error.stack); quit(1); });
