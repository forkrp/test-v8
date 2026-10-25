// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
'use strict';
const [suitePath, casesPath, api = 'parse'] = arguments;
load(suitePath + '/common.js');
const cases = JSON.parse(read(casesPath));
async function main() {
  if (typeof JSON[api] !== 'function') throw Error('API unavailable: ' + api);
  for (const entry of cases) {
    let result;
    try {
      const value = api === 'parseAsync' ? await JSON.parseAsync(entry.text) : JSON.parse(entry.text);
      result = {name: entry.name, category: entry.category, accepted: true,
                tree: largeJson.fingerprint(value)};
    } catch (error) {
      result = {name: entry.name, category: entry.category, accepted: false,
                error_name: error.name, error_message: error.message};
    }
    result.conforms = entry.category === 'i' ||
        result.accepted === (entry.category === 'y');
    print(JSON.stringify(result));
  }
}
main().catch(error => { print(error.stack || error); quit(1); });
