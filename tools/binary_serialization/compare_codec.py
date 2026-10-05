#!/usr/bin/env python3
"""Compare frozen and candidate native encode/decode on the fixed corpus."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess

from compare_decode import Worker, digest, execute
from run import ROOT, corpus


def main():
    p = argparse.ArgumentParser()
    for label in ['before', 'after']:
        p.add_argument('--' + label, type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--baseline-commit', default='d9c5ab6a9')
    p.add_argument('--rounds', type=int, default=5)
    p.add_argument('--seed', type=int, default=20261005)
    p.add_argument('--target-ms', type=float, default=100)
    p.add_argument('--workloads')
    p.add_argument('--c-peer', type=Path, required=True)
    p.add_argument('--adb-serial', required=True)
    p.add_argument('--remote-dir', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    execute(['adb', '-s', args.adb_serial, 'shell', 'mkdir', '-p', str(args.remote_dir)])
    workers = {label: Worker(getattr(args, label), label, args.adb_serial, args.remote_dir)
               for label in ['before', 'after']}
    manifests = {label: worker.manifest for label, worker in workers.items()}
    old = manifests['before']
    core = lambda m: {path: h for path, h in m['reusedLinkInputs'].items()
                      if Path(path).is_relative_to(Path(m['v8Build']))}
    for label, manifest in manifests.items():
        if (manifest['platform'], manifest['v8Build'], manifest['builderSha256'], core(manifest)) != \
                (old['platform'], old['v8Build'], old['builderSha256'], core(old)):
            raise RuntimeError('V8 core or object builder differs')
        for source, expected in {**manifest['sources'], **manifest['headers']}.items():
            actual = hashlib.sha256(subprocess.check_output(
                ['git', 'show', args.baseline_commit + ':' + source], cwd=ROOT)).hexdigest() \
                if label == 'before' else digest(ROOT / source)
            if actual != expected:
                raise RuntimeError('Source/header mismatch: ' + label + ':' + source)
    report = {'startedUtc': datetime.now(timezone.utc).isoformat(),
              'platform': old['platform'], 'serial': args.adb_serial, 'cpuMask': 'f0',
              'seed': args.seed, 'rounds': args.rounds, 'baselineCommit': args.baseline_commit,
              'scriptSha256': digest(__file__), 'corpusScriptSha256': digest(ROOT / 'tools/binary_serialization/run.py'),
              'cPeer': {'binary': str(args.c_peer.resolve()), 'sha256': digest(args.c_peer)},
              'methodology': 'Two variants, identical wire bytes and core objects; '
                             'fresh processes shuffled per round; 20 warmups; GC before '
                             'timing and timed GC included. Counts shared across all '
                             'variants per operation/codec. Full-graph materialization; '
                             'both encoders return owned buffers.',
              'workers': {label: worker.metadata() for label, worker in workers.items()},
              'validation': {'sameCoreAndBuilder': True, 'sameBuilder': True},
              'workloads': []}
    workloads = corpus()
    workloads['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    # Additional controls exercise homogeneous scripts and surrogate-heavy text.
    for name, text in [('cjk_unique', '中文序列化配置性能测试'),
                       ('greek_unique', 'Ελληνικάκείμενα'),
                       ('emoji_unique', '🌏😀🚀🧪')]:
        workloads[name] = [{'id': i, 'body': f'{i}:' + text * 40} for i in range(2500)]
    if args.workloads:
        workloads = {name: workloads[name] for name in args.workloads.split(',')}
    tasks = [(label, op, codec) for label in workers for op in ['decode', 'encode']
             for codec in ['json', 'msgpack']]
    rng = random.Random(args.seed)
    for name, value in workloads.items():
        source = args.output / (name + '.source.json')
        source.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
        execute(['adb', '-s', args.adb_serial, 'push', str(source), str(args.remote_dir / source.name)])
        sizes = {}
        for label, worker in workers.items():
            prefix = args.output / (name + '.' + label)
            worker.invoke('--prepare', source, prefix)
            for codec in ['json', 'msgpack', 'msgpack32', 'v8']:
                payload = Path(str(prefix) + '.' + codec)
                execute(['adb', '-s', args.adb_serial, 'pull', str(args.remote_dir / payload.name), str(payload)])
                if label == 'before':
                    sizes[codec] = {'bytes': payload.stat().st_size, 'sha256': digest(payload)}
                elif sizes[codec]['sha256'] != digest(payload):
                    raise RuntimeError('Wire bytes changed: ' + name + ':' + codec)
        peers = {codec: json.loads(execute([str(args.c_peer), str(args.output / (name + '.after.' + codec))]))
                 for codec in ['msgpack', 'msgpack32']}
        if not all(v['cMasterWireRoundTrip'] == 'PASS' for v in peers.values()):
            raise RuntimeError('Independent C wire check failed')
        def measure(label, op, codec, count):
            payload = args.output / (name + '.' + label + '.' + ('json' if op == 'encode' else codec))
            return workers[label].invoke('--measure' if op == 'decode' else '--encode', codec, payload, count)
        counts = {}
        for label, op, codec in tasks:
            sample = measure(label, op, codec, 3)
            key = op + ':' + codec
            count = max(3, min(5000, int(args.target_ms / max(sample['millisecondsPerOperation'], .00001))))
            counts[key] = max(counts.get(key, 0), count)
        samples = {':'.join(t): [] for t in tasks}
        orders = []
        for _ in range(args.rounds):
            order = tasks.copy(); rng.shuffle(order); orders.append(order)
            for label, op, codec in order:
                samples[':'.join((label, op, codec))].append(measure(label, op, codec, counts[op + ':' + codec]))
        medians = {key: {metric: statistics.median(s[metric] for s in values)
                        for metric in ['millisecondsPerOperation', 'retainedHeapBytesPerGraph', 'peakRssBytes']}
                   for key, values in samples.items()}
        report['workloads'].append({'name': name, 'sourceSha256': digest(source), 'sizes': sizes,
                                   'interoperability': peers, 'iterations': counts,
                                   'samples': samples, 'medians': medians, 'orders': orders})
        (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'workload': name, 'ms': {k: v['millisecondsPerOperation']
                                                  for k, v in medians.items()}}), flush=True)
    report['completedUtc'] = datetime.now(timezone.utc).isoformat()
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
