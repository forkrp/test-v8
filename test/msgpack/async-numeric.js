// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// d8 --expose-gc async-numeric.js
'use strict';
function check(ok, message) { if (!ok) throw new Error(message); }
async function main() {
  // Exercise every numeric tag width, unsafe 64-bit integers, mixed numeric
  // storage, and fixed tape-block boundaries through the public API.
  const pattern = [0, 127, 128, 255, 256, 65535, 65536, 4294967295,
    Number.MAX_SAFE_INTEGER, -1, -32, -33, -128, -129, -32768, -32769,
    -2147483648, -2147483649, Number.MIN_SAFE_INTEGER, -0, 0.5, 0.1,
    Number.MIN_VALUE, Number.MAX_VALUE, Infinity, -Infinity, NaN,
    9223372036854775807n, -9223372036854775808n, 18446744073709551615n];
  for (const count of [1, 257, 4095, 4096, 4097, 8193]) {
    const value = Array.from({length: count}, (_, i) => pattern[i % pattern.length]);
    const bytes = MSGPACK.encode(value);
    const expected = MSGPACK.decode(bytes);
    const pending = MSGPACK.decodeAsync(bytes);
    gc();
    const actual = await pending;
    check(actual.length === count, 'numeric array length');
    for (let i = 0; i < count; ++i)
      check(Object.is(actual[i], expected[i]), `numeric value ${count}:${i}`);
    for (let cut = 1; cut <= Math.min(8, bytes.length - 1); ++cut) {
      try {
        await MSGPACK.decodeAsync(bytes.subarray(0, bytes.length - cut));
        throw new Error('accepted truncated numeric array');
      } catch (error) {
        check(error instanceof SyntaxError, 'truncated numeric array error');
      }
    }
  }
  // A valid array of tiny wire values needs much more native tape storage
  // than input storage. Reject before materialization and release the job.
  const count = 1 << 24;
  const bytes = new Uint8Array(count + 5);
  bytes.set([0xdd, 1, 0, 0, 0]);
  try {
    await MSGPACK.decodeAsync(bytes);
    throw new Error('accepted native tape budget overflow');
  } catch (error) {
    check(error instanceof RangeError, 'native tape budget error');
  }
  const recovered = await MSGPACK.decodeAsync(MSGPACK.encode([1, -0, 0.1]));
  check(recovered.length === 3 && recovered[0] === 1 &&
        Object.is(recovered[1], -0) && recovered[2] === 0.1,
        'decode recovery after native budget rejection');
  print('MSGPACK async numeric checks passed');
}
main().catch(error => { print(error.stack || error); quit(1); });
