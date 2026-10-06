#!/usr/bin/env python3
"""Audit source, tests and complete device evidence for an async candidate."""
import argparse
import json
import math
from pathlib import Path

from run import ROOT
from summarize_async import matrix, sha, sync_control

WORKLOADS = {'records_250', 'records_2500', 'records_25000', 'short_decimals',
             'integers', 'text_heavy', 'unicode', 'unicode_unique', 'ascii_unique',
             'latin1_unique', 'alternating_roles', 'mixed_late', 'nested_numeric',
             'varying_shapes', 'protocol_fixture', 'cjk_unique', 'greek_unique',
             'emoji_unique', 'tiny'}
MSGPACK_TESTS = ['MessagePackAsyncYieldsAndSurvivesGC',
                 'MessagePackAsyncConcurrentWorkersAndSnapshot',
                 'MessagePackAsyncUnsupportedPlatform', 'MessagePackAsyncTermination',
                 'MessagePackAsyncTasksOutliveIsolate', 'MessagePackAsyncNativeAccounting',
                 'MessagePackAsyncCompleteWireAccounting']
JSON_TESTS = ['JsonParseAsyncConcurrentWorkers', 'JsonParseAsyncTasksOutliveIsolate',
              'JsonStringifyAsyncConcurrentWorkers', 'JsonStringifyAsyncTasksOutliveIsolate']
JSON_SUITES = ['parse-async', 'stringify-async', 'async-json-android-round2']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ownership-review', type=Path)
    parser.add_argument('--performance-review', type=Path)
    args = parser.parse_args()
    checks = []

    def check(name, action):
        try:
            evidence = action()
            checks.append(dict(requirement=name, status='pass', **evidence))
        except (OSError, ValueError, KeyError, AssertionError) as error:
            checks.append(dict(requirement=name, status='incomplete', detail=str(error)))

    def identity(item):
        path = Path(item['path'])
        assert sha(path) == item['sha256'], f'Identity mismatch: {path}'

    def selected(build):
        folder = args.evidence / f'frozen-{build}-{args.label}'
        path = folder / 'build-manifest.json'
        return folder, path, json.loads(path.read_text())

    def selected_benchmark(arch, item):
        folder, manifest_path, manifest = selected(arch)
        assert Path(item['path']).resolve() == (folder / 'msgpack_js_benchmark').resolve()
        assert item['sha256'] == manifest['binaries']['msgpack_js_benchmark']
        assert Path(item['manifest']['path']).resolve() == manifest_path.resolve()
        assert item['manifest']['sha256'] == sha(manifest_path)

    for build in ['host', 'verify', 'arm64', 'arm32']:
        def frozen(build=build):
            folder = args.evidence / f'frozen-{build}-{args.label}'
            path = folder / 'build-manifest.json'
            data = json.loads(path.read_text())
            for name, digest in data['compileInputs'].items():
                assert sha(Path(name)) == digest, f'Compile input changed: {name}'
            for name, digest in data['binaries'].items():
                assert sha(folder / name) == digest, f'Executable changed: {name}'
            for name, digest in data['untrackedSources'].items():
                assert sha(folder / 'source-untracked' / name) == digest
            assert sha(folder / data['sourcePatch']['savedAs']) == data['sourcePatch']['sha256']
            assert sha(folder / 'commands.txt') == data['commandsSha256']
            assert sha(folder / 'compiler-identity.json') == data['compilerIdentitySha256']
            return dict(manifest=str(path), sha256=sha(path), compilerInputs=len(data['compileInputs']))
        check(build + ' linked source and archived identity', frozen)

    for build in ['host', 'verify']:
        def host_tests(build=build):
            folder, _, manifest = selected(build)
            for binary in ['d8', 'cctest']:
                tested = Path(manifest['build']) / binary
                assert sha(tested) == manifest['binaries'][binary], f'Host test executable changed: {tested}'
            for script in ['test/msgpack/async.js', 'test/msgpack/async-numeric.js']:
                assert sha(ROOT / script) == manifest['untrackedSources'][script]
            path = args.evidence / f'validate-{build}-{args.label}-progress.log'
            text = path.read_text()
            done = [line for line in text.splitlines() if line.startswith('DONE ')]
            assert f'FINAL_{build.upper()}_VALIDATION_COMPLETE' in text
            assert all(line.endswith('exit=0') for line in done)
            for mode in ['normal', 'moving', 'incremental']:
                for kind in ['async', 'numeric', 'sync']:
                    assert f'DONE {build}-{args.label}-{kind}-{mode} exit=0' in done
            for name in MSGPACK_TESTS:
                assert f'DONE {build}-{args.label}-native-{name} exit=0' in done
            if build == 'host':
                for name in JSON_TESTS + ['JsonParseAsyncQueuedWorkerOutlivesIsolate',
                                          'JsonStringifyAsyncQueuedWorkerOutlivesIsolate']:
                    assert f'DONE {build}-{args.label}-native-{name} exit=0' in done
                for name in JSON_SUITES:
                    assert f'DONE {build}-{args.label}-json-{name} exit=0' in done
            return dict(log=str(path), sha256=sha(path), completedSteps=len(done))
        check(build + ' GC modes and native lifecycle', host_tests)

    expected_tests = {f'{kind}-{mode}' for kind in ['async', 'numeric', 'sync']
                      for mode in ['normal', 'moving', 'incremental']}
    expected_tests |= {'native-' + name for name in MSGPACK_TESTS + JSON_TESTS}
    expected_tests |= {'json-' + name for name in JSON_SUITES}
    for arch in ['arm64', 'arm32']:
        def device_tests(arch=arch):
            path = args.evidence / f'tests-{arch}-{args.label}.json'
            data = json.loads(path.read_text())
            folder, _, manifest = selected(arch)
            assert data['arch'] == arch and data['label'] == args.label
            assert data['serial'] == '885841c1'
            for binary in ['d8', 'cctest']:
                assert data['files'][str((folder / binary).resolve())] == manifest['binaries'][binary]
            assert len(data['tests']) == len(expected_tests)
            assert {row['name'] for row in data['tests']} == expected_tests
            assert all(row['exit'] == 0 for row in data['tests'])
            for name, digest in data['files'].items():
                assert sha(Path(name)) == digest, f'Tested file changed: {name}'
            return dict(report=str(path), sha256=sha(path), tests=len(data['tests']))
        check(arch + ' API GC numeric lifecycle JSON tests', device_tests)

        def performance(arch=arch):
            path = args.evidence / f'full-{arch}-{args.label}' / 'results.json'
            data = json.loads(path.read_text())
            required = {(w, op, codec, jobs, pass_) for w in WORKLOADS
                        for op in ['encode', 'decode']
                        for codec in ['msgpack', 'msgpack_async', 'json', 'json_async',
                                      'json_bytes', 'json_async_bytes']
                        for jobs in [1, 4] for pass_ in range(5)}
            assert {tuple(row) for row in data['order']} == required
            assert data['passes'] == 5 and data['samplesPerPass'] == 31
            summary = matrix(path)
            identity({'path': data['binary'], 'sha256': data['binarySha256']})
            identity(data['frozenBuildManifest'])
            identity(data['frozenBuildManifest']['compilerIdentity'])
            selected_benchmark(arch, {'path': data['binary'], 'sha256': data['binarySha256'],
                                      'manifest': data['frozenBuildManifest']})
            assert sha(ROOT / 'test/msgpack/async-benchmark.js') == data['scriptSha256']
            assert sha(ROOT / 'tools/binary_serialization/run_async.py') == data['runnerSha256']
            assert data['serial'] == '885841c1' and data['cpuMask'] == 'f0'
            controls_path = args.evidence / f'sync-control-{arch}-{args.label}' / 'results.json'
            control = json.loads(controls_path.read_text())
            runtime = control['identities']['libc++_shared.so']
            identity(runtime)
            assert data['runtimeSha256'] == runtime['sha256']
            return dict(report=str(path), sha256=sha(path), processes=summary['independentProcesses'])
        check(arch + ' complete public API matrix and identities', performance)

        def controls(arch=arch):
            path = args.evidence / f'sync-control-{arch}-{args.label}' / 'results.json'
            data = json.loads(path.read_text())
            required = {(w, op, variant, pass_) for w in WORKLOADS
                        for op in ['encode', 'decode'] for variant in ['baseline', 'candidate']
                        for pass_ in range(5)}
            assert {tuple(row) for row in data['order']} == required
            summary = sync_control(path)
            selected_benchmark(arch, data['identities']['candidate'])
            for item in data['identities'].values():
                identity(item)
                for key in ['manifest', 'compilerIdentity']:
                    if key in item:
                        identity(item[key])
            assert sha(ROOT / 'tools/binary_serialization/run_sync_control.py') == data['runnerSha256']
            assert set(data['identities']) == {'baseline', 'candidate', 'libc++_shared.so'}
            assert data['serial'] == '885841c1' and data['cpuMask'] == 'f0'
            return dict(report=str(path), sha256=sha(path), processes=summary['independentProcesses'])
        check(arch + ' complete synchronous controls and identities', controls)

        def repeat_matrix(tiny=False, arch=arch):
            prefix = 'repeat-tiny-public' if tiny else 'repeat-public'
            path = args.evidence / f'{prefix}-{arch}-{args.label}' / 'results.json'
            data = json.loads(path.read_text())
            full = json.loads((args.evidence / f'full-{arch}-{args.label}' / 'results.json').read_text())
            selection = json.loads((args.evidence / f'repeat-selection-{args.label}.json').read_text())
            workloads = {'tiny'} if tiny else set(selection['architectures'][arch]['publicRepeats'])
            required = {(w, op, codec, jobs, pass_) for w in workloads
                        for op in ['encode', 'decode']
                        for codec in ['msgpack', 'msgpack_async', 'json_async_bytes']
                        for jobs in [1, 4] for pass_ in range(5)}
            assert {tuple(row) for row in data['order']} == required
            assert data['passes'] == 5 and data['samplesPerPass'] == 31
            summary = matrix(path)
            for field in ['binarySha256', 'scriptSha256', 'runnerSha256', 'runtimeSha256',
                          'serial', 'cpuMask', 'frozenBuildManifest']:
                assert data[field] == full[field], f'Repeat identity differs: {field}'
            for name, item in data['inputs'].items():
                assert item['sha256'] == full['inputs'][name]['sha256']
            selected_benchmark(arch, {'path': data['binary'], 'sha256': data['binarySha256'],
                                      'manifest': data['frozenBuildManifest']})
            return dict(report=str(path), sha256=sha(path), processes=summary['independentProcesses'])
        check(arch + ' unfavorable public API repeats', repeat_matrix)
        check(arch + ' tiny public API repeats', lambda arch=arch: repeat_matrix(True, arch))

        def repeat_sync(arch=arch):
            path = args.evidence / f'repeat-sync-{arch}-{args.label}' / 'results.json'
            data = json.loads(path.read_text())
            selection = json.loads((args.evidence / f'repeat-selection-{args.label}.json').read_text())
            workloads = set(selection['architectures'][arch]['syncRepeatWorkloads'])
            required = {(w, op, variant, pass_) for w in workloads for op in ['encode', 'decode']
                        for variant in ['baseline', 'candidate'] for pass_ in range(10)}
            assert {tuple(row) for row in data['order']} == required
            summary = sync_control(path)
            selected_benchmark(arch, data['identities']['candidate'])
            for item in data['identities'].values():
                identity(item)
                for key in ['manifest', 'compilerIdentity']:
                    if key in item:
                        identity(item[key])
            assert data['serial'] == '885841c1' and data['cpuMask'] == 'f0'
            assert sha(ROOT / 'tools/binary_serialization/run_sync_control.py') == data['runnerSha256']
            return dict(report=str(path), sha256=sha(path), processes=summary['independentProcesses'])
        check(arch + ' ten-pass synchronous repeats', repeat_sync)

        def matched(prefix, baseline_label, workloads, codecs, jobs, arch=arch):
            folder = args.evidence / f'{prefix}-{arch}-{baseline_label}-{args.label}'
            path = folder / 'results.json'
            data = json.loads(path.read_text())
            assert data['complete'] and data['finishedUtc']
            assert data['passes'] == 5 and data['samplesPerPass'] == 31
            required = {(w, op, codec, count, pass_, variant) for w in workloads
                        for op in ['encode', 'decode'] for codec in codecs for count in jobs
                        for pass_ in range(5) for variant in ['baseline', 'candidate']}
            assert {tuple(row) for row in data['order']} == required
            actual = [(r['workload'], r['operation'], r['codec'], r['jobs'], r['passIndex'], r['variant'])
                      for r in data['rows']]
            assert actual == [tuple(row) for row in data['order']]
            assert len(set(actual)) == len(actual)
            selected_benchmark(arch, data['binaries']['candidate'])
            baseline_folder = args.evidence / f'frozen-{arch}-{baseline_label}'
            assert Path(data['binaries']['baseline']['path']).resolve() == (baseline_folder / 'msgpack_js_benchmark').resolve()
            for item in data['binaries'].values():
                identity(item)
                identity(item['manifest'])
                identity(item['compilerIdentity'])
            assert sha(folder / data['runnerSource']) == data['runnerSha256']
            assert sha(folder / data['benchmarkSource']) == data['scriptSha256']
            assert sha(ROOT / 'test/msgpack/async-benchmark.js') == data['scriptSha256']
            full = json.loads((args.evidence / f'full-{arch}-{args.label}' / 'results.json').read_text())
            assert data['runtimeSha256'] == full['runtimeSha256']
            assert data['serial'] == '885841c1' and data['cpuMask'] == 'f0'
            sizes, decoded = {}, {}
            for name, item in data['inputs'].items():
                identity(item)
                assert item['sha256'] == full['inputs'][name]['sha256']
                decoded[name] = len(json.loads(Path(item['path']).read_text()))
            for row in data['rows']:
                for field in ['elapsed', 'submit']:
                    assert len(row[field]) == 31
                    assert all(math.isfinite(value) and value >= 0 for value in row[field])
                inner = 1000 if row['workload'] == 'tiny' else 1
                assert row['innerIterations'] == inner
                wire = row['msgpackBytes'], row['jsonCodeUnits']
                assert all(type(size) is int and size > 0 for size in wire)
                assert sizes.setdefault(row['workload'], wire) == wire
                batches = 41 * inner * row['jobs']
                result_size = decoded[row['workload']] if row['operation'] == 'decode' else row['msgpackBytes']
                assert row['sink'] == result_size * batches
            return dict(report=str(path), sha256=sha(path), processes=len(actual))
        check(arch + ' matched record repeat', lambda arch=arch: matched(
            'repeat-record', 'r8', {'records_25000'}, {'msgpack', 'msgpack_async'}, [1, 4], arch))
        check(arch + ' matched encoder repeat', lambda arch=arch: matched(
            'repeat-encoder', 'r10', {'records_25000', 'ascii_unique', 'cjk_unique', 'short_decimals'},
            {'msgpack_async'}, [1, 4], arch))
        check(arch + ' matched tiny repeat', lambda arch=arch: matched(
            'repeat-tiny-matched', 'r8', {'tiny'}, {'msgpack_async'}, [4], arch))

    def manual_review(path, expected_kind):
        data = json.loads(path.read_text())
        assert data['label'] == args.label and data['kind'] == expected_kind
        assert data['status'] == 'pass' and data['findings']
        assert all(item['status'] == 'pass' and item['reason'] for item in data['findings'])
        assert data['sources'] and data['evidence']
        for name, digest in data['sources'].items():
            assert sha(ROOT / name) == digest, f'Reviewed source changed: {name}'
        for item in data['evidence']:
            identity(item)
        return dict(review=str(path), sha256=sha(path), findings=len(data['findings']))

    for kind, path in [('ownership', args.ownership_review),
                       ('performance exceptions and repeats', args.performance_review)]:
        if path:
            check(kind + ' manual review', lambda path=path, kind=kind: manual_review(path, kind))
        else:
            checks.append(dict(requirement=kind + ' manual review', status='pending',
                               detail='A source-matched manual review is required; complete collections alone do not prove acceptance.'))
    report = dict(label=args.label, auditorSha256=sha(__file__), checks=checks,
                  remaining=[item['requirement'] for item in checks if item['status'] != 'pass'])
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'remaining': report['remaining']}))


if __name__ == '__main__':
    main()
