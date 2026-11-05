#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Build the pinned NDK compiler-rt builtins for VFPv3-D16, without NEON."""
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_sha(path):
    # Gitiles stamps tar members with the request time. Pin file names and
    # contents instead of the non-reproducible archive envelope.
    with tarfile.open(path, 'r:gz') as archive:
        files = {}
        for member in archive:
            if member.isdir():
                continue
            if not member.isfile() or member.name in files:
                raise ValueError('Unexpected runtime archive member')
            files[member.name] = hashlib.sha256(
                archive.extractfile(member).read()).hexdigest()
    return hashlib.sha256(json.dumps(files, sort_keys=True,
                                    separators=(',', ':')).encode()).hexdigest()


def prepare(ndk, compiler, out, api):
    lock_path = HERE/'builtins-sources.json'
    lock = json.loads(lock_path.read_text())
    properties = (ndk/'source.properties').read_text()
    if not re.search(r'^Pkg.Revision\s*=\s*' + re.escape(lock['ndk_revision']) +
                     r'\s*$', properties, re.M):
        raise ValueError('The pinned ARM32 runtime rebuild requires NDK ' +
                         lock['ndk_revision'])
    source = out/'builtins-source'
    stamp = out/'builtins-source-manifest.json'
    archives = out/'archives'
    archives.mkdir(exist_ok=True)

    def download(row):
        path = archives/(row['tree_sha256'] + '.tar.gz')
        if not path.exists():
            data = urllib.request.urlopen(row['url'], timeout=90).read()
            path.write_bytes(data)
        if tree_sha(path) != row['tree_sha256']:
            raise ValueError('Modified cached archive: ' + str(path))
        return path

    if stamp.exists():
        existing = json.loads(stamp.read_text())
        if existing['lock_sha256'] != sha(lock_path):
            raise ValueError('Use a new output directory for changed runtime sources')
        for name, digest in existing['files'].items():
            if sha(source/name) != digest:
                raise ValueError('Modified cached runtime source: ' + name)
    else:
        source.mkdir(exist_ok=True)
        for row in lock['archives']:
            target = source/row['path']
            target.mkdir(parents=True, exist_ok=True)
            with tarfile.open(download(row), 'r:gz') as archive:
                archive.extractall(target, filter='data')
        with tarfile.open(download(lock['patch_archive']), 'r:gz') as archive:
            for i, row in enumerate(lock['patches']):
                data = archive.extractfile(row['name']).read()
                if hashlib.sha256(data).hexdigest() != row['sha256']:
                    raise ValueError('Patch checksum mismatch: ' + row['name'])
                parts = re.split(rb'(?=^diff --git )', data, flags=re.M)
                data = b''.join(p for p in parts if p.startswith(
                    b'diff --git a/compiler-rt/lib/builtins/'))
                if hashlib.sha256(data).hexdigest() != row['filtered_sha256']:
                    raise ValueError('Unexpected builtins patch scope')
                patch = out/f'builtins-{i}.patch'
                patch.write_bytes(data)
                subprocess.run(['patch', '-p1', '-t', '-N', '-i', str(patch)],
                               cwd=source, check=True)
        row = lock['license']
        data = base64.b64decode(urllib.request.urlopen(row['url'], timeout=60).read())
        if hashlib.sha256(data).hexdigest() != row['sha256']:
            raise ValueError('Runtime license checksum mismatch')
        (source/'compiler-rt/LICENSE.TXT').write_bytes(data)
        stamp.write_text(json.dumps(dict(lock_sha256=sha(lock_path), files={
            p.relative_to(source).as_posix():sha(p)
            for p in sorted(source.rglob('*')) if p.is_file()}), indent=2)+'\n')

    build = out/'builtins-build'
    toolchain = compiler.parent.parent
    baseline = '-mfpu=vfpv3-d16 -mfloat-abi=softfp -march=armv7-a'
    # Use compiler-rt's standalone cross build. The NDK r27 CMake Android
    # toolchain explicitly rejects ANDROID_ARM_NEON=OFF.
    command = ['cmake', '-G', 'Ninja', '-S', str(source/'compiler-rt/lib/builtins'),
               '-B', str(build), '-DCMAKE_SYSTEM_NAME=Linux',
               '-DCMAKE_SYSTEM_PROCESSOR=arm',
               '-DCMAKE_MAKE_PROGRAM=' + str(Path(shutil.which('ninja')).resolve()),
               '-DCMAKE_C_COMPILER=' + str(compiler),
               '-DCMAKE_ASM_COMPILER=' + str(compiler),
               f'-DCMAKE_C_COMPILER_TARGET=arm-linux-androideabi{api}',
               f'-DCMAKE_ASM_COMPILER_TARGET=arm-linux-androideabi{api}',
               '-DCMAKE_SYSROOT=' + str(toolchain/'sysroot'),
               '-DCMAKE_C_FLAGS=' + baseline, '-DCMAKE_ASM_FLAGS=' + baseline,
               '-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON',
               '-DCOMPILER_RT_EXCLUDE_ATOMIC_BUILTIN=OFF',
               '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_RUNTIMES_BUILD=ON',
               '-DCOMPILER_RT_OS_DIR=linux']
    with (out/'builtins-configure.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (out/'builtins-build.log').open('w') as log:
        subprocess.run(['cmake', '--build', str(build), '-j', '8'], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    archive = build/'lib/linux/libclang_rt.builtins-arm.a'
    if not archive.is_file():
        raise ValueError('Compiler-rt did not produce ARM builtins')
    resource = out/'clang-resource'
    (resource/'lib/linux').mkdir(parents=True, exist_ok=True)
    output = resource/'lib/linux/libclang_rt.builtins-arm-android.a'
    shutil.copyfile(archive, output)
    headers = resource/'include'
    original = Path(subprocess.check_output([str(compiler), '-print-resource-dir'],
                                          text=True).strip())
    if not headers.exists():
        headers.symlink_to(original/'include')
    # libunwind is supplied by the compatible V8 archive, not this overlay.
    # The embedder must link with --unwindlib=none, as the GN build does.
    return dict(command=command, source_manifest_sha256=sha(stamp),
                source_lock_sha256=sha(lock_path), archive_sha256=sha(output),
                archive=str(output))
