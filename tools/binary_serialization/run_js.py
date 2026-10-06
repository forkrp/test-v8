#!/usr/bin/env python3
"""Fresh-process JS API matrix; byte transport and JSON strings kept separate."""
import argparse
from datetime import datetime, timezone
import hashlib
import gzip
import json
from pathlib import Path
import random
import shlex
import statistics
import struct
import subprocess

from run import ROOT, corpus
from device_guard import DeviceGuard


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def execute(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{command}: {result.stdout[-2000:]} {result.stderr[-3000:]}')
    return result.stdout


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--build-manifest', type=Path,
                   help='Frozen GN manifest; verifies the tested binary and binds it to compile inputs')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--adb-serial')
    p.add_argument('--remote-dir', type=Path)
    p.add_argument('--runtime-library', type=Path,
                   help='Matching Android libc++_shared.so; copied and fingerprinted')
    p.add_argument('--rounds', type=int, default=5)
    p.add_argument('--seed', type=int, default=20261014)
    p.add_argument('--target-ms', type=float, default=150)
    p.add_argument('--min-iterations', type=int, default=3,
                   help='Lower bound shared by every codec, for longer steady-state batches')
    p.add_argument('--warmups', type=int, default=20)
    p.add_argument('--cold', action='store_true',
                   help='Time exactly the first API call in each process, using separately prepared bytes')
    p.add_argument('--workloads', help='Pilot subset; acceptance uses the full 18-workload corpus')
    p.add_argument('--include-resource', action='store_true',
                   help='Add resource decode; cold runs also measure offline encode and file sizes')
    args = p.parse_args()
    if args.min_iterations < 1 or args.min_iterations > 1000 or args.target_ms <= 0:
        p.error('Require positive target-ms and min-iterations in [1, 1000]')
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    build_manifest = args.build_manifest
    if not build_manifest and (binary.parent / 'build-manifest.json').is_file():
        build_manifest = binary.parent / 'build-manifest.json'
    manifest_identity = None
    if build_manifest:
        manifest = json.loads(build_manifest.read_text())
        if manifest['binaries']['msgpack_js_benchmark'] != digest(binary):
            raise RuntimeError('Frozen GN manifest does not match tested binary')
        manifest_identity = {'path': str(build_manifest.resolve()), 'sha256': digest(build_manifest),
                             'gitHead': manifest['gitHead']}
    remote = args.remote_dir
    if args.adb_serial:
        if not remote:
            p.error('--remote-dir required for Android')
        execute(['adb', '-s', args.adb_serial, 'shell', 'mkdir', '-p', str(remote)])
        executable = remote / 'js-binary-benchmark'
        execute(['adb', '-s', args.adb_serial, 'push', str(binary), str(executable)])
        execute(['adb', '-s', args.adb_serial, 'shell', 'chmod', '755', str(executable)])
        actual = execute(['adb', '-s', args.adb_serial, 'shell', 'sha256sum', str(executable)]).split()[0]
        if actual != digest(binary):
            raise RuntimeError('Device binary fingerprint mismatch')
        if not args.runtime_library:
            machine = struct.unpack('<H', binary.read_bytes()[18:20])[0]
            arch = {183: 'aarch64-linux-android', 40: 'arm-linux-androideabi'}[machine]
            args.runtime_library = Path('/Users/james/software/android/sdk/ndk/27.3.13750724/'
                'toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib') / arch / 'libc++_shared.so'
        library = args.runtime_library.resolve()
        execute(['adb', '-s', args.adb_serial, 'push', str(library), str(remote / library.name)])
        actual_library = execute(['adb', '-s', args.adb_serial, 'shell', 'sha256sum',
                                  str(remote / library.name)]).split()[0]
        if actual_library != digest(library):
            raise RuntimeError('Device runtime library fingerprint mismatch')
    else:
        executable = binary
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'binary': str(binary), 'binarySha256': digest(binary),
              'frozenBuildManifest': manifest_identity,
              'runnerSha256': digest(__file__),
              'deviceGuardSha256': digest(Path(__file__).parent / 'device_guard.py'),
              'benchmarkSourceSha256': digest(ROOT / 'tools/binary_serialization/js_benchmark.cc'),
              'serial': args.adb_serial, 'cpuMask': 'f0' if args.adb_serial else None,
              'rounds': args.rounds, 'seed': args.seed, 'warmups': args.warmups,
              'targetMs': args.target_ms, 'minIterations': args.min_iterations,
              'firstApiCall': args.cold,
              'methodology': 'Actual MSGPACK JS builtins versus JSON plus native UTF-8 '
                             'helpers returning owned Uint8Array buffers. JSON string-only '
                             'results are separate. Fresh processes, shared iteration counts '
                             'per operation across codecs, shuffled rounds, requested loop warmups '
                             'and full GC before timing; timed GC included. Each process '
                             'validates complete values before measuring. Preparation, I/O '
                             'and startup excluded. No-loop-warmup is not a first-API cold test.',
              'sources': {str(path.relative_to(ROOT)): digest(path) for path in
                          [ROOT / 'src/msgpack/messagepack.cc', ROOT / 'src/msgpack/messagepack.h',
                           ROOT / 'src/msgpack/messagepack-string.h',
                           ROOT / 'src/msgpack/messagepack-resource-format.h',
                           ROOT / 'src/msgpack/messagepack-resource-encoder.h',
                           ROOT / 'src/msgpack/messagepack-resource-decoder.h',
                           ROOT / 'src/msgpack/messagepack-decoder.h', ROOT / 'src/builtins/builtins-msgpack.cc',
                           ROOT / 'src/builtins/builtins-definitions.h', ROOT / 'src/init/bootstrapper.cc',
                           ROOT / 'src/api/api.cc', ROOT / 'BUILD.gn']},
              'workloads': []}
    if manifest_identity:
        # These hashes identify the actual frozen build rather than later
        # workspace edits made while a long measurement is running.
        report['sources'] = {path: sha for path, sha in manifest['compileInputs'].items()
                             if path.startswith(str(ROOT)) and '/out/' not in path}
    if args.cold:
        report['warmups'] = 0
        report['methodology'] = ('First API call in a fresh process and realm, one timed call, '
            'zero warmups. Decode consumes bytes prepared in separate processes; no decoded '
            'graph or MessagePack cache exists before timing. Encode starts from JSON.parse '
            'data. Complete value validation follows timing. Startup and preparation excluded.')
    if args.adb_serial:
        report['deviceProperties'] = execute(['adb', '-s', args.adb_serial, 'shell', 'getprop'])
        report['runtimeLibrary'] = {'path': str(library), 'sha256': digest(library)}
    workloads = corpus()
    workloads['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    for name, text in [('cjk_unique', '中文序列化配置性能测试'),
                       ('greek_unique', 'Ελληνικάκείμενα'), ('emoji_unique', '🌏😀🚀🧪')]:
        workloads[name] = [{'id': i, 'body': f'{i}:' + text * 40} for i in range(2500)]
    if args.workloads:
        workloads = {name: workloads[name] for name in args.workloads.split(',')}
    tasks = [(op, codec) for op in ['decode', 'encode']
             for codec in ['msgpack', 'json_bytes', 'json_string']]
    if args.include_resource:
        tasks.append(('decode', 'resource'))
        if args.cold: tasks.append(('encode', 'resource'))
    rng = random.Random(args.seed)
    guard = DeviceGuard(args.adb_serial, remote, args.output / 'excluded-device-overlaps.json') if args.adb_serial else None
    def invoke(command):
        if args.adb_serial:
            command = ['adb', '-s', args.adb_serial, 'shell',
                       shlex.join(['env', 'LD_LIBRARY_PATH=' + str(remote), 'taskset', 'f0', *command])]
        return guard.run(command) if guard else execute(command)
    for name, value in workloads.items():
        source = args.output / (name + '.source.json')
        source.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
        if args.adb_serial:
            execute(['adb', '-s', args.adb_serial, 'push', str(source), str(remote / source.name)])
        prepared = {}
        if args.cold:
            for codec in ['msgpack', 'json_bytes', 'json_string'] + (['resource'] if args.include_resource else []):
                path = args.output / (name + '.' + codec)
                source_path = remote / source.name if args.adb_serial else source
                wire_path = remote / path.name if args.adb_serial else path
                invoke([str(executable), '--prepare', codec, str(source_path), str(wire_path)])
                if args.adb_serial:
                    execute(['adb', '-s', args.adb_serial, 'pull', str(wire_path), str(path)])
                prepared[codec] = {'sha256': digest(path), 'bytes': path.stat().st_size,
                    'gzipBytes': len(gzip.compress(path.read_bytes(), compresslevel=6, mtime=0))}
        def measure(op, codec, count):
            path = remote / source.name if args.adb_serial else source
            command = [str(executable), op, codec, str(path), str(count), str(0 if args.cold else args.warmups)]
            if args.cold:
                wire = (remote if args.adb_serial else args.output) / (name + '.' + codec)
                command += ['--cold', str(wire)]
            return json.loads(invoke(command).strip().splitlines()[-1])
        counts = {}
        if args.cold:
            counts = {'encode': 1, 'decode': 1}
        else:
            for op, codec in tasks:
                sample = measure(op, codec, 3)
                count = max(args.min_iterations, min(1000, int(args.target_ms / max(sample['millisecondsPerOperation'], .00001))))
                counts[op] = max(counts.get(op, 0), count)
        samples = {op + ':' + codec: [] for op, codec in tasks}
        orders = []
        for _ in range(args.rounds):
            order = tasks.copy(); rng.shuffle(order); orders.append(order)
            for op, codec in order:
                samples[op + ':' + codec].append(measure(op, codec, counts[op]))
        medians = {key: statistics.median(s['millisecondsPerOperation'] for s in values)
                   for key, values in samples.items()}
        item = {'name': name, 'sourceSha256': digest(source), 'iterations': counts,
                'preparedInputs': prepared,
                'samples': samples, 'orders': orders, 'medians': medians,
                'speedupVersusJsonBytes': {op: medians[op + ':json_bytes'] / medians[op + ':msgpack']
                                         for op in ['decode', 'encode']},
                'speedupVersusJsonString': {op: medians[op + ':json_string'] / medians[op + ':msgpack']
                                          for op in ['decode', 'encode']}}
        report['workloads'].append(item)
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'workload': name, 'medians': medians,
                          'versusJsonBytes': item['speedupVersusJsonBytes']}), flush=True)
    report['completedUtc'] = datetime.now(timezone.utc).isoformat()
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
