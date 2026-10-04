#!/usr/bin/env python3
"""Validate and aggregate two paired decoder batches per Android ABI."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    for arch in ['arm64', 'arm32']:
        for batch in ['a', 'b']:
            parser.add_argument('--' + arch + '-' + batch, type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    summary = {'createdUtc': datetime.now(timezone.utc).isoformat(),
               'reportCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
               'methodology': 'Two independently shuffled five-round paired batches per ABI. '
                              'Shared iteration counts for before/after per codec. Warm native '
                              'byte-input decoding, GC in timed loop included. Fully materialized '
                              'plain JS graphs. Same V8 core objects and unchanged wire bytes.',
               'architectures': {},
               'limits': 'One physical OnePlus 6, Android API35, CPU mask f0. Synthetic corpora '
                         'plus V8 protocol fixture. Retained heap is five-graph incremental delta/5. '
                         'Peak RSS includes initialization, warmup, timing and retained graphs.'}
    cross_arch_sizes = None
    for arch in ['arm64', 'arm32']:
        paths = [getattr(args, arch + '_' + batch) for batch in ['a', 'b']]
        reports = [json.loads(path.read_text()) for path in paths]
        require(all('completedUtc' in r and r['rounds'] == 5 and len(r['workloads']) == 15
                    for r in reports), 'Incomplete batch')
        require(reports[0]['seed'] != reports[1]['seed'], 'Repeat must shuffle independently')
        require(all(r['platform'] == 'android-' + arch and r['serial'] == '885841c1'
                    and r['cpuMask'] == 'f0' for r in reports), 'Wrong device or target')
        require(reports[0]['scriptSha256'] == reports[1]['scriptSha256'] ==
                digest(ROOT / 'tools/binary_serialization/compare_decode.py'), 'Driver changed')
        require(reports[0]['corpusScriptSha256'] == reports[1]['corpusScriptSha256'] ==
                digest(ROOT / 'tools/binary_serialization/run.py'), 'Corpus changed')
        for label in ['before', 'after']:
            metadata = reports[0]['workers'][label]
            require(metadata == reports[1]['workers'][label], 'Worker changed between batches')
            require(digest(metadata['binary']) == metadata['binarySha256'], 'Binary fingerprint mismatch')
            require(digest(metadata['buildManifest']) == metadata['buildManifestSha256'], 'Manifest changed')
            require(metadata['selfTest']['selfTest'] == 'PASS', 'Self-test failed')
        manifest = json.loads(Path(reports[0]['workers']['after']['buildManifest']).read_text())
        for source, expected in {**manifest['sources'], **manifest['headers']}.items():
            require(digest(ROOT / source) == expected, 'Optimized source changed')
        items = []
        for left, right in zip(reports[0]['workloads'], reports[1]['workloads']):
            require(left['name'] == right['name'] and left['sourceSha256'] == right['sourceSha256']
                    and left['sizes'] == right['sizes'], 'Payload changed between batches')
            for path, item in zip(paths, [left, right]):
                require(all(p['cMasterWireRoundTrip'] == 'PASS' for p in item['interoperability'].values()),
                        'Independent C peer failed')
                for codec, expected in item['sizes'].items():
                    for label in ['before', 'after']:
                        payload = path.parent / (item['name'] + '.' + label + '.' + codec)
                        require(digest(payload) == expected['sha256'], 'Payload fingerprint mismatch')
                for codec in ['json', 'msgpack', 'msgpack32']:
                    require(item['iterations']['before:' + codec] == item['iterations']['after:' + codec],
                            'Unpaired iteration counts')
                require(all(len(s) == 5 for s in item['samples'].values()), 'Missing samples')
            medians = {}
            for key in left['samples']:
                samples = left['samples'][key] + right['samples'][key]
                medians[key] = {metric: statistics.median(s[metric] for s in samples)
                                for metric in ['millisecondsPerOperation', 'retainedHeapBytesPerGraph', 'peakRssBytes']}
            timing = lambda key: medians[key]['millisecondsPerOperation']
            items.append({'name': left['name'], 'sizes': left['sizes'], 'medians': medians,
                          'nativeSpeedup': timing('before:msgpack') / timing('after:msgpack'),
                          'speedupVersusJson': timing('after:json') / timing('after:msgpack'),
                          'jsonControlRatio': timing('before:json') / timing('after:json'),
                          'batchNativeMs': {label: [item['medians'][label + ':msgpack']['millisecondsPerOperation']
                                                    for item in [left, right]] for label in ['before', 'after']}})
        standard_sizes = {item['name']: {c: v for c, v in item['sizes'].items() if c != 'v8'}
                          for item in items}
        if cross_arch_sizes is None:
            cross_arch_sizes = standard_sizes
        else:
            require(cross_arch_sizes == standard_sizes, 'Standard wire differs across ABIs')
        summary['architectures'][arch] = {'workers': reports[0]['workers'], 'nativeCodeCommit': manifest['v8BaseCommit'],
            'sourceSha256': manifest['sources'], 'headerSha256': manifest['headers'],
            'batchFiles': [str(p) for p in paths], 'batchSha256': [digest(p) for p in paths],
            'seeds': [r['seed'] for r in reports], 'samplesPerCodec': 10, 'workloads': items}
    summary['validation'] = {'sameNativeCodeAcrossBatches': True, 'sameCoreAndBuilder': True,
        'sameWireBeforeAfterAndAcrossAbis': True, 'pairedIterationCounts': True,
        'allCorpusRoundTripsAndCPeerChecksPassed': True}
    args.output.write_text(json.dumps(summary, indent=2) + '\n')
    for arch, data in summary['architectures'].items():
        print(arch)
        for w in data['workloads']:
            print(w['name'], 'native speedup', round(w['nativeSpeedup'], 3),
                  'versus JSON', round(w['speedupVersusJson'], 3))


if __name__ == '__main__':
    main()
