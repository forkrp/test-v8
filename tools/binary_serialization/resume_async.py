#!/usr/bin/env python3
"""Resume an interrupted Android public-API matrix without replacing samples."""
import argparse
from datetime import datetime, timezone
import fcntl
import json
import math
from pathlib import Path
import random
import shlex
import statistics
import subprocess

from run import ROOT
from run_async import sha
from summarize_async import matrix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--failure-evidence', type=Path, required=True)
    args = parser.parse_args()
    destination = args.output / 'results.json'
    report = json.loads(destination.read_text())
    if report.get('finishedUtc'):
        parser.error('Matrix is already complete')
    if not report.get('serial'):
        parser.error('This resume path requires an Android matrix')
    assert sha(ROOT / 'test/msgpack/async-benchmark.js') == report['scriptSha256']
    assert sha(ROOT / 'tools/binary_serialization/run_async.py') == report['runnerSha256']
    assert sha(report['binary']) == report['binarySha256']
    assert sha(args.runtime) == report['runtimeSha256']
    manifest_identity = report['frozenBuildManifest']
    assert sha(manifest_identity['path']) == manifest_identity['sha256']
    compiler = manifest_identity['compilerIdentity']
    assert sha(compiler['path']) == compiler['sha256']
    manifest = json.loads(Path(manifest_identity['path']).read_text())
    assert manifest['binaries']['msgpack_js_benchmark'] == report['binarySha256']
    for name, digest in manifest['compileInputs'].items():
        assert sha(name) == digest, 'Source changed: ' + name
    decoded_sizes = {}
    for name, item in report['inputs'].items():
        assert sha(item['path']) == item['sha256']
        decoded_sizes[name] = len(json.loads(Path(item['path']).read_text()))

    invocation = report['invocation']
    def option(name, default=None):
        return invocation[invocation.index(name) + 1] if name in invocation else default

    assert '--native-harness' in invocation
    codecs = option('--codecs', 'msgpack,msgpack_async,json,json_async,json_bytes,json_async_bytes').split(',')
    jobs = [int(value) for value in option('--jobs', '1,4').split(',')]
    expected = [(name, op, codec, count, pass_) for pass_ in range(report['passes'])
                for name in report['inputs'] for op in ['encode', 'decode']
                for codec in codecs for count in jobs]
    random.Random(20261005).shuffle(expected)
    assert [tuple(item) for item in report['order']] == expected
    actual = [(r['workload'], r['operation'], r['codec'], r['jobs'], r['passIndex'])
              for r in report['rows']]
    assert actual == expected[:len(actual)] and len(actual) < len(expected)
    wire_sizes = {}

    def validate(row, case):
        name, op, codec, count, _ = case
        assert (row['operation'], row['codec'], row['jobs']) == (op, codec, count)
        for field in ['elapsed', 'submit']:
            assert len(row[field]) == report['samplesPerPass']
            assert all(math.isfinite(v) and v >= 0 for v in row[field])
        inner = 1000 if name == 'tiny' else 1
        assert row['innerIterations'] == inner
        sizes = row['msgpackBytes'], row['jsonCodeUnits']
        assert all(type(size) is int and size > 0 for size in sizes)
        assert wire_sizes.setdefault(name, sizes) == sizes
        batches = (10 + report['samplesPerPass']) * inner * count
        if op == 'decode':
            assert row['sink'] == decoded_sizes[name] * batches
        elif codec.startswith('msgpack'):
            assert row['sink'] == sizes[0] * batches
        elif codec in ['json', 'json_async']:
            assert row['sink'] == sizes[1] * batches
        else:
            assert sizes[1] * batches <= row['sink'] <= 3 * sizes[1] * batches

    for row, case in zip(report['rows'], expected):
        validate(row, case)

    adb = ['adb', '-s', report['serial']]
    remote = option('--remote')
    assert remote and report['cpuMask'] == 'f0'
    def execute(command):
        result = subprocess.run(command, text=True, capture_output=True)
        if result.returncode:
            failure = {'utc': datetime.now(timezone.utc).isoformat(), 'command': command,
                       'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr,
                       'completedProcesses': len(report['rows'])}
            index = len(list(args.output.glob('resume-failure-*.json')))
            (args.output / f'resume-failure-{index}.json').write_text(json.dumps(failure, indent=2) + '\n')
            raise RuntimeError(f"Invocation failed, exit {result.returncode}; diagnostics retained")
        return result.stdout

    def save():
        temporary = destination.with_suffix('.json.tmp')
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(destination)

    with open('/tmp/v8-msgpack-async-device.lock', 'a') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        assets = [(Path(report['binary']), 'd8'), (ROOT / 'test/msgpack/async-benchmark.js', 'benchmark.js'),
                  (args.runtime, 'libc++_shared.so')]
        assets += [(Path(item['path']), name + '.json') for name, item in report['inputs'].items()]
        for source, name in assets:
            assert execute(adb + ['shell', 'sha256sum', remote + '/' + name]).split()[0] == sha(source)
        checkpoint = args.output / f'resume-checkpoint-{len(report.get("continuations", []))}.json'
        checkpoint.write_bytes(destination.read_bytes())
        entry = {'startedUtc': datetime.now(timezone.utc).isoformat(),
                 'startIndex': len(report['rows']), 'preservedPrefixSha256': sha(destination),
                 'checkpoint': {'path': str(checkpoint.resolve()), 'sha256': sha(checkpoint)},
                 'resumer': {'path': str(Path(__file__).resolve()), 'sha256': sha(__file__)},
                 'failureEvidence': {'path': str(args.failure_evidence.resolve()), 'sha256': sha(args.failure_evidence)},
                 'deviceConditionsBefore': execute(adb + ['shell', report['deviceConditionCommand']]),
                 'note': 'Interrupted matrix resumed in the original shuffled order. Earlier samples retained unchanged; interrupted invocation retried in a fresh process. Device conditions may differ across segments.'}
        report.setdefault('continuations', []).append(entry)
        save()
        for index in range(len(report['rows']), len(expected)):
            case = expected[index]
            name, op, codec, count, pass_ = case
            command = adb + ['shell', shlex.join(['env', 'LD_LIBRARY_PATH=' + remote,
                'taskset', 'f0', remote + '/d8', '--async', remote + '/benchmark.js',
                remote + '/' + name + '.json', op, codec, str(count), str(report['samplesPerPass'])])]
            row = json.loads(execute(command).strip().splitlines()[-1])
            validate(row, case)
            row.update(workload=name, passIndex=pass_, command=command)
            report['rows'].append(row)
            save()
            print(json.dumps({'index': index + 1, 'count': len(expected), 'workload': name,
                  'operation': op, 'codec': codec, 'jobs': count,
                  'medianMs': statistics.median(row['elapsed']),
                  'submissionMs': statistics.median(row['submit'])}), flush=True)
        entry['finishedUtc'] = datetime.now(timezone.utc).isoformat()
        report['finishedUtc'] = entry['finishedUtc']
        report['deviceConditionsAfter'] = execute(adb + ['shell', report['deviceConditionCommand']])
        save()
    audited = matrix(destination)
    (args.output / 'resumed-audit.json').write_text(json.dumps(audited, indent=2) + '\n')
    print('RESUMED_MATRIX_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
