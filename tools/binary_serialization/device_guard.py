#!/usr/bin/env python3
"""Serialize measurements around foreign V8 device jobs and retain overlaps."""
from datetime import datetime, timezone
import fcntl
import json
from pathlib import Path
import subprocess
import threading
import time


class DeviceGuard:
    def __init__(self, serial, remote, log):
        self.serial, self.remote, self.log = serial, str(remote), Path(log)
        self.overlaps = []
        self.ready = False
        # The neighboring async codec workflow uses this same advisory lock.
        # Keep it for the controller's entire lifetime; monitors additionally
        # catch generators or older runners that do not participate.
        self.device_lock = open('/tmp/v8-msgpack-async-device.lock', 'a')
        fcntl.flock(self.device_lock.fileno(), fcntl.LOCK_EX)

    def foreign_jobs(self):
        output = subprocess.check_output(
            ['adb', '-s', self.serial, 'shell', 'ps', '-A', '-o', 'PID,NAME,ARGS'], text=True)
        jobs = []
        for line in output.splitlines()[1:]:
            fields = line.split(None, 2)
            if len(fields) < 3: continue
            if fields[1] not in ['d8', 'cctest', 'mksnapshot', 'torque', 'benchmark',
                                 'simpleperf', 'js-binary-benchmark', 'msgpack_js_benchmark']:
                continue
            if self.remote not in fields[2]: jobs.append(line.strip())
        return jobs

    def wait_idle(self):
        quiet = 0
        while quiet < 3:
            quiet = 0 if self.foreign_jobs() else quiet + 1
            if quiet < 3: time.sleep(1)

    def run(self, command):
        # A retry runs in a fresh process, so first-call measurements stay cold.
        while True:
            if not self.ready or self.foreign_jobs(): self.wait_idle()
            self.ready = True
            stop, overlap = threading.Event(), []
            def monitor():
                while not stop.is_set():
                    try:
                        jobs = self.foreign_jobs()
                        if jobs: overlap.append({'utc': datetime.now(timezone.utc).isoformat(), 'jobs': jobs})
                    except Exception as error:
                        overlap.append({'monitorError': str(error)})
                    stop.wait(.25)
            thread = threading.Thread(target=monitor, daemon=True)
            thread.start()
            try:
                result = subprocess.run(command, capture_output=True, text=True)
            finally:
                stop.set(); thread.join()
            if overlap:
                self.ready = False
                self.overlaps.append({'command': command, 'observations': overlap,
                                      'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
                self.log.parent.mkdir(parents=True, exist_ok=True)
                self.log.write_text(json.dumps(self.overlaps, indent=2) + '\n')
                print('Excluded device overlap; waiting to retry the sample.', flush=True)
                continue
            if result.returncode:
                raise RuntimeError(f'{command}: {result.stdout[-2000:]} {result.stderr[-3000:]}')
            return result.stdout
