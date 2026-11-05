#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Build ARM32 startup objects and compiler builtins without NEON or VFP D32.

NDK r27's crtbegin_dynamic.o contains NEON before main. These pinned upstream
sources preserve its constructor/destructor protocol and Android ELF note.
Only the compile baseline changes. Sources and licenses stay in the output cache.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import urllib.request

from prepare_builtins import prepare as prepare_builtins

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', required=True, type=Path)
    parser.add_argument('--api', default=21, type=int)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.api < 21:
        parser.error('This build targets Android API 21 or newer')
    host = {'Darwin':'darwin-x86_64', 'Linux':'linux-x86_64'}[platform.system()]
    compiler = args.ndk.resolve()/'toolchains/llvm/prebuilt'/host/'bin/clang'
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    lock = json.loads((HERE/'crt-sources.json').read_text())
    for row in lock['files']:
        path = args.out/'sources'/row['source']/row['path']
        if not path.exists():
            source = lock['sources'][row['source']]
            url = (f'https://android.googlesource.com/{source["repository"]}/+/'
                   f'{source["commit"]}/{row["path"]}?format=TEXT')
            data = base64.b64decode(urllib.request.urlopen(url, timeout=60).read())
            if hashlib.sha256(data).hexdigest() != row['sha256']:
                raise ValueError(f'Upstream checksum mismatch: {url}')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        if sha(path) != row['sha256']:
            raise ValueError(f'Modified cached CRT source: {path}')
    brand = args.out/'sources/ndk/sources/crt/crtbrand.S'
    common = [str(compiler), f'--target=armv7a-linux-androideabi{args.api}',
              '-mfpu=vfpv3-d16', '-O2', '-fPIC', '-fno-stack-protector',
              '-fuse-ld=lld', '-nostdlib', '-no-pie', '-Wl,-r',
              '-Wa,--noexecstack', '-Wl,-z,noexecstack',
              '-D__LIBC_HIDDEN__=__attribute__((visibility("hidden")))',
              f'-DPLATFORM_SDK_VERSION={args.api}',
              '-DABI_NDK_VERSION="V8 non-NEON CRT"',
              '-DABI_NDK_BUILD_NUMBER="source-rebuild"']
    builds = []
    for source, output in [('crtbegin.c', 'crtbegin_dynamic.o'),
                           ('crtbegin_so.c', 'crtbegin_so.o')]:
        path = args.out/'sources/bionic/libc/arch-common/bionic'/source
        command = common + [str(path), str(brand), '-o', str(args.out/output)]
        subprocess.run(command, check=True)
        builds.append(dict(command=command, output=output, sha256=sha(args.out/output)))
    builtins = prepare_builtins(args.ndk.resolve(), compiler, args.out, args.api)
    (args.out/'manifest.json').write_text(json.dumps(dict(
        compiler=str(compiler), compiler_sha256=sha(compiler), api=args.api,
        source_lock_sha256=sha(HERE/'crt-sources.json'), builds=builds,
        builtins=builtins), indent=2)+'\n')
    print(f'Non-NEON ARM32 CRT and compiler runtime: {args.out}')


if __name__ == '__main__':
    main()
