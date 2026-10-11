#!/usr/bin/env python3
"""Paired Android before/after matrices, with independent codec controls."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import shlex
import statistics
import time

from device_guard import DeviceGuard
from run import ROOT
from run_resource_async import CODECS, cases, execute, identity, save, sha, validate_row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for version in ['before', 'after']:
        for abi in ['arm64', 'arm32']:
            parser.add_argument('--' + version + '-' + abi, type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--serial', default='885841c1')
    parser.add_argument('--passes', type=int, default=3)
    parser.add_argument('--samples', type=int, default=13)
    parser.add_argument('--workloads')
    parser.add_argument('--codecs', default=','.join(CODECS))
    parser.add_argument('--jobs', default='1,4')
    parser.add_argument('--operations', default='encode,decode')
    parser.add_argument('--seed', type=int, default=20261010)
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    codecs, jobs = args.codecs.split(','), [int(x) for x in args.jobs.split(',')]
    operations = args.operations.split(',')
    if (args.passes < 1 or args.samples < 1 or set(codecs) - set(CODECS) or
            any(x < 1 or x > 32 for x in jobs) or
            not operations or len(set(operations)) != len(operations) or
            set(operations) - {'encode', 'decode'}):
        parser.error('Invalid codec or sample/job/pass count')
    identities = {version: {abi: identity(getattr(args, version + '_' + abi).resolve())
                            for abi in ['arm64', 'arm32']}
                  for version in ['before', 'after']}
    for version, abis in identities.items():
        if len({v['engineCommit'] for v in abis.values()}) != 1:
            raise RuntimeError('ABI source commits differ: ' + version)
    script = ROOT / 'test/msgpack/async-benchmark.js'
    values = cases()
    if args.workloads:
        values = {name: values[name] for name in args.workloads.split(',')}
    config = dict(passes=args.passes, samples=args.samples, workloads=list(values),
                  codecs=codecs, jobs=jobs, seed=args.seed, serial=args.serial)
    if operations != ['encode', 'decode']:
        config['operations'] = operations
    dependencies = {str(path): sha(path) for path in [Path(__file__), script,
                    ROOT/'tools/binary_serialization/run_resource_async.py',
                    ROOT/'tools/binary_serialization/run.py',
                    ROOT/'tools/binary_serialization/device_guard.py']}
    output = args.output.resolve()
    path = output / 'results.json'
    remote = '/data/local/tmp/msgpack-resource-paired-' + output.name
    if args.resume:
        report = json.loads(path.read_text())
        if (report.get('finishedUtc') or report['config'] != config or
                report['identities'] != identities or report['dependencies'] != dependencies):
            raise RuntimeError('Completed matrix or changed resume identity/configuration')
        checkpoint = output / ('resume-' + str(len(report.get('continuations', []))) + '.json')
        checkpoint.write_bytes(path.read_bytes())
        report.setdefault('continuations', []).append(dict(startIndex=len(report['rows']),
            startedUtc=datetime.now(timezone.utc).isoformat(), checkpointSha256=sha(checkpoint)))
    else:
        output.mkdir(parents=True, exist_ok=False)
        for source in dependencies:
            (output/Path(source).name).write_bytes(Path(source).read_bytes())
        inputs = {}
        for name, value in values.items():
            source = output / (name + '.json')
            source.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
            inputs[name] = dict(path=str(source), sha256=sha(source), rootUnits=len(value))
        rng = random.Random(args.seed)
        order = []
        for pass_index in range(args.passes):
            blocks = [(abi, name, op, count, codec) for abi in ['arm64', 'arm32']
                      for name in values for op in operations
                      for count in jobs for codec in codecs]
            rng.shuffle(blocks)
            for abi, name, op, count, codec in blocks:
                versions = ['before', 'after']
                rng.shuffle(versions)
                order.extend([version, abi, name, op, codec, count, pass_index]
                             for version in versions)
        report = dict(startedUtc=datetime.now(timezone.utc).isoformat(), config=config,
            identities=identities, dependencies=dependencies, inputs=inputs, order=order,
            rows=[], samplesPerProcess=args.samples, remote=remote, cpuMask='f0',
            methodology='Adjacent before/after fresh processes in shuffled blocks, '
            'independent shuffled passes. Full values, exact same-version wire, sizes and '
            'completed operation counters validated. Ten warmups; timed GC included; '
            'tiny uses 1000 batches per sample. Submission and awaited total separate. '
            'All six codecs retained as controls; all regressions retained.')
    guard = DeviceGuard(args.serial, remote, output/'excluded-overlaps.json')
    guard.wait_idle()
    adb = ['adb', '-s', args.serial]
    execute(adb + ['shell', 'mkdir', '-p', remote+'/inputs'])
    assets = [(script, remote+'/benchmark.js')]
    for version, abis in identities.items():
        for abi, info in abis.items():
            directory = remote+'/'+version+'/'+abi
            execute(adb + ['shell', 'mkdir', '-p', directory])
            assets += [(Path(info['binary']), directory+'/benchmark'),
                       (Path(info['runtime']), directory+'/libc++_shared.so')]
    assets += [(Path(v['path']), remote+'/inputs/'+name+'.json')
               for name, v in report['inputs'].items()]
    for source, destination in assets:
        if not args.resume:
            execute(adb + ['push', str(source), destination])
        if execute(adb + ['shell', 'sha256sum', destination]).split()[0] != sha(source):
            raise RuntimeError('Changed device asset: '+destination)
    for version in identities:
        for abi in identities[version]:
            execute(adb + ['shell', 'chmod', '755', remote+'/'+version+'/'+abi+'/benchmark'])
    conditions = ('cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor '
                  '/sys/class/thermal/thermal_zone0/temp; '
                  'cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq; '
                  'cat /proc/meminfo | head -5')
    report.setdefault('deviceProperties', execute(adb+['shell', 'getprop']))
    report.setdefault('deviceConditionsBefore', execute(adb+['shell', conditions]))
    sizes = {}
    def validate(row, case):
        version, abi, name, op, codec, count, pass_index = case
        validate_row(row, [abi, name, op, codec, count, pass_index], report)
        size = tuple(row[x] for x in ['msgpackBytes', 'resourceBytes',
                                     'resourceCompressed', 'jsonBytes', 'jsonCodeUnits'])
        if sizes.setdefault(name, size) != size:
            raise RuntimeError('Wire sizes or selection changed across versions/ABIs')
    for row, case in zip(report['rows'], report['order']):
        validate(row, case)
    save(path, report)
    for index in range(len(report['rows']), len(report['order'])):
        case = report['order'][index]
        version, abi, name, op, codec, count, pass_index = case
        directory = remote+'/'+version+'/'+abi
        command = adb+['shell', shlex.join(['env', 'LD_LIBRARY_PATH='+directory,
            'timeout', '-k', '10', '900', 'taskset', 'f0', directory+'/benchmark',
            '--async', remote+'/benchmark.js', remote+'/inputs/'+name+'.json',
            op, codec, str(count), str(args.samples)])]
        started = time.monotonic()
        try:
            row = json.loads(guard.run(command).strip().splitlines()[-1])
            validate(row, case)
        except Exception as error:
            save(output/('failure-'+str(index)+'.json'), dict(case=case, command=command,
                 error=str(error), completedRows=len(report['rows'])))
            raise
        row.update(version=version, abi=abi, workload=name, passIndex=pass_index,
                   command=command, processSeconds=time.monotonic()-started)
        if index % 24 == 0:
            row['deviceConditionsAfter'] = execute(adb+['shell', conditions])
        report['rows'].append(row)
        save(path, report)
        print(json.dumps(dict(done=index+1, total=len(report['order']), version=version,
            abi=abi, workload=name, op=op, codec=codec, jobs=count, passIndex=pass_index,
            totalMs=statistics.median(row['elapsed']), submitMs=statistics.median(row['submit']))),
            flush=True)
    for source, destination in assets:
        if sha(source) != execute(adb+['shell', 'sha256sum', destination]).split()[0]:
            raise RuntimeError('Device asset changed during matrix')
    report['finishedUtc'] = datetime.now(timezone.utc).isoformat()
    report['deviceConditionsAfter'] = execute(adb+['shell', conditions])
    save(path, report)
    print('PAIRED_RESOURCE_MATRIX_COMPLETE', flush=True)


if __name__ == '__main__':
    main()
