// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Flags: --allow-natives-syntax --expose-gc

// Exercise shared key/map caches and input/output ownership across both APIs.
async function testAsyncResourceIntegration() {
  for (const method of ['encode', 'decode', 'encodeAsync', 'decodeAsync',
                        'encodeResource', 'decodeResource',
                        'encodeResourceAsync', 'decodeResourceAsync']) {
    assertEquals(1, MSGPACK[method].length);
    assertFalse(Object.getOwnPropertyDescriptor(MSGPACK, method).enumerable);
  }
  for (const length of [63, 64, 65, 255, 256, 4095, 4096, 4097]) {
    let text = 'a中🌏é Ελληνικά'.repeat(length).slice(0, length);
    if (text.charCodeAt(text.length - 1) >= 0xd800 &&
        text.charCodeAt(text.length - 1) <= 0xdbff) {
      text = text.slice(0, -1) + 'a';
    }
    const value = Array.from({length: 300}, (_, i) => ({
      [text]: i % 2 ? text : String(i),
      identifier: i, label: text, child: {value: i / 10, active: !!(i & 1)}
    }));
    const expected = MSGPACK.decode(MSGPACK.encode(value));
    const standard = MSGPACK.encode(value);
    const encoding = MSGPACK.encodeAsync(value);
    const resource = MSGPACK.encodeResource(value);
    const resourceEncoding = MSGPACK.encodeResourceAsync(value);
    const resourceInput = resource.slice();
    const resourceDecoding = MSGPACK.decodeResourceAsync(resourceInput);
    const decoding = MSGPACK.decodeAsync(standard);
    value[0].label = 'changed after capture';
    standard.fill(0xc1);
    resourceInput.fill(0xc1);
    gc();
    assertEquals(expected, MSGPACK.decodeResource(resource));
    assertEquals(resource, await resourceEncoding);
    assertEquals(expected, await resourceDecoding);
    const encoded = await encoding;
    assertEquals(MSGPACK.encode(expected), encoded);
    assertEquals(expected, await decoding);
    assertEquals(expected, await MSGPACK.decodeAsync(encoded));
    // The resource decoder also accepts ordinary MessagePack.
    assertEquals(expected, MSGPACK.decodeResource(encoded));
    assertEquals(expected, await MSGPACK.decodeResourceAsync(encoded));
    assertEquals(expected, MSGPACK.decode(encoded));
    if (String.fromCharCode(...resource.subarray(0, 4)) === 'V8MR') {
      let rejected = false;
      try { await MSGPACK.decodeAsync(resource); }
      catch (error) { assertTrue(error instanceof SyntaxError); rejected = true; }
      assertTrue(rejected);
    }
  }
  print('PASS: async/resource integration, shared caches, mixed UTF-16, snapshots');
}
testAsyncResourceIntegration().catch(error => {
  print(error.stack || error);
  quit(1);
});
