#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Fetch immutable public seeds and JSONTestSuite with their license notices."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time
import urllib.parse
import urllib.request


def read_url(url):
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'V8-local-JSON-benchmark'})
            return urllib.request.urlopen(request, timeout=60).read()
        except Exception:
            if attempt == 2:
                raise
            time.sleep(attempt + 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--pins', type=Path,
                        default=Path(__file__).with_name('source-pins.json'))
    args = parser.parse_args()
    pins = json.loads(args.pins.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    assert not (args.out / 'source-manifest.json').exists(), 'Use a fresh output folder'
    entries = []
    for repo in pins:
        for name in ['README.md', 'LICENSE']:
            entries.append((repo, name, None))
    for name in ['canada', 'citm_catalog', 'twitter']:
        entries.append(('miloyip/nativejson-benchmark', f'data/{name}.json', None))
    repo = 'nst/JSONTestSuite'
    tree = json.loads(read_url(f'https://api.github.com/repos/{repo}/git/trees/{pins[repo]}?recursive=1'))
    assert not tree.get('truncated')
    entries += [(repo, item['path'], item['sha']) for item in tree['tree']
                if item['type'] == 'blob' and item['path'].startswith('test_parsing/')
                and item['path'].endswith('.json')]

    def fetch(entry):
        repo, relative, blob = entry
        url = f'https://raw.githubusercontent.com/{repo}/{pins[repo]}/' + urllib.parse.quote(relative, safe='/')
        raw = read_url(url)
        if blob:
            actual = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
            assert actual == blob, relative
        destination = args.out / repo / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
        return dict(repository=repo, commit=pins[repo], path=relative,
                    url=url, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())

    with ThreadPoolExecutor(max_workers=4) as pool:
        manifest = list(pool.map(fetch, entries))
    (args.out / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    cases, exclusions = [], []
    for repo, relative, _ in entries:
        if not relative.startswith('test_parsing/'):
            continue
        name = Path(relative).name
        raw = (args.out / repo / relative).read_bytes()
        try:
            text = raw.decode('utf-8', errors='strict')
        except UnicodeDecodeError:
            exclusions.append(dict(name=name, reason='Invalid UTF-8: byte decoding is outside the UTF-16 JSON.parse API'))
            continue
        cases.append(dict(name=name, category=name[0], text=text))
    (args.out / 'conformance-cases.json').write_text(json.dumps(cases, ensure_ascii=True) + '\n')
    (args.out / 'conformance-exclusions.json').write_text(json.dumps(exclusions, indent=2) + '\n')
    print(len(manifest), 'downloaded files;', len(cases), 'UTF-16 API cases;', len(exclusions), 'encoding exclusions')


if __name__ == '__main__':
    main()
