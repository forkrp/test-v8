#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Generate deterministic large UTF-8 JSON fixtures from pinned public seeds."""
import argparse
import hashlib
import json
from pathlib import Path


def compact(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'),
                      allow_nan=False)


def record(index, width):
    values = {}
    for field in range(width):
        kind = field % 6
        if kind == 0:
            value = index * 1009 + field
        elif kind == 1:
            value = -0.0 if index % 31 == 0 else (index % 99991 - 1234) / 8
        elif kind == 2:
            value = 'value' if index % 3 else f'entry-{index}-field-{field}'
        elif kind == 3:
            value = bool((index + field) & 1)
        elif kind == 4:
            value = None
        else:
            value = 100000000000 + index * 17 + field
        values[f'k{field}'] = value
    return values


def mixed(index):
    value = {
        'id': index,
        'name': f'記録-{index}-😀',
        'active': bool(index & 1),
        'payload': {'position': [index / 7, -index / 13, 1.2345678901234567],
                    'tags': ['alpha', f'tag-{index % 97}', '漢字'],
                    'metrics': {'min': -0.0, 'max': 1e200,
                                'small': 1e-200, 'count': index}},
        'text': ('quote=" slash=\\ newline=\n tab=\t nul=\x00 漢字 😀 ' * 3)
                + str(index),
        'shape': ({'a': index, 'b': index / 8} if index & 1 else
                  {'a': index, 'b': index / 8, 'c': 'extra'}),
        'variant': [index, 'text', None, {'nested': True}][index % 4],
        '0': index,
        '9': -index,
        '__proto__': {'own': True},
        'line\nkey': 'escaped property',
    }
    for level in range(index % 7):
        value = {'level': level, 'child': value, 'tail': [True, None, level / 8]}
    return value


def write_fixture(target, units, copies=None, target_bytes=None):
    digest = hashlib.sha256()
    byte_count = code_units = count = 0
    with target.open('wb') as stream:
        def emit(text):
            nonlocal byte_count, code_units
            raw = text.encode('utf-8')
            stream.write(raw)
            digest.update(raw)
            byte_count += len(raw)
            code_units += len(text.encode('utf-16-le')) // 2
        emit('[')
        for unit in units:
            if copies is not None and count >= copies:
                break
            if target_bytes is not None and byte_count >= target_bytes - 1:
                break
            if count:
                emit(',')
            emit(unit)
            count += 1
        emit(']')
    return dict(bytes=byte_count, code_units=code_units,
                sha256=digest.hexdigest(), top_level_items=count)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--pins', type=Path, required=True)
    parser.add_argument('--sizes', type=int, nargs='+', default=[10, 20, 50])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    assert not (args.out / 'manifest.json').exists(), 'Use a fresh output folder'
    pins = json.loads(args.pins.read_text())
    fixtures = []
    for family in ['canada', 'citm_catalog', 'twitter',
                   'records12', 'records60', 'mixed_nested']:
        if family in ['canada', 'citm_catalog', 'twitter']:
            seed_path = args.sources / 'miloyip/nativejson-benchmark/data' / (family + '.json')
            seed = compact(json.loads(seed_path.read_bytes()))
            seed_bytes = len(seed.encode('utf-8'))
        for size in args.sizes:
            name = f'{family}-{size}mb'
            target = args.out / (name + '.json')
            assert not target.exists(), target
            desired = size * 1_000_000
            if family in ['canada', 'citm_catalog', 'twitter']:
                copies = max(1, round((desired - 1) / (seed_bytes + 1)))
                result = write_fixture(target, (seed for _ in range(copies)), copies=copies)
                construction = dict(kind='repeated compact complete seed documents',
                                    seed_repository='miloyip/nativejson-benchmark',
                                    seed_commit=pins['miloyip/nativejson-benchmark'],
                                    seed_file=family + '.json',
                                    seed_sha256=hashlib.sha256(seed_path.read_bytes()).hexdigest(),
                                    copies=copies)
            else:
                def units():
                    index = 0
                    while True:
                        value = mixed(index) if family == 'mixed_nested' else record(index, int(family[7:]))
                        yield compact(value)
                        index += 1
                result = write_fixture(target, units(), target_bytes=desired)
                construction = dict(kind='deterministic generated records', family=family)
            assert abs(result['bytes'] / desired - 1) < 0.05, (name, result['bytes'])
            entry = dict(name=name, family=family, requested_mb=size,
                         path=target.name, construction=construction, **result)
            fixtures.append(entry)
            print(name, result['bytes'], 'bytes', result['top_level_items'], 'items', flush=True)
    manifest = dict(version=1, units='decimal MB (1 MB = 1,000,000 UTF-8 bytes)',
                    source_pins=pins, fixtures=fixtures)
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
