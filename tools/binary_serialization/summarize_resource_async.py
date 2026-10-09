#!/usr/bin/env python3
"""Audit paired resource async matrices; report medians, ranges and slow cells."""
import argparse
from collections import defaultdict
import hashlib
import itertools
import json
import math
from pathlib import Path
import statistics

from run_resource_async import CODECS, validate_row


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def verify_abi(path, abi):
    with Path(path).open('rb') as stream:
        header = stream.read(20)
    expected_class, expected_machine = {'arm64': (2, 183), 'arm32': (1, 40)}[abi]
    if (len(header) != 20 or header[:4] != b'\x7fELF' or
            header[4] != expected_class or header[5] != 1 or
            int.from_bytes(header[18:20], 'little') != expected_machine):
        raise ValueError('Executable/runtime does not match Android ABI: ' + str(path))


def summarize(path):
    report = json.loads(path.read_text())
    if not report.get('finishedUtc') or len(report['rows']) != len(report['order']):
        raise ValueError('Incomplete matrix: ' + str(path))
    actual = [(row['abi'], row['workload'], row['operation'], row['codec'], row['jobs'], row['passIndex'])
              for row in report['rows']]
    if actual != [tuple(case) for case in report['order']] or len(set(actual)) != len(actual):
        raise ValueError('Missing, reordered or duplicated process')
    config = report['config']
    expected = set(itertools.product(report['identities'], config['workloads'],
                                     ['encode', 'decode'], config['codecs'],
                                     config['jobs'], range(config['passes'])))
    if set(actual) != expected:
        raise ValueError('Matrix does not cover the configured Cartesian product')
    for segment in report.get('continuations', []):
        checkpoint = Path(segment['checkpoint'])
        if sha(checkpoint) != segment['checkpointSha256']:
            raise ValueError('Resume checkpoint changed')
        earlier = json.loads(checkpoint.read_text())
        count = segment['startIndex']
        if count != len(earlier['rows']) or report['rows'][:count] != earlier['rows']:
            raise ValueError('Earlier samples replaced during resume')
    for key, file in [('scriptSha256', 'benchmark.js'), ('runnerSha256', 'runner.py'),
                      ('deviceGuardSha256', 'device_guard.py')]:
        if sha(path.parent / file) != report[key]:
            raise ValueError('Measurement script snapshot changed: ' + file)
    for name, item in report['inputs'].items():
        source = Path(item['path'])
        if sha(source) != item['sha256'] or len(json.loads(source.read_text())) != item['rootUnits']:
            raise ValueError('Input changed: ' + name)
    compile_input_counts = {}
    for abi, item in report['identities'].items():
        verify_abi(item['binary'], abi)
        verify_abi(item['runtime'], abi)
        if sha(item['binary']) != item['binarySha256'] or sha(item['runtime']) != item['runtimeSha256']:
            raise ValueError('Executable or runtime changed')
        if sha(item['manifest']) != item['manifestSha256']:
            raise ValueError('Frozen source manifest changed')
        manifest_path = Path(item['manifest'])
        manifest = json.loads(manifest_path.read_text())
        if manifest['gitHead'] != item['engineCommit']:
            raise ValueError('Engine source commit mismatch')
        if manifest['binaries']['msgpack_js_benchmark'] != item['binarySha256']:
            raise ValueError('Benchmark does not match the frozen build manifest')
        for file, key in [('compiler-identity.json', 'compilerIdentitySha256'),
                          ('commands.txt', 'commandsSha256')]:
            if sha(manifest_path.parent / file) != manifest[key]:
                raise ValueError('Build provenance changed: ' + file)
        if manifest['compilerIdentitySha256'] != item['compilerIdentitySha256']:
            raise ValueError('Compiler identity mismatch')
        for source, expected_sha in manifest['compileInputs'].items():
            if sha(source) != expected_sha:
                raise ValueError('Compiled source or generated input changed: ' + source)
        compile_input_counts[abi] = len(manifest['compileInputs'])
    groups = defaultdict(lambda: defaultdict(list))
    wire = {}
    for row, case in zip(report['rows'], report['order']):
        validate_row(row, case, report)
        sizes = {key: row[key] for key in ['msgpackBytes', 'resourceBytes', 'jsonBytes', 'resourceCompressed']}
        if wire.setdefault(row['workload'], sizes) != sizes:
            raise ValueError('Wire sizes changed across processes/ABIs')
        groups[(row['abi'], row['workload'], row['operation'], row['jobs'])][row['codec']].append(row)
    results = []
    for (abi, workload, operation, jobs), codecs in sorted(groups.items()):
        item = {'abi': abi, 'workload': workload, 'operation': operation, 'jobs': jobs,
                'wire': wire[workload], 'codecs': {}}
        if set(codecs) != set(report['config']['codecs']):
            raise ValueError('Missing codec comparison')
        for codec, rows in codecs.items():
            if len(rows) != report['config']['passes'] or {r['passIndex'] for r in rows} != set(range(report['config']['passes'])):
                raise ValueError('Incorrect independent process count')
            stats = {'processes': len(rows), 'passMedians': []}
            for pass_index, row in sorted((r['passIndex'], r) for r in rows):
                stats['passMedians'].append({'pass': pass_index, 'totalMs': statistics.median(row['elapsed']),
                                             'submissionMs': statistics.median(row['submit'])})
            for field, label in [('elapsed', 'totalMs'), ('submit', 'submissionMs')]:
                medians = [statistics.median(r[field]) for r in rows]
                stats[label] = statistics.median(medians)
                stats[label + 'PassRange'] = [min(medians), max(medians)]
            stats['amortizedMsPerOperation'] = stats['totalMs'] / jobs
            stats['operationsPerSecond'] = jobs * 1000 / stats['totalMs']
            item['codecs'][codec] = stats
        c = item['codecs']
        if set(CODECS) <= set(c):
            resource = c['msgpack_resource_async']
            sync = c['msgpack_resource']
            item['resourceAsyncSpeedupOverResourceSync'] = sync['totalMs'] / resource['totalMs']
            item['resourceAsyncSubmissionReductionPct'] = 100 * (1 - resource['submissionMs'] / sync['submissionMs'])
            item['resourceAsyncSpeedupOverStandardAsync'] = c['msgpack_async']['totalMs'] / resource['totalMs']
            item['resourceAsyncSpeedupOverJsonBytesAsync'] = c['json_async_bytes']['totalMs'] / resource['totalMs']
            item['resourceRawReductionPct'] = 100 * (1 - item['wire']['resourceBytes'] / item['wire']['msgpackBytes'])
        results.append(item)
    aggregates = []
    regressions = []
    for abi in ['arm64', 'arm32']:
        for operation in ['encode', 'decode']:
            for jobs in report['config']['jobs']:
                rows = [r for r in results if r['abi'] == abi and r['operation'] == operation and r['jobs'] == jobs and r['workload'] != 'tiny']
                if not rows or 'resourceAsyncSpeedupOverResourceSync' not in rows[0]:
                    continue
                entry = {'abi': abi, 'operation': operation, 'jobs': jobs, 'workloads': len(rows)}
                for label in ['resourceAsyncSpeedupOverResourceSync', 'resourceAsyncSpeedupOverStandardAsync',
                              'resourceAsyncSpeedupOverJsonBytesAsync']:
                    entry[label] = math.exp(statistics.mean(math.log(r[label]) for r in rows))
                    entry[label + 'SlowerWorkloads'] = [r['workload'] for r in rows if r[label] < 1]
                entry['medianSubmissionReductionPct'] = statistics.median(r['resourceAsyncSubmissionReductionPct'] for r in rows)
                entry['increasedSubmissionWorkloads'] = [r['workload'] for r in rows if r['resourceAsyncSubmissionReductionPct'] < 0]
                aggregates.append(entry)
    for item in results:
        for comparison in ['resourceAsyncSpeedupOverResourceSync', 'resourceAsyncSpeedupOverStandardAsync',
                           'resourceAsyncSpeedupOverJsonBytesAsync']:
            if item.get(comparison, 1) < 1:
                baseline = {'resourceAsyncSpeedupOverResourceSync': 'msgpack_resource',
                            'resourceAsyncSpeedupOverStandardAsync': 'msgpack_async',
                            'resourceAsyncSpeedupOverJsonBytesAsync': 'json_async_bytes'}[comparison]
                paired = [a['totalMs'] / b['totalMs'] for a, b in zip(
                    item['codecs'][baseline]['passMedians'],
                    item['codecs']['msgpack_resource_async']['passMedians'])]
                regressions.append({key: item[key] for key in ['abi', 'workload', 'operation', 'jobs']} |
                                   {'comparison': comparison, 'speedup': item[comparison],
                                    'slowdownPct': 100 * (1 / item[comparison] - 1),
                                    'pairedPassSpeedups': paired,
                                    'passesSlowerByAtLeast5Pct': sum(v <= 1 / 1.05 for v in paired),
                                    'note': 'Descriptive repeated-pass ratios; no significance test.'})
    return {'source': str(path.resolve()), 'sourceSha256': sha(path), 'config': report['config'],
            'identities': report['identities'], 'independentProcesses': len(report['rows']),
            'samplesPerProcess': report['samplesPerProcess'],
            'audit': {'status': 'passed', 'compileInputsVerifiedPerAbi': compile_input_counts,
                      'completeCartesianCoverage': True, 'completedResultsAndWireParity': True,
                      'measurementSnapshotsAndInputHashes': True,
                      'frozenBinariesRuntimeCompilerAndCommands': True,
                      'retainedContinuationPrefixes': True},
            'methodology': 'Median of independent fresh-process medians. Pass ranges show variation, '
            'not confidence intervals. Bulk geometric means weight each non-tiny workload equally. '
            'Submission covers the calling-thread API invocations, not all foreground materialization. '
            'Total is awaited batch wall time; per-operation division is throughput amortization for jobs > 1.',
            'aggregates': aggregates, 'results': results, 'wireSizes': wire, 'regressions': regressions,
            'continuations': report.get('continuations', [])}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--matrix', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    result = summarize(args.matrix)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'processes': result['independentProcesses'], 'regressions': len(result['regressions']),
                      'output': str(args.output)}))


if __name__ == '__main__':
    main()
