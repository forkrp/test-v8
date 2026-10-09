#!/usr/bin/env python3
"""Compare resource/standard MessagePack and JSON bytes on both Android ABIs.

Uses source-matched frozen native public-JS harnesses. Each row is a fresh
process; sync/async codec order is shuffled within matched workload blocks.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import random
import shlex
import statistics
import subprocess
import time

from device_guard import DeviceGuard
from run import ROOT, corpus

CODECS = ['msgpack_resource', 'msgpack_resource_async', 'msgpack',
          'msgpack_async', 'json_bytes', 'json_async_bytes']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def execute(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def save(path, value):
    temporary = path.with_suffix('.json.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def cases():
    values = corpus()
    values['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    for name, text in [('cjk_unique', '中文序列化配置性能测试'),
                       ('greek_unique', 'Ελληνικάκείμενα'), ('emoji_unique', '🌏😀🚀🧪')]:
        values[name] = [{'id': i, 'body': f'{i}:' + text * 40} for i in range(2500)]
    values['tiny'] = {'id': 42, 'name': 'small', 'ok': True}
    return values


def identity(path):
    manifest_path = path / 'build-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    binary = path / 'msgpack_js_benchmark'
    if sha(binary) != manifest['binaries']['msgpack_js_benchmark']:
        raise RuntimeError('Frozen benchmark identity mismatch: ' + str(binary))
    return {'binary': str(binary.resolve()), 'binarySha256': sha(binary),
            'manifest': str(manifest_path.resolve()), 'manifestSha256': sha(manifest_path),
            'engineCommit': manifest['gitHead'],
            'compilerIdentitySha256': sha(path / 'compiler-identity.json'),
            'runtime': str((path / 'libc++_shared.so').resolve()),
            'runtimeSha256': sha(path / 'libc++_shared.so')}


def validate_row(row, case, report):
    arch, name, op, codec, jobs, pass_index = case
    if (row['operation'], row['codec'], row['jobs']) != (op, codec, jobs):
        raise RuntimeError('Unexpected benchmark row')
    samples = report['samplesPerProcess']
    for metric in ['elapsed', 'submit']:
        if len(row[metric]) != samples or any(
                not math.isfinite(v) or v <= 0 for v in row[metric]):
            raise RuntimeError('Invalid or incomplete timing samples')
    inner = 1000 if name == 'tiny' else 1
    count = (10 + samples) * inner * jobs
    if row['innerIterations'] != inner or row['warmupBatches'] != 10:
        raise RuntimeError('Unexpected operation counts')
    if row['completedOperations'] != count or not row['valueValidated']:
        raise RuntimeError('Incomplete or unvalidated completed results')
    if op == 'encode' and codec.startswith('msgpack') and not row['wireValidated']:
        raise RuntimeError('Missing exact wire parity check')
    for field in ['msgpackBytes', 'resourceBytes', 'jsonBytes', 'jsonCodeUnits']:
        if type(row[field]) is not int or row[field] <= 0:
            raise RuntimeError('Invalid wire size')
    if row['resourceBytes'] > row['msgpackBytes']:
        raise RuntimeError('Resource output exceeded ordinary wire size')
    if not row['resourceCompressed'] and row['resourceBytes'] != row['msgpackBytes']:
        raise RuntimeError('Invalid exact standard fallback size')
    if op == 'decode':
        units = report['inputs'][name]['rootUnits']
    elif codec.startswith('msgpack_resource'):
        units = row['resourceBytes']
    elif codec.startswith('msgpack'):
        units = row['msgpackBytes']
    else:
        units = row['jsonBytes']
    if row['sink'] != units * count:
        raise RuntimeError('Result counter or payload length mismatch')
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arm64', type=Path, required=True)
    parser.add_argument('--arm32', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial', default='885841c1')
    parser.add_argument('--passes', type=int, default=5)
    parser.add_argument('--samples', type=int, default=31)
    parser.add_argument('--workloads')
    parser.add_argument('--codecs', default=','.join(CODECS))
    parser.add_argument('--jobs', default='1,4')
    parser.add_argument('--seed', type=int, default=20261009)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    selected_codecs = args.codecs.split(',')
    jobs = [int(n) for n in args.jobs.split(',')]
    if args.passes < 1 or args.samples < 1 or set(selected_codecs) - set(CODECS):
        parser.error('Invalid counts or codecs')
    if any(n < 1 or n > 32 for n in jobs):
        parser.error('Jobs must be between 1 and 32')
    script = ROOT / 'test/msgpack/async-benchmark.js'
    identities = {arch: identity(path.resolve()) for arch, path in
                  [('arm64', args.arm64), ('arm32', args.arm32)]}
    if len({item['engineCommit'] for item in identities.values()}) != 1:
        raise RuntimeError('ABI source commits differ')
    values = cases()
    if args.workloads:
        values = {name: values[name] for name in args.workloads.split(',')}
    path = args.output.resolve() / 'results.json'
    remote = '/data/local/tmp/msgpack-resource-perf-' + identities['arm64']['engineCommit'][:9] + '-' + args.output.name
    config = {'passes': args.passes, 'samples': args.samples, 'workloads': list(values),
              'codecs': selected_codecs, 'jobs': jobs, 'seed': args.seed, 'serial': args.serial}
    if args.resume:
        report = json.loads(path.read_text())
        if report.get('finishedUtc') or report['config'] != config or report['identities'] != identities:
            raise RuntimeError('Completed matrix or resume identity/configuration mismatch')
        for key, source in [('scriptSha256', script), ('runnerSha256', __file__),
                            ('deviceGuardSha256', ROOT / 'tools/binary_serialization/device_guard.py')]:
            if report[key] != sha(source):
                raise RuntimeError('Resume source identity changed: ' + str(source))
        checkpoint = args.output / ('resume-checkpoint-' + str(len(report.get('continuations', []))) + '.json')
        checkpoint.write_bytes(path.read_bytes())
        report.setdefault('continuations', []).append({
            'startedUtc': datetime.now(timezone.utc).isoformat(), 'startIndex': len(report['rows']),
            'checkpoint': str(checkpoint.resolve()), 'checkpointSha256': sha(checkpoint)})
    else:
        args.output.mkdir(parents=True, exist_ok=False)
        for source, name in [(script, 'benchmark.js'), (Path(__file__), 'runner.py'),
                             (ROOT / 'tools/binary_serialization/device_guard.py', 'device_guard.py'),
                             (ROOT / 'tools/binary_serialization/run.py', 'corpus-generator.py')]:
            (args.output / name).write_bytes(source.read_bytes())
        inputs = {}
        for name, value in values.items():
            source = args.output / (name + '.json')
            source.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
            inputs[name] = {'path': str(source.resolve()), 'sha256': sha(source),
                            'rootUnits': len(value), 'sourceBytes': source.stat().st_size}
        rng = random.Random(args.seed)
        order = []
        for pass_index in range(args.passes):
            abis = list(identities)
            rng.shuffle(abis)
            for arch in abis:
                blocks = [(name, op, count) for name in values
                          for op in ['encode', 'decode'] for count in jobs]
                rng.shuffle(blocks)
                for name, op, count in blocks:
                    codecs = selected_codecs.copy()
                    rng.shuffle(codecs)
                    order.extend((arch, name, op, codec, count, pass_index) for codec in codecs)
        report = {'startedUtc': datetime.now(timezone.utc).isoformat(), 'config': config,
                  'identities': identities, 'scriptSha256': sha(script), 'runnerSha256': sha(__file__),
                  'deviceGuardSha256': sha(ROOT / 'tools/binary_serialization/device_guard.py'),
                  'samplesPerProcess': args.samples, 'inputs': inputs, 'order': order, 'rows': [],
                  'methodology': 'Fresh process per ABI/workload/operation/codec/jobs/pass. '
                  'Codec order shuffled within matched blocks; block and ABI order shuffled per pass. '
                  'Ten warmup batches; full GC before timing, timed GC included. '
                  'Validate complete values and exact MessagePack wire before timing. '
                  'Record synchronous submission and awaited batch total separately; tiny uses '
                  '1000 inner batches per sample. JSON sync/async include native UTF-8 owned-byte transport. '
                  'Summaries use medians of independent process medians and retain all regressions.'}
    guard = DeviceGuard(args.serial, remote, args.output / 'excluded-overlaps.json')
    guard.wait_idle()
    adb = ['adb', '-s', args.serial]
    execute(adb + ['shell', 'mkdir', '-p', remote + '/inputs'])
    assets = [(script, remote + '/benchmark.js')]
    for arch, item in identities.items():
        execute(adb + ['shell', 'mkdir', '-p', remote + '/' + arch])
        assets += [(Path(item['binary']), remote + '/' + arch + '/benchmark'),
                   (Path(item['runtime']), remote + '/' + arch + '/libc++_shared.so')]
    assets += [(Path(item['path']), remote + '/inputs/' + name + '.json')
               for name, item in report['inputs'].items()]
    for source, destination in assets:
        if args.resume:
            if execute(adb + ['shell', 'sha256sum', destination]).split()[0] != sha(source):
                raise RuntimeError('Changed device asset: ' + destination)
        else:
            execute(adb + ['push', str(source), destination])
            if execute(adb + ['shell', 'sha256sum', destination]).split()[0] != sha(source):
                raise RuntimeError('Device asset hash mismatch: ' + destination)
    for arch in identities:
        execute(adb + ['shell', 'chmod', '755', remote + '/' + arch + '/benchmark'])
    conditions = 'cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor /sys/class/thermal/thermal_zone0/temp; cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq; cat /proc/meminfo | head -5'
    report.setdefault('deviceProperties', execute(adb + ['shell', 'getprop']))
    report.setdefault('deviceConditionsBefore', execute(adb + ['shell', conditions]))
    report['remote'] = remote
    report['cpuMask'] = 'f0'
    save(path, report)
    sizes = {}
    for index, row in enumerate(report['rows']):
        case = report['order'][index]
        validate_row(row, case, report)
        key = row['workload']
        size = tuple(row[n] for n in ['msgpackBytes', 'resourceBytes', 'resourceCompressed', 'jsonBytes', 'jsonCodeUnits'])
        if sizes.setdefault(key, size) != size:
            raise RuntimeError('Retained row payload sizes differ')
    for index in range(len(report['rows']), len(report['order'])):
        case = report['order'][index]
        arch, name, op, codec, count, pass_index = case
        command = adb + ['shell', shlex.join([
            'env', 'LD_LIBRARY_PATH=' + remote + '/' + arch, 'timeout', '-k', '10', '900',
            'taskset', 'f0', remote + '/' + arch + '/benchmark', '--async',
            remote + '/benchmark.js', remote + '/inputs/' + name + '.json',
            op, codec, str(count), str(args.samples)])]
        start = time.monotonic()
        try:
            output = guard.run(command)
            row = json.loads(output.strip().splitlines()[-1])
            validate_row(row, case, report)
            size = tuple(row[n] for n in ['msgpackBytes', 'resourceBytes', 'resourceCompressed', 'jsonBytes', 'jsonCodeUnits'])
            if sizes.setdefault(name, size) != size:
                raise RuntimeError('Wire sizes or fallback format changed across processes/ABIs')
        except Exception as error:
            failure = {'utc': datetime.now(timezone.utc).isoformat(), 'case': case,
                       'command': command, 'error': str(error), 'completedRows': len(report['rows'])}
            destination = args.output / ('failure-' + str(index) + '.json')
            save(destination, failure)
            raise
        row.update(abi=arch, workload=name, passIndex=pass_index, command=command,
                   processSeconds=time.monotonic() - start)
        report['rows'].append(row)
        if index % 12 == 0:
            row['deviceConditionsAfter'] = execute(adb + ['shell', conditions])
        save(path, report)
        print(json.dumps({'index': index + 1, 'count': len(report['order']), 'abi': arch,
                         'workload': name, 'operation': op, 'codec': codec, 'jobs': count,
                         'pass': pass_index, 'totalMs': statistics.median(row['elapsed']),
                         'submitMs': statistics.median(row['submit'])}), flush=True)
    report['finishedUtc'] = datetime.now(timezone.utc).isoformat()
    report['deviceConditionsAfter'] = execute(adb + ['shell', conditions])
    for source, destination in assets:
        if sha(source) != execute(adb + ['shell', 'sha256sum', destination]).split()[0]:
            raise RuntimeError('Device asset changed during measurements')
    save(path, report)
    print('RESOURCE_ASYNC_MATRIX_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
