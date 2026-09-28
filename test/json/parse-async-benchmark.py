#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Compare d8 binaries using the same inputs; CPU/RSS include setup and GC."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time


def measure(binary, script, workers, runs, case, jobs, mode):
    command = [str(Path(binary).resolve()), '--expose-gc',
               '--thread-pool-size=' + str(workers), str(script), '--',
               str(runs), case, str(jobs), mode]
    start = time.monotonic()
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(command, stdout=output, stderr=output)
        # Bound hangs without losing the per-child rusage returned by wait4.
        timer = threading.Timer(120, process.kill)
        timer.daemon = True
        timer.start()
        try:
            if hasattr(os, 'wait4'):
                _, status, usage = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                cpu = usage.ru_utime + usage.ru_stime
                rss = usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)
            else:
                process.wait()
                cpu = rss = None
        finally:
            timer.cancel()
        output.seek(0)
        text = output.read().decode('utf-8', errors='replace')
    if process.returncode:
        raise RuntimeError(f'{command}: exit {process.returncode}\n{text}')
    rows = [json.loads(line) for line in text.splitlines() if line.startswith('{')]
    if len(rows) != 1:
        raise RuntimeError(f'Expected one completed benchmark row: {text}')
    return dict(binary=binary, workers=workers, process_cpu_s=cpu,
                process_peak_rss_bytes=rss, process_wall_s=time.monotonic() - start,
                **rows[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binaries', nargs='+')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--runs', type=int, default=7)
    parser.add_argument('--jobs', type=int, nargs='+', default=[1, 4])
    parser.add_argument('--mode', choices=['both', 'sync', 'async'], default='both')
    parser.add_argument('--script', type=Path,
                        default=Path(__file__).with_suffix('.js'))
    parser.add_argument('--cases', nargs='+', default=[
        'records', 'numbers', 'strings', 'nested', 'varied', 'long_strings',
        'unicode'])
    args = parser.parse_args()
    if not 1 <= args.workers <= 16:
        parser.error('workers must be in [1, 16], matching this platform limit')
    script = args.script.resolve()
    for index, case in enumerate(args.cases):
        for jobs in args.jobs:
            # Also alternate binary order between cases to reduce ordering bias.
            binaries = args.binaries if index % 2 == 0 else args.binaries[::-1]
            for binary in binaries:
                print(json.dumps(measure(binary, script, args.workers, args.runs,
                                         case, jobs, args.mode)), flush=True)


if __name__ == '__main__':
    main()
