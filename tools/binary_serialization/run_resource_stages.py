#!/usr/bin/env python3
"""Measure cumulative wire-feature ablations through one unchanged decoder."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import random
import shlex
import statistics

from run_js import digest, execute
from device_guard import DeviceGuard


def main():
    p = argparse.ArgumentParser()
    for name in ['binary', 'warm-matrix', 'cold-matrix', 'runtime-library', 'output']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--adb-serial', required=True)
    p.add_argument('--remote-dir', type=Path, required=True)
    p.add_argument('--rounds', type=int, default=5)
    p.add_argument('--seed', type=int, default=47013)
    a = p.parse_args()
    warm = json.loads(a.warm_matrix.read_text())
    cold = json.loads(a.cold_matrix.read_text())
    binary_hash = digest(a.binary)
    if warm['binarySha256'] != binary_hash or cold['binarySha256'] != binary_hash:
        raise RuntimeError('Matrix/binary identity mismatch')
    a.output.mkdir(parents=True, exist_ok=False)
    remote = a.remote_dir

    def adb(*args):
        return execute(['adb', '-s', a.adb_serial, *args])

    def push(path, name):
        adb('push', str(path), str(remote / name))
        if adb('shell', 'sha256sum', str(remote / name)).split()[0] != digest(path):
            raise RuntimeError('Device file identity mismatch: ' + name)

    adb('shell', 'mkdir', '-p', str(remote))
    push(a.binary, 'js-binary-benchmark')
    push(a.runtime_library, 'libc++_shared.so')
    adb('shell', 'chmod', '755', str(remote / 'js-binary-benchmark'))
    cold_by_name = {w['name']: w for w in cold['workloads']}
    cases, wires = [], {}
    for w in warm['workloads']:
        name = w['name']
        source = a.cold_matrix.parent / (name + '.source.json')
        if digest(source) != w['sourceSha256']:
            raise RuntimeError('Source identity mismatch')
        push(source, source.name)
        # Identical stage wires need no repeat timing. Report their aliases.
        seen = {}
        for stage in ['msgpack', 'shapes', 'shapes_strings', 'resource']:
            path = a.cold_matrix.parent / (name + '.' + stage)
            sha = digest(path)
            key = name + ':' + stage
            if sha in seen:
                wires[key] = {'sha256': sha, 'bytes': path.stat().st_size,
                              'measurementAlias': seen[sha]}
                continue
            seen[sha] = key
            wires[key] = {'sha256': sha, 'bytes': path.stat().st_size}
            push(path, path.name)
            for mode in ['cold', 'warm']:
                cases.append({'key': key + ':' + mode, 'name': name,
                              'stage': stage, 'mode': mode,
                              'iterations': 1 if mode == 'cold' else w['iterations']['decode']})
        for codec in ['msgpack', 'resource']:
            if wires[name + ':' + codec]['sha256'] != cold_by_name[name]['preparedInputs'][codec]['sha256']:
                raise RuntimeError('Prepared wire identity mismatch')
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'binarySha256': binary_hash, 'runnerSha256': digest(__file__), 'deviceGuardSha256': digest(Path(__file__).parent / 'device_guard.py'),
              'warmMatrixSha256': digest(a.warm_matrix), 'coldMatrixSha256': digest(a.cold_matrix),
              'runtimeSha256': digest(a.runtime_library), 'serial': a.adb_serial,
              'cpuMask': 'f0', 'rounds': a.rounds, 'seed': a.seed,
              'wires': wires, 'cases': cases, 'samples': {}, 'orders': [],
              'methodology': 'Independent wire transformations remove numeric blocks, then string '
                             'references, from final files. Each stage includes its own full tables '
                             'and file-level fallback. One unchanged native decoder reads prepared '
                             'bytes; no recompilation or encoding precedes timing. Stages with '
                             'identical bytes share measurements. Fresh processes, shuffled order, '
                             '20 warmups for warmed decoding; cold decoding is exactly its first '
                             'API call. Table validation/construction and timed GC are included.'}
    rng = random.Random(a.seed)
    guard = DeviceGuard(a.adb_serial, remote, a.output / 'excluded-device-overlaps.json')
    for round_index in range(a.rounds):
        order = cases.copy()
        rng.shuffle(order)
        report['orders'].append([case['key'] for case in order])
        for case in order:
            cold_call = case['mode'] == 'cold'
            # Use one API for every wire stage, including the standard control.
            # This also makes identical-wire aliases valid measurements.
            codec = 'resource'
            command = ['env', 'LD_LIBRARY_PATH=' + str(remote), 'taskset', 'f0',
                       str(remote / 'js-binary-benchmark'), 'decode', codec,
                       str(remote / (case['name'] + '.source.json')), str(case['iterations']),
                       '0' if cold_call else '20', '--cold' if cold_call else '--input',
                       str(remote / (case['name'] + '.' + case['stage']))]
            output = guard.run(['adb', '-s', a.adb_serial, 'shell', shlex.join(command)])
            report['samples'].setdefault(case['key'], []).append(json.loads(output.strip().splitlines()[-1]))
        (a.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print('completed stage round', round_index + 1, '/', a.rounds, flush=True)
    report['medians'] = {key: statistics.median(s['millisecondsPerOperation'] for s in samples)
                         for key, samples in report['samples'].items()}
    for key, wire in wires.items():
        if 'measurementAlias' in wire:
            for mode in ['cold', 'warm']:
                report['medians'][key + ':' + mode] = report['medians'][wire['measurementAlias'] + ':' + mode]
    report['completedUtc'] = datetime.now(timezone.utc).isoformat()
    (a.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
