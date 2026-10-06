#!/usr/bin/env python3
"""Bind current resource sources and frozen binaries to their build manifests."""
import argparse
import json
from pathlib import Path

from run_js import digest

ROOT = Path(__file__).resolve().parents[2]


def audit(root, label):
    report, hashes = {}, {}
    for arch in ['host', 'host-verify', 'arm64', 'arm32']:
        folder = root / ('frozen-' + arch + '-' + label)
        path = folder / 'build-manifest.json'
        manifest = json.loads(path.read_text())
        if digest(folder / 'commands.txt') != manifest['commandsSha256']:
            raise RuntimeError('Frozen compiler commands changed: ' + str(folder))
        args_path = str(Path(manifest['buildDirectory']) / 'args.gn')
        if digest(folder / 'args.gn') != manifest['compileInputs'][args_path]:
            raise RuntimeError('Frozen GN configuration changed: ' + str(folder))
        changed, checked = [], 0
        for filename, expected in manifest['compileInputs'].items():
            source = Path(filename)
            try:
                relative = source.relative_to(ROOT)
            except ValueError:
                continue
            if relative.parts[0] == 'out':
                continue
            if source not in hashes:
                hashes[source] = digest(source) if source.is_file() else None
            checked += 1
            if hashes[source] != expected:
                changed.append({'path': filename, 'expected': expected,
                                'actual': hashes[source]})
        if not checked:
            raise RuntimeError('No current checkout source inputs: ' + str(path))
        for name, expected in manifest['binaries'].items():
            if digest(folder / name) != expected:
                raise RuntimeError('Frozen binary changed: ' + str(folder / name))
        report[arch] = {'manifestSha256': digest(path),
                        'checkedSourceInputs': checked,
                        'changedSourceInputs': changed,
                        'binaries': manifest['binaries']}
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--label', default='r6')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = audit(args.root, args.label)
    output = args.output or args.root / ('source-closure-audit-' + args.label + '.json')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({arch: {'checked': r['checkedSourceInputs'],
                            'changed': len(r['changedSourceInputs'])}
                      for arch, r in report.items()}))
    if any(r['changedSourceInputs'] for r in report.values()):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
