#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Validate a frozen candidate against retained complete reference outputs."""
import argparse
import json
import os
from pathlib import Path

from run import execute, save, sha


def validate(out, label):
    state = json.loads((out/'state.json').read_text())
    root = Path(os.environ.get('V8_JSON_ROOT', state['root']))
    suite = out/'suite'
    binary = out/'binaries'/f'{label}-d8'
    manifest = json.loads((out/'manifests'/f'{label}.json').read_text())
    assert sha(binary) == manifest['binary_sha256']
    fixtures = json.loads((out/'fixtures/manifest.json').read_text())['fixtures']
    canonical = json.loads((out/'reference-large.json').read_text())
    reference = json.loads((out/'reference-conformance.json').read_text())
    flags = ['--expose-gc', '--thread-pool-size=4', '--max-old-space-size=4096']
    with (out/'results'/f'{label}-large-validation-raw.jsonl').open('x') as raw:
        for fixture in fixtures:
            file_path = out/'fixtures'/fixture['path']
            assert sha(file_path) == fixture['sha256']
            for mode in ['oracle', 'parseAsync']:
                command = [str(binary), *flags, str(suite/'benchmark.js'), '--',
                           str(suite), str(file_path), mode, '1', '1', str(out/'expected.json')]
                result = execute(command, root, oracle=mode=='oracle')
                if mode=='oracle':
                    assert result['output'] == canonical[fixture['name']], fixture['name']
                else:
                    assert result['output'][0]['validated_all_results']
                raw.write(json.dumps(dict(label=label, fixture=fixture['name'], mode=mode,
                                          command=command, **result))+'\n')
                raw.flush()
            print('VALIDATE', label, fixture['name'], 'complete SHA256/tree + async tree passed', flush=True)
    conformance = []
    for api in ['parse','parseAsync']:
        command = [str(binary), *flags, str(suite/'conformance.js'), '--', str(suite),
                   str(out/'conformance-cases.json'), api]
        result = execute(command, root)
        assert len(result['output']) == len(reference)
        assert all(row['conforms'] for row in result['output'])
        decisions = [{k:r[k] for k in ['name','category','accepted']} |
                     ({'tree':r['tree']} if r['accepted'] else {}) for r in result['output']]
        assert decisions == reference, api
        save(out/'results'/f'{label}-conformance-{api}.json', result)
        conformance.append(dict(api=api, cases=len(decisions), failures=0))
    assert sha(binary) == manifest['binary_sha256']
    save(out/'results'/f'{label}-validation.json', dict(label=label,
         binary_sha256=manifest['binary_sha256'], fixtures=len(fixtures),
         fixture_manifest_sha256=sha(out/'fixtures/manifest.json'),
         identical_full_canonical_sha256=True, identical_full_tree_fingerprints=True,
         async_full_tree_fingerprints=True, conformance=conformance))
    print('VALIDATED', label, len(fixtures), 'large inputs and both APIs', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--label', required=True)
    args = parser.parse_args()
    validate(args.out.resolve(), args.label)


if __name__ == '__main__':
    main()
