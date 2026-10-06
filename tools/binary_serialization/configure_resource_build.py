#!/usr/bin/env python3
"""Configure an isolated Mac-host Android GN build, restoring source adapters.

The existing fork supports Linux-host Android builds. Mac testing additionally
needs Darwin/x86_64 NDK host selection and a script wrapper which executes
Android mksnapshot on the target. No tracked build configuration is retained.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--gn', type=Path, required=True)
    p.add_argument('--script-executable', type=Path, required=True)
    args = p.parse_args()
    replacements = {
        'build/config/BUILDCONFIG.gn': [('assert(host_os == "linux", "Android builds are only supported on Linux.")',
            'assert(host_os == "linux" || host_os == "mac", "Android cross build host")')],
        'build/config/android/config.gni': [('if (host_cpu == "x64") {',
            'if (host_cpu == "x64" || (host_os == "mac" && host_cpu == "arm64")) {')],
    }
    saved = []
    try:
        for name, edits in replacements.items():
            path = ROOT / name
            content = path.read_bytes(); stat = path.stat()
            saved.append((path, content, stat))
            text = content.decode()
            for before, after in edits:
                if text.count(before) != 1: raise RuntimeError('Unexpected build configuration: ' + name)
                text = text.replace(before, after)
            path.write_text(text)
        command = [str(args.gn.resolve()), 'gen', str(args.build),
                   '--script-executable=' + str(args.script_executable.resolve())]
        subprocess.run(command, cwd=ROOT, check=True)
        (args.build / 'mac-configuration.json').write_text(json.dumps({
            'command': command, 'temporaryGnAdaptations': replacements,
            'note': 'Source files restored with original mtimes after generation.'}, indent=2)+'\n')
    finally:
        for path, content, stat in saved:
            path.write_bytes(content)
            os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))

if __name__ == '__main__': main()
