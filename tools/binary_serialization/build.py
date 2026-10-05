#!/usr/bin/env python3
"""Link native experiments against an existing, read-only V8 object build.

Recompiles the changed JSON parser and experimental engine adapter; fingerprints
every reused object. Does not run Ninja or write in the reference checkout.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--v8-build', type=Path, required=True)
    p.add_argument('--msgpack', type=Path, required=True)
    p.add_argument('--mpack',type=Path,default=ROOT/'out/binary-serialization/mpack-source')
    p.add_argument('--output', type=Path, default=ROOT/'out/binary-serialization')
    p.add_argument('--platform',choices=['macos','android-arm64','android-arm32'],default='macos')
    p.add_argument('--disable-msgpack-simd', action='store_true',
                   help='Build scalar control without the explicit NEON paths')
    p.add_argument('--reference-builtin-commit',
                   help='Match builtin IDs/IsolateData layout to reused core objects')
    p.add_argument('--profile-msgpack-cache', action='store_true',
                   help='Diagnostic cache counters on stderr; exclude from timings')
    p.add_argument('--ndk',type=Path,default=Path('/Users/james/software/android/sdk/ndk/27.3.13750724'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = ['src/json/json-parser.cc','src/msgpack/messagepack.cc','tools/binary_serialization/benchmark.cc']
    source_hashes = {s:digest(ROOT/s) for s in sources}
    header_hashes = {s:digest(ROOT/s) for s in
        ['src/msgpack/messagepack.h','src/msgpack/messagepack-string.h',
         'src/msgpack/messagepack-decoder.h','src/objects/js-data-object-builder.h']}
    ninja = (args.v8_build/'obj/v8_base_without_compiler.ninja').read_text()
    variables = dict(line.split(' = ',1) for line in ninja.splitlines() if ' = ' in line and not line.startswith(' '))
    flags = shlex.split(variables['defines']+' '+variables['cflags']+' '+variables['cflags_cc'])
    flags = [f for f in flags if not f.startswith('-fcrash-diagnostics-dir=')]
    includes = []
    reference_builtin = None
    if args.reference_builtin_commit:
        # Adding builtins changes IsolateData offsets as well as IDs. An
        # experimental adapter linked with frozen core objects must use the
        # core's definition list, not an unbuilt JS integration's new list.
        relative = 'src/builtins/builtins-definitions.h'
        overlay = args.output / 'reference-headers'
        path = overlay / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(subprocess.check_output(
            ['git', 'show', args.reference_builtin_commit + ':' + relative], cwd=ROOT))
        includes.append('-I' + str(overlay.resolve()))
        reference_builtin = {'commit': args.reference_builtin_commit,
                             'path': str(path.resolve()), 'sha256': digest(path)}
    for flag in shlex.split(variables['include_dirs']):
        loc = flag[2:]
        candidate = (args.v8_build/loc).resolve()
        reference_root = args.v8_build.parent.parent.resolve()
        if candidate.is_relative_to(reference_root) and not candidate.is_relative_to(args.v8_build.resolve()):
            candidate = ROOT/candidate.relative_to(reference_root)
        includes.append('-I'+str(candidate))
    includes.append('-I'+str(args.msgpack/'include'))
    includes.append('-I'+str(args.mpack/'src/mpack'))
    for i, flag in enumerate(flags):
        if flag == '-isysroot': flags[i+1] = '/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk'
        if flag.startswith('--sysroot='):
            flags[i]='--sysroot='+str((args.v8_build/flag.split('=',1)[1]).resolve())
    android=args.platform.startswith('android')
    compiler = str(args.ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/clang++') if android else '/Library/Developer/CommandLineTools/usr/bin/clang++'
    replacement = {}
    commands = []
    mpack_objects=[]
    for source in ['mpack-platform.c','mpack-common.c','mpack-reader.c','mpack-expect.c','mpack-writer.c','mpack-node.c']:
        output=args.output/(source+'.o')
        platform_flags=[f for f in flags if f.startswith(('--target=','--sysroot=','-march=','-mfloat-abi=','-mfpu=','-mthumb','-fPIC'))] if android else ['-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk','-mmacos-version-min=11.0']
        command=[compiler.replace('clang++','clang'),'-std=c11','-O2' if android else '-O3','-DNDEBUG',
            *platform_flags,'-c',str(args.mpack/'src/mpack'/source),'-o',str(output)]
        commands.append(command);subprocess.run(command,check=True,cwd=ROOT);mpack_objects.append(output)
    for source in sources:
        output = args.output/(Path(source).stem+'.o')
        command = [compiler,*flags,*includes]
        if 'messagepack.cc' in source or 'benchmark.cc' in source: command += ['-fexceptions']
        if 'messagepack.cc' in source and args.disable_msgpack_simd:
            command += ['-DMSGPACK_DISABLE_SIMD']
        if 'messagepack.cc' in source and args.profile_msgpack_cache:
            command += ['-DMSGPACK_PROFILE_CACHE']
        command += ['-c',str(ROOT/source),'-o',str(output)]
        commands.append(command)
        subprocess.run(command,check=True,cwd=ROOT)
        replacement[source] = output
    monolith = (args.v8_build/'obj/v8_monolith.ninja').read_text()
    link_line = next(x for x in monolith.splitlines() if x.startswith('build obj/libv8_monolith.a:'))
    objects = [args.v8_build/x for x in link_line.split(': alink ',1)[1].split(' || ',1)[0].split()]
    objects = [replacement['src/json/json-parser.cc'] if x.name=='json-parser.o' else x for x in objects]
    objects += [*mpack_objects,replacement['src/msgpack/messagepack.cc'],replacement['tools/binary_serialization/benchmark.cc'],
        args.v8_build/'obj/third_party/zlib/libchrome_zlib.a',
        args.v8_build/'obj/third_party/zlib/google/libcompression_utils_portable.a']
    missing = [str(x) for x in objects if not x.exists()]
    if missing: raise RuntimeError('Missing link inputs: '+repr(missing))
    binary = args.output/'native-binary-benchmark'
    if android:
        d8_variables=dict(line.strip().split(' = ',1) for line in (args.v8_build/'obj/d8.ninja').read_text().splitlines() if line.startswith(('  ldflags =','  libs =')))
        ldflags=shlex.split(d8_variables['ldflags'])
        ldflags=['--sysroot='+str((args.v8_build/f.split('=',1)[1]).resolve()) if f.startswith('--sysroot=') else f for f in ldflags]
        command=[compiler,*ldflags,'-static-libstdc++','-o',str(binary),*[str(x) for x in objects],*shlex.split(d8_variables['libs'])]
    else:
        command = [compiler,'-std=c++20','-O3','--target=arm64-apple-macos',
            '-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk',
            '-mmacos-version-min=11.0','-o',str(binary),*[str(x) for x in objects],'-framework','Foundation']
    commands.append(command)
    subprocess.run(command,check=True,cwd=ROOT)
    if any(digest(ROOT/s)!=expected for s,expected in {**source_hashes,**header_hashes}.items()):
        raise RuntimeError('Experiment source changed during build; do not use this binary. Rebuild with frozen sources.')
    manifest = {'v8BaseCommit': subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'msgpackCommit':subprocess.check_output(['git','rev-parse','cpp_master'],cwd=args.msgpack,text=True).strip(),
        'msgpackCCommit':subprocess.check_output(['git','rev-parse','c_master'],cwd=args.msgpack,text=True).strip(),
        'mpackCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=args.mpack,text=True).strip(),
        'platform':args.platform,'explicitMsgpackSimd':not args.disable_msgpack_simd,
        'referenceBuiltinDefinitions':reference_builtin,
        'profileMsgpackCache':args.profile_msgpack_cache,
        'v8Build':str(args.v8_build),'binary':str(binary),'binarySha256':digest(binary),
        'sources':source_hashes,'headers':header_hashes,
        'builderSha256':digest(ROOT/'src/objects/js-data-object-builder.h'),
        'reusedLinkInputs':{str(x):digest(x) for x in objects},'commands':commands,
        'note':'Native prototype linked with existing V8 core objects. Only listed experiment sources recompiled. No reference checkout mutation or GN production integration.'}
    (args.output/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(binary)

if __name__=='__main__':
    try: main()
    except subprocess.CalledProcessError as error: raise SystemExit(f'Native build failed: exit {error.returncode}. See compiler diagnostics above.')
