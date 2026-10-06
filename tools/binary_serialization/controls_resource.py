#!/usr/bin/env python3
"""Paired resource/standard first-load and warmed Android decode controls."""
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
    p=argparse.ArgumentParser()
    for name in ['binary','warm-matrix','cold-matrix','output','runtime-library']:
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--adb-serial',required=True)
    p.add_argument('--remote-dir',type=Path,required=True)
    p.add_argument('--rounds',type=int,default=60)
    p.add_argument('--workloads',help='Comma-separated subset; default all 18 workloads, including fallbacks')
    p.add_argument('--seed',type=int,default=20261005)
    a=p.parse_args()
    if a.rounds<60:p.error('At least 60 control rounds are required')
    warm=json.loads(a.warm_matrix.read_text());cold=json.loads(a.cold_matrix.read_text())
    binary=a.binary.resolve(); bh=digest(binary)
    if warm['binarySha256']!=bh or cold['binarySha256']!=bh:raise RuntimeError('Matrices use another binary')
    manifest=binary.parent/'build-manifest.json'
    if json.loads(manifest.read_text())['binaries']['msgpack_js_benchmark']!=bh:raise RuntimeError('Frozen identity')
    a.output.mkdir(parents=True,exist_ok=False);remote=a.remote_dir
    def adb(*cmd):return execute(['adb','-s',a.adb_serial,*cmd])
    def push(path,name):
        adb('push',str(path),str(remote/name))
        if adb('shell','sha256sum',str(remote/name)).split()[0]!=digest(path):raise RuntimeError('Device identity: '+name)
    adb('shell','mkdir','-p',str(remote));push(binary,'js-binary-benchmark');push(a.runtime_library,'libc++_shared.so')
    adb('shell','chmod','755',str(remote/'js-binary-benchmark'))
    selected=set(a.workloads.split(',')) if a.workloads else None
    cold_by_name={w['name']:w for w in cold['workloads']};cases=[];sources={}
    for w in warm['workloads']:
        name=w['name'];c=cold_by_name[name]
        if selected is not None and name not in selected:continue
        source=a.warm_matrix.parent/(name+'.source.json')
        if digest(source)!=w['sourceSha256']:raise RuntimeError('Changed source')
        push(source,source.name);sources[name]=digest(source)
        for codec in ['msgpack','resource']:
            path=a.cold_matrix.parent/(name+'.'+codec)
            if digest(path)!=c['preparedInputs'][codec]['sha256']:raise RuntimeError('Changed payload')
            push(path,path.name)
        cases += [{'id':name+':warm','name':name,'count':w['iterations']['decode'],'cold':False},
                  {'id':name+':cold','name':name,'count':1,'cold':True}]
    report={'startedUtc':datetime.now(timezone.utc).isoformat(),'binarySha256':bh,
            'manifestSha256':digest(manifest),'warmMatrixSha256':digest(a.warm_matrix),
            'coldMatrixSha256':digest(a.cold_matrix),'runtimeSha256':digest(a.runtime_library),
            'runnerSha256':digest(__file__), 'deviceGuardSha256': digest(Path(__file__).parent / 'device_guard.py'),'serial':a.adb_serial,'cpuMask':'f0','rounds':a.rounds,
            'seed':a.seed,'cases':cases,'sources':sources,'samples':{},'orders':[],
            'methodology':'Shuffled fresh-process paired standard/resource decoding. Alternating codec order; matrix loop counts shared. Cold calls use independently prepared files, no prior codec call. Warm calls include table preparation each time, 20 warmups, full GC before clock; timed GC included.'}
    rng=random.Random(a.seed)
    guard=DeviceGuard(a.adb_serial,remote,a.output/'excluded-device-overlaps.json')
    for round_index in range(a.rounds):
        order=cases.copy();rng.shuffle(order);executed=[]
        for pos,case in enumerate(order):
            codecs=['msgpack','resource']
            if (round_index+pos)%2:codecs.reverse()
            for codec in codecs:
                args=[str(remote/'js-binary-benchmark'),'decode',codec,str(remote/(case['name']+'.source.json')),str(case['count']),'0' if case['cold'] else '20']
                args+=['--cold' if case['cold'] else '--input',str(remote/(case['name']+'.'+codec))]
                output=guard.run(['adb','-s',a.adb_serial,'shell',shlex.join(['env','LD_LIBRARY_PATH='+str(remote),'taskset','f0',*args])])
                key=case['id']+':'+codec
                report['samples'].setdefault(key,[]).append(json.loads(output.strip().splitlines()[-1]));executed.append(key)
        report['orders'].append(executed)
        (a.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print('completed resource round',round_index+1,'/',a.rounds,flush=True)
    report['medians']={k:statistics.median(s['millisecondsPerOperation'] for s in v) for k,v in report['samples'].items()}
    report['ratios']={c['id']:report['medians'][c['id']+':msgpack']/report['medians'][c['id']+':resource'] for c in cases}
    report['halfRunRatios']={c['id']:[
        statistics.median(s['millisecondsPerOperation'] for s in report['samples'][c['id']+':msgpack'][lo:hi]) /
        statistics.median(s['millisecondsPerOperation'] for s in report['samples'][c['id']+':resource'][lo:hi])
        for lo,hi in [(0,a.rounds//2),(a.rounds//2,a.rounds)]] for c in cases}
    report['repeatableRegressions']=[c['id'] for c in cases
        if report['ratios'][c['id']]<=1/1.05 and max(report['halfRunRatios'][c['id']])<=1/1.05]
    report['completedUtc']=datetime.now(timezone.utc).isoformat()
    (a.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['ratios']))

if __name__=='__main__':main()
