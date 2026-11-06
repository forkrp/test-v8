#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Compare JSON API errors with the original V8 12.6 d8 on the host or Android."""
import argparse
import gzip
import hashlib
import itertools
import json
from pathlib import Path
import shlex
import subprocess
import sys

from external import ROOT, execute, save, sha

SCRIPT = ROOT / 'test/json/error-parity.js'


def rejected_corpus(prepared, reference_results, output):
    """Recover the inputs rejected by the previously run external reference."""
    manifest_path = prepared / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    reference = json.loads(reference_results.read_text())
    corpus = prepared / 'corpus.js'
    if (not reference['all_passed'] or not reference['reference'] or
            reference['fixture_sha256'] != sha(manifest_path) or
            manifest['corpus_sha256'] != sha(corpus)):
        raise ValueError('Need a successful reference for the prepared corpus')
    parse = next(row for row in reference['corpus'] if row['api'] == 'parse')
    transcript = gzip.decompress(Path(parse['output']).read_bytes())
    if hashlib.sha256(transcript).hexdigest() != parse['stdout_sha256']:
        raise ValueError('Reference parse transcript hash mismatch')
    rejected = {
        line.split(b'\t')[1].decode('ascii')
        for line in transcript.splitlines()
        if line.startswith(b'CASE\t') and line.endswith(b'\tthrow:SyntaxError')
    }
    source = corpus.read_text().strip()
    if not source.startswith('globalThis.externalCases = ') or not source.endswith(';'):
        raise ValueError('Invalid prepared corpus')
    cases = json.loads(source.removeprefix('globalThis.externalCases = ').removesuffix(';'))
    selected = [{key: row[key] for key in ('id', 'text', 'large') if key in row}
                for row in cases if row['id'] in rejected]
    if not selected or len(selected) != len(rejected):
        raise ValueError('Invalid set of rejected case IDs')
    output.write_text('globalThis.rejectedCases = ' + json.dumps(selected) + ';\n')
    return len(selected)


def check_transcript(output, expected):
    rows = output.read_bytes().splitlines()
    if len(rows) != expected + 1 or rows[-1] != f'DONE\t{expected}\t0'.encode():
        raise ValueError(f'Missing cases or failed expectations in {output}')
    ids = set()
    errors = 0
    for row in rows[:-1]:
        fields = row.split(b'\t')
        if len(fields) != 5 or fields[0] != b'CASE' or fields[2] != b'PASS' or fields[1] in ids:
            raise ValueError(f'Unexpected row in {output}: {row[:200]!r}')
        ids.add(fields[1])
        errors += not fields[3].startswith(b'value:')
    return errors


def compare(reference, actual):
    with reference.open('rb') as left, actual.open('rb') as right:
        for index, (a, b) in enumerate(itertools.zip_longest(left, right), 1):
            if a != b:
                return {'line': index, 'reference': repr(a), 'actual': repr(b)}
    return None


def run(args, files, plan, inputs, *, adb=None, remote=None):
    def path(name):
        return f'{remote}/{name}' if remote else str(files[name])

    def command(parts):
        if adb:
            return [*adb, 'shell', shlex.join(['env', f'LD_LIBRARY_PATH={remote}', *parts])]
        return parts

    def verify():
        for name, local in files.items():
            if sha(local) != inputs[name]:
                raise ValueError(f'Local input changed: {local}')
        if adb:
            hashes = subprocess.check_output(
                command(['sha256sum', *(path(name) for name in files)]),
                text=True, timeout=60)
            observed = {name: digest for digest, name in
                        (line.split(None, 1) for line in hashes.splitlines())}
            if observed != {path(name): digest for name, digest in inputs.items()}:
                raise ValueError('Android payload hash mismatch')

    report = dict(inputs=inputs, adb_serial=args.adb_serial, runs=[], all_passed=False)
    result_path = args.out / 'results.json'
    verify()
    try:
        for api, suite, expected in plan:
            base = args.out / f'reference-{api}-{suite}.stdout'
            runs = [('reference', api, []), ('candidate', api, args.flag),
                    ('candidate', api + 'Async', args.flag)]
            for target, method, flags in runs:
                output = args.out / f'{target}-{method}-{suite}.stdout'
                parts = [path(target), '--thread-pool-size=4', *flags]
                if suite == 'external':
                    parts.append(path('rejected-corpus.js'))
                parts.extend([path('error-parity.js'), '--', method, suite])
                result = execute(command(parts), output, output.with_suffix('.stderr'), 120)
                result.update(target=target, api=method, suite=suite, rows=expected)
                report['runs'].append(result)
                if result['returncode'] != 0:
                    raise RuntimeError(f'd8 failed: {output}')
                result['errors'] = check_transcript(output, expected)
                if target == 'candidate':
                    result['difference'] = compare(base, output)
                print(target, method, suite, expected, 'rows', flush=True)
        verify()
        report['all_passed'] = all(not row.get('difference') for row in report['runs'])
    finally:
        save(result_path, report)
    print(result_path, 'PASS' if report['all_passed'] else 'FAIL', flush=True)
    return 0 if report['all_passed'] else 1


def run_android(args, files, plan, inputs):
    sys.path.insert(0, str(ROOT / 'tools/binary_serialization'))
    from device_guard import DeviceGuard

    adb = ['adb', '-s', args.adb_serial]
    remote = subprocess.check_output(
        [*adb, 'shell', 'mktemp', '-d', '/data/local/tmp/v8-json-errors.XXXXXX'],
        text=True, timeout=60).strip()
    guard = DeviceGuard(args.adb_serial, remote, args.out / 'overlaps.json')
    with guard.device_lock:
        try:
            guard.wait_idle()
            for name, local in files.items():
                subprocess.run([*adb, 'push', str(local), f'{remote}/{name}'],
                               check=True, stdout=subprocess.DEVNULL, timeout=60)
            subprocess.run([*adb, 'shell', 'chmod', '755', f'{remote}/reference',
                            f'{remote}/candidate'], check=True, timeout=60)
            return run(args, files, plan, inputs, adb=adb, remote=remote)
        finally:
            subprocess.run([*adb, 'shell', 'rm', '-r', remote], check=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--flag', action='append', default=[])
    parser.add_argument('--prepared', type=Path)
    parser.add_argument('--external-reference-results', type=Path)
    parser.add_argument('--adb-serial')
    parser.add_argument('--libcxx', type=Path)
    args = parser.parse_args()
    if bool(args.prepared) != bool(args.external_reference_results):
        parser.error('--prepared and --external-reference-results must be used together')
    if args.libcxx and not args.adb_serial:
        parser.error('--libcxx requires --adb-serial')

    args.out.mkdir(parents=True, exist_ok=False)
    files = {'reference': args.reference.resolve(), 'candidate': args.candidate.resolve(),
             'error-parity.js': SCRIPT}
    if args.libcxx:
        files['libc++_shared.so'] = args.libcxx.resolve()
    plan = [('parse', 'edges', 84), ('parse', 'large', 44), ('stringify', 'edges', 111)]
    if args.prepared:
        corpus = (args.out / 'rejected-corpus.js').resolve()
        count = rejected_corpus(args.prepared, args.external_reference_results, corpus)
        files['rejected-corpus.js'] = corpus
        plan.append(('parse', 'external', count))
    inputs = {name: sha(path) for name, path in files.items()}
    if args.adb_serial:
        return run_android(args, files, plan, inputs)
    return run(args, files, plan, inputs)


if __name__ == '__main__':
    sys.exit(main())
