#!/usr/bin/env python3
"""Pair frozen and optimized native decoders on identical inputs and V8 cores."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import random
import shlex
import statistics
import subprocess

from run import ROOT, corpus


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def execute(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'{command}: {result.stdout[-2000:]} {result.stderr[-3000:]}')
    return result.stdout


class Worker:
    def __init__(self, binary, label, serial, remote):
        self.binary = binary.resolve()
        self.label = label
        self.serial = serial
        self.remote = remote
        self.executable = remote / ('native-' + label) if serial else self.binary
        self.manifest_path = self.binary.parent / 'build-manifest.json'
        self.manifest = json.loads(self.manifest_path.read_text())
        if digest(self.binary) != self.manifest['binarySha256']:
            raise RuntimeError('Local binary fingerprint mismatch')
        if serial:
            execute(['adb', '-s', serial, 'push', str(self.binary), str(self.executable)])
            execute(['adb', '-s', serial, 'shell', 'chmod', '755', str(self.executable)])
            remote_hash = execute(['adb', '-s', serial, 'shell', 'sha256sum',
                                   str(self.executable)]).split()[0]
            if remote_hash != digest(self.binary):
                raise RuntimeError('Device binary fingerprint mismatch')

    def invoke(self, *args):
        if self.serial:
            args = [str(self.remote / arg.name) if isinstance(arg, Path) else str(arg)
                    for arg in args]
            command = ['adb', '-s', self.serial, 'shell',
                       shlex.join(['taskset', 'f0', str(self.executable), *args])]
        else:
            command = [str(self.executable), *map(str, args)]
        return json.loads(execute(command).strip().splitlines()[-1])

    def metadata(self):
        return {'binary': str(self.binary), 'binarySha256': digest(self.binary),
                'buildManifest': str(self.manifest_path),
                'buildManifestSha256': digest(self.manifest_path),
                'sourceSha256': self.manifest['sources'],
                'selfTest': self.invoke('--self-test')}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-commit', default='fe97eae01')
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--seed', type=int, default=20261005)
    parser.add_argument('--target-ms', type=float, default=120)
    parser.add_argument('--workloads', help='Comma-separated subset for pilots')
    parser.add_argument('--c-peer', type=Path, required=True)
    parser.add_argument('--adb-serial')
    parser.add_argument('--remote-dir', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    if args.adb_serial and not args.remote_dir:
        parser.error('--remote-dir is required with --adb-serial')
    if args.adb_serial:
        execute(['adb', '-s', args.adb_serial, 'shell', 'mkdir', '-p', str(args.remote_dir)])
    workers = {label: Worker(binary, label, args.adb_serial, args.remote_dir)
               for label, binary in [('before', args.before), ('after', args.after)]}
    old, new = [workers[label].manifest for label in ['before', 'after']]
    if old['platform'] != new['platform'] or old['v8Build'] != new['v8Build']:
        raise RuntimeError('Targets or V8 core builds differ')
    core = lambda m: {p: h for p, h in m['reusedLinkInputs'].items()
                      if Path(p).is_relative_to(Path(m['v8Build']))}
    if core(old) != core(new):
        raise RuntimeError('Reused V8 core object fingerprints differ')
    for source, expected in old['sources'].items():
        blob = subprocess.check_output(['git', 'show', args.baseline_commit + ':' + source], cwd=ROOT)
        if hashlib.sha256(blob).hexdigest() != expected:
            raise RuntimeError('Frozen baseline source does not match selected commit')
    for source, expected in new['sources'].items():
        if digest(ROOT / source) != expected:
            raise RuntimeError('Optimized source fingerprint mismatch')
    if digest(ROOT / 'src/objects/js-data-object-builder.h') != new['builderSha256']:
        raise RuntimeError('Builder fingerprint mismatch')
    if old['builderSha256'] != new['builderSha256']:
        raise RuntimeError('JSON object builder changed between baseline and optimization')
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'platform': new['platform'], 'serial': args.adb_serial, 'cpuMask': 'f0',
              'seed': args.seed, 'rounds': args.rounds, 'baselineCommit': args.baseline_commit,
              'scriptSha256': digest(__file__), 'corpusScriptSha256': digest(ROOT / 'tools/binary_serialization/run.py'),
              'methodology': 'Identical payloads, paired shuffled fresh-process workers. '
                             '20 warmups, GC before timed loop, GC within timing included. '
                             'JSON includes UTF-8 conversion. Retained heap is five-graph delta/5; '
                             'peak RSS includes bootstrap, warmup, timing and retained graphs.',
              'workers': {label: worker.metadata() for label, worker in workers.items()},
              'validation': {'sameCoreObjects': True, 'sameBuilder': True}, 'workloads': []}
    workloads = corpus()
    workloads['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    if args.workloads:
        selected = args.workloads.split(',')
        workloads = {name: workloads[name] for name in selected}
    rng = random.Random(args.seed)
    tasks = [(label, codec) for label in workers for codec in ['json', 'msgpack', 'msgpack32']]
    for name, value in workloads.items():
        source = args.output / (name + '.source.json')
        source.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
        if args.adb_serial:
            execute(['adb', '-s', args.adb_serial, 'push', str(source), str(args.remote_dir / source.name)])
        sizes = {}
        for label, worker in workers.items():
            prefix = args.output / (name + '.' + label)
            worker.invoke('--prepare', source, prefix)
            for codec in ['json', 'msgpack', 'msgpack32', 'v8']:
                payload = Path(str(prefix) + '.' + codec)
                if args.adb_serial:
                    execute(['adb', '-s', args.adb_serial, 'pull',
                             str(args.remote_dir / payload.name), str(payload)])
                if label == 'before':
                    sizes[codec] = {'bytes': payload.stat().st_size, 'sha256': digest(payload)}
                elif sizes[codec]['sha256'] != digest(payload):
                    raise RuntimeError('Wire bytes changed after decoder optimization: ' + name)
        peers = {codec: json.loads(execute([str(args.c_peer),
                         str(args.output / (name + '.after.' + codec))]))
                 for codec in ['msgpack', 'msgpack32']}
        if not all(peer['cMasterWireRoundTrip'] == 'PASS' for peer in peers.values()):
            raise RuntimeError('Independent C wire check failed')
        counts = {}
        samples = {label + ':' + codec: [] for label, codec in tasks}
        for label, codec in tasks:
            payload = args.output / (name + '.' + label + '.' + codec)
            result = workers[label].invoke('--measure', codec, payload, 3)
            counts[(label, codec)] = max(3, min(5000, int(args.target_ms / max(result['millisecondsPerOperation'], .00001))))
        orders = []
        for _ in range(args.rounds):
            order = tasks.copy()
            rng.shuffle(order)
            orders.append(order)
            for label, codec in order:
                payload = args.output / (name + '.' + label + '.' + codec)
                samples[label + ':' + codec].append(
                    workers[label].invoke('--measure', codec, payload, counts[(label, codec)]))
        medians = {key: {metric: statistics.median(sample[metric] for sample in values)
                        for metric in ['millisecondsPerOperation', 'retainedHeapBytesPerGraph', 'peakRssBytes']}
                   for key, values in samples.items()}
        report['workloads'].append({'name': name, 'sourceSha256': digest(source), 'sizes': sizes,
                                   'interoperability': peers, 'medians': medians, 'samples': samples,
                                   'iterations': {label + ':' + codec: count for (label, codec), count in counts.items()},
                                   'orders': orders})
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'workload': name, 'decodeMs': {k: v['millisecondsPerOperation']
                                                       for k, v in medians.items()}}), flush=True)
    report['completedUtc'] = datetime.now(timezone.utc).isoformat()
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
