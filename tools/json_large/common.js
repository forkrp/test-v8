// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
'use strict';
// Iterative full-tree checks keep validation outside the timed API interval.
globalThis.largeJson = (() => {
  function wordsDigest() {
    let first = 2166136261, second = 0x9e3779b9;
    return {
      word(value) {
        first = Math.imul(first ^ value, 16777619);
        second = Math.imul(second ^ value, 0x85ebca6b);
      },
      result() {
        return [first, second].map(v => ('00000000' + (v >>> 0).toString(16)).slice(-8)).join('');
      }
    };
  }
  function textDigest(text) {
    const digest = wordsDigest();
    digest.word(text.length);
    for (let i = 0; i < text.length; ++i) digest.word(text.charCodeAt(i));
    return {code_units: text.length, digest: digest.result()};
  }
  function fingerprint(root) {
    const digest = wordsDigest();
    const bytes = new DataView(new ArrayBuffer(8));
    const parents = [], indices = [], keys = [], lengths = [];
    const counts = {objects: 0, arrays: 0, numbers: 0, strings: 0,
                    booleans: 0, nulls: 0, properties: 0,
                    string_code_units: 0, max_depth: 0};
    function mixText(text) {
      digest.word(text.length);
      for (let i = 0; i < text.length; ++i) digest.word(text.charCodeAt(i));
      counts.string_code_units += text.length;
    }
    let value = root;
    visit: while (true) {
      const depth = parents.length + 1;
      if (depth > counts.max_depth) counts.max_depth = depth;
      if (value === null) {
        digest.word(0); ++counts.nulls;
      } else if (typeof value === 'number') {
        digest.word(1); ++counts.numbers;
        bytes.setFloat64(0, value, true);
        digest.word(bytes.getUint32(0, true));
        digest.word(bytes.getUint32(4, true));
      } else if (typeof value === 'string') {
        digest.word(2); ++counts.strings; mixText(value);
      } else if (typeof value === 'boolean') {
        digest.word(3); digest.word(value ? 1 : 0); ++counts.booleans;
      } else if (typeof value === 'object') {
        const array = Array.isArray(value);
        if (Object.getPrototypeOf(value) !== (array ? Array.prototype : Object.prototype)) {
          throw Error('Unexpected prototype');
        }
        digest.word(array ? 4 : 5);
        if (array) ++counts.arrays; else ++counts.objects;
        const names = array ? null : Object.keys(value);
        const length = array ? value.length : names.length;
        digest.word(length);
        if (!array) counts.properties += length;
        if (length) {
          parents.push(value); indices.push(1); keys.push(names); lengths.push(length);
          if (names) mixText(names[0]);
          value = value[names ? names[0] : 0];
          continue;
        }
      } else {
        throw Error('Non-JSON value: ' + typeof value);
      }
      while (parents.length) {
        const top = parents.length - 1;
        if (indices[top] < lengths[top]) {
          const index = indices[top]++;
          const names = keys[top];
          if (names) mixText(names[index]);
          value = parents[top][names ? names[index] : index];
          continue visit;
        }
        parents.pop(); indices.pop(); keys.pop(); lengths.pop();
      }
      return {version: 1, digest: digest.result(), counts};
    }
  }
  function same(actual, expected, label) {
    if (JSON.stringify(actual) !== JSON.stringify(expected)) {
      throw Error(label + '\nactual=' + JSON.stringify(actual) +
                  '\nexpected=' + JSON.stringify(expected));
    }
  }
  function median(values) {
    const sorted = [...values].sort((a,b) => a-b);
    const mid = sorted.length >> 1;
    return sorted.length & 1 ? sorted[mid] : (sorted[mid-1] + sorted[mid]) / 2;
  }
  return {fingerprint, textDigest, same, median};
})();
