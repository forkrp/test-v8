#!/usr/bin/env python3
"""Freeze source-matched, fully linked d8/cctest outputs for async evidence."""
import argparse, hashlib, json, re, shutil, shlex, subprocess
from pathlib import Path
from run import ROOT

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def compiler_identity(build, commands):
    identities = {}
    for line in commands.splitlines():
        if ' -c ' not in line:
            continue
        compiler = (build / shlex.split(line)[0]).resolve()
        if not compiler.is_file() or str(compiler) in identities:
            continue
        paths = [compiler]
        # The local Android toolchain has shell dispatchers choosing NDK
        # clang for Android targets and CLT clang for host generators. An
        # unqualified --version on the dispatcher reports only the host.
        with compiler.open('rb') as stream:
            is_dispatcher = stream.read(2) == b'#!'
        if is_dispatcher:
            for target in re.findall(r'\bexec\s+(/[^\s;]+)', compiler.read_text()):
                path = Path(target).resolve()
                if path.is_file():
                    paths.append(path)
        identities[str(compiler)] = {
            str(path): {'sha256': sha(path),
                        'version': subprocess.check_output([str(path), '--version'], text=True)}
            for path in paths}
    return identities
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();build=args.build.resolve()
    dry=subprocess.check_output(['ninja','-C',str(build),'-n','d8','cctest','msgpack_js_benchmark'],text=True)
    if 'no work to do' not in dry: raise RuntimeError('Build not current:\n'+dry)
    args.output.mkdir(parents=True,exist_ok=False)
    inputs={}
    deps=subprocess.check_output(['ninja','-C',str(build),'-t','deps'],text=True)
    for line in deps.splitlines():
        if not line.startswith('    '):continue
        path=(build/line.strip()).resolve()
        if path.is_file() and str(path) not in inputs:inputs[str(path)]=sha(path)
    for path in [ROOT/'BUILD.gn',build/'args.gn',build/'build.ninja']:
        inputs[str(path)]=sha(path)
    commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','d8','cctest','msgpack_js_benchmark'],text=True)
    (args.output/'commands.txt').write_text(commands)
    shutil.copy2(build/'args.gn',args.output/'args.gn')
    binaries={}
    for name in ['d8','cctest','msgpack_js_benchmark']:
        shutil.copy2(build/name,args.output/name);binaries[name]=sha(args.output/name)
    toolchain=compiler_identity(build, commands)
    (args.output/'compiler-identity.json').write_text(json.dumps(toolchain,indent=2)+'\n')
    # These builds contain uncommitted code. Retain the patch and new files,
    # rather than leaving hashes that cannot reconstruct an earlier candidate.
    diff = subprocess.check_output(['git','diff','HEAD'],cwd=ROOT)
    (args.output/'source-patch.diff').write_bytes(diff)
    untracked = {}
    for entry in subprocess.check_output(
            ['git','ls-files','--others','--exclude-standard','-z'],cwd=ROOT).split(b'\0'):
        if not entry:
            continue
        relative = Path(entry.decode())
        source = ROOT / relative
        if source.is_file():
            saved = args.output / 'source-untracked' / relative
            saved.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,saved)
            untracked[str(relative)] = sha(saved)
    adaptations = {}
    if 'target_os="android"' in (build/'args.gn').read_text().replace(' ', ''):
        recipe_dir = ROOT/'out/json-android-review'
        saved_dir = args.output/'build-adaptations'
        saved_dir.mkdir()
        for name in ['gen.py', 'gn-python.py', 'BUILD.gn']:
            path = recipe_dir/name
            if path.is_file():
                shutil.copy2(path, saved_dir/name)
                adaptations[str(path)] = {'sha256': sha(path), 'savedAs': str(saved_dir/name)}
    report={'toolchain':toolchain,
        'gitStatus':subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),
        'gitDiffSha256':hashlib.sha256(diff).hexdigest(),
        'sourcePatch': {'savedAs': 'source-patch.diff', 'sha256': sha(args.output/'source-patch.diff')},
        'untrackedSources': untracked,
        'gitHead':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        'build':str(build),'compileInputs':inputs,'commandsSha256':sha(args.output/'commands.txt'),
        'freezerSha256':sha(__file__),'compilerIdentitySha256':sha(args.output/'compiler-identity.json'),
        'buildAdaptations': adaptations,
        'binaries':binaries,'note':'Current linked build, ninja dry-run clean; complete recorded compile dependency closure.'}
    (args.output/'build-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'frozen':str(args.output),'inputs':len(inputs),'binaries':binaries}))
if __name__=='__main__':main()
