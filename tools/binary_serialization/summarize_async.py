#!/usr/bin/env python3
"""Audit complete async matrices and expose per-process medians and regressions."""
import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def matrix(path):
    report = json.loads(path.read_text())
    if not report.get('finishedUtc') or len(report['rows']) != len(report['order']):
        raise ValueError(f'Incomplete matrix: {path}')
    actual = [(r['workload'], r['operation'], r['codec'], r['jobs'], r['passIndex'])
              for r in report['rows']]
    expected = [tuple(item) for item in report['order']]
    if actual != expected or len(set(actual)) != len(actual):
        raise ValueError(f'Missing, duplicate, or unexpected process: {path}')
    for continuation in report.get('continuations', []):
        checkpoint = continuation['checkpoint']
        if sha(checkpoint['path']) != checkpoint['sha256']:
            raise ValueError(f'Changed resume checkpoint: {checkpoint["path"]}')
        previous = json.loads(Path(checkpoint['path']).read_text())
        count = continuation['startIndex']
        if count != len(previous['rows']) or report['rows'][:count] != previous['rows']:
            raise ValueError(f'Replaced measurements during resume: {path}')
        if checkpoint['sha256'] != continuation['preservedPrefixSha256']:
            raise ValueError(f'Resume prefix identity mismatch: {path}')
        for identity in ['resumer', 'failureEvidence']:
            item = continuation[identity]
            if sha(item['path']) != item['sha256']:
                raise ValueError(f'Changed resume evidence: {item["path"]}')
    decoded_sizes = {}
    for name, item in report['inputs'].items():
        source = Path(item['path'])
        if sha(source) != item['sha256']:
            raise ValueError(f'Input identity mismatch: {source}')
        value = json.loads(source.read_text())
        decoded_sizes[name] = len(value)
    wire_sizes = {}
    groups = defaultdict(lambda: defaultdict(list))
    for row in report['rows']:
        if any(len(row[field]) != report['samplesPerPass'] for field in ['elapsed', 'submit']):
            raise ValueError(f'Wrong sample count: {row}')
        if any(not math.isfinite(v) or v < 0 for field in ['elapsed', 'submit'] for v in row[field]):
            raise ValueError(f'Invalid sample: {row}')
        inner = 1000 if row['workload'] == 'tiny' else 1
        if row['innerIterations'] != inner:
            raise ValueError(f'Wrong inner iteration count: {row}')
        sizes = row['msgpackBytes'], row['jsonCodeUnits']
        if any(type(n) is not int or n <= 0 for n in sizes):
            raise ValueError(f'Invalid wire size: {row}')
        previous = wire_sizes.setdefault(row['workload'], sizes)
        if sizes != previous:
            raise ValueError(f'Wire sizes changed across processes: {row}')
        batches = (10 + report['samplesPerPass']) * inner * row['jobs']
        expected_sink = None
        if row['operation'] == 'decode':
            expected_sink = decoded_sizes[row['workload']] * batches
        elif row['codec'].startswith('msgpack'):
            expected_sink = row['msgpackBytes'] * batches
        elif row['codec'] in ['json', 'json_async']:
            expected_sink = row['jsonCodeUnits'] * batches
        if expected_sink is not None and row['sink'] != expected_sink:
            raise ValueError(f'Wrong completed operation count or result size: {row}')
        if expected_sink is None:
            # UTF-8 output is at least one byte per UTF-16 code unit and at
            # most three. Byte-transport restoration was checked by the JS
            # harness; these bounds also catch missing timed operations.
            minimum = row['jsonCodeUnits'] * batches
            if not minimum <= row['sink'] <= 3 * minimum:
                raise ValueError(f'Invalid completed UTF-8 byte count: {row}')
        groups[(row['workload'], row['operation'], row['jobs'])][row['codec']].append(row)
    results = []
    for (workload, operation, jobs), codecs in sorted(groups.items()):
        item = {'workload': workload, 'operation': operation, 'jobs': jobs, 'codecs': {}}
        for codec, rows in codecs.items():
            if len(rows) != report['passes']:
                raise ValueError(f'Wrong independent process count: {workload} {codec}')
            values = {}
            for field, metric in [('elapsed', 'totalMs'), ('submit', 'submissionMs')]:
                medians = [statistics.median(row[field]) for row in rows]
                values[metric] = statistics.median(medians)
                values[metric + 'PassRange'] = [min(medians), max(medians)]
            values['processes'] = len(rows)
            values['totalMsPerOperation'] = values['totalMs'] / jobs
            item['codecs'][codec] = values
        msgpack = item['codecs']['msgpack_async']
        json_bytes = item['codecs']['json_async_bytes']
        sync = item['codecs']['msgpack']
        item['jsonBytesSpeedup'] = json_bytes['totalMs'] / msgpack['totalMs']
        item['jsonBytesLatencyChangePct'] = 100 * (msgpack['totalMs'] / json_bytes['totalMs'] - 1)
        item['submissionReductionPct'] = 100 * (1 - msgpack['submissionMs'] / sync['submissionMs'])
        item['syncLatencyChangePct'] = 100 * (msgpack['totalMs'] / sync['totalMs'] - 1)
        results.append(item)
    aggregates = []
    for operation in ['encode', 'decode']:
        for jobs in sorted({r['jobs'] for r in results}):
            rows = [r for r in results if r['operation'] == operation and r['jobs'] == jobs
                    and r['workload'] != 'tiny']
            if not rows:
                continue  # Tiny-only matrices have no non-tiny aggregate.
            aggregates.append({'operation': operation, 'jobs': jobs, 'workloads': len(rows),
                'jsonBytesGeometricMeanSpeedup': math.exp(statistics.mean(math.log(r['jsonBytesSpeedup']) for r in rows)),
                'medianSubmissionReductionPct': statistics.median(r['submissionReductionPct'] for r in rows),
                'slowerThanJsonBytes': [r['workload'] for r in sorted(rows, key=lambda r: r['jsonBytesSpeedup'])
                                       if r['jsonBytesSpeedup'] < 1],
                'increasedSubmission': [r['workload'] for r in rows if r['submissionReductionPct'] < 0]})
    return {'path': str(path.resolve()), 'sha256': sha(path), 'binarySha256': report['binarySha256'],
            'frozenBuildManifest': report.get('frozenBuildManifest'),
            'continuations': report.get('continuations', []),
            'independentProcesses': len(report['rows']), 'aggregates': aggregates, 'results': results}


def sync_control(path):
    report = json.loads(path.read_text())
    if not report.get('finishedUtc') or len(report['rows']) != len(report['order']):
        raise ValueError(f'Incomplete sync control: {path}')
    actual = [(r['workload'], r['operation'], r['variant'], r['passIndex']) for r in report['rows']]
    if actual != [tuple(r) for r in report['order']] or len(set(actual)) != len(actual):
        raise ValueError(f'Missing or unexpected sync process: {path}')
    decoded_sizes = {}
    for name, digest in report['inputs'].items():
        source = path.parent / (name + '.json')
        if sha(source) != digest:
            raise ValueError(f'Sync input identity mismatch: {source}')
        decoded_sizes[name] = len(json.loads(source.read_text()))
    calibration = defaultdict(dict)
    for row in report['calibrationRows']:
        key = row['workload'], row['operation']
        if row['iterations'] != 3 or row['warmups'] != 20 or row['variant'] in calibration[key]:
            raise ValueError(f'Invalid synchronous calibration: {row}')
        calibration[key][row['variant']] = row['observed']
    exact_sizes = {}
    for key, variants in calibration.items():
        if set(variants) != {'baseline', 'candidate'} or variants['baseline'] != variants['candidate']:
            raise ValueError(f'Different before/after calibration result sizes: {key}')
        size = variants['baseline']
        if type(size) is not int or size <= 0:
            raise ValueError(f'Invalid calibration result size: {key}')
        if key[1] == 'decode' and size != decoded_sizes[key[0]]:
            raise ValueError(f'Wrong decoded calibration result size: {key}')
        exact_sizes[key] = size
    operation_counts = {}
    groups = defaultdict(lambda: defaultdict(list))
    for row in report['rows']:
        key = row['workload'], row['operation']
        count = row['iterations']
        if type(count) is not int or count <= 0 or row['warmups'] != 20:
            raise ValueError(f'Invalid synchronous operation count: {row}')
        if operation_counts.setdefault(key, count) != count:
            raise ValueError(f'Different before/after iteration counts: {row}')
        wire_size = exact_sizes[(row['workload'], 'encode')]
        # Compare the raw rounded native field at its six significant digits;
        # the odd-call calibration checksum above retains the exact byte count.
        if row['payloadBytes'] != float(format(wire_size, '.6g')):
            raise ValueError(f'Different before/after wire sizes: {row}')
        observed_size = exact_sizes[key]
        if row['observed'] != (observed_size if count % 2 else 0):
            raise ValueError(f'Wrong synchronous completed result size: {row}')
        if not math.isfinite(row['millisecondsPerOperation']) or row['millisecondsPerOperation'] <= 0:
            raise ValueError(f'Invalid synchronous timing sample: {row}')
        groups[(row['workload'], row['operation'])][row['variant']].append(row['millisecondsPerOperation'])
    rows = []
    for (workload, operation), variants in sorted(groups.items()):
        baseline, candidate = variants['baseline'], variants['candidate']
        b, c = statistics.median(baseline), statistics.median(candidate)
        rows.append({'workload': workload, 'operation': operation, 'baselineMs': b, 'candidateMs': c,
                     'changePct': 100 * (c / b - 1),
                     'baselinePassRange': [min(baseline), max(baseline)],
                     'candidatePassRange': [min(candidate), max(candidate)]})
    return {'path': str(path.resolve()), 'sha256': sha(path), 'identities': report['identities'],
            'independentProcesses': len(report['rows']), 'results': rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--matrix', type=Path, action='append', required=True)
    parser.add_argument('--sync-control', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = {'summaryRunner': {'path': str(Path(__file__).resolve()), 'sha256': sha(__file__)},
              'methodology': 'Median of independent fresh-process medians. Pass ranges expose process '
              'variation; they are not confidence intervals. Geometric means weight each non-tiny workload '
              'equally. Total latency is the whole batch; per-operation time divides by the job count.',
              'matrices': [matrix(p) for p in args.matrix],
              'syncControls': [sync_control(p) for p in args.sync_control]}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'output': str(args.output), 'matrixProcesses': sum(m['independentProcesses'] for m in report['matrices']),
                      'syncProcesses': sum(m['independentProcesses'] for m in report['syncControls'])}))


if __name__ == '__main__':
    main()
