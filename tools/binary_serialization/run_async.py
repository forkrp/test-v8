#!/usr/bin/env python3
"""Public async APIs: total latency and synchronous submission, retained samples."""
import argparse, fcntl, hashlib, json, platform, random, shlex, statistics, subprocess
from pathlib import Path
from datetime import datetime, timezone
from run import ROOT, corpus

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def execute(args):
    p = subprocess.run(args, text=True, capture_output=True)
    if p.returncode: raise RuntimeError(f'{args}: {p.stdout[-2000:]} {p.stderr[-3000:]}')
    return p.stdout

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--build-manifest', type=Path)
    p.add_argument('--device-lock', type=Path, default=Path('/tmp/v8-msgpack-async-device.lock'))
    p.add_argument('--serial')
    p.add_argument('--native-harness', action='store_true')
    p.add_argument('--remote')
    p.add_argument('--runtime', type=Path)
    p.add_argument('--samples', type=int, default=31)
    p.add_argument('--passes', type=int, default=5)
    p.add_argument('--workloads')
    p.add_argument('--codecs', default='msgpack,msgpack_async,json,json_async,json_bytes,json_async_bytes')
    p.add_argument('--jobs', default='1,4')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    manifest_path = args.build_manifest or binary.parent / 'build-manifest.json'
    manifest_identity = None
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text())
        name = 'msgpack_js_benchmark' if args.native_harness else 'd8'
        if manifest['binaries'][name] != sha(binary):
            raise RuntimeError('Frozen build does not match tested binary')
        manifest_identity = {'path': str(manifest_path.resolve()), 'sha256': sha(manifest_path),
                             'gitHead': manifest['gitHead']}
        compiler_path = binary.parent / 'compiler-identity.json'
        if compiler_path.is_file():
            manifest_identity['compilerIdentity'] = {'path': str(compiler_path.resolve()), 'sha256': sha(compiler_path)}
    elif args.build_manifest:
        p.error('Build manifest does not exist')
    # Snapshot generation and all cooperating timed matrices serialize on the
    # same device lock. Keep this descriptor live for the entire measurement.
    device_lock = None
    if args.serial:
        device_lock = args.device_lock.open('a')
        fcntl.flock(device_lock.fileno(), fcntl.LOCK_EX)
    script = ROOT / 'test/msgpack/async-benchmark.js'
    cases = corpus()
    cases['protocol_fixture'] = json.loads((ROOT / 'include/js_protocol-1.3.json').read_text())
    for name, text in [('cjk_unique', '中文序列化配置性能测试'), ('greek_unique','Ελληνικάκείμενα'), ('emoji_unique','🌏😀🚀🧪')]:
        cases[name] = [{'id':i, 'body':f'{i}:' + text * 40} for i in range(2500)]
    cases['tiny'] = {'id':42, 'name':'small', 'ok':True}
    if args.workloads: cases = {name:cases[name] for name in args.workloads.split(',')}
    inputs = {}
    for name, value in cases.items():
        path = args.output / (name + '.json')
        path.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
        inputs[name] = {'path':str(path.resolve()), 'sha256':sha(path), 'jsonBytes':path.stat().st_size}
    report = {'startedUtc':datetime.now(timezone.utc).isoformat(),
        'hostInfo':{'platform':platform.platform(), 'machine':platform.machine()},
        'binary':str(binary), 'binarySha256':sha(binary), 'scriptSha256':sha(script),
        'runnerSha256':sha(__file__), 'frozenBuildManifest':manifest_identity,
        'invocation':__import__('sys').argv, 'passes':args.passes, 'samplesPerPass':args.samples,
        'inputs':inputs, 'serial':args.serial, 'rows':[],
        'methodology':'Fresh process per workload/operation/codec/jobs/pass. Shuffled order (seed 20261005). '
          '10 warmup batches, full GC before timing, timed GC included. Complete value validation before '
          'timing, exact MessagePack wire parity. Await public async APIs; record batch total and '
          'synchronous submission separately. JSON string controls exclude UTF-8 transport; JSON byte variants include native UTF-8 conversion into/from owned bytes. Tiny calls use 1000 inner batches per sample.'}
    if args.serial:
        if not args.remote or not args.runtime: p.error('Android requires --remote and --runtime')
        adb = ['adb','-s',args.serial]
        execute(adb + ['shell','mkdir','-p',args.remote])
        for source, name in [(binary,'d8'),(script,'benchmark.js'),(args.runtime,'libc++_shared.so')]:
            execute(adb + ['push',str(source),args.remote+'/'+name])
            if execute(adb+['shell','sha256sum',args.remote+'/'+name]).split()[0] != sha(source):
                raise RuntimeError('Device hash mismatch: '+name)
        execute(adb + ['shell','chmod','755',args.remote+'/d8'])
        for name, item in inputs.items():
            execute(adb+['push',item['path'],args.remote+'/'+name+'.json'])
            if execute(adb+['shell','sha256sum',args.remote+'/'+name+'.json']).split()[0] != item['sha256']:
                raise RuntimeError('Device input hash mismatch: '+name)
        report['runtimeSha256'] = sha(args.runtime)
        report['deviceProperties'] = execute(adb + ['shell','getprop'])
        report['cpuMask'] = 'f0'
        report['deviceConditionCommand'] = 'cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor /sys/class/thermal/thermal_zone0/temp'
        report['deviceConditionsBefore'] = execute(adb+['shell',report['deviceConditionCommand']])
    order = [(name, op, codec, int(jobs), pass_) for pass_ in range(args.passes)
        for name in cases for op in ['encode','decode'] for codec in args.codecs.split(',')
        for jobs in args.jobs.split(',')]
    random.Random(20261005).shuffle(order)
    report['order'] = order
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    for index,(name,op,codec,jobs,pass_) in enumerate(order):
        if args.serial:
            command = shlex.join(['taskset','f0',args.remote+'/d8', *(['--async',args.remote+'/benchmark.js'] if args.native_harness else ['--expose-gc',args.remote+'/benchmark.js','--']),args.remote+'/'+name+'.json',op,codec,str(jobs),str(args.samples)])
            invocation = adb+['shell','LD_LIBRARY_PATH='+shlex.quote(args.remote)+' '+command]
            output = execute(invocation)
        else:
            invocation = [str(binary), *(['--async',str(script)] if args.native_harness else ['--expose-gc',str(script),'--']),inputs[name]['path'],
                op,codec,str(jobs),str(args.samples)]
            output = execute(invocation)
        row = json.loads(output.strip().splitlines()[-1]); row.update(workload=name,passIndex=pass_,command=invocation)
        report['rows'].append(row)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({'index':index+1,'count':len(order),'workload':name,'operation':op,'codec':codec,
            'jobs':jobs,'medianMs':statistics.median(row['elapsed']),
            'submissionMs':statistics.median(row['submit'])}),flush=True)
    grouped = {}
    for row in report['rows']:
        key = (row['workload'],row['operation'],row['jobs'])
        grouped.setdefault(key,{}).setdefault(row['codec'],[]).append(row)
    summary = []
    for key, codecs in sorted(grouped.items()):
        item = dict(zip(['workload','operation','jobs'],key)); item['codecs'] = {}
        for codec,rows in codecs.items():
            item['codecs'][codec] = {metric:statistics.median([v for row in rows for v in row[field]])
                for metric,field in [('totalMs','elapsed'),('submissionMs','submit')]}
            item['codecs'][codec]['passMedians'] = [
                {'passIndex': row['passIndex'], 'totalMs': statistics.median(row['elapsed']),
                 'submissionMs': statistics.median(row['submit'])} for row in rows]
        if 'msgpack_async' in codecs:
            m = item['codecs']['msgpack_async']
            if 'json_async' in codecs and m['totalMs'] > 0: item['jsonAsyncSpeedup'] = item['codecs']['json_async']['totalMs']/m['totalMs']
            if 'json_async_bytes' in codecs and m['totalMs'] > 0: item['jsonBytesAsyncSpeedup'] = item['codecs']['json_async_bytes']['totalMs']/m['totalMs']
            if 'msgpack' in codecs and m['totalMs'] > 0 and item['codecs']['msgpack']['submissionMs'] > 0:
                item['syncTotalRatio'] = item['codecs']['msgpack']['totalMs']/m['totalMs']
                item['submissionReduction'] = 1-m['submissionMs']/item['codecs']['msgpack']['submissionMs']
        summary.append(item)
    report['summary'] = summary
    report['finishedUtc'] = datetime.now(timezone.utc).isoformat()
    if args.serial:
        report['deviceConditionsAfter'] = execute(adb+['shell',report['deviceConditionCommand']])
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')

if __name__ == '__main__': main()
