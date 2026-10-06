#!/usr/bin/env python3
"""Matched public-API comparison of two frozen native benchmark executables."""
import argparse
from datetime import datetime, timezone
import fcntl
import json
import math
from pathlib import Path
import random
import shlex
import statistics

from run import ROOT, corpus
from run_async import execute, sha


def identity(binary):
    binary = binary.resolve()
    manifest = binary.parent / 'build-manifest.json'
    data = json.loads(manifest.read_text())
    digest = sha(binary)
    if data['binaries']['msgpack_js_benchmark'] != digest:
        raise RuntimeError('Frozen executable mismatch: ' + str(binary))
    item = {'path': str(binary), 'sha256': digest,
            'manifest': {'path': str(manifest), 'sha256': sha(manifest),
                         'gitHead': data['gitHead']}}
    compiler = binary.parent / 'compiler-identity.json'
    if compiler.is_file():
        item['compilerIdentity'] = {'path': str(compiler), 'sha256': sha(compiler)}
    return item


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial')
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--remote')
    parser.add_argument('--passes', type=int, default=5)
    parser.add_argument('--samples', type=int, default=31)
    parser.add_argument('--seed', type=int, default=20261009)
    parser.add_argument('--workloads', default='records_25000,ascii_unique,cjk_unique,short_decimals')
    parser.add_argument('--codecs', default='msgpack,msgpack_async,json_async_bytes')
    parser.add_argument('--jobs', default='1,4')
    args = parser.parse_args()
    if args.passes < 1 or args.samples < 1:
        parser.error('passes and samples must be positive')
    if args.serial and (not args.runtime or not args.remote):
        parser.error('Android requires --runtime and --remote')
    codecs = args.codecs.split(',')
    if set(codecs) - {'msgpack', 'msgpack_async', 'json_async_bytes'}:
        parser.error('unsupported codec')
    jobs = [int(value) for value in args.jobs.split(',')]
    if any(value < 1 for value in jobs):
        parser.error('jobs must be positive')
    args.output.mkdir(parents=True, exist_ok=False)
    script = ROOT / 'test/msgpack/async-benchmark.js'
    (args.output / 'runner.py').write_bytes(Path(__file__).read_bytes())
    (args.output / 'benchmark.js').write_bytes(script.read_bytes())
    binaries = {name: identity(path) for name, path in
                [('baseline', args.baseline), ('candidate', args.candidate)]}
    cases = corpus()
    cases['cjk_unique'] = [{'id': i, 'body': f'{i}:' + '中文序列化配置性能测试' * 40}
                           for i in range(2500)]
    cases['tiny'] = {'id': 42, 'name': 'small', 'ok': True}
    cases = {name: cases[name] for name in args.workloads.split(',')}
    inputs = {}
    for name, value in cases.items():
        path = args.output / (name + '.json')
        path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
        inputs[name] = {'path': str(path.resolve()), 'sha256': sha(path)}
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'binaries': binaries, 'scriptSha256': sha(script), 'runnerSha256': sha(__file__),
              'inputs': inputs, 'passes': args.passes, 'samplesPerPass': args.samples,
              'seed': args.seed, 'serial': args.serial, 'rows': [],
              'runnerSource': 'runner.py', 'benchmarkSource': 'benchmark.js',
              'methodology': 'Shuffled matched fresh processes. Ten warmups and full GC; '
              'timed GC included. Complete value and exact MessagePack wire validation. '
              'Total and submission medians are medians of independent process medians; '
              'ranges describe variability, not confidence intervals.'}
    lock = None
    conditions = 'cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor /sys/class/thermal/thermal_zone0/temp'
    if args.serial:
        lock = open('/tmp/v8-msgpack-async-device.lock', 'a')
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        adb = ['adb', '-s', args.serial]
        execute(adb + ['shell', 'mkdir', '-p', args.remote])
        assets = [(Path(item['path']), name) for name, item in binaries.items()]
        assets += [(script, 'benchmark.js'), (args.runtime, 'libc++_shared.so')]
        assets += [(Path(item['path']), name + '.json') for name, item in inputs.items()]
        for source, name in assets:
            execute(adb + ['push', str(source), args.remote + '/' + name])
            if execute(adb + ['shell', 'sha256sum', args.remote + '/' + name]).split()[0] != sha(source):
                raise RuntimeError('Device hash mismatch: ' + name)
        execute(adb + ['shell', 'chmod', '755', args.remote + '/baseline', args.remote + '/candidate'])
        report.update(runtimeSha256=sha(args.runtime), cpuMask='f0',
                      deviceProperties=execute(adb + ['shell', 'getprop']),
                      deviceConditionsBefore=execute(adb + ['shell', conditions]))
    order = [(name, op, codec, count, pass_, variant)
             for name in cases for op in ['encode', 'decode'] for codec in codecs
             for count in jobs for pass_ in range(args.passes) for variant in binaries]
    random.Random(args.seed).shuffle(order)
    report['order'] = order
    destination = args.output / 'results.json'
    destination.write_text(json.dumps(report, indent=2) + '\n')
    wire_sizes, result_units = {}, {}
    for index, (name, op, codec, count, pass_, variant) in enumerate(order):
        if args.serial:
            invocation = adb + ['shell', shlex.join([
                'env', 'LD_LIBRARY_PATH=' + args.remote, 'taskset', 'f0',
                args.remote + '/' + variant, '--async', args.remote + '/benchmark.js',
                args.remote + '/' + name + '.json', op, codec, str(count), str(args.samples)])]
        else:
            invocation = [binaries[variant]['path'], '--async', str(script),
                          inputs[name]['path'], op, codec, str(count), str(args.samples)]
        row = json.loads(execute(invocation).strip().splitlines()[-1])
        if (row['operation'], row['codec'], row['jobs']) != (op, codec, count):
            raise RuntimeError('Unexpected benchmark case')
        if len(row['elapsed']) != args.samples or len(row['submit']) != args.samples:
            raise RuntimeError('Incomplete benchmark samples')
        if any(not math.isfinite(value) or value < 0
               for field in ['elapsed', 'submit'] for value in row[field]):
            raise RuntimeError('Invalid benchmark timing')
        inner = 1000 if name == 'tiny' else 1
        if row['innerIterations'] != inner:
            raise RuntimeError('Unexpected inner iteration count')
        sizes = (row['msgpackBytes'], row['jsonCodeUnits'])
        if wire_sizes.setdefault(name, sizes) != sizes:
            raise RuntimeError('Inconsistent payload sizes')
        batches = (args.samples + 10) * count * inner
        sink = row['sink']
        if not isinstance(sink, int) or sink % batches:
            raise RuntimeError('Invalid completed-result counter')
        units = sink // batches
        if op == 'decode' and units != len(cases[name]):
            raise RuntimeError('Incomplete decoded results')
        if op == 'encode' and codec.startswith('msgpack') and units != sizes[0]:
            raise RuntimeError('Incomplete encoded results')
        if op == 'encode' and codec == 'json_async_bytes' and not sizes[1] <= units <= 3 * sizes[1]:
            raise RuntimeError('Invalid UTF-8 result length')
        if result_units.setdefault((name, op, codec), units) != units:
            raise RuntimeError('Inconsistent completed results')
        row.update(workload=name, passIndex=pass_, variant=variant, command=invocation)
        report['rows'].append(row)
        destination.write_text(json.dumps(report, indent=2) + '\n')
        print(index + 1, len(order), name, op, codec, count, variant,
              statistics.median(row['elapsed']), statistics.median(row['submit']), flush=True)
    summary = []
    for name in cases:
        for op in ['encode', 'decode']:
            for codec in codecs:
                for count in jobs:
                    item = dict(workload=name, operation=op, codec=codec, jobs=count)
                    for variant in binaries:
                        rows = [row for row in report['rows'] if
                                (row['workload'], row['operation'], row['codec'], row['jobs'], row['variant']) ==
                                (name, op, codec, count, variant)]
                        stats = {}
                        for label, field in [('totalMs', 'elapsed'), ('submissionMs', 'submit')]:
                            values = [statistics.median(row[field]) for row in rows]
                            stats[label] = statistics.median(values)
                            stats[label + 'Range'] = [min(values), max(values)]
                        item[variant] = stats
                    for label in ['totalMs', 'submissionMs']:
                        item[label + 'ChangePct'] = 100 * (item['candidate'][label] / item['baseline'][label] - 1)
                    summary.append(item)
    report.update(summary=summary, complete=True, finishedUtc=datetime.now(timezone.utc).isoformat())
    if args.serial:
        report['deviceConditionsAfter'] = execute(adb + ['shell', conditions])
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print('MATCHED_COMPARISON_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
