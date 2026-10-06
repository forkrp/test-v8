#!/usr/bin/env python3
"""Produce the reviewable per-workload resource report from completed evidence."""
import argparse
import json
from pathlib import Path
import statistics
from run_js import digest
from audit_resource import audit, ROOT


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--label', default='r6')
    p.add_argument('--document', type=Path, required=True)
    p.add_argument('--evidence', type=Path, required=True)
    a = p.parse_args()
    data, identity = {}, {}
    independent_path = a.root / ('summary-' + a.label + '-final.json')
    independent = json.loads(independent_path.read_text())
    if not independent['crossAbiWireIdentity'] or len(independent['independentValidation']) != 54:
        raise RuntimeError('Require independent validation of all 18 workloads on three ABIs')
    if not all(v['valueAndNumericBitsMatch'] for v in independent['independentValidation']):
        raise RuntimeError('Independent value/bit mismatch')
    identity[independent_path.name] = digest(independent_path)
    closure_path = a.root / ('source-closure-audit-' + a.label + '.json')
    closure = json.loads(closure_path.read_text())
    if audit(a.root, a.label) != closure:
        raise RuntimeError('Source closure audit is stale or frozen artifacts changed')
    if set(closure) != {'host', 'host-verify', 'arm64', 'arm32'} or any(v['changedSourceInputs'] for v in closure.values()):
        raise RuntimeError('Require current source-matched binaries on every target')
    identity[closure_path.name] = digest(closure_path)
    def read(relative):
        path = a.root / relative
        value = json.loads(path.read_text())
        if 'completedUtc' not in value:
            raise RuntimeError('Incomplete evidence: ' + relative)
        if value.get('acceptanceExcludedReason'):
            raise RuntimeError('Excluded evidence: ' + relative)
        identity[relative] = digest(path)
        return value
    def check_api_sources(api):
        if any(digest(ROOT / name) != sha for name, sha in api['sources'].items()):
            raise RuntimeError('API tests do not match current test sources')
    for arch in ['host', 'arm64', 'arm32']:
        data['cold-' + arch] = read('cold-' + arch + '-' + a.label + '/results.json')
        api = read('api-' + arch + '-' + a.label + (('-final2' if arch == 'host' else '')) + '.json')
        if any(t['exit'] for t in api['tests']): raise RuntimeError('API test failure')
        check_api_sources(api)
        if api['binarySha256'] != json.loads((a.root / ('frozen-' + arch + '-' + a.label) / 'build-manifest.json').read_text())['binaries']['d8']:
            raise RuntimeError('API binary mismatch')
    verify = read('api-host-verify-' + a.label + '-final2.json')
    if any(t['exit'] for t in verify['tests']): raise RuntimeError('Verifier failure')
    check_api_sources(verify)
    if any('--verify-heap' not in t['command'] for t in verify['tests']):
        raise RuntimeError('Require actual heap verification')
    if verify['binarySha256'] != closure['host-verify']['binaries']['d8']:
        raise RuntimeError('Verifier binary mismatch')
    for arch in ['arm64', 'arm32']:
        data['warm-' + arch] = read('warm-' + arch + '-' + a.label + '/results.json')
        data['resource-controls-' + arch] = read('resource-controls-' + arch + '-' + a.label + '/results.json')
        data['standard-controls-' + arch] = read('standard-controls-' + arch + '-' + a.label + '/results.json')
        data['stages-' + arch] = read('stages-' + arch + '-' + a.label + '/results.json')
        h = data['warm-' + arch]['binarySha256']
        runtime = data['warm-' + arch]['runtimeLibrary']['sha256']
        if data['cold-' + arch]['runtimeLibrary']['sha256'] != runtime:
            raise RuntimeError('Cold/warm runtime mismatch')
        for prefix in ['cold-', 'resource-controls-', 'standard-controls-', 'stages-']:
            if data[prefix + arch]['binarySha256'] != h: raise RuntimeError('Performance binary mismatch')
            if prefix != 'cold-' and data[prefix + arch]['runtimeSha256'] != runtime:
                raise RuntimeError('Control/stage runtime mismatch')
        controls = data['resource-controls-' + arch]
        if controls['rounds'] < 60 or len(controls['cases']) != 36:
            raise RuntimeError('Require all 18 workloads in 60 resource control rounds')
        if data['standard-controls-' + arch]['rounds'] < 60:
            raise RuntimeError('Require 60 standard control rounds')
    names = [w['name'] for w in data['cold-host']['workloads']]
    if len(names) != 18: raise RuntimeError('Expected all 18 workloads')
    by_name = {k: {w['name']: w for w in m['workloads']} for k,m in data.items() if 'workloads' in m}
    gates, regressions = {}, {}
    for arch in ['arm64', 'arm32']:
        extended = data['standard-controls-' + arch]['summary']
        controlled = {}
        for s in extended:
            key = (s['name'], s['operation'])
            controlled[key] = controlled.get(key, True) and s['allMedianGates']
        gates[arch] = all(controlled.get((n, op), by_name['warm-' + arch][n]['speedupVersusJsonBytes'][op] >= 1.5)
                          for n in names for op in ['decode', 'encode'])
        regressions[arch] = data['resource-controls-' + arch]['repeatableRegressions']
    accepted = all(gates.values()) and not any(regressions.values())
    compact_data = {}
    for key,m in data.items():
        if 'workloads' in m:
            compact_data[key] = {field:m[field] for field in ['binarySha256','frozenBuildManifest','rounds','seed','methodology']}
            compact_data[key]['workloads'] = []
            for w in m['workloads']:
                entry = {field:w[field] for field in ['name','sourceSha256','iterations','medians','speedupVersusJsonBytes']}
                if 'preparedInputs' in w: entry['sizes'] = w['preparedInputs']
                if 'encode:resource' in w['samples']:
                    entry['offlineEncode'] = {'cpuSecondsMedian':statistics.median(s['cpuSeconds'] for s in w['samples']['encode:resource']),
                        'peakRssRaw':max(s['peakRssRaw'] for s in w['samples']['encode:resource'])}
                compact_data[key]['workloads'].append(entry)
        else:
            compact_data[key] = {field:value for field,value in m.items() if field not in ['samples','orders']}
    summary = {'evidenceSha256': identity, 'standardGates': gates,
               'resourceRepeatableRegressions': regressions, 'accepted': accepted,
               'sourceClosureAudit': closure, 'independentChecks': independent['independentValidation'],
               'matrices': compact_data}
    a.evidence.parent.mkdir(parents=True, exist_ok=True)
    a.evidence.write_text(json.dumps(summary, indent=2) + '\n')
    lines = ['# MessagePack resource results', '',
             'Candidate ' + a.label + ' on rooted OnePlus 6 (`885841c1`), Android 15, CPU mask `f0`.',
             'Acceptance: **' + ('passed' if accepted else 'open; failed gates below') + '**.',
             'The fixed 18 fixtures establish these corpus results; no production asset corpus was supplied.', '',
             'Every complete resource file is no larger than its standard MessagePack equivalent. '
             'The encoder returns the exact standard bytes when the full compact candidate is not smaller. '
             'Raw size and gzip size are separate; gzip is measured for comparison and is not part of the codec.', '',
             '## Complete file sizes', '',
             '| Workload | Minified JSON | Standard MessagePack | Resource | Raw reduction vs MessagePack | Gzip JSON | Gzip MessagePack | Gzip resource |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
    totals = [0]*6
    for name in names:
        w = by_name['cold-host'][name];s=w['preparedInputs']
        values=[s[c][f] for f in ['bytes','gzipBytes'] for c in ['json_bytes','msgpack','resource']]
        totals=[x+y for x,y in zip(totals,values)]
        lines.append('| '+name+' | '+' | '.join([f'{v:,}' for v in values[:3]])+f' | {100*(1-values[2]/values[1]):.2f}% | '+' | '.join(f'{v:,}' for v in values[3:])+' |')
    lines.append('| **Total** | '+' | '.join(f'{v:,}' for v in totals[:3])+f' | **{100*(1-totals[2]/totals[1]):.2f}%** | '+' | '.join(f'{v:,}' for v in totals[3:])+' |')
    lines += ['', '## Android decoding and standard API gates', '',
              'Five shuffled fresh-process matrix rounds, shared loop counts, 20 warmed calls and a full GC before timing. '
              'Timed GC is included. First-call decoding uses separately prepared bytes and exactly one API call. '
              'Table preparation, UTF-8 validation, object construction and numeric materialization are inside decoding. '
              'Preparation, startup and file I/O are excluded. JSON gates include owned UTF-8 byte transport. '
              'All times are milliseconds per operation. Ratios above 1 favor MessagePack/resource.', '']
    for arch in ['arm64','arm32']:
        lines += ['### '+arch.upper(), '',
                  '| Workload | First standard | First resource | Warm standard | Warm resource | Warm standard/resource | Standard decode/JSON speedup | Standard encode/JSON speedup |',
                  '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |']
        for name in names:
            c=by_name['cold-'+arch][name]['medians'];w=by_name['warm-'+arch][name]
            m=w['medians'];g=w['speedupVersusJsonBytes']
            v=[c['decode:msgpack'],c['decode:resource'],m['decode:msgpack'],m['decode:resource'],m['decode:msgpack']/m['decode:resource'],g['decode'],g['encode']]
            lines.append('| '+name+' | '+' | '.join(f'{x:.3f}' for x in v)+' |')
        controls=data['resource-controls-'+arch];std=data['standard-controls-'+arch]
        lines += ['', '60 shuffled paired control rounds cover every first-call and warmed resource workload, including standard fallbacks. '
                  'Repeatable regressions of at least 5%: `'+str(regressions[arch])+'`. '
                  'The close standard gates use 60 rounds and both half-run medians. Standard 1.5x acceptance: `'+str(gates[arch])+'`.', '',
                  '| Workload | First standard/resource control | Warm standard/resource control | Warm first half | Warm second half |',
                  '| --- | ---: | ---: | ---: | ---: |']
        for name in names:
            lines.append('| '+name+' | '+f"{controls['ratios'][name+':cold']:.3f} | {controls['ratios'][name+':warm']:.3f} | "+' | '.join(f'{x:.3f}' for x in controls['halfRunRatios'][name+':warm'])+' |')
        lines += ['', '| Extended standard gate | Iterations | JSON/standard | First half | Second half | Pass |',
                  '| --- | ---: | ---: | ---: | ---: | --- |']
        for result in std['summary']:
            lines.append('| '+result['name']+' '+result['operation']+' | '+str(result['iterations'])+
                         ' | '+f"{result['ratio']:.3f} | "+
                         ' | '.join(f'{x:.3f}' for x in result['halfRunRatios'])+
                         ' | '+str(result['allMedianGates'])+' |')
        lines.append('')
    lines += ['', '## Offline generation', '',
              'Additional offline encoding work is intentional. Times below are cold encodeResource calls. '
              'RSS is the maximum whole-process high-water mark over the measured fresh processes, including source parsing and result validation; '
              'it is not encoder-exclusive memory. CPU time is also retained in the JSON evidence. '
              'Host measurements did not reserve exclusive host CPU access.', '',
              '| Workload | Host ms | Host peak MiB | ARM64 ms | ARM64 peak MiB | ARM32 ms | ARM32 peak MiB |',
              '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for name in names:
        values=[]
        for arch in ['host','arm64','arm32']:
            w=by_name['cold-'+arch][name];rss=max(s['peakRssRaw'] for s in w['samples']['encode:resource'])
            values += [w['medians']['encode:resource'],rss/(1024**2 if arch=='host' else 1024)]
        lines.append('| '+name+' | '+' | '.join(f'{x:.2f}' for x in values)+' |')
    lines += ['', '## Cumulative feature ablations', '',
              'Independent transformations of final files remove numeric blocks, then dictionary references. '
              'Each stage retains its required full tables and applies file-level standard fallback. '
              'One unchanged native decoder reads the prepared wires; identical wire stages share a measurement. '
              'These are cumulative wire controls, not timings from differently compiled decoders or separate offline encoder builds.', '',
              '| Workload | Standard bytes | Shapes bytes | Shapes + strings bytes | Full bytes | ARM64 warm: shapes / strings / full ms | ARM32 warm: shapes / strings / full ms |',
              '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for name in names:
        stage=data['stages-arm64'];v=[stage['wires'][name+':'+s]['bytes'] for s in ['msgpack','shapes','shapes_strings','resource']]
        times=[' / '.join(f"{data['stages-'+arch]['medians'][name+':'+s+':warm']:.3f}" for s in ['shapes','shapes_strings','resource']) for arch in ['arm64','arm32']]
        lines.append('| '+name+' | '+' | '.join(f'{x:,}' for x in v)+' | '+' | '.join(times)+' |')
    lines += ['', '| Workload | ARM64 first: shapes / strings / full ms | ARM32 first: shapes / strings / full ms |',
              '| --- | ---: | ---: |']
    for name in names:
        times=[' / '.join(f"{data['stages-'+arch]['medians'][name+':'+s+':cold']:.3f}" for s in ['shapes','shapes_strings','resource']) for arch in ['arm64','arm32']]
        lines.append('| '+name+' | '+' | '.join(times)+' |')
    lines += ['', '## Correctness, rejected experiments and identity', '',
              'Standard and resource suites pass on host and both device ABIs under normal, moving and incremental GC. '
              'The host DCHECK build passes with actual heap verification enabled. An independent Python reader checks '
              'all 18 workloads for equivalent values and numeric bits and verifies cross-ABI wire identity. '
              'Coverage includes numeric boundaries, signed zero/nonfinite values, Unicode, table/shape collisions, '
              'indices, malformed/truncated blocks, unsupported versions, 256-container nesting, buffer views, '
              'ownership, realm prototypes, representation generalization, and 2,000 seeded malformed resources.', '',
              'r1-r4 were rejected because the existing standard Unicode encoding gates fell below 1.5x. '
              'Separate resource packer instantiations restored standard inlining, but separate builtin entry points '
              'and 64-byte function alignment did not clear the gates. Paired frozen-baseline controls and simpleperf '
              'profiles retain the slowdown. r5 removes the second scalar pass for bounded mixed UTF-16 on ARM64 '
              'without changing standard wire bytes or Unicode validation. '
              'r5 was superseded because it excluded exact float32 nonfinite blocks; r6 applies the bit-equality '
              'selection to those values too. The original 18 corpus files remain byte-identical. '
              'Failed build/test invocations and all '
              'rejected frozen binaries, source snapshots and raw samples remain under `out/msgpack-resources`.', '',
              '| Artifact | SHA-256 |', '| --- | --- |']
    for arch in ['host','host-verify','arm64','arm32']:
        folder=a.root/('frozen-'+arch+'-'+a.label);m=json.loads((folder/'build-manifest.json').read_text())
        for name,sha in m['binaries'].items():lines.append('| '+folder.name+'/'+name+' | `'+sha+'` |')
        lines.append('| '+folder.name+'/build-manifest.json | `'+digest(folder/'build-manifest.json')+'` |')
    lines += ['', 'Full configuration, compile-input closures, runtime/runner/fixture hashes, sample ordering, '
              'CPU/RSS/heap data and control half-runs are retained in the frozen manifests and '
              '[machine-readable evidence](../'+str(a.evidence)+'). '
              'The codec uses no external compression and no lossy numeric conversion. Compiled engine size is outside acceptance.', '']
    a.document.parent.mkdir(parents=True,exist_ok=True);a.document.write_text('\n'.join(lines))
    print(json.dumps({'accepted':accepted,'standardGates':gates,'repeatableRegressions':regressions}))


if __name__ == '__main__': main()
