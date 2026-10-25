// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Run with --allow-natives-syntax --expose-gc.
'use strict';

let checks = 0;
function check(condition, message = 'JSON.clone check failed') {
  ++checks;
  if (!condition) throw new Error(message);
}

function equal(actual, expected, path = '$') {
  if (expected === null || typeof expected !== 'object') {
    check(Object.is(actual, expected), path + ': primitive mismatch');
    return;
  }
  check(actual !== null && typeof actual === 'object', path + ': not object');
  check(Array.isArray(actual) === Array.isArray(expected), path + ': array');
  check(Object.getPrototypeOf(actual) === Object.getPrototypeOf(expected),
        path + ': prototype');
  check(Object.isExtensible(actual), path + ': extensibility');
  const keys = Reflect.ownKeys(expected);
  const actualKeys = Reflect.ownKeys(actual);
  check(keys.length === actualKeys.length, path + ': key count');
  for (let i = 0; i < keys.length; ++i) {
    const key = keys[i];
    check(key === actualKeys[i], path + ': key order');
    const a = Object.getOwnPropertyDescriptor(actual, key);
    const b = Object.getOwnPropertyDescriptor(expected, key);
    check(a.enumerable === b.enumerable && a.configurable === b.configurable &&
          a.writable === b.writable && 'value' in a, path + ': descriptor');
    equal(a.value, b.value, path + '.' + String(key));
  }
}

const jsonRoundTrip = value => JSON.parse(JSON.stringify(value));
function compare(value) {
  const result = JSON.clone(value);
  equal(result, jsonRoundTrip(value));
  return result;
}

// All result containers must be new, including repeated references. Strings
// and other immutable primitives may share storage inside V8.
function independent(source, result) {
  const sourceObjects = new Set();
  function collect(value) {
    if (value === null || typeof value !== 'object' || sourceObjects.has(value)) {
      return;
    }
    sourceObjects.add(value);
    for (const key of Object.keys(value)) collect(value[key]);
  }
  collect(source);
  const resultObjects = new Set();
  function visit(value) {
    if (value === null || typeof value !== 'object') return;
    check(!sourceObjects.has(value), 'container shared with input');
    check(!resultObjects.has(value), 'repeated reference preserved');
    resultObjects.add(value);
    for (const key of Object.keys(value)) visit(value[key]);
  }
  visit(result);
}

function outcome(operation) {
  try { return {value: operation()}; }
  catch (error) { return {error: error.name, message: error.message}; }
}

// Use fresh inputs so getters, proxies and toJSON see exactly the same state.
function effects(factory) {
  const actual = factory();
  const expected = factory();
  equal(outcome(() => JSON.clone(actual.value)),
        outcome(() => jsonRoundTrip(expected.value)));
  equal(actual.log, expected.log);
}

check(JSON.clone.name === 'clone');
check(JSON.clone.length === 1);
const descriptor = Object.getOwnPropertyDescriptor(JSON, 'clone');
check(!descriptor.enumerable && descriptor.writable && descriptor.configurable);
check(!Object.keys(JSON).includes('clone'));
check(outcome(() => new JSON.clone({})).error === 'TypeError');
equal(outcome(() => JSON.clone()), outcome(() => jsonRoundTrip(undefined)));

for (const value of [null, true, false, 0, -0, 1, -1, 2147483648, 0.1,
    Number.MIN_VALUE, Number.MAX_VALUE, Number.MAX_SAFE_INTEGER,
    NaN, Infinity, -Infinity, '', 'ASCII', '漢字🌏', '\ud800', '\udc00',
    '"\\\n\u0000', 'x'.repeat(10000), 'x'.repeat(9999) + '漢']) {
  compare(value);
  compare({value});
  compare([value]);
}
for (const value of [undefined, function f() {}, Symbol('s'), 1n, {a: 1n}]) {
  equal(outcome(() => JSON.clone(value)), outcome(() => jsonRoundTrip(value)));
}

const cons = 'left'.repeat(2000) + 'right'.repeat(2000);
const sliced = cons.slice(30, cons.length - 30);
for (const value of [cons, sliced, {cons, sliced}]) compare(value);

const shared = {number: 1.25, child: {text: 'original'}, list: [0.5, 1.5]};
const tree = {left: shared, right: shared, array: [shared, shared]};
const treeCopy = compare(tree);
independent(tree, treeCopy);
treeCopy.left.child.text = 'copy';
treeCopy.array[0].list[0] = 7.5;
check(shared.child.text === 'original' && shared.list[0] === 0.5);
check(treeCopy.right.child.text === 'original');
check(treeCopy.array[1].list[0] === 0.5);

// In-object and out-of-object mutable double boxes must be independent in
// both directions, including after optimization and a moving collection.
function numericRecord() {
  const value = {inObject: 1.25};
  for (let i = 0; i < 12; ++i) value['out' + i] = i + 0.25;
  return value;
}
const numericSource = numericRecord();
check(%HasFastProperties(numericSource));
const numericCopies = [];
for (let iteration = 0; iteration < 100; ++iteration) {
  numericCopies.push(JSON.clone(numericSource));
}
gc();
function writeNumbers(value) {
  value.inObject = 9.75;
  value.out11 = 111.5;
}
%PrepareFunctionForOptimization(writeNumbers);
for (let i = 0; i < 10; ++i) writeNumbers(numericCopies[i]);
%OptimizeFunctionOnNextCall(writeNumbers);
writeNumbers(numericCopies[10]);
for (const value of numericCopies) {
  writeNumbers(value);
}
check(numericSource.inObject === 1.25 && numericSource.out11 === 11.25);
numericSource.inObject = 3.5;
numericSource.out11 = 21.5;
gc();
for (const value of numericCopies) {
  check(value.inObject === 9.75 && value.out11 === 111.5);
}

// Reuse layouts through numeric, object and string representation changes.
function record(value) { return {first: value, last: value}; }
for (const value of [1, 1.25, 2.5, {n: 3.25}, 'text', null, -0, NaN, 8]) {
  const input = record(value);
  const copy = compare(input);
  if (value !== null && typeof value === 'object') independent(input, copy);
}

const smiArray = [1, 2, 3];
const objectArray = ['left', 'right', null, true];
const doubleArray = [];
for (let i = 0; i < 20000; ++i) doubleArray.push(i + 0.25);
const emptyDoubles = [1.25];
emptyDoubles.pop();
check(%HasSmiElements(smiArray));
check(%HasDoubleElements(doubleArray));
for (const input of [[], emptyDoubles, smiArray, objectArray, doubleArray,
                     [{x: 1.25}, ['nested'], {x: 2.5}]]) {
  const copy = compare(input);
  independent(input, copy);
  if (copy.length) copy[0] = 'changed';
}
equal(smiArray, [1, 2, 3]);
equal(objectArray, ['left', 'right', null, true]);
check(doubleArray[0] === 0.25);

// The fallback restores standard JSON normalization and property attributes.
const nullPrototype = Object.assign(Object.create(null), {x: {n: 1.25}});
const dictionary = {};
for (let i = 0; i < 200; ++i) dictionary['key' + i] = {i};
const specialProperties = {visible: 1};
Object.defineProperty(specialProperties, 'hidden', {value: {x: 2}});
Object.defineProperty(specialProperties, 'readonly', {
  value: 3, enumerable: true, writable: false, configurable: false
});
specialProperties[Symbol('ignored')] = {x: 4};
const indexed = {'10': {x: 1}, '2': 'two', named: 5};
const specialArray = [1, 2];
specialArray.ignored = {x: 5};
Object.defineProperty(specialArray, 'unused', {
  get() { throw new Error('array named property must not be read'); }
});
const readonlyLength = [1, 2];
Object.defineProperty(readonlyLength, 'length', {writable: false});
const usedAsPrototype = {x: 1.25, child: {n: 2.5}};
const inheritor = Object.create(usedAsPrototype);
check(Object.getPrototypeOf(inheritor) === usedAsPrototype);
function AlternateConstructor() { this.x = {n: 1.25}; }
AlternateConstructor.prototype = Object.prototype;
for (const value of [nullPrototype, dictionary, specialProperties, indexed,
    specialArray, readonlyLength, usedAsPrototype, new AlternateConstructor(),
    Object.freeze({x: {n: 1.25}}),
    Object.seal([1, 2]), Object.preventExtensions({}), Object.preventExtensions([]),
    [1, , 3], new Array(5), [undefined, function f() {}, Symbol('s')],
    {a: undefined, b() {}, c: Symbol('s'), d: NaN, e: -0},
    new Date('2026-10-23T00:00:00Z'), /regexp/g, new Map([['x', 1]]),
    new Set([1]), new Number(-0), new String('漢'), new Boolean(false),
    Object(Symbol('s')), new Uint8Array([1, 2]), JSON.rawJSON('12')]) {
  compare(value);
}
const protoKey = JSON.parse('{"__proto__":{"polluted":true},"x":1}');
const protoCopy = compare(protoKey);
check(Object.hasOwn(protoCopy, '__proto__'));
check(Object.getPrototypeOf(protoCopy) === Object.prototype);
check({}.polluted === undefined);

effects(() => {
  const log = [];
  const value = {first: {n: 1.25}, get next() {
    log.push('get next');
    delete this.tail;
    this.added = 4;
    return {x: 2};
  }, tail: 3};
  return {value, log};
});
effects(() => {
  const log = [];
  const value = {first: {n: 1}, later: {get toJSON() {
    log.push('get toJSON');
    return function(key) {
      log.push('call toJSON:' + key);
      return {changed: true};
    };
  }}};
  return {value, log};
});
effects(() => {
  const log = [];
  const proxy = new Proxy({x: 1, y: {n: 1.25}}, {
    get(target, key, receiver) {
      log.push('get:' + String(key));
      return Reflect.get(target, key, receiver);
    },
    ownKeys(target) { log.push('keys'); return Reflect.ownKeys(target); },
    getOwnPropertyDescriptor(target, key) {
      log.push('descriptor:' + String(key));
      return Reflect.getOwnPropertyDescriptor(target, key);
    },
    getPrototypeOf(target) { log.push('prototype'); return Reflect.getPrototypeOf(target); }
  });
  return {value: {before: {ok: true}, proxy}, log};
});
effects(() => {
  const log = [];
  const value = {before: {ok: true}, get late() {
    log.push('throw');
    throw new RangeError('from getter');
  }};
  return {value, log};
});
effects(() => {
  const log = [];
  const proto = {toJSON(key) { log.push('inherited:' + key); return {x: 7}; }};
  return {value: {before: {ok: true}, later: Object.create(proto)}, log};
});
effects(() => {
  const log = [];
  const value = {before: {ok: true}, get late() {
    log.push('cycle');
    return value;
  }};
  return {value, log};
});
const cycle = {x: 1};
cycle.self = cycle;
for (const value of [cycle, {a: cycle}, (() => { const a = []; a.push(a); return a; })()]) {
  equal(outcome(() => JSON.clone(value)), outcome(() => jsonRoundTrip(value)));
}
const revoked = Proxy.revocable({}, {});
revoked.revoke();
equal(outcome(() => JSON.clone(revoked.proxy)),
      outcome(() => jsonRoundTrip(revoked.proxy)));

// Exercise native depth bailout without imposing that limit on the API.
let deep = {leaf: 'end'};
for (let i = 0; i < 300; ++i) deep = {child: deep};
compare(deep);

// Fixed-seed differential coverage includes fast trees and normalization /
// descriptor / dictionary fallbacks. No timing or fixture creation is hidden.
let seed = 0x3518ad2;
const next = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0);
function randomValue(depth) {
  const n = next();
  switch (n % (depth ? 10 : 6)) {
    case 0: return null;
    case 1: return !!(n & 0x100);
    case 2: return (n | 0) / 7;
    case 3: return ['', 'ascii', '漢😀', '\ud800', '"\n\\'][next() % 5];
    case 4: return [undefined, -0, NaN, Infinity][next() % 4];
    case 5: return n | 0;
    case 6: {
      const result = [];
      for (let i = 0, size = next() % 7; i < size; ++i) {
        result.push(randomValue(depth - 1));
      }
      return result;
    }
    default: {
      const result = next() % 5 ? {} : Object.create(null);
      for (let i = 0, size = next() % 9; i < size; ++i) {
        const key = next() % 5 ? 'key' + i : String((next() % 100) * 10 + i);
        const value = randomValue(depth - 1);
        if (next() % 7) result[key] = value;
        else Object.defineProperty(result, key, {value, enumerable: true});
      }
      return result;
    }
  }
}
for (let i = 0; i < 2000; ++i) {
  const value = randomValue(4);
  const expected = outcome(() => jsonRoundTrip(value));
  const actual = outcome(() => JSON.clone(value));
  equal(actual, expected);
  if (!('error' in actual)) independent(value, actual.value);
  if (i % 200 === 0) gc();
}

// Cross-realm inputs must produce containers in the calling builtin's realm.
const realm = Realm.create();
const foreign = Realm.eval(realm, '({a: {n: 1.25}, b: [1, 2]})');
compare(foreign);
const foreignClone = Realm.eval(realm, 'JSON.clone');
const foreignResult = foreignClone({x: {n: 1.25}, y: [2, 3]});
check(Object.getPrototypeOf(foreignResult) === Realm.eval(realm, 'Object.prototype'));
check(Object.getPrototypeOf(foreignResult.y) === Realm.eval(realm, 'Array.prototype'));

// The builtin uses the intrinsic algorithms, not replaceable JSON methods.
const savedParse = JSON.parse;
const savedStringify = JSON.stringify;
let intrinsicCopy;
try {
  JSON.parse = JSON.stringify = () => { throw new Error('patched JSON method'); };
  intrinsicCopy = JSON.clone({get x() { return 1; }});
} finally {
  JSON.parse = savedParse;
  JSON.stringify = savedStringify;
}
equal(intrinsicCopy, {x: 1});

// Run prototype mutations last: V8 may conservatively keep the interesting-
// property flag even after toJSON is deleted.
for (const prototype of [Object.prototype, Array.prototype]) {
  let calls = 0;
  try {
    Object.defineProperty(prototype, 'toJSON', {
      configurable: true,
      get() {
        ++calls;
        return function(key) { ++calls; return 'hook:' + key; };
      }
    });
    const input = prototype === Array.prototype ? [1, 2] : {x: 1};
    check(JSON.clone(input) === 'hook:');
    check(calls === 2, 'prototype toJSON evaluated more than once');
  } finally {
    delete prototype.toJSON;
  }
}
let inheritedReads = 0;
let inheritedCopy;
try {
  Object.defineProperty(Array.prototype, '1', {
    configurable: true,
    get() { ++inheritedReads; return 42; }
  });
  inheritedCopy = JSON.clone([0, , 2]);
} finally {
  delete Array.prototype[1];
}
equal(inheritedCopy, [0, 42, 2]);
check(inheritedReads === 1);

print('JSON.clone: ' + checks + ' checks passed');
