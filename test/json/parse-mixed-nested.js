// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc, and again with --stress-compaction. The output must
// match an independently frozen d8, including SyntaxError positions.
'use strict';
let comparisons = 0, hash = 2166136261;
function check(ok, message) { if (!ok) throw Error(message); }
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  }
  hash = Math.imul(hash ^ 0xffff, 16777619) >>> 0;
}
function same(actual, expected) {
  check(typeof actual === typeof expected, 'type');
  if (actual === null || typeof actual !== 'object') {
    check(Object.is(actual, expected), 'value');
    record(typeof actual);
    record(String(actual));
    return;
  }
  const array = Array.isArray(expected);
  check(Array.isArray(actual) === array, 'array');
  check(Object.getPrototypeOf(actual) ===
        (array ? Array.prototype : Object.prototype), 'prototype');
  const keys = Object.keys(expected);
  check(JSON.stringify(Object.keys(actual)) === JSON.stringify(keys), 'keys');
  for (const key of keys) {
    const desc = Object.getOwnPropertyDescriptor(actual, key);
    check(desc.enumerable && desc.configurable && desc.writable &&
          Object.hasOwn(desc, 'value'), 'descriptor');
    record(key);
    same(desc.value, expected[key]);
  }
}
async function compare(source, expected) {
  same(JSON.parse(source), expected);
  same(await JSON.parseAsync(source), expected);
  ++comparisons;
}
async function invalid(source) {
  let message;
  try { JSON.parse(source); } catch (error) {
    check(error instanceof SyntaxError, 'sync error');
    message = error.message;
  }
  check(message !== undefined, 'accepted invalid string');
  try { await JSON.parseAsync(source); } catch (error) {
    check(error instanceof SyntaxError && error.message === message, 'async error');
    record(message);
    ++comparisons;
    return;
  }
  throw Error('async accepted invalid string');
}
async function main() {
  const escapes = [
    ['\\"', '"'], ['\\\\', '\\'], ['\\n', '\n'], ['\\t', '\t'],
    ['\\u0000', '\0'], ['\\u00ff', '\xff'], ['\\u0100', '\u0100'],
    ['\\ud83d\\ude00', '\ud83d\ude00'], ['\\ud800', '\ud800'],
  ];
  // Both sides of the short probe and vector block boundaries, plus long
  // suffixes. Unicode before a boundary must survive the continuation.
  for (const length of [0, 1, 7, 15, 16, 31, 32, 33, 63, 64, 65]) {
    for (const unit of ['a', '\xff', '\u0100', '漢', '\ud800']) {
      const prefix = unit.repeat(length);
      for (const [escaped, decoded] of escapes) {
        for (const force of ['a', '漢']) {
          const suffix = length & 1 ? 'x'.repeat(73) + '終' : 'tail';
          const encoded = prefix + escaped + suffix;
          const expected = prefix + decoded + suffix;
          await compare('["' + force + '","' + encoded + '"]', [force, expected]);
          await compare('{"' + encoded + '":"' + force + '"}', {[expected]: force});
        }
      }
      // No escape: crossing a probe boundary must retain width information.
      await compare('["' + prefix + 'tail"]', [prefix + 'tail']);
    }
    for (const force of ['', '漢']) {
      const prefix = force + 'a'.repeat(length);
      for (const ending of ['', '\\', '\\u', '\\u123', '\\u00G0"',
                            '\\q"', '\n"', '\0"']) {
        await invalid('"' + prefix + ending);
      }
    }
    if (typeof gc === 'function') gc();
  }
  // Ordinary nested objects get no feedback from their parent; adjacent
  // outer objects also vary their layout and the types of nested values.
  for (let i = 0; i < 96; ++i) {
    let source = '{"id":' + i + ',"payload":{"items":[-0,1.25,"漢",null],' +
                 '"line\\nkey":"quote=\\\" slash=\\\\"},"active":' + !!(i & 1) + '}';
    let value = {id:i, payload:{items:[-0,1.25,'漢',null],
                               'line\nkey':'quote=" slash=\\'}, active:!!(i & 1)};
    for (let depth = 0; depth < i % 25; ++depth) {
      source = '{"level":' + depth + ',"child":' + source +
               ',"tail":[true,null,' + depth / 8 + ']}';
      value = {level:depth, child:value, tail:[true,null,depth/8]};
    }
    await compare('[' + source + ',' + source + ']', [value,value]);
  }
  // Extra native frames must still hand deeply nested input to the existing
  // iterative fallback, with correct handles and identical error positions.
  for (const depth of [64, 512, 2048, 4096]) {
    const source = '{"n":1,"child":'.repeat(depth) + '"漢\\n"' + '}'.repeat(depth);
    for (const value of [JSON.parse(source), await JSON.parseAsync(source)]) {
      let node = value;
      for (let i = 0; i < depth; ++i) {
        check(Object.getPrototypeOf(node) === Object.prototype, 'deep prototype');
        check(Object.keys(node).join(',') === 'n,child' && node.n === 1, 'deep node');
        node = node.child;
      }
      check(node === '漢\n', 'deep leaf');
      record(String(depth));
      record(node);
    }
    ++comparisons;
    await invalid(source.slice(0, -1));
    if (typeof gc === 'function') gc();
  }
  print('mixed-nested comparisons=' + comparisons + ' hash=' + hash.toString(16));
}
main().catch(error => { print(error.stack || error); quit(1); });
