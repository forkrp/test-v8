#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Summarize the mixed-object fix, retaining rejected and negative results."""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics

from run import save, sha


def summarize(out, extra_tags, candidate='final', base_tags=None):
    tags = [*(base_tags or ['parse-final','stringify-final','async4-final']), *extra_tags]
    records, metadata = [], {}
    for tag in tags:
        meta = json.loads((out/'results'/f'{tag}-metadata.json').read_text())
        assert meta.get('finished_at'), tag
        metadata[tag] = meta
        for line in (out/'results'/f'{tag}-raw.jsonl').read_text().splitlines():
            row = json.loads(line)
            assert row['output'][0]['validated_all_results']
            assert len(row['output'][0]['measurements']) == 9
            row['tag'] = tag
            row['binary_label'] = row['label']
            if row['label'] == candidate:
                row['label'] = 'final'
            records.append(row)
    groups = collections.defaultdict(list)
    for row in records:
        groups[(row['fixture'],row['mode'],row['jobs'])].append(row)
    cells, failures = [], []
    for (fixture, mode, jobs), rows in sorted(groups.items()):
        labels = sorted({r['label'] for r in rows})
        values = {}
        for label in labels:
            selected = [r for r in rows if r['label']==label]
            medians = [r['output'][0]['wall_ms'] for r in selected]
            values[label] = dict(wall_ms=statistics.median(medians),
                per_process_wall_ms=medians,
                entry_ms=statistics.median(r['output'][0]['entry_ms'] for r in selected),
                process_peak_rss_max_bytes=max(r['process_peak_rss_bytes'] for r in selected))
        comparisons = {}
        for reference in ['before','v12']:
            if reference not in values:
                continue
            paired = []
            for tag,pair in sorted({(r['tag'],r['pair']) for r in rows}):
                times = {r['label']:r['output'][0]['wall_ms'] for r in rows if r['tag']==tag and r['pair']==pair}
                if reference in times and 'final' in times:
                    paired.append(100*(1-times['final']/times[reference]))
            comparisons[reference] = dict(
                speedup=values[reference]['wall_ms']/values['final']['wall_ms'],
                less_time_percent=100*(1-values['final']['wall_ms']/values[reference]['wall_ms']),
                paired_less_time_percent=paired,
                paired_median_less_time_percent=statistics.median(paired))
        first = rows[0]
        cell = dict(fixture=fixture, family=first['family'], requested_mb=first['requested_mb'],
                    mode=mode,jobs=jobs, values=values, comparisons=comparisons)
        cells.append(cell)
        change = comparisons['before']
        threshold = 5 if mode=='parseAsync' else 2 if mode=='parse' and first['family'] in ['records12','records60'] else 3
        if min(change['less_time_percent'],change['paired_median_less_time_percent']) < -threshold:
            failures.append(dict(fixture=fixture,mode=mode,jobs=jobs,reason='regression vs before',threshold_percent=threshold,change=change))
        if mode=='parse' and first['family'] in ['records12','records60']:
            if comparisons['v12']['less_time_percent']<=0:
                failures.append(dict(fixture=fixture,mode=mode,jobs=jobs,reason='same-shape input lost gain vs v12',change=comparisons['v12']))
        if mode=='parse' and first['family']=='mixed_nested':
            if min(change['less_time_percent'],change['paired_median_less_time_percent'])<=0:
                failures.append(dict(fixture=fixture,mode=mode,jobs=jobs,reason='mixed input did not improve vs before',change=change))
            if comparisons['v12']['less_time_percent'] < -1:
                failures.append(dict(fixture=fixture,mode=mode,jobs=jobs,reason='mixed input remains slower than v12 by more than 1%',change=comparisons['v12']))
    geomeans = []
    for size in [10,20,50]:
        selected = [r for r in cells if r['requested_mb']==size and r['mode']=='parse' and r['jobs']==1]
        assert len(selected)==6
        row={'requested_mb':size}
        for reference in ['before','v12']:
            gm=math.exp(statistics.mean(math.log(r['comparisons'][reference]['speedup']) for r in selected))
            row[reference]=dict(speedup=gm,less_time_percent=100*(1-1/gm))
        geomeans.append(row)
    pilots={p.name.removesuffix('-summary.json'):json.loads(p.read_text()) for p in sorted((out/'results').glob('*-pilot-summary.json'))}
    summary=dict(date='2026-10-26',platform='Mac ARM64',candidate_label=candidate,state=json.loads((out/'state.json').read_text()),
        builds={role:json.loads((out/'manifests'/f'{label}.json').read_text()) for role,label in [('before','before'),('final',candidate),('v12','v12'),('final-dcheck',candidate+'-dcheck')]},
        acceptance_gates=json.loads((out/'results/acceptance-gates.json').read_text()),
        fixtures=json.loads((out/'fixtures/manifest.json').read_text()),
        correctness=json.loads((out/'results'/f'{candidate}-validation.json').read_text()),
        regression_checks=json.loads((out/'results'/('regression-checks.json' if candidate=='final' else candidate+'-regression-checks.json')).read_text()),
        formal_tags=tags,formal_processes=len(records),measured_batches=len(records)*9,
        tag_process_counts={tag:sum(r['tag']==tag for r in records) for tag in tags},
        raw_sha256={tag:sha(out/'results'/f'{tag}-raw.jsonl') for tag in tags},
        cells=cells,parse_geomeans=geomeans,
        negative_vs_before=[r for r in cells if r['comparisons']['before']['less_time_percent']<0],
        negative_parse_vs_v12=[r for r in cells if r['mode']=='parse' and r['comparisons']['v12']['less_time_percent']<0],
        gate_failures=failures,rejected_and_winning_pilots=pilots,
        function_sizes=json.loads((out/'results/noinline-properties-function-sizes.json').read_text()))
    save(out/'results/fix-summary.json',summary)
    print('PROCESSES',len(records),'BATCHES',len(records)*9,'GATE_FAILURES',len(failures))
    for row in cells:
        if row['mode']=='parse':
            print(row['fixture'],{n:round(v['wall_ms'],3) for n,v in row['values'].items()},
                  {n:round(v['less_time_percent'],2) for n,v in row['comparisons'].items()})
    for failure in failures:print('GATE',failure)
    return summary


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--extra-tags',nargs='*',default=[])
    parser.add_argument('--candidate',default='final')
    parser.add_argument('--tags',nargs='+')
    args=parser.parse_args()
    summarize(args.out.resolve(),args.extra_tags,args.candidate,args.tags)


if __name__=='__main__':
    main()
