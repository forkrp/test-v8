// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --expose-gc, and again with --stress-compaction.
'use strict';

function check(condition, message) {
  if (!condition) throw Error(message);
}

let comparisons = 0, hash = 2166136261;
function record(text) {
  for (let i = 0; i < text.length; ++i) {
    hash = Math.imul(hash ^ text.charCodeAt(i), 16777619) >>> 0;
  }
}

function signature(root) {
  const saved = hash;
  hash = 2166136261;
  const stack = [root];
  while (stack.length) {
    const value = stack.pop();
    record(typeof value + ':');
    if (value === null || typeof value !== 'object') {
      record(Object.is(value, -0) ? '-0' : String(value));
      continue;
    }
    const array = Array.isArray(value);
    check(Object.getPrototypeOf(value) ===
          (array ? Array.prototype : Object.prototype), 'prototype');
    record(array ? 'array' : 'object');
    const keys = Object.keys(value);
    record(JSON.stringify(keys));
    for (const key of keys) {
      const desc = Object.getOwnPropertyDescriptor(value, key);
      check(desc.writable && desc.enumerable && desc.configurable &&
            Object.hasOwn(desc, 'value'), 'data property');
      stack.push(desc.value);
    }
  }
  const result = hash;
  hash = saved;
  return result;
}

function sourceAtDepth(depth, kind, wide) {
  let source = '{"value":-0,"fraction":1.25,"text":' +
      (wide ? '"漢字"' : '"value"') + ',"__proto__":{"own":true}}';
  for (let i = 1; i < depth; ++i) {
    const array = kind === 0 || (kind === 2 && (i & 1));
    source = array ? '[' + source + ',{"tail":' + i + '}]' :
        '{"child":' + source + ',"tail":' + i + '}';
  }
  return source;
}

async function main() {
  // Repeated siblings reuse the stack after it has spilled and unwound. The
  // two encodings and a reviver exercise both worker specializations and tapes.
  for (const depth of [1, 2, 15, 16, 17, 31, 32, 33, 128, 512]) {
    for (const kind of [0, 1, 2]) {
      for (const wide of [false, true]) {
        const nested = sourceAtDepth(depth, kind, wide);
        const source = '[' + [nested, nested, nested].join(',') + ']';
        const expected = signature(JSON.parse(source));
        const pending = Array.from({length: 4}, () => JSON.parseAsync(source));
        gc();
        for (const value of await Promise.all(pending)) {
          check(signature(value) === expected, 'concurrent stack growth');
          record(String(signature(value)));
          ++comparisons;
        }

        const traces = [[], []];
        function reviver(index) {
          return function(key, value, context) {
            traces[index].push([key, context.source]);
            return value;
          };
        }
        const sync = JSON.parse(source, reviver(0));
        const async = await JSON.parseAsync(source, reviver(1));
        check(signature(sync) === signature(async), 'reviver value');
        check(JSON.stringify(traces[0]) === JSON.stringify(traces[1]),
              'reviver source and order');
        record(String(signature(async)));
        ++comparisons;
      }
    }
  }
  for (const depth of [15, 16, 17, 32, 33, 512]) {
    for (const source of ['['.repeat(depth) + '0' + ']'.repeat(depth - 1),
                          '['.repeat(depth) + '01' + ']'.repeat(depth)]) {
      let expected;
      try { JSON.parse(source); } catch (error) { expected = error; }
      check(expected instanceof SyntaxError, 'invalid fixture');
      try {
        await JSON.parseAsync(source);
        throw Error('missing rejection');
      } catch (error) {
        check(error instanceof SyntaxError && error.message === expected.message,
              'error after stack growth');
      }
      ++comparisons;
    }
  }
  print('async-stack comparisons=' + comparisons + ' hash=' + hash.toString(16));
}

main().catch(error => { print(error.stack || error); quit(1); });
