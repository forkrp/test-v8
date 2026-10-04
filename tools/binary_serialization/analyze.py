#!/usr/bin/env python3
"""Validate fixed-code device batches and aggregate medians with size metrics."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

ROOT=Path(__file__).resolve().parents[2]
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    p=argparse.ArgumentParser()
    p.add_argument('--arm64-a',type=Path,required=True)
    p.add_argument('--arm64-b',type=Path,required=True)
    p.add_argument('--arm32-a',type=Path,required=True)
    p.add_argument('--arm32-b',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    summary={'sourceCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
      'architectures':{},'validation':{'sameSourceAndBinaryAcrossBatches':True,
        'samePayloadBytesAcrossArchitectures':True,'allCorpusRoundTripsAndCPeerChecksPassed':True},
      'scope':'OnePlus 6 Android API35, both native ARM64 and ARM32. Fully materialized plain JS graphs; no JS codec. Warm native operations with fixed performance-core CPU mask.',
      'limits':'Peak RSS includes initialization, warmup, timing, and five retained graphs. Compression figures measure gzip level6 size only. Experimental internal API; no production GN library target/public binding yet.'}
    shared_sizes=None
    for architecture,paths in [('arm64',[args.arm64_a,args.arm64_b]),('arm32',[args.arm32_a,args.arm32_b])]:
        reports=[json.loads(path.read_text()) for path in paths]
        assert all('completedUtc' in report and len(report['workloads'])==9 for report in reports),'Incomplete batch'
        assert reports[0]['seed']!=reports[1]['seed'],'Repeat must use an independent execution order'
        assert all(report['rounds']==5 for report in reports),'Expected two five-round batches'
        assert reports[0]['binarySha256']==reports[1]['binarySha256'],'Binary changed between batches'
        assert reports[0]['scriptSha256']==reports[1]['scriptSha256']==digest(ROOT/'tools/binary_serialization/run.py'),'Runner changed between batches'
        assert reports[0]['buildManifestSha256']==reports[1]['buildManifestSha256'],'Build inputs changed between batches'
        manifest_path=paths[0].parent.parent/'build-manifest.json'
        manifest=json.loads(manifest_path.read_text())
        assert digest(manifest_path)==reports[0]['buildManifestSha256'],'Manifest fingerprint mismatch'
        assert digest(manifest['binary'])==reports[0]['binarySha256'],'Current binary mismatch'
        assert all(digest(ROOT/source)==expected for source,expected in manifest['sources'].items()),'Current source mismatch'
        assert digest(ROOT/'src/objects/js-data-object-builder.h')==manifest['builderSha256'],'Builder mismatch'
        for report in reports:
            assert report['measurementPlatform']=='android-'+architecture,'Wrong target ABI'
            assert report['device']['binarySha256Remote']==report['binarySha256'],'Device binary mismatch'
            assert report['device']['serial']==reports[0]['device']['serial']
            assert report['device']['cpuMask']==reports[0]['device']['cpuMask'],'CPU affinity changed'
            assert report['selfTest']['selfTest']=='PASS'
        items=[]
        for left,right in zip(reports[0]['workloads'],reports[1]['workloads']):
            assert left['name']==right['name'] and left['sourceSha256']==right['sourceSha256']
            assert left['sizes']==right['sizes'],'Payload changed between batches'
            for item in [left,right]:
                assert all(peer['cMasterWireRoundTrip']=='PASS' for peer in item['cMasterInteroperability'].values())
                for codec,info in item['sizes'].items():
                    payload=paths[0 if item is left else 1].parent/(item['name']+'.'+codec)
                    assert digest(payload)==info['sha256'],'Payload fingerprint mismatch'
            medians={}
            for key in left['samples']:
                assert len(left['samples'][key])==len(right['samples'][key])==5,'Incomplete timing rounds'
                combined=left['samples'][key]+right['samples'][key]
                medians[key]={metric:statistics.median(sample[metric] for sample in combined)
                  for metric in ['millisecondsPerOperation','retainedHeapBytesPerGraph','peakRssBytes']}
            json_size=left['sizes']['json']['bytes']
            json_decode=medians['decode:json']['millisecondsPerOperation']
            json_encode=medians['encode:json']['millisecondsPerOperation']
            items.append({'name':left['name'],'sizes':left['sizes'],'medians':medians,
              'msgpack32SizeReductionPercent':100*(1-left['sizes']['msgpack32']['bytes']/json_size),
              'msgpackDecodeSpeedup':json_decode/medians['decode:msgpack']['millisecondsPerOperation'],
              'msgpackEncodeSpeedup':json_encode/medians['encode:msgpack']['millisecondsPerOperation'],
              'batchDecodeMs':{codec:[item['medians']['decode:'+codec]['millisecondsPerOperation'] for item in [left,right]]
                 for codec in ['json','msgpack','mpack','v8']}})
        sizes={item['name']:{codec:info for codec,info in item['sizes'].items() if codec!='v8'} for item in items}
        if shared_sizes is None:shared_sizes=sizes
        else:assert shared_sizes==sizes,'Standard payload differs between ARM64 and ARM32'
        summary['architectures'][architecture]={'binarySha256':reports[0]['binarySha256'],
          'device':reports[0]['device']['serial'],'cpuMask':reports[0]['device']['cpuMask'],
          'seeds':[report['seed'] for report in reports],'samplesPerCodec':10,
          'sources':manifest['sources'],'builderSha256':manifest['builderSha256'],
          'dependencyCommits':{key:manifest[key] for key in ['msgpackCommit','msgpackCCommit','mpackCommit']},
          'batchFiles':[str(path) for path in paths],'batchSha256':[digest(path) for path in paths],
          'buildManifest':str(manifest_path),'buildManifestSha256':digest(manifest_path),
          'selfTest':reports[0]['selfTest'],'workloads':items}
    args.output.write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary['validation']))
    for architecture,info in summary['architectures'].items():
        print(architecture)
        for item in info['workloads']:
            print(item['name'], 'decode speedup',round(item['msgpackDecodeSpeedup'],3),
              'encode speedup',round(item['msgpackEncodeSpeedup'],3),
              'compact size reduction',round(item['msgpack32SizeReductionPercent'],1))

if __name__=='__main__':main()
