#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license in LICENSE.
"""Compare all four JSON APIs with independent, frozen V8 12.6 transcripts."""
import argparse
import gzip
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'test/json/correctness-differential.js'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(out, binary, api, seed, count, group, flags, label):
    name = f'{label}-{api}-{seed:x}-{group}'
    output, errors = out / (name + '.gz'), out / (name + '.stderr')
    command = [str(binary), '--expose-gc', '--thread-pool-size=4', *flags,
               str(SCRIPT), '--', api, str(seed), str(count), group]
    start = time.monotonic()
    lines, final = 0, b''
    unexpected = []
    digest = hashlib.sha256()
    with errors.open('xb') as err, output.open('xb') as raw:
        with gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as log:
            process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                                       stderr=err)
            timer = threading.Timer(1800, process.kill)
            timer.start()
            try:
                for line in process.stdout:
                    log.write(line)
                    digest.update(line)
                    lines += 1
                    final = line
                    if not line.startswith(b'DONE\t'):
                        case, separator, outcome = line.partition(b'\t')
                        arbitrary = case.startswith((b'grammar:', b'mutate:'))
                        expected_throw = case.startswith(
                            (b'special:[object BigInt]', b'large-bad:'))
                        if (not separator or not arbitrary and
                                outcome.startswith(b'throw:') != expected_throw):
                            if len(unexpected) < 10:
                                unexpected.append(line[:1200].decode(errors='replace'))
                status = process.wait()
            finally:
                timer.cancel()
    row = dict(label=label, api=api, seed=seed, count=count, group=group,
               command=command, binary_sha256=sha(binary),
               script_sha256=sha(SCRIPT), returncode=status,
               elapsed_seconds=time.monotonic()-start, rows=lines,
               stdout_sha256=digest.hexdigest(), output=str(output),
               stderr=str(errors), completed=final.startswith(b'DONE\t'),
               intended_cases_valid=not unexpected, unexpected_outcomes=unexpected)
    return row


def compare(expected, actual):
    with gzip.open(expected['output'], 'rb') as left, \
         gzip.open(actual['output'], 'rb') as right:
        for index, (a, b) in enumerate(itertools.zip_longest(left, right)):
            if a != b:
                return dict(equal=False, line=index+1,
                            reference=(a or b'')[:1200].decode(errors='replace'),
                            actual=(b or b'')[:1200].decode(errors='replace'))
    return dict(equal=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--label', default='current')
    parser.add_argument('--seed', type=lambda v:int(v,0), default=0x4a534f4e)
    parser.add_argument('--count', type=int, default=4000)
    parser.add_argument('--group', choices=['all','boundaries','fuzz','effects','large'], default='all')
    parser.add_argument('--flag', action='append', default=[])
    parser.add_argument('--reference-results', type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    before = {str(p):sha(p) for p in [SCRIPT,args.reference,args.candidate]}
    records = []
    refs = {}
    completed = False
    if args.reference_results:
        old = json.loads(args.reference_results.read_text())
        for record in old['runs']:
            if record['label'] != 'reference': continue
            assert record['binary_sha256'] == sha(args.reference)
            assert record['script_sha256'] == sha(SCRIPT)
            assert (record['seed'],record['count'],record['group']) == (args.seed,args.count,args.group)
            refs[record['api']] = record
    def save():
        (args.out/'results.json').write_text(json.dumps(dict(
            inputs=before, runs=records, reference_results=str(args.reference_results),
            completed=completed,
            all_passed=completed and all(r['returncode']==0 and r['completed'] and
                r['intended_cases_valid'] and
                r.get('comparison',{}).get('equal',True) for r in records)),indent=2)+'\n')
    for api in ['parse','stringify']:
        if api not in refs:
            refs[api] = run(args.out,args.reference.resolve(),api,args.seed,args.count,args.group,[], 'reference')
            records.append(refs[api]);save()
            assert refs[api]['returncode']==0 and refs[api]['completed'] and refs[api]['intended_cases_valid'], refs[api]
            print('reference',api,refs[api]['rows'],'rows',flush=True)
    failed = False
    for api in ['parse','parseAsync','stringify','stringifyAsync']:
        row = run(args.out,args.candidate.resolve(),api,args.seed,args.count,args.group,args.flag,args.label)
        if row['returncode'] != 0 or not row['completed'] or not row['intended_cases_valid']:
            records.append(row);save();raise RuntimeError(row)
        row['comparison'] = compare(refs['parse' if api.startswith('parse') else 'stringify'],row)
        records.append(row);save()
        print(args.label,api,row['rows'],'rows',row['comparison'],flush=True)
        failed |= not row['comparison']['equal']
    for file,digest in before.items():assert sha(Path(file)) == digest,file
    completed = True
    save()
    if failed:raise SystemExit(1)


if __name__ == '__main__':
    main()
