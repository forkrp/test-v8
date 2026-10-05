#!/usr/bin/env python3
"""Freeze a linked GN benchmark with its compile inputs and configuration."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    build = args.build.resolve()
    dry = subprocess.check_output(['ninja', '-C', str(build), '-n',
                                   'd8', 'msgpack_js_benchmark'], text=True)
    if 'no work to do' not in dry:
        raise RuntimeError('Build is not current; finish linking before freezing:\n' + dry)
    args.output.mkdir(parents=True, exist_ok=False)
    dependencies = subprocess.check_output(['ninja', '-C', str(build), '-t', 'deps'], text=True)
    inputs = {}
    for line in dependencies.splitlines():
        if not line.startswith('    '):
            continue
        path = (build / line.strip()).resolve()
        if path.is_file():
            inputs[str(path)] = digest(path) if str(path) not in inputs else inputs[str(path)]
    # GN and vendored license/version records are not compiler dependencies.
    config = [ROOT / 'BUILD.gn', build / 'args.gn', build / 'build.ninja',
              ROOT / 'third_party/msgpack/README.v8', ROOT / 'third_party/mpack/README.v8']
    for path in config:
        inputs[str(path)] = digest(path)
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands',
                                        'msgpack_js_benchmark'], text=True)
    (args.output / 'commands.txt').write_text(commands)
    (args.output / 'args.gn').write_bytes((build / 'args.gn').read_bytes())
    binaries = {}
    for name in ['d8', 'msgpack_js_benchmark', 'exe.unstripped/msgpack_js_benchmark']:
        source = build / name
        if not source.is_file():
            continue
        target = args.output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        binaries[name] = digest(target)
    manifest = {'gitHead': subprocess.check_output(['git', 'rev-parse', 'HEAD'],
                    cwd=ROOT, text=True).strip(),
                'buildDirectory': str(build), 'compileInputs': inputs,
                'commandsSha256': digest(args.output / 'commands.txt'),
                'binaries': binaries,
                'note': 'Current linked GN output, verified by ninja dry-run. Compile inputs '
                        'are the recorded dependency closure, including generated headers '
                        'and SDK headers. Binaries copied only after the successful link.'}
    (args.output / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'frozen': str(args.output), 'inputs': len(inputs), 'binaries': binaries}))


if __name__ == '__main__':
    main()
