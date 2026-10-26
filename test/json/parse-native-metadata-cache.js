// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Large inputs exercise worker caches, including first-entry collisions.
'use strict';
let comparisons = 0, hash = 2166136261;
function check(ok, message) { if (!ok) throw Error(message); }
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  hash = Math.imul(hash ^ 0xffff, 16777619) >>> 0;
}
async function compare(value, reviver = false) {
  const source = JSON.stringify(value);
  check(source.length > 100000, 'worker-sized input');
  const sync = JSON.parse(source);
  check(JSON.stringify(sync) === source, 'sync complete output');
  const callback = reviver ? function(key, item, context) {
    if (typeof item === 'string' && context && Object.hasOwn(context, 'source')) {
      record(context.source);
    }
    return item;
  } : undefined;
  const actual = await JSON.parseAsync(source, callback);
  check(JSON.stringify(actual) === source, 'async complete output');
  record(source);
  ++comparisons;
  if (typeof gc === 'function') gc();
}
async function main() {
  // aa and bB collide in the bounded short-string hash. Empty strings are
  // valid cached values; empty keys must keep their uncached-key handling.
  for (const wide of [false, true]) {
    const words = wide ? ['aa', 'bB', '', '漢', '😀', '\u0100'] :
                         ['aa', 'bB', '', 'Aa', 'BB', '\xff'];
    const rows = Array.from({length:12000}, (_, i) => {
      const row = {id:i, text:words[i % words.length]};
      row[i & 1 ? 'aa' : 'bB'] = words[(i + 1) % words.length];
      row[i & 1 ? '' : 'Aa'] = words[(i + 2) % words.length];
      row['line\nkey'] = 'quote=" slash=\\';
      return row;
    });
    await compare(rows);
    await compare(rows, true);
  }
  // Cross the 64-slot/field limit while retaining colliding short values.
  const wideRows = Array.from({length:1000}, (_, i) => {
    const row = {};
    for (let k = 0; k < 66; ++k) {
      row['k' + k] = k % 3 === 0 ? i : k & 1 ? 'aa' : 'bB';
    }
    return row;
  });
  await compare(wideRows);
  await compare(Array.from({length:50000}, (_, i) =>
      ['aa','bB','', 'value', '漢', '😀'][i % 6]));
  print('native-metadata-cache comparisons=' + comparisons + ' hash=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
