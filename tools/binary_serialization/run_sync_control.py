#!/usr/bin/env python3
"""Compare frozen before/after synchronous MSGPACK builtins on the full corpus."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
from pathlib import Path
import random
import shlex
import statistics
import subprocess

from run import ROOT, corpus


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def execute(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{command}: {result.stdout[-2000:]} {result.stderr[-3000:]}')
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--remote', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--passes', type=int, default=5)
    parser.add_argument('--workloads')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    with open('/tmp/v8-msgpack-async-device.lock', 'a') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        adb = ['adb', '-s', args.serial]
        execute(adb + ['shell', 'mkdir', '-p', args.remote])
        identities = {}
        for name, path in [('baseline', args.baseline), ('candidate', args.candidate),
                           ('libc++_shared.so', args.runtime)]:
            execute(adb + ['push', str(path), args.remote + '/' + name])
            if execute(adb + ['shell', 'sha256sum', args.remote + '/' + name]).split()[0] != sha(path):
                raise RuntimeError('Device hash mismatch: ' + name)
            identities[name] = {'path': str(path.resolve()), 'sha256': sha(path)}
            manifest = path.parent / 'build-manifest.json'
            if name != 'libc++_shared.so' and manifest.is_file():
                data = json.loads(manifest.read_text())
                if data['binaries']['msgpack_js_benchmark'] != sha(path):
                    raise RuntimeError('Frozen build mismatch: ' + name)
                identities[name]['manifest'] = {'path': str(manifest.resolve()), 'sha256': sha(manifest),
                                               'gitHead': data['gitHead']}
                compiler_path = path.parent / 'compiler-identity.json'
                if compiler_path.is_file():
                    identities[name]['compilerIdentity'] = {'path': str(compiler_path.resolve()),
                                                          'sha256': sha(compiler_path)}
        execute(adb + ['shell', 'chmod', '755', args.remote + '/baseline', args.remote + '/candidate'])
        cases = corpus()
        cases['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
        for name, text in [('cjk_unique', '中文序列化配置性能测试'), ('greek_unique', 'Ελληνικάκείμενα'),
                           ('emoji_unique', '🌏😀🚀🧪')]:
            cases[name] = [{'id': i, 'body': f'{i}:' + text * 40} for i in range(2500)]
        cases['tiny'] = {'id': 42, 'name': 'small', 'ok': True}
        if args.workloads:
            cases = {name: cases[name] for name in args.workloads.split(',')}
        conditions = 'cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor /sys/class/thermal/thermal_zone0/temp'
        report = {'startedUtc': datetime.now(timezone.utc).isoformat(), 'identities': identities,
                  'runnerSha256': sha(__file__), 'serial': args.serial, 'cpuMask': 'f0', 'rows': [],
                  'calibrationRows': [],
                  'deviceProperties': execute(adb + ['shell', 'getprop']),
                  'deviceConditionsBefore': execute(adb + ['shell', conditions]),
                  'methodology': 'Complete value validation, 20 warmups, full GC before timing, timed GC included. '
                  'Shared calibrated iteration count across before/after per operation; shuffled fresh processes, '
                  'five passes by default. Measures synchronous MSGPACK public builtins only.'}
        counts = {}
        inputs = {}

        def measure(name, op, variant, count):
            command = adb + ['shell', shlex.join(['env', 'LD_LIBRARY_PATH=' + args.remote, 'taskset', 'f0',
                       args.remote + '/' + variant, op, 'msgpack', args.remote + '/' + name + '.json', str(count), '20'])]
            row = json.loads(execute(command).strip().splitlines()[-1])
            row['command'] = command
            return row

        for name, value in cases.items():
            path = args.output / (name + '.json')
            path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
            execute(adb + ['push', str(path), args.remote + '/' + path.name])
            if execute(adb + ['shell', 'sha256sum', args.remote + '/' + path.name]).split()[0] != sha(path):
                raise RuntimeError('Device input mismatch: ' + name)
            inputs[name] = sha(path)
            for op in ['encode', 'decode']:
                calibration = []
                for variant in ['baseline', 'candidate']:
                    row = measure(name, op, variant, 3)
                    row.update(workload=name, operation=op, variant=variant)
                    report['calibrationRows'].append(row)
                    calibration.append(row['millisecondsPerOperation'])
                # The existing native harness prints payloadBytes through a
                # double with default stream precision. Its XOR checksum for
                # an odd number of calls retains the exact integer length.
                before, after = report['calibrationRows'][-2:]
                if before['observed'] != after['observed']:
                    raise RuntimeError('Before/after result length mismatch: ' + name + ' ' + op)
                counts[(name, op)] = max(3, min(1000, int(100 / max(calibration))))
        report['inputs'] = inputs
        order = [(name, op, variant, pass_) for pass_ in range(args.passes) for name in cases
                 for op in ['encode', 'decode'] for variant in ['baseline', 'candidate']]
        random.Random(20261006).shuffle(order)
        report['order'] = order
        for index, (name, op, variant, pass_) in enumerate(order):
            row = measure(name, op, variant, counts[(name, op)])
            row.update(workload=name, operation=op, variant=variant, passIndex=pass_)
            report['rows'].append(row)
            (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({'index': index + 1, 'count': len(order), 'workload': name, 'operation': op,
                              'variant': variant, 'ms': row['millisecondsPerOperation']}), flush=True)
        summary = []
        for name in cases:
            for op in ['encode', 'decode']:
                item = {'workload': name, 'operation': op}
                for variant in ['baseline', 'candidate']:
                    item[variant] = statistics.median(row['millisecondsPerOperation'] for row in report['rows']
                                  if (row['workload'], row['operation'], row['variant']) == (name, op, variant))
                item['candidateRatio'] = item['candidate'] / item['baseline']
                summary.append(item)
        report['summary'] = summary
        report['finishedUtc'] = datetime.now(timezone.utc).isoformat()
        report['deviceConditionsAfter'] = execute(adb + ['shell', conditions])
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
