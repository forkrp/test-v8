#!/usr/bin/env python3
"""Deterministic native corpus, correctness preparation, shuffled timing batches."""
import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import json
from pathlib import Path
import platform
import random
import shlex
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
DEVICE=None
REMOTE=None
CPU_MASK=None
def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def invoke(binary, *args):
    if DEVICE:
        translated=[str(REMOTE/a.name) if isinstance(a,Path) else str(a) for a in args]
        command=['taskset',CPU_MASK,str(REMOTE/'native-binary-benchmark'),*translated]
        run=subprocess.run(['adb','-s',DEVICE,'shell',shlex.join(command)],text=True,capture_output=True)
    else:
        run = subprocess.run([str(binary), *map(str,args)], text=True,capture_output=True)
    if run.returncode: raise RuntimeError(f'{args}: {run.stdout[-2000:]} {run.stderr[-3000:]}')
    return json.loads(run.stdout.strip().splitlines()[-1])

def corpus():
    def records(count):
        return [{'id':i,'name':f'item-{i}','enabled':i%2==0,'score':(i%1000)/10,
                 'tags':['native',f'group-{i%8}','enabled'],
                 'position':{'x':i/8,'y':0 if i%97==0 else -(i%97)/4}} for i in range(count)]
    return {'records_250':records(250),'records_2500':records(2500),'records_25000':records(25000),
      'short_decimals':[ ((i%1000)-500)/10 for i in range(120000)],
      'integers':[i%65536 for i in range(120000)],
      'text_heavy':[{'id':i,'body':f'Document {i}. '+'Native platforms load configuration and content as JavaScript objects. '*6} for i in range(2500)],
      'unicode':[{'id':i,'标题':f'内容-{i}','body':'中文文本 🌏 café Ελληνικά '*12} for i in range(2500)],
      'varying_shapes':[{f'key_{i%127}':i, 'label' if i%2 else 'title':f'entry {i}',
          f'extra_{i%31}':{f'nested_{i%7}':i%11},'flag':i%2==0,
          'optional':None if i%3 else [i,i+1]} for i in range(5000)]}

def main():
    global DEVICE,REMOTE,CPU_MASK
    p=argparse.ArgumentParser()
    p.add_argument('--binary',type=Path,default=ROOT/'out/binary-serialization/native-binary-benchmark')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--rounds',type=int,default=7)
    p.add_argument('--target-ms',type=float,default=100)
    p.add_argument('--seed',type=int,default=20261004)
    p.add_argument('--fixture',type=Path,default=ROOT/'include/js_protocol-1.3.json')
    p.add_argument('--c-peer',type=Path)
    p.add_argument('--adb-serial')
    p.add_argument('--remote-dir',type=Path)
    p.add_argument('--cpu-mask',default='f0')
    args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    if args.adb_serial:
        if not args.remote_dir: p.error('--remote-dir is required with --adb-serial')
        DEVICE=args.adb_serial;REMOTE=args.remote_dir;CPU_MASK=args.cpu_mask
        subprocess.run(['adb','-s',DEVICE,'shell','mkdir','-p',str(REMOTE)],check=True,capture_output=True)
        subprocess.run(['adb','-s',DEVICE,'push',str(args.binary),str(REMOTE/'native-binary-benchmark')],check=True,capture_output=True)
        subprocess.run(['adb','-s',DEVICE,'shell','chmod','755',str(REMOTE/'native-binary-benchmark')],check=True,capture_output=True)
    report={'startedUtc':datetime.now(timezone.utc).isoformat(),'platform':platform.platform(),
        'machine':platform.machine(),'binarySha256':digest(args.binary),
        'scriptSha256':digest(__file__),'rounds':args.rounds,'seed':args.seed,
        'methodology':{'input':'Native owned bytes; fully materialized plain JS objects/arrays. No third-party JS codecs.',
          'warmup':'20 untimed operations per fresh worker; full GC before timed loop.',
          'timing':'Per-codec calibration then shuffled fresh-process rounds. GC within timed loop included. Median across rounds.',
          'json':'json includes UTF-8-to-V8-string conversion; json_string measures parsing a retained source string.',
          'encoding':'Produces owned native byte buffers; includes UTF-8 conversion and output ownership copy where applicable.',
          'numberWidth':'msgpack32 emits float32 only if it exactly reproduces the JS Number; otherwise it emits float64. No decimal rounding or lossy precision policy.',
          'memory':'Five decoded graphs retained after timing/GC; heap delta per graph. Peak RSS includes initialization, warmup, timing, and five retained graphs; it is not a one-decode peak measurement.',
          'compression':'gzip level 6 byte counts only; no decompression time measured.',
          'scope':'macOS ARM64 native V8 prototype. Not Android/iOS acceptance. Reuses fingerprinted core build objects.'},
        'selfTest':invoke(args.binary,'--self-test'),'workloads':[],
        'buildManifestSha256':digest(args.binary.parent/'build-manifest.json')}
    report['measurementPlatform']=json.loads((args.binary.parent/'build-manifest.json').read_text()).get('platform','macos')
    if DEVICE:
        report['device']={'serial':DEVICE,'remoteDir':str(REMOTE),'cpuMask':CPU_MASK,
          'properties':subprocess.check_output(['adb','-s',DEVICE,'shell','getprop'],text=True),
          'binarySha256Remote':subprocess.check_output(['adb','-s',DEVICE,'shell','sha256sum',str(REMOTE/'native-binary-benchmark')],text=True).split()[0]}
        if report['device']['binarySha256Remote']!=report['binarySha256']:raise RuntimeError('Device binary hash mismatch')
        report['methodology']['scope']='Physical Android device, pinned to specified CPU mask; native V8 process. Reuses fingerprinted architecture-specific core build objects.'
    rng=random.Random(args.seed)
    workloads=corpus()
    workloads['protocol_fixture']=json.loads(args.fixture.read_text())
    for name,value in workloads.items():
        source=args.output/(name+'.source.json')
        source.write_text(json.dumps(value,ensure_ascii=False,separators=(',',':')))
        if DEVICE:subprocess.run(['adb','-s',DEVICE,'push',str(source),str(REMOTE/source.name)],check=True,capture_output=True)
        prefix=args.output/name
        invoke(args.binary,'--prepare',source,prefix)
        if DEVICE:
            for codec in ['json','msgpack','msgpack32','v8']:
                file=Path(str(prefix)+'.'+codec)
                subprocess.run(['adb','-s',DEVICE,'pull',str(REMOTE/file.name),str(file)],check=True,capture_output=True)
        if args.c_peer:
            interoperability={}
            for codec in ['msgpack','msgpack32']:
                peer=subprocess.run([str(args.c_peer),str(Path(str(prefix)+'.'+codec))],text=True,capture_output=True,check=True)
                interoperability[codec]=json.loads(peer.stdout)
        else:interoperability=None
        sizes={codec:{'bytes':(f:=Path(str(prefix)+'.'+codec)).stat().st_size,
              'gzipBytes':len(gzip.compress(f.read_bytes(),compresslevel=6,mtime=0)),
              'sha256':digest(f)} for codec in ['json','msgpack','msgpack32','v8']}
        tasks=[('decode',c) for c in ['json','json_string','msgpack','msgpack32','msgpack_tree','mpack','v8']]
        tasks += [('encode',c) for c in ['json','msgpack','msgpack32','v8']]
        counts={};samples={f'{op}:{codec}':[] for op,codec in tasks}
        for op,codec in tasks:
            file=Path(str(prefix)+'.'+('json' if op=='encode' or codec=='json_string' else 'msgpack' if codec in ['msgpack_tree','mpack'] else codec))
            result=invoke(args.binary,'--measure' if op=='decode' else '--encode',codec,file,3)
            counts[(op,codec)]=max(3,min(5000,int(args.target_ms/max(result['millisecondsPerOperation'],.00001))))
        orders=[]
        for round_index in range(args.rounds):
            order=tasks.copy();rng.shuffle(order);orders.append(order)
            for op,codec in order:
                file=Path(str(prefix)+'.'+('json' if op=='encode' or codec=='json_string' else 'msgpack' if codec in ['msgpack_tree','mpack'] else codec))
                result=invoke(args.binary,'--measure' if op=='decode' else '--encode',codec,file,counts[(op,codec)])
                samples[f'{op}:{codec}'].append(result)
        medians={key:{metric:statistics.median(s[metric] for s in runs)
            for metric in ['millisecondsPerOperation','cpuSeconds','retainedHeapBytesPerGraph','peakRssBytes']}
            for key,runs in samples.items()}
        telemetry=subprocess.run(['adb','-s',DEVICE,'shell','cat /sys/class/thermal/thermal_zone0/temp; cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq'],text=True,capture_output=True).stdout if DEVICE else None
        item={'name':name,'sourceSha256':digest(source),'sizes':sizes,'medians':medians,
              'deviceTelemetryAfterWorkload':telemetry,
              'cMasterInteroperability':interoperability,
              'counts':{f'{op}:{codec}':n for (op,codec),n in counts.items()},'samples':samples,'roundOrder':orders}
        report['workloads'].append(item)
        (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({'workload':name,'sizes':{c:s['bytes'] for c,s in sizes.items()},
          'decodeMs':{c:medians['decode:'+c]['millisecondsPerOperation'] for c in ['json','msgpack','msgpack32','msgpack_tree','mpack','v8']}}),flush=True)
    report['completedUtc']=datetime.now(timezone.utc).isoformat()
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
