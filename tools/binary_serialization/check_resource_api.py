#!/usr/bin/env python3
"""Run standard/resource API suites with normal and moving/incremental GC."""
import argparse
from datetime import datetime,timezone
import json
import fcntl
from pathlib import Path
import shlex
from run_js import digest,execute

ROOT=Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--adb-serial')
    p.add_argument('--remote-dir',type=Path);p.add_argument('--runtime-library',type=Path)
    p.add_argument('--verify-heap',action='store_true');a=p.parse_args()
    binary=a.binary.resolve();manifest=binary.parent/'build-manifest.json'
    if manifest.is_file() and json.loads(manifest.read_text())['binaries']['d8']!=digest(binary):raise RuntimeError('Frozen d8 identity')
    files=['test/intl/assert.js','test/mjsunit/msgpack.js','test/mjsunit/msgpack-resource.js']
    report={'startedUtc':datetime.now(timezone.utc).isoformat(),'binarySha256':digest(binary),
            'runnerSha256':digest(__file__),'sources':{f:digest(ROOT/f) for f in files},'tests':[]}
    executable=str(binary)
    if a.adb_serial:
        device_lock=open('/tmp/v8-msgpack-async-device.lock','a')
        fcntl.flock(device_lock.fileno(),fcntl.LOCK_EX)
        if not a.remote_dir or not a.runtime_library:p.error('Android requires remote-dir and runtime-library')
        def adb(*cmd):return execute(['adb','-s',a.adb_serial,*cmd])
        adb('shell','mkdir','-p',str(a.remote_dir))
        for path,name in [(binary,'d8'),(a.runtime_library,'libc++_shared.so')]+[(ROOT/f,Path(f).name) for f in files]:
            adb('push',str(path),str(a.remote_dir/name))
            if adb('shell','sha256sum',str(a.remote_dir/name)).split()[0]!=digest(path):raise RuntimeError('Device identity')
        adb('shell','chmod','755',str(a.remote_dir/'d8'))
        report['serial']=a.adb_serial;report['runtimeSha256']=digest(a.runtime_library)
        executable=str(a.remote_dir/'d8')
    modes=[('normal',[]),('moving',['--stress-compaction','--gc-interval=37']),('incremental',['--stress-incremental-marking'])]
    for name,flags in modes:
        command=[executable,'--allow-natives-syntax','--expose-gc',*flags]
        if a.verify_heap:command+=['--verify-heap']
        command += [str(a.remote_dir/Path(f).name) if a.adb_serial else str(ROOT/f) for f in files]
        if a.adb_serial:command=['adb','-s',a.adb_serial,'shell',shlex.join(['env','LD_LIBRARY_PATH='+str(a.remote_dir),*command])]
        import subprocess
        r=subprocess.run(command,capture_output=True,text=True)
        report['tests'].append({'mode':name,'command':command,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
        print(name,r.returncode,r.stdout[-300:],r.stderr[-500:],flush=True)
        if r.returncode:raise SystemExit(r.returncode)
    report['completedUtc']=datetime.now(timezone.utc).isoformat();a.output.write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
