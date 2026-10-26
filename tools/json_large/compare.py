#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Compare named frozen builds on an unchanged large-JSON corpus."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import random
import statistics

from run import execute, save, sha


def compare(args):
    out = args.out.resolve()
    state = json.loads((out / 'state.json').read_text())
    root = Path(os.environ.get('V8_JSON_ROOT', state['root']))
    suite = out / 'suite'
    fixtures = [f for f in json.loads((out / 'fixtures/manifest.json').read_text())['fixtures']
                if (not args.families or f['family'] in args.families)
                and f['requested_mb'] in args.sizes]
    assert fixtures and len(set(args.labels)) == len(args.labels)
    manifests = {n: json.loads((out/'manifests'/f'{n}.json').read_text()) for n in args.labels}
    binaries = {n: out/'binaries'/f'{n}-d8' for n in args.labels}
    flags = ['--expose-gc', '--thread-pool-size=4', '--max-old-space-size=4096']
    hashes = {n: sha(p) for n, p in binaries.items()}
    assert all(hashes[n] == manifests[n]['binary_sha256'] for n in args.labels)
    metadata = dict(tag=args.tag, labels=args.labels, pairs=args.pairs, samples=args.samples,
                    warmups=2, seed=20261026, flags=flags,
                    started_at=datetime.now(timezone.utc).isoformat(),
                    binary_hashes=hashes, manifests=manifests,
                    suite_hashes={p.name: sha(p) for p in suite.glob('*.js')},
                    expected_sha256=sha(out/'expected.json'),
                    fixtures_manifest_sha256=sha(out/'fixtures/manifest.json'),
                    process_metrics_scope='whole process including IO, validation and GC',
                    orchestrator_sha256=sha(Path(__file__)), run_source_sha256=sha(Path(__file__).with_name('run.py')),
                    order='shuffled cells; rotate labels; reverse each complete rotation cycle')
    meta_path = out/'results'/f'{args.tag}-metadata.json'
    assert not meta_path.exists(), 'Use a fresh tag'
    save(meta_path, metadata)
    cells = [(f, mode, jobs) for f in fixtures for mode in args.modes for jobs in args.jobs]
    total = len(cells)*len(args.labels)*args.pairs
    rng = random.Random(20261026)
    rows = []
    with (out/'results'/f'{args.tag}-raw.jsonl').open('x') as raw:
        for pair in range(args.pairs):
            order = cells[:]
            rng.shuffle(order)
            offset = pair % len(args.labels)
            labels = args.labels[offset:] + args.labels[:offset]
            if (pair // len(args.labels)) % 2:
                labels = labels[::-1]
            for fixture, mode, jobs in order:
                for label in labels:
                    environment = os.environ.copy()
                    environment.update(manifests[label].get('environment', {}))
                    command = [str(binaries[label]), *flags, str(suite/'benchmark.js'), '--',
                               str(suite), str(out/'fixtures'/fixture['path']), mode,
                               str(jobs), str(args.samples), str(out/'expected.json')]
                    result = execute(command, root, env=environment)
                    assert len(result['output']) == 1
                    metric = result['output'][0]
                    assert metric['validated_all_results'] and len(metric['measurements']) == args.samples
                    row = dict(pair=pair, label=label, fixture=fixture['name'],
                               family=fixture['family'], requested_mb=fixture['requested_mb'],
                               input_bytes=fixture['bytes'], mode=mode, jobs=jobs,
                               load=os.getloadavg(), command=command, **result)
                    raw.write(json.dumps(row)+'\n')
                    raw.flush()
                    rows.append(row)
                    if len(rows) % 6 == 0 or len(rows) == total:
                        print('PROGRESS', args.tag, len(rows), '/', total, flush=True)
    summary = []
    reference = args.labels[0]
    for fixture, mode, jobs in cells:
        selected = [r for r in rows if r['fixture']==fixture['name']
                    and r['mode']==mode and r['jobs']==jobs]
        values = {}
        for label in args.labels:
            group = [r for r in selected if r['label']==label]
            medians = [r['output'][0]['wall_ms'] for r in group]
            values[label] = dict(wall_ms=statistics.median(medians),
                                per_process_wall_ms=medians,
                                entry_ms=statistics.median(r['output'][0]['entry_ms'] for r in group),
                                process_peak_rss_max_bytes=max(r['process_peak_rss_bytes'] for r in group))
        changes = {}
        for label in args.labels[1:]:
            paired = []
            for pair in range(args.pairs):
                times = {r['label']:r['output'][0]['wall_ms'] for r in selected if r['pair']==pair}
                paired.append(100*(1-times[label]/times[reference]))
            changes[label] = dict(less_time_percent=100*(1-values[label]['wall_ms']/values[reference]['wall_ms']),
                                  paired_less_time_percent=paired,
                                  paired_median_less_time_percent=statistics.median(paired))
        summary.append(dict(fixture=fixture['name'], family=fixture['family'],
                            requested_mb=fixture['requested_mb'], mode=mode, jobs=jobs,
                            values=values, reference=reference, changes=changes))
    save(out/'results'/f'{args.tag}-summary.json', summary)
    metadata['finished_at'] = datetime.now(timezone.utc).isoformat()
    metadata['processes'] = len(rows)
    metadata['measured_batches'] = len(rows)*args.samples
    assert hashes == {n: sha(p) for n,p in binaries.items()}
    assert metadata['suite_hashes'] == {p.name: sha(p) for p in suite.glob('*.js')}
    assert metadata['expected_sha256'] == sha(out/'expected.json')
    assert metadata['orchestrator_sha256'] == sha(Path(__file__))
    assert metadata['run_source_sha256'] == sha(Path(__file__).with_name('run.py'))
    save(meta_path, metadata)
    for cell in summary:
        print(cell['fixture'], cell['mode'], cell['jobs'],
              {n:round(v['wall_ms'],3) for n,v in cell['values'].items()},
              {n:round(v['less_time_percent'],2) for n,v in cell['changes'].items()},flush=True)
    print('COMPLETE', args.tag, len(rows), 'processes', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--labels', nargs='+', required=True)
    parser.add_argument('--families', nargs='+')
    parser.add_argument('--sizes', nargs='+', type=int, default=[10,20,50])
    parser.add_argument('--modes', nargs='+', choices=['parse','parseAsync','stringify'], default=['parse'])
    parser.add_argument('--jobs', nargs='+', type=int, default=[1])
    parser.add_argument('--pairs', type=int, default=4)
    parser.add_argument('--samples', type=int, default=9)
    compare(parser.parse_args())


if __name__ == '__main__':
    main()
