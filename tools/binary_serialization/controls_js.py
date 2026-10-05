#!/usr/bin/env python3
"""Extended fresh-process controls using the counts of a completed JS matrix."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import shlex
import statistics

from run_js import digest, execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--adb-serial', required=True)
    parser.add_argument('--remote-dir', type=Path, required=True)
    parser.add_argument('--runtime-library', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=60)
    parser.add_argument('--max-speedup', type=float, default=1.8,
                        help='Extend every operation at or below this matrix ratio')
    parser.add_argument('--seed', type=int, default=20261005)
    parser.add_argument('--sustained-protocol-encode', action='store_true',
                        help='Also check protocol encoding at 5000 iterations')
    args = parser.parse_args()
    if args.rounds < 60:
        parser.error('Acceptance controls require at least 60 rounds')
    matrix = json.loads(args.matrix.read_text())
    binary = args.binary.resolve()
    if matrix['binarySha256'] != digest(binary):
        raise RuntimeError('Matrix and control binary differ')
    manifest_path = binary.parent / 'build-manifest.json'
    manifest = json.loads(manifest_path.read_text())
    if manifest['binaries']['msgpack_js_benchmark'] != digest(binary):
        raise RuntimeError('Frozen manifest does not identify this binary')
    args.output.mkdir(parents=True, exist_ok=False)
    remote = args.remote_dir

    def adb(*command):
        return execute(['adb', '-s', args.adb_serial, *command])

    def push(path, name):
        destination = remote / name
        adb('push', str(path), str(destination))
        if adb('shell', 'sha256sum', str(destination)).split()[0] != digest(path):
            raise RuntimeError('Remote fingerprint mismatch: ' + name)

    adb('shell', 'mkdir', '-p', str(remote))
    push(binary, 'benchmark')
    push(args.runtime_library, 'libc++_shared.so')
    adb('shell', 'chmod', '755', str(remote / 'benchmark'))
    cases = []
    sources = {}
    for workload in matrix['workloads']:
        name = workload['name']
        source = args.matrix.parent / (name + '.source.json')
        if digest(source) != workload['sourceSha256']:
            raise RuntimeError('Matrix input changed: ' + name)
        selected = [(operation, workload['iterations'][operation])
                    for operation in ['decode', 'encode']
                    if workload['speedupVersusJsonBytes'][operation] <= args.max_speedup]
        if args.sustained_protocol_encode and name == 'protocol_fixture':
            selected.append(('encode', 5000))
        if not selected:
            continue
        push(source, source.name)
        sources[name] = {'path': str(source), 'sha256': digest(source)}
        for operation, count in selected:
            cases.append({'name': name, 'operation': operation, 'iterations': count,
                          'id': f'{name}:{operation}:{count}'})
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'binarySha256': digest(binary), 'manifestSha256': digest(manifest_path),
              'matrixSha256': digest(args.matrix), 'runnerSha256': digest(__file__),
              'runtimeSha256': digest(args.runtime_library), 'serial': args.adb_serial,
              'cpuMask': 'f0', 'rounds': args.rounds, 'seed': args.seed, 'warmups': 20,
              'cases': cases, 'sources': sources, 'samples': {}, 'orders': [],
              'methodology': 'Fresh processes, matrix counts shared by codecs, 20 warmups '
                             'and full GC before timing; timed GC included. Each shuffled '
                             'round alternates which codec runs first for each case. '
                             'All results receive complete benchmark validation.'}
    rng = random.Random(args.seed)
    for round_index in range(args.rounds):
        order = cases.copy()
        rng.shuffle(order)
        executed = []
        for case_index, case in enumerate(order):
            codecs = ['msgpack', 'json_bytes']
            if (round_index + case_index) % 2:
                codecs.reverse()
            for codec in codecs:
                command = ['env', 'LD_LIBRARY_PATH=' + str(remote), 'taskset', 'f0',
                           str(remote / 'benchmark'), case['operation'], codec,
                           str(remote / (case['name'] + '.source.json')),
                           str(case['iterations']), '20']
                output = adb('shell', shlex.join(command))
                sample = json.loads(output.strip().splitlines()[-1])
                key = case['id'] + ':' + codec
                report['samples'].setdefault(key, []).append(sample)
                executed.append(key)
        report['orders'].append(executed)
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print('completed round', round_index + 1, '/', args.rounds, flush=True)
    report['summary'] = []
    for case in cases:
        medians = {codec: statistics.median(sample['millisecondsPerOperation']
                   for sample in report['samples'][case['id'] + ':' + codec])
                   for codec in ['msgpack', 'json_bytes']}
        ratio = medians['json_bytes'] / medians['msgpack']
        blocks = []
        for start in [0, args.rounds // 2]:
            stop = args.rounds // 2 if start == 0 else args.rounds
            values = {codec: statistics.median(sample['millisecondsPerOperation']
                      for sample in report['samples'][case['id'] + ':' + codec][start:stop])
                      for codec in ['msgpack', 'json_bytes']}
            blocks.append(values['json_bytes'] / values['msgpack'])
        report['summary'].append({**case, 'medians': medians, 'ratio': ratio,
                                  'halfRunRatios': blocks,
                                  'allMedianGates': ratio >= 1.5 and min(blocks) >= 1.5})
    report['completedUtc'] = datetime.now(timezone.utc).isoformat()
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['summary'], indent=2))


if __name__ == '__main__':
    main()
