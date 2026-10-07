#!/usr/bin/env python3
"""Package a verified native CLI with its format, licenses and file hashes."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[2]


def digest(filename):
    return hashlib.sha256(filename.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--test-report', type=Path, required=True)
    parser.add_argument('--platform', required=True, help='OS-CPU, e.g. macos-arm64')
    parser.add_argument('--destination', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    report = json.loads(args.test_report.read_text())
    if report['result'] != 'passed' or Path(report['binary']).resolve() != binary:
        parser.error('The test report must pass and identify this executable')
    if report['binarySha256'] != digest(binary):
        parser.error('The executable changed after verification')
    if not args.platform or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-'
                                for c in args.platform):
        parser.error('Platform must contain only lowercase letters, digits and hyphens')
    name = 'msgpack-resource-0.1.0-' + args.platform
    folder = args.destination.resolve() / name
    folder.mkdir(parents=True, exist_ok=False)
    files = {
        binary.name: binary,
        'README.md': Path(__file__).with_name('README.md'),
        'FORMAT.md': ROOT / 'docs/msgpack-resource-format.md',
        'LICENSE': ROOT / 'LICENSE',
        'LICENSE.msgpack': ROOT / 'third_party/msgpack/COPYING',
        'LICENSE.msgpack-boost': ROOT / 'third_party/msgpack/LICENSE_1_0.txt',
        'LICENSE.mpack': ROOT / 'third_party/mpack/LICENSE',
        'LICENSE.abseil': ROOT / 'third_party/abseil-cpp/LICENSE',
        'LICENSE.highway': ROOT / 'third_party/highway/LICENSE',
        'LICENSE.highway-source': ROOT / 'third_party/highway/src/LICENSE',
        'LICENSE.zlib': ROOT / 'third_party/zlib/LICENSE',
        'LICENSE.fp16': ROOT / 'third_party/fp16/src/LICENSE',
    }
    for target, source in files.items():
        shutil.copy2(source, folder / target)
    # Keep verification identity and results without exposing builder-local paths.
    (folder / 'verification.json').write_text(json.dumps(
        {key: value for key, value in report.items() if key not in ('binary', 'd8')},
        indent=2) + '\n')
    version = subprocess.check_output([str(binary), '--version'], text=True).strip()
    sources = ['BUILD.gn', 'tools/msgpack-resource/main.cc',
               'src/msgpack/messagepack.cc', 'src/msgpack/messagepack-resource-encoder.h',
               'src/msgpack/messagepack-resource-format.h', 'src/msgpack/messagepack-string.h']
    manifest = {
        'version': version, 'platform': args.platform,
        'gitHead': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'sourceSha256': {name: digest(ROOT / name) for name in sources},
        'files': {name: digest(folder / name) for name in [*files, 'verification.json']},
        'buildArgs': (binary.parent / 'args.gn').read_text(),
    }
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    names = [*files, 'verification.json', 'manifest.json']
    (folder / 'SHA256SUMS').write_text(''.join(
        f'{digest(folder / name)}  {name}\n' for name in names))
    archive = folder.parent / (folder.name + '.tar.gz')
    with tarfile.open(archive, 'w:gz') as output:
        output.add(folder, arcname=folder.name)
    print(json.dumps({'folder': str(folder), 'archive': str(archive),
                      'archiveSha256': digest(archive), 'bytes': archive.stat().st_size}))


if __name__ == '__main__':
    main()
