// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run by test_cli.py with the matching d8 binary; never part of CLI conversion.
const cases = JSON.parse(read(arguments[0]));
function check(value, message) {
  if (!value) throw new Error(message);
}
function equal(a, b, path = '$') {
  if (a === null || typeof a !== 'object') {
    check(Object.is(a, b), 'Value mismatch at ' + path);
    return;
  }
  check(b !== null && typeof b === 'object', 'Object mismatch at ' + path);
  check(Array.isArray(a) === Array.isArray(b), 'Array mismatch at ' + path);
  const keys = Object.keys(a), other = Object.keys(b);
  check(keys.length === other.length, 'Key count mismatch at ' + path);
  for (let i = 0; i < keys.length; i++) {
    check(keys[i] === other[i], 'Key order mismatch at ' + path);
    equal(a[keys[i]], b[keys[i]], path + '.' + keys[i]);
  }
  if (Array.isArray(a)) check(a.length === b.length, 'Array length mismatch at ' + path);
}
for (const item of cases) {
  let source = read(item.input);
  if (source.charCodeAt(0) === 0xfeff) source = source.slice(1);
  const value = JSON.parse(source);
  const wire = new Uint8Array(readbuffer(item.wire));
  const reference = item.standard ? MSGPACK.encode(value) : MSGPACK.encodeResource(value);
  check(wire.length === reference.length, 'Wire length mismatch: ' + item.name);
  for (let i = 0; i < wire.length; i++) {
    check(wire[i] === reference[i], 'Wire byte mismatch: ' + item.name + ' at ' + i);
  }
  equal(MSGPACK.decode(MSGPACK.encode(value)), MSGPACK.decodeResource(wire));
}
print('PASS: ' + cases.length + ' CLI outputs match public API bytes and decoded values');
