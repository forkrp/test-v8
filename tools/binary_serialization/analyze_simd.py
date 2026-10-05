#!/usr/bin/env python3
"""Validate and combine source-matched three-variant Android SIMD batches."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics
import subprocess

from compare_decode import digest
from run import ROOT


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    for arch in ['arm64', 'arm32']:
        for batch in ['a', 'b']:
            p.add_argument('--' + arch + '-' + batch, type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    summary = {'createdUtc': datetime.now(timezone.utc).isoformat(),
               'methodology': 'Two independently shuffled five-round batches per ABI; '
                              'three native variants share counts per operation/codec. '
                              '20 warmups, fresh processes, GC before timing and timed GC included. '
                              'Scalar control disables explicit adapter NEON only; compiler '
                              'vectorization and existing V8 helper SIMD remain enabled.',
               'limits': 'One physical OnePlus 6, CPU mask f0. Synthetic workloads and '
                         'V8 protocol fixture. Warm in-memory operations; file I/O and cold '
                         'startup excluded. Peak RSS includes the whole benchmark process.',
               'architectures': {}}
    standard_sizes = None
    for arch in ['arm64', 'arm32']:
        paths = [getattr(args, arch + '_' + batch) for batch in ['a', 'b']]
        reports = [json.loads(path.read_text()) for path in paths]
        require(all('completedUtc' in r and r['rounds'] == 5 and len(r['workloads']) == 18
                    for r in reports), 'Incomplete batch')
        require(reports[0]['seed'] != reports[1]['seed'], 'Shuffles must differ')
        require(all(r['serial'] == '885841c1' and r['platform'] == 'android-' + arch
                    and r['cpuMask'] == 'f0' for r in reports), 'Wrong target')
        for r in reports:
            require(r['scriptSha256'] == digest(ROOT / 'tools/binary_serialization/compare_simd.py')
                    and r['corpusScriptSha256'] == digest(ROOT / 'tools/binary_serialization/run.py'),
                    'Runner or corpus changed')
            require(digest(r['cPeer']['binary']) == r['cPeer']['sha256'], 'C peer changed')
            require(all(r['validation'].values()), 'Variant validation failed')
        for label in ['before', 'after', 'scalar']:
            meta = reports[0]['workers'][label]
            require(meta == reports[1]['workers'][label], 'Worker changed')
            require(digest(meta['binary']) == meta['binarySha256'] and
                    digest(meta['buildManifest']) == meta['buildManifestSha256'], 'Binary changed')
            require(meta['selfTest']['selfTest'] == 'PASS', 'Self-test failed')
            m = json.loads(Path(meta['buildManifest']).read_text())
            if label != 'before':
                for file, h in {**m['sources'], **m['headers']}.items():
                    require(digest(ROOT / file) == h, 'Source/header changed: ' + file)
        items = []
        for left, right in zip(reports[0]['workloads'], reports[1]['workloads']):
            require(left['name'] == right['name'] and left['sourceSha256'] == right['sourceSha256']
                    and left['sizes'] == right['sizes'], 'Workload changed')
            for path, item in zip(paths, [left, right]):
                require(digest(path.parent / (item['name'] + '.source.json')) == item['sourceSha256'],
                        'Source payload changed')
                require(all(peer['cMasterWireRoundTrip'] == 'PASS'
                            for peer in item['interoperability'].values()), 'C wire check failed')
                for label in ['before', 'after', 'scalar']:
                    for codec, expected in item['sizes'].items():
                        require(digest(path.parent / (item['name'] + '.' + label + '.' + codec))
                                == expected['sha256'], 'Wire payload changed')
                for key, samples in item['samples'].items():
                    require(len(samples) == 5, 'Missing sample')
                    op_codec = ':'.join(key.split(':')[1:])
                    require(all(s['iterations'] == item['iterations'][op_codec] for s in samples),
                            'Unpaired iteration count')
            medians = {key: {metric: statistics.median(s[metric] for s in left['samples'][key] + right['samples'][key])
                             for metric in ['millisecondsPerOperation', 'retainedHeapBytesPerGraph', 'peakRssBytes']}
                       for key in left['samples']}
            gains = {}
            for op in ['decode', 'encode']:
                t = lambda label, codec='msgpack': medians[label + ':' + op + ':' + codec]['millisecondsPerOperation']
                gains[op] = {'overallSpeedup': t('before') / t('after'),
                             'explicitSimdSpeedup': t('scalar') / t('after'),
                             'jsonControlRatio': t('before', 'json') / t('after', 'json')}
            items.append({'name': left['name'], 'sizes': left['sizes'], 'medians': medians, 'gains': gains})
        sizes = {item['name']: {c: v for c, v in item['sizes'].items() if c != 'v8'} for item in items}
        if standard_sizes is None:
            standard_sizes = sizes
        else:
            require(sizes == standard_sizes, 'Wire differs across ABIs')
        summary['architectures'][arch] = {
            'workers': reports[0]['workers'], 'batches': [str(path) for path in paths],
            'batchSha256': [digest(path) for path in paths], 'samplesPerVariant': 10,
            'seeds': [r['seed'] for r in reports], 'workloads': items}
    summary['validation'] = {'sourceAndBinaryFingerprints': True, 'sameCoreAndBuilder': True,
        'sameSimdAndScalarSources': True, 'sharedIterationCounts': True,
        'wireIdenticalBeforeAfterScalarAndAcrossAbis': True, 'corpusAndCPeerPassed': True}
    args.output.write_text(json.dumps(summary, indent=2) + '\n')
    for arch, data in summary['architectures'].items():
        print(arch)
        for w in data['workloads']:
            print(w['name'], json.dumps(w['gains']))


if __name__ == '__main__':
    main()
