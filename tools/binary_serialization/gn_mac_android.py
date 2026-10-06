#!/opt/homebrew/bin/python3
"""GN script adapter for CLT metadata and serialized on-device generators."""
import hashlib
import fcntl
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time

script = sys.argv[1]
if script.endswith('/build/config/apple/sdk_info.py'):
    print('xcode_version="1600"\nxcode_version_int=1600\nxcode_build="CLT"')
elif script.endswith('/build/mac/find_sdk.py'):
    for command in [['xcrun','--show-sdk-path'], None, ['xcrun','--show-sdk-build-version'], ['xcrun','--show-sdk-version']]:
        print('/Library/Developer/CommandLineTools/usr/bin/' if command is None else subprocess.check_output(command,text=True).strip())
elif script.endswith('/tools/run.py') and Path(sys.argv[2]).is_file() and Path(sys.argv[2]).read_bytes()[:4] == b'\x7fELF':
    exe = Path(sys.argv[2]).resolve()
    arch = 'arm32' if 'arm32' in str(Path.cwd()) else 'arm64'
    serial = os.environ.get('MSGPACK_RESOURCE_SERIAL', '885841c1')
    remote = '/data/local/tmp/msgpack-resources-build-' + arch
    def adb(*args): return subprocess.check_output(['adb','-s',serial,*args],text=True)
    device_lock = open('/tmp/v8-msgpack-async-device.lock', 'a')
    fcntl.flock(device_lock.fileno(), fcntl.LOCK_EX)
    # Device generators must not perturb another codec's timed matrix. Wait
    # for a quiet interval, rather than slipping into a gap between samples.
    quiet = 0
    while quiet < 3:
        names = adb('shell', 'ps', '-A', '-o', 'NAME').splitlines()
        busy = any(n in names for n in ['d8', 'cctest', 'mksnapshot', 'torque',
                                       'js-binary-benchmark', 'msgpack_js_benchmark',
                                       'benchmark', 'simpleperf'])
        quiet = 0 if busy else quiet + 1
        if quiet < 3: time.sleep(2)
    adb('shell','mkdir','-p',remote)
    ndk = Path(os.environ.get('MSGPACK_RESOURCE_NDK', '/Users/james/software/android/sdk/ndk/27.3.13750724'))
    triple = 'arm-linux-androideabi' if arch == 'arm32' else 'aarch64-linux-android'
    library = ndk/'toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib'/triple/'libc++_shared.so'
    adb('push',str(library),remote+'/libc++_shared.so')
    adb('push',str(exe),remote+'/'+exe.name)
    adb('shell','chmod','755',remote+'/'+exe.name)
    args = sys.argv[3:]; outputs = []
    if exe.name == 'bytecode_builtins_list_generator':
        local=Path(args[0]).resolve();local.parent.mkdir(parents=True,exist_ok=True)
        args[0]=remote+'/'+local.name;outputs.append((args[0],local))
    elif exe.name == 'mksnapshot':
        args += ['--no-short-builtin-calls']
        for flag in ['--embedded_src','--startup_src','--startup_blob']:
            if flag in args:
                idx=args.index(flag)+1;local=Path(args[idx]).resolve();local.parent.mkdir(parents=True,exist_ok=True)
                args[idx]=remote+'/'+local.name;outputs.append((args[idx],local))
    elif exe.name == 'torque':
        root = Path(args[args.index('-v8-root')+1]).resolve()
        destination = Path(args[args.index('-o')+1]).resolve()
        with tempfile.TemporaryDirectory(prefix='msgpack-torque-') as temp:
            archive=Path(temp)/'source.tar'
            with tarfile.open(archive,'w') as tar:
                for name in args:
                    if name.endswith('.tq'): tar.add(root/name,arcname=name)
            adb('push',str(archive),remote+'/source.tar')
            adb('shell',shlex.join(['mkdir','-p',remote+'/source',remote+'/generated/src/builtins',remote+'/generated/src/objects',remote+'/generated/test/torque',remote+'/generated/third_party/v8/builtins']))
            adb('shell',shlex.join(['tar','xf',remote+'/source.tar','-C',remote+'/source']))
            args[args.index('-v8-root')+1]=remote+'/source'
            args[args.index('-o')+1]=remote+'/generated'
            adb('shell',shlex.join(['env','LD_LIBRARY_PATH='+remote,remote+'/'+exe.name,*args]))
            adb('shell',shlex.join(['tar','cf',remote+'/generated.tar','-C',remote+'/generated','.']))
            archive=Path(temp)/'generated.tar';adb('pull',remote+'/generated.tar',str(archive))
            destination.mkdir(parents=True,exist_ok=True)
            with tarfile.open(archive) as tar: tar.extractall(destination,filter='data')
        sys.exit(0)
    else:
        raise RuntimeError('Unknown Android build generator: '+exe.name)
    adb('shell',shlex.join(['env','LD_LIBRARY_PATH='+remote,remote+'/'+exe.name,*args]))
    for source,local in outputs: adb('pull',source,str(local))
else:
    os.execv('/opt/homebrew/bin/python3',['python3',*sys.argv[1:]])
