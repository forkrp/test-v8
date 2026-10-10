#!/usr/bin/env python3
"""Audit and summarize a complete paired resource matrix, retaining losses."""
import argparse
from collections import defaultdict
import csv
import itertools
import json
import math
from pathlib import Path
import statistics

from run_resource_async import sha, validate_row


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('results', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    report = json.loads(a.results.read_text())
    if not report.get('finishedUtc'):
        raise RuntimeError('Matrix is incomplete')
    config = report['config']
    expected = set(itertools.product(['before', 'after'], ['arm64', 'arm32'],
        config['workloads'], ['encode', 'decode'], config['codecs'], config['jobs'],
        range(config['passes'])))
    actual = []
    groups = defaultdict(list)
    sizes = {}
    for row, case in zip(report['rows'], report['order']):
        key = (row['version'], row['abi'], row['workload'], row['operation'],
               row['codec'], row['jobs'], row['passIndex'])
        if tuple(case) != key:
            raise RuntimeError('Row/order mismatch')
        validate_row(row, list(key[1:]), report)
        actual.append(key)
        groups[key[:-1]].append(dict(passIndex=key[-1],
            totalMs=statistics.median(row['elapsed']), submitMs=statistics.median(row['submit'])))
        size = tuple(row[x] for x in ['msgpackBytes', 'resourceBytes', 'resourceCompressed',
                                     'jsonBytes', 'jsonCodeUnits'])
        if sizes.setdefault(row['workload'], size) != size:
            raise RuntimeError('Wire sizes differ across versions/ABIs')
    if (len(actual) != len(report['rows']) or len(actual) != len(report['order']) or
            len(actual) != len(expected) or len(set(actual)) != len(actual) or set(actual) != expected):
        raise RuntimeError('Incomplete or duplicated Cartesian matrix')
    for path, value in report['dependencies'].items():
        if sha(path) != value:
            raise RuntimeError('Measurement dependency changed: ' + path)
    for item in report['inputs'].values():
        if sha(item['path']) != item['sha256']:
            raise RuntimeError('Input changed')
    for abis in report['identities'].values():
        for item in abis.values():
            for field, digest in [('binary', 'binarySha256'), ('runtime', 'runtimeSha256'),
                                  ('manifest', 'manifestSha256')]:
                if sha(item[field]) != item[digest]:
                    raise RuntimeError('Frozen identity changed')
            manifest = json.loads(Path(item['manifest']).read_text())
            directory = Path(item['manifest']).parent
            if sha(directory/'compiler-identity.json') != item['compilerIdentitySha256']:
                raise RuntimeError('Compiler identity changed')
            if sha(directory/'source-patch.diff') != manifest['sourcePatch']['sha256']:
                raise RuntimeError('Saved source patch changed')
            for path, value in manifest['compileInputs'].items():
                # The baseline checkout and candidate worktree remain frozen
                # until this audit; verify their complete recorded closures.
                if sha(path) != value:
                    raise RuntimeError('Compile input changed: ' + path)
    medians = {key: dict(totalMs=statistics.median(v['totalMs'] for v in rows),
                         submitMs=statistics.median(v['submitMs'] for v in rows),
                         processes=sorted(rows, key=lambda v:v['passIndex']))
               for key, rows in groups.items()}
    comparisons = []
    for abi, name, op, codec, jobs in itertools.product(['arm64', 'arm32'],
            config['workloads'], ['encode', 'decode'], config['codecs'], config['jobs']):
        before = medians['before', abi, name, op, codec, jobs]
        after = medians['after', abi, name, op, codec, jobs]
        comparisons.append(dict(abi=abi, workload=name, operation=op, codec=codec, jobs=jobs,
            beforeMs=before['totalMs'], afterMs=after['totalMs'],
            gain=before['totalMs']/after['totalMs'], beforeSubmitMs=before['submitMs'],
            afterSubmitMs=after['submitMs'], processesBefore=before['processes'],
            processesAfter=after['processes']))
    def gm(values):
        return math.exp(statistics.mean(math.log(x) for x in values))
    aggregates = []
    for abi, op, jobs in itertools.product(['arm64', 'arm32'], ['encode', 'decode'], config['jobs']):
        names = [n for n in config['workloads'] if n != 'tiny']
        def time(version, name, codec):
            return medians[version, abi, name, op, codec, jobs]['totalMs']
        aggregates.append(dict(abi=abi, operation=op, jobs=jobs,
            asyncBeforeOverAfter=gm(time('before', n, 'msgpack_resource_async') /
                                   time('after', n, 'msgpack_resource_async') for n in names),
            afterSyncOverAsync=gm(time('after', n, 'msgpack_resource') /
                                  time('after', n, 'msgpack_resource_async') for n in names),
            beforeSyncOverAsync=gm(time('before', n, 'msgpack_resource') /
                                   time('before', n, 'msgpack_resource_async') for n in names)))
    a.output.mkdir(parents=True, exist_ok=False)
    summary = dict(resultsSha256=sha(a.results), config=config, freshProcesses=len(actual),
        timedSamples=len(actual)*report['samplesPerProcess'],
        completedOperations=sum(row['completedOperations'] for row in report['rows']),
        aggregates=aggregates, comparisons=comparisons,
        regressions=[c for c in comparisons if c['gain'] < 1], wireSizes=sizes)
    (a.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    fields = ['abi','workload','operation','codec','jobs','beforeMs','afterMs','gain',
              'beforeSubmitMs','afterSubmitMs']
    with (a.output/'comparison.csv').open('w') as stream:
        writer=csv.DictWriter(stream,fieldnames=fields,extrasaction='ignore')
        writer.writeheader();writer.writerows(comparisons)
    print(json.dumps(dict(freshProcesses=len(actual), aggregates=aggregates),indent=2))


if __name__ == '__main__':
    main()
