#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Snapshot public JSON responses and expand records without changing root shape.

Original response bytes are immutable. Generated files are synthetic derivatives,
not additional real observations. Replay generation from the retained snapshot;
fetching a live URL again creates a new corpus, never a comparable replacement.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import random
import urllib.request


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'),
                      allow_nan=False).encode('utf-8')


def strict_load(raw):
    # These corpus recipes require unique keys and finite JSON literals so the
    # Python transformation cannot silently discard input members/non-JSON data.
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('Duplicate source key: ' + key)
            result[key] = value
        return result

    def invalid(value):
        raise ValueError('Non-JSON numeric literal: ' + value)

    return json.loads(raw.decode('utf-8'), object_pairs_hook=pairs,
                      parse_constant=invalid)


def describe(raw):
    text = raw.decode('utf-8')
    root = strict_load(raw)
    counts = dict(objects=0, arrays=0, strings=0, numbers=0, booleans=0,
                  nulls=0, max_depth=0, max_object_members=0, max_array_items=0)
    shapes = set()
    pending = [(root, 1)]
    while pending:
        value, depth = pending.pop()
        counts['max_depth'] = max(counts['max_depth'], depth)
        if isinstance(value, dict):
            counts['objects'] += 1
            counts['max_object_members'] = max(counts['max_object_members'], len(value))
            # A digest avoids retaining every wide dictionary's key tuple.
            shapes.add(sha(encode(list(value))))
            pending.extend((v, depth + 1) for v in value.values())
        elif isinstance(value, list):
            counts['arrays'] += 1
            counts['max_array_items'] = max(counts['max_array_items'], len(value))
            pending.extend((v, depth + 1) for v in value)
        else:
            kind = ('nulls' if value is None else 'booleans' if isinstance(value, bool)
                    else 'strings' if isinstance(value, str) else 'numbers')
            counts[kind] += 1
    counts['distinct_ordered_object_shapes'] = len(shapes)
    return dict(bytes=len(raw), sha256=sha(raw),
                code_units=len(text.encode('utf-16-le')) // 2,
                latin1_text=all(ord(c) <= 255 for c in text),
                root='array' if isinstance(root, list) else 'object',
                counts=counts)


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def fresh(path):
    path.mkdir(parents=True, exist_ok=True)
    if any(path.iterdir()):
        raise ValueError('Use an empty output directory: ' + str(path))


def fetch(args):
    fresh(args.out)
    sources = json.loads(args.config.read_text())['sources']
    manifest = dict(version=1, fetched_utc=datetime.now(timezone.utc).isoformat(),
                    config_sha256=sha(args.config.read_bytes()), sources=[])
    for source in sources:
        request = urllib.request.Request(source['url'], headers={
            'User-Agent': 'V8-JSON-corpus/1.0',
            'Accept': 'application/json', 'Accept-Encoding': 'identity'})
        with urllib.request.urlopen(request, timeout=60) as response:
            raw = response.read(64 * 1024 * 1024 + 1)
            if len(raw) > 64 * 1024 * 1024:
                raise ValueError('Response exceeds corpus download limit')
            headers = {key: response.headers.get(key) for key in
                       ('Content-Type', 'Content-Encoding', 'ETag',
                        'Last-Modified', 'Date')}
            final_url = response.url
        info = describe(raw)
        name = source['name'] + '.json'
        (args.out / name).write_bytes(raw)
        manifest['sources'].append(dict(source, path=name, response_url=final_url,
                                        response_headers=headers, **info))
        save(args.out / 'manifest.json', manifest)
        print(source['name'], info['bytes'], info['root'], flush=True)
    manifest['complete'] = True
    save(args.out / 'manifest.json', manifest)


def expand(root, recipe, desired, seed):
    """Return UTF-8 JSON, retaining original records before adding replicas."""
    if recipe == 'geojson-features':
        records = root['features']
        outer = {key: value for key, value in root.items() if key != 'features'}
        outer['metadata'] = dict(outer['metadata'])
        field, dictionary = 'features', False
        existing_ids = {row['id'] for row in records}
    elif recipe == 'station-array':
        records = root
        outer, field, dictionary = {}, None, False
        existing_ids = {row['sno'] for row in records}
    elif recipe == 'npm-versions':
        records = list(root['versions'].items())
        outer = {key: value for key, value in root.items() if key != 'versions'}
        outer['time'] = dict(outer.get('time', {}))
        field, dictionary = 'versions', True
        existing_ids = set(root['versions'])
    else:
        raise ValueError(recipe)
    if not records:
        raise ValueError('No records to expand')
    members = ([encode(key) + b':' + encode(value) for key, value in records]
               if dictionary else [encode(value) for value in records])
    # Outer size changes slightly as metadata.count or npm time entries grow.
    size = sum(map(len, members)) + len(members) + len(encode(outer)) + 32
    rng = random.Random(seed)
    order = list(range(len(records)))
    added = 0
    changed = []
    while size < desired:
        rng.shuffle(order)
        for index in order:
            if size >= desired:
                break
            added += 1
            if recipe == 'geojson-features':
                row = dict(records[index])
                identity = row['id'] + '-jsonbench-' + str(added)
                row['id'] = identity
                member = encode(row)
                changed = ['features[*].id', 'metadata.count']
            elif recipe == 'station-array':
                row = dict(records[index])
                identity = str(900000000 + added)
                row['sno'] = identity
                member = encode(row)
                changed = ['[*].sno']
            else:
                old_version, original = records[index]
                identity = '0.0.0-jsonbench.' + str(added)
                row = dict(original, version=identity)
                if '_id' in row:
                    row['_id'] = row['name'] + '@' + identity
                member = encode(identity) + b':' + encode(row)
                if old_version in root.get('time', {}):
                    stamp = root['time'][old_version]
                    outer['time'][identity] = stamp
                    size += len(encode(identity)) + len(encode(stamp)) + 2
                changed = ['versions keys', 'versions[*].version',
                           'versions[*]._id when present', 'time keys']
            if identity in existing_ids:
                raise ValueError('Synthetic identity collision: ' + identity)
            existing_ids.add(identity)
            members.append(member)
            size += len(member) + 1
    if recipe == 'geojson-features':
        outer['metadata']['count'] = len(members)
    container = (b'{' if dictionary else b'[') + b','.join(members)
    container += b'}' if dictionary else b']'
    if field is None:
        result = container
    else:
        # Preserve root member order, including the original array/dict position.
        result = b'{' + b','.join(
            encode(key) + b':' + (container if key == field else encode(outer[key]))
            for key in root) + b'}'
    return result, dict(seed=seed, original_records=len(records),
                        added_records=added, total_records=len(members),
                        changed_fields=changed,
                        reused_values='Other record values repeat from the real snapshot; '
                                      'each additional cycle shuffles the full source pool')


def generate(args):
    fresh(args.out)
    source_manifest = json.loads((args.sources / 'manifest.json').read_text())
    if not source_manifest.get('complete'):
        raise ValueError('Incomplete source snapshot')
    manifest = dict(version=1, source_manifest_sha256=sha(
        (args.sources / 'manifest.json').read_bytes()),
        units='decimal MB of UTF-8 bytes',
        algorithm='root-preserving-record-expansion-v1',
        generator_sha256=sha(Path(__file__).read_bytes()), fixtures=[], skipped=[])
    for source in source_manifest['sources']:
        raw = (args.sources / source['path']).read_bytes()
        if sha(raw) != source['sha256']:
            raise ValueError('Source snapshot checksum mismatch: ' + source['name'])
        root = strict_load(raw)
        original_size = len(encode(root))
        for mb in args.sizes:
            name = source['name'] + '-' + str(mb) + 'mb'
            if source['recipe'] == 'original-only' or mb * 1_000_000 <= original_size:
                manifest['skipped'].append(dict(name=name, reason=(
                    'Keep schema references and path/definition proportions intact; use raw document'
                    if source['recipe'] == 'original-only' else
                    'Original compact document already exceeds target; do not downsample')))
                continue
            result, construction = expand(root, source['recipe'], mb * 1_000_000, args.seed)
            info = describe(result)  # Independent reparse catches malformed output.
            if abs(info['bytes'] / (mb * 1_000_000) - 1) > 0.02:
                raise ValueError('Unexpected size error: ' + name)
            if info['root'] != source['root']:
                raise ValueError('Root shape changed')
            path = args.out / (name + '.json')
            path.write_bytes(result)
            manifest['fixtures'].append(dict(name=name, path=path.name,
                source=source['name'], source_sha256=source['sha256'], requested_mb=mb,
                kind='synthetic expansion of real records', recipe=source['recipe'],
                construction=construction, **info))
            save(args.out / 'manifest.json', manifest)
            print(name, info['bytes'], construction['total_records'], flush=True)
    manifest['complete'] = True
    save(args.out / 'manifest.json', manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    fetch_parser = commands.add_parser('fetch')
    fetch_parser.add_argument('--config', type=Path,
                              default=Path(__file__).with_name('real-sources.json'))
    fetch_parser.add_argument('--out', type=Path, required=True)
    generate_parser = commands.add_parser('generate')
    generate_parser.add_argument('--sources', type=Path, required=True)
    generate_parser.add_argument('--out', type=Path, required=True)
    generate_parser.add_argument('--sizes', type=int, nargs='+', default=[10, 20, 50])
    generate_parser.add_argument('--seed', type=int, default=20261107)
    args = parser.parse_args()
    if args.command == 'fetch':
        fetch(args)
    else:
        if any(size <= 0 for size in args.sizes):
            parser.error('Sizes must be positive')
        generate(args)


if __name__ == '__main__':
    main()
