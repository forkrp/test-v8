#!/usr/bin/env python3
"""Build an independent c_master wire checker without switching the clone."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[2]
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--msgpack',type=Path,required=True)
    p.add_argument('--output',type=Path,default=ROOT/'out/binary-serialization')
    args=p.parse_args()
    source=args.output/'msgpack-c-source';build=args.output/'msgpack-c-build'
    source.mkdir(parents=True,exist_ok=True)
    archive=subprocess.check_output(['git','archive','c_master'],cwd=args.msgpack)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar: tar.extractall(source,filter='data')
    compiler='/Library/Developer/CommandLineTools/usr/bin/clang'
    sdk='/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'
    subprocess.run(['cmake','-S',str(source),'-B',str(build),'-DMSGPACK_BUILD_TESTS=OFF',
        '-DCMAKE_C_COMPILER='+compiler,'-DCMAKE_OSX_SYSROOT='+sdk],check=True)
    binary=args.output/'c-peer'
    command=[compiler,'-O3','-DNDEBUG','-isysroot',sdk,'-I'+str(source/'include'),
        '-I'+str(build/'include'),'-I'+str(build/'include/msgpack'),str(ROOT/'tools/binary_serialization/c_peer.c'),
        *[str(source/'src'/s) for s in ['unpack.c','zone.c','objectc.c']],'-o',str(binary)]
    subprocess.run(command,check=True)
    (args.output/'c-peer-manifest.json').write_text(json.dumps({
        'commit':subprocess.check_output(['git','rev-parse','c_master'],cwd=args.msgpack,text=True).strip(),
        'archiveSha256':hashlib.sha256(archive).hexdigest(),
        'binarySha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'command':command},indent=2)+'\n')
    print(binary)
if __name__=='__main__':main()
