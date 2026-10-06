#!/usr/bin/env python3
"""Audit resource matrices, independent decoding and cross-ABI wire identity."""
import argparse
import gzip
import json
import statistics
from pathlib import Path
from run_js import digest
from resource_peer import decode,equivalent
from resource_ablate import ablate


def main():
    p=argparse.ArgumentParser();p.add_argument('--cold',type=Path,action='append',required=True)
    p.add_argument('--warm',type=Path,action='append',default=[])
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    report={'coldMatrices':{},'warmMatrices':{},'workloads':{},'crossAbiWireIdentity':True if len(a.cold)>1 else None,'independentValidation':[]}
    expected_names=None
    for matrix_path in a.cold:
        m=json.loads(matrix_path.read_text());name=matrix_path.parent.name
        if 'completedUtc' not in m:raise RuntimeError('Incomplete cold matrix: '+name)
        names=[w['name'] for w in m['workloads']]
        if len(names)!=18 or len(set(names))!=18:raise RuntimeError('Expected fixed 18-workload corpus')
        if expected_names is not None and set(names)!=expected_names:raise RuntimeError('Different corpus')
        expected_names=set(names)
        report['coldMatrices'][name]={'sha256':digest(matrix_path),'binarySha256':m['binarySha256'],'runnerSha256':m['runnerSha256']}
        for w in m['workloads']:
            source=matrix_path.parent/(w['name']+'.source.json')
            if digest(source)!=w['sourceSha256']:raise RuntimeError('Source identity')
            original=json.loads(source.read_text());payloads={}
            for codec in ['msgpack','resource','json_bytes']:
                path=matrix_path.parent/(w['name']+'.'+codec);entry=w['preparedInputs'][codec]
                data=path.read_bytes()
                if digest(path)!=entry['sha256'] or len(data)!=entry['bytes'] or len(gzip.compress(data,compresslevel=6,mtime=0))!=entry['gzipBytes']:raise RuntimeError('Payload identity')
                if codec in ['msgpack','resource'] and not equivalent(original,decode(data)):raise RuntimeError('Independent value/bit mismatch: '+w['name'])
                payloads[codec]=data
            if len(payloads['resource'])>len(payloads['msgpack']):raise RuntimeError('Resource grew')
            if not payloads['resource'].startswith(b'V8MR\x01') and payloads['resource']!=payloads['msgpack']:raise RuntimeError('Fallback differs from standard bytes')
            item=report['workloads'].setdefault(w['name'],{'sizes':w['preparedInputs'],'architectures':{},'ablations':{}})
            if item['sizes']['resource']['sha256']!=w['preparedInputs']['resource']['sha256'] or item['sizes']['msgpack']['sha256']!=w['preparedInputs']['msgpack']['sha256']:raise RuntimeError('Cross-ABI wire differs')
            med=w['medians'];item['architectures'][name]={'coldDecodeSpeedupVsMsgpack':med['decode:msgpack']/med['decode:resource'],
                'coldMedians':med,
                'offlineEncodeCpuSecondsMedian':statistics.median(s['cpuSeconds'] for s in w['samples']['encode:resource']),
                'offlineEncodePeakRssRaw':max(s['peakRssRaw'] for s in w['samples']['encode:resource']),
                'offlineEncodeRssUnit':'KiB' if m['serial'] else 'bytes',
                'rssScope':'Whole-process high-water mark, including source preparation and result validation; not encoder-exclusive memory.'}
            item['rawReductionVsMsgpackPercent']=100*(1-len(payloads['resource'])/len(payloads['msgpack']))
            for label,enabled in [('shapes',False),('shapes_strings',True)]:
                data=ablate(payloads['resource'],payloads['msgpack'],enabled)
                item['ablations'][label]={'bytes':len(data),'gzipBytes':len(gzip.compress(data,compresslevel=6,mtime=0))}
                (matrix_path.parent/(w['name']+'.'+label)).write_bytes(data)
            report['independentValidation'].append({'matrix':name,'name':w['name'],'valueAndNumericBitsMatch':True})
    for path in a.warm:
        m=json.loads(path.read_text());name=path.parent.name
        if 'completedUtc' not in m or {w['name'] for w in m['workloads']}!=expected_names:raise RuntimeError('Incomplete warm matrix')
        report['warmMatrices'][name]={'sha256':digest(path),'binarySha256':m['binarySha256']}
        for w in m['workloads']:
            med=w['medians'];entry=report['workloads'][w['name']]['architectures'].setdefault(name,{})
            entry.update({'warmDecodeSpeedupVsMsgpack':med['decode:msgpack']/med['decode:resource'],
                'standardSpeedupVsJsonBytes':w['speedupVersusJsonBytes'],'warmMedians':med})
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'workloads':len(report['workloads']),'independentChecks':len(report['independentValidation']),'crossAbiWireIdentity':True if len(a.cold)>1 else None}))
    for name,w in report['workloads'].items():print(name,w['sizes']['msgpack']['bytes'],w['sizes']['resource']['bytes'],round(w['rawReductionVsMsgpackPercent'],2))

if __name__=='__main__':main()
