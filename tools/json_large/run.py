#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Validate and measure frozen d8 builds on generated large JSON files."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import tempfile
import threading
import time


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


def execute(command, cwd, timeout=300, oracle=False, env=None):
    start = time.monotonic()
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        child = subprocess.Popen(command, cwd=cwd, stdout=stdout, stderr=stderr, env=env)
        timer = threading.Timer(timeout, child.kill)
        timer.daemon = True
        timer.start()
        try:
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
        finally:
            timer.cancel()
        stderr.seek(0)
        errors = stderr.read().decode('utf-8', errors='replace')
        stdout.seek(0)
        if child.returncode:
            prefix = stdout.read(4000).decode('utf-8', errors='replace')
            raise RuntimeError((command, child.returncode, prefix, errors))
        if oracle:
            header = json.loads(stdout.readline())
            digest = hashlib.sha256()
            count = 0
            for block in iter(lambda: stdout.read(1024 * 1024), b''):
                digest.update(block)
                count += len(block)
            assert count > 0
            output = dict(header=header, canonical_bytes=count,
                          canonical_sha256=digest.hexdigest())
        else:
            output = [json.loads(line) for line in stdout.read().decode().splitlines()
                      if line.startswith('{')]
    return dict(output=output, stderr=errors, process_cpu_s=usage.ru_utime + usage.ru_stime,
                process_wall_s=time.monotonic() - start,
                process_peak_rss_bytes=usage.ru_maxrss)


class Experiment:
    def __init__(self, out):
        self.out = out.resolve()
        self.state = json.loads((self.out / 'state.json').read_text())
        self.root = Path(os.environ.get('V8_JSON_ROOT', self.state['root']))
        self.manifest = json.loads((self.out / 'fixtures/manifest.json').read_text())
        self.fixtures = self.manifest['fixtures']
        self.suite = self.out / 'suite'
        self.flags = ['--expose-gc', '--thread-pool-size=4', '--max-old-space-size=4096']

    def command(self, label, fixture, mode, jobs=1, samples=9):
        return [str(self.out / 'binaries' / f'{label}-d8'), *self.flags,
                str(self.suite / 'benchmark.js'), '--', str(self.suite),
                str(self.out / 'fixtures' / fixture['path']), mode,
                str(jobs), str(samples), str(self.out / 'expected.json')]

    def validate(self):
        assert not (self.out / 'expected.json').exists(), 'Validation already exists'
        expected = {}
        with (self.out / 'results/large-validation-raw.jsonl').open('x') as raw:
            for fixture in self.fixtures:
                file_path = self.out / 'fixtures' / fixture['path']
                assert sha(file_path) == fixture['sha256']
                reference = None
                for label in ['baseline', 'current']:
                    command = self.command(label, fixture, 'oracle')
                    result = execute(command, self.root, oracle=True)
                    record = dict(label=label, fixture=fixture['name'], command=command, **result)
                    raw.write(json.dumps(record) + '\n')
                    raw.flush()
                    value = result['output']
                    assert value['header']['code_units'] == fixture['code_units']
                    if label == 'baseline':
                        reference = value
                        expected[fixture['name']] = value['header']
                    else:
                        assert value == reference, (fixture['name'], value, reference)
                    print('VALIDATE', label, fixture['name'], value['canonical_bytes'], flush=True)
        save(self.out / 'expected.json', expected)
        cases = json.loads((self.out / 'conformance-cases.json').read_text())
        reference = None
        conformance = []
        for label, api in [('baseline', 'parse'), ('current', 'parse'), ('current', 'parseAsync')]:
            command = [str(self.out / 'binaries' / f'{label}-d8'), *self.flags,
                       str(self.suite / 'conformance.js'), '--', str(self.suite),
                       str(self.out / 'conformance-cases.json'), api]
            result = execute(command, self.root)
            rows = result['output']
            assert len(rows) == len(cases), (len(rows), len(cases))
            failures = [r for r in rows if not r['conforms']]
            decisions = [{k: r[k] for k in ['name', 'category', 'accepted']} |
                         ({'tree': r['tree']} if r['accepted'] else {}) for r in rows]
            if reference is None:
                reference = decisions
            else:
                assert decisions == reference, (label, api, 'branch/API difference')
            save(self.out / 'results' / f'conformance-{label}-{api}.json', result)
            assert not failures, failures
            conformance.append(dict(label=label, api=api, cases=len(rows), failures=0))
            print('CONFORMANCE', label, api, len(rows), 'passed', flush=True)
        save(self.out / 'results/validation.json', dict(large_files=len(self.fixtures),
             identical_full_canonical_sha256=True, identical_full_tree_fingerprints=True,
             conformance=conformance))

    def matrix(self, tag, phase, pairs=4, samples=9):
        assert (self.out / 'results/validation.json').exists()
        if phase == 'branches':
            cells = [(f, mode, 1) for f in self.fixtures for mode in ['parse', 'stringify']]
        elif phase == 'async':
            cells = [(f, 'parseAsync', 1) for f in self.fixtures]
        elif phase == 'concurrent':
            cells = [(f, mode, 4) for f in self.fixtures
                     if f['family'] in ['records12', 'records60']
                     for mode in ['parse', 'parseAsync']]
        else:
            raise ValueError(phase)
        metadata = dict(tag=tag, phase=phase, pairs=pairs, samples=samples, warmups=2,
                        workers=4, flags=self.flags, seed=20261026,
                        started_at=datetime.now(timezone.utc).isoformat(),
                        load_before=os.getloadavg(),
                        binary_hashes={n: sha(self.out / 'binaries' / f'{n}-d8') for n in ['baseline', 'current']},
                        suite_hashes={p.name: sha(p) for p in self.suite.glob('*.js')},
                        fixtures_manifest_sha256=sha(self.out / 'fixtures/manifest.json'),
                        expected_sha256=sha(self.out / 'expected.json'),
                        process_metrics_scope='whole process including startup, read/decode, validation and explicit GC')
        assert not (self.out / 'results' / f'{tag}-metadata.json').exists()
        save(self.out / 'results' / f'{tag}-metadata.json', metadata)
        rng = random.Random(20261026)
        records = []
        total = pairs * len(cells) * (2 if phase == 'branches' else 1)
        with (self.out / 'results' / f'{tag}-raw.jsonl').open('x') as raw:
            for pair in range(pairs):
                order = cells[:]
                rng.shuffle(order)
                for fixture, mode, jobs in order:
                    labels = (['baseline', 'current'] if pair % 2 == 0 else ['current', 'baseline']) if phase == 'branches' else ['current']
                    for label in labels:
                        command = self.command(label, fixture, mode, jobs, samples)
                        result = execute(command, self.root)
                        assert len(result['output']) == 1
                        metric = result['output'][0]
                        assert metric['validated_all_results'] and len(metric['measurements']) == samples
                        row = dict(pair=pair, label=label, fixture=fixture['name'],
                                   family=fixture['family'], requested_mb=fixture['requested_mb'],
                                   input_bytes=fixture['bytes'], jobs=jobs, mode=mode,
                                   load=os.getloadavg(), command=command, **result)
                        raw.write(json.dumps(row) + '\n')
                        raw.flush()
                        records.append(row)
                        if len(records) % 6 == 0 or len(records) == total:
                            print('PROGRESS', tag, len(records), '/', total, flush=True)
        summary = []
        for fixture, mode, jobs in cells:
            selected = [r for r in records if r['fixture'] == fixture['name'] and r['mode'] == mode and r['jobs'] == jobs]
            values = {}
            for label in (['baseline', 'current'] if phase == 'branches' else ['current']):
                group = [r for r in selected if r['label'] == label]
                wall = statistics.median(r['output'][0]['wall_ms'] for r in group)
                values[label] = dict(wall_ms=wall,
                    entry_ms=statistics.median(r['output'][0]['entry_ms'] for r in group),
                    throughput_input_mb_s=fixture['bytes'] * jobs / wall / 1000,
                    process_peak_rss_max_bytes=max(r['process_peak_rss_bytes'] for r in group),
                    per_process_wall_ms=[r['output'][0]['wall_ms'] for r in group])
            item = dict(fixture=fixture['name'], family=fixture['family'],
                        requested_mb=fixture['requested_mb'], input_bytes=fixture['bytes'],
                        mode=mode, jobs=jobs, values=values)
            if phase == 'branches':
                changes = []
                for pair in range(pairs):
                    pair_rows = [r for r in selected if r['pair'] == pair]
                    times = {r['label']: r['output'][0]['wall_ms'] for r in pair_rows}
                    changes.append(100 * (1 - times['current'] / times['baseline']))
                item.update(less_time_percent=100 * (1 - values['current']['wall_ms'] / values['baseline']['wall_ms']),
                            speedup=values['baseline']['wall_ms'] / values['current']['wall_ms'],
                            paired_less_time_percent=changes,
                            paired_median_less_time_percent=statistics.median(changes))
            summary.append(item)
        save(self.out / 'results' / f'{tag}-summary.json', summary)
        metadata['finished_at'] = datetime.now(timezone.utc).isoformat()
        metadata['load_after'] = os.getloadavg()
        assert metadata['binary_hashes'] == {n: sha(self.out / 'binaries' / f'{n}-d8') for n in ['baseline', 'current']}
        assert metadata['suite_hashes'] == {p.name: sha(p) for p in self.suite.glob('*.js')}
        assert metadata['expected_sha256'] == sha(self.out / 'expected.json')
        save(self.out / 'results' / f'{tag}-metadata.json', metadata)
        print('COMPLETE', tag, total, 'processes', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['validate', 'matrix'])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tag')
    parser.add_argument('--phase', choices=['branches', 'async', 'concurrent'])
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--samples', type=int, default=9)
    args = parser.parse_args()
    experiment = Experiment(args.out)
    if args.action == 'validate':
        experiment.validate()
    else:
        assert args.tag and args.phase
        experiment.matrix(args.tag, args.phase, args.pairs, args.samples)


if __name__ == '__main__':
    main()
