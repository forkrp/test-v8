#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Plot the frozen mixed-parse fix; ranges are process medians, not CIs."""
import argparse
import json
from pathlib import Path


def plot(out, root):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    summary=json.loads((out/'results/delivery-summary.json').read_text())
    families=['canada','citm_catalog','twitter','records12','records60','mixed_nested']
    names=['Canada / GeoJSON','CITM catalog','Twitter / Unicode',
           'Records, 12 fields','Records, 60 fields','Mixed nested / escapes']
    styles=[('v12','12.6 reference','#666666','s','--'),
            ('before','Frozen parent','#df8b17','^',':'),
            ('final','After fix','#2165b5','o','-')]
    plt.rcParams.update({'font.size':10,'axes.spines.top':False,'axes.spines.right':False})
    fig,axes=plt.subplots(3,2,figsize=(12,10),layout='constrained')
    fig.suptitle('JSON.parse on Mac ARM64 — same 10 / 20 / 50 MB corpus',fontsize=15)
    for ax,family,title in zip(axes.flat,families,names):
        cells=sorted([r for r in summary['cells'] if r['family']==family and r['mode']=='parse' and r['jobs']==1],key=lambda r:r['requested_mb'])
        xs=[r['requested_mb'] for r in cells]
        for role,label,color,marker,linestyle in styles:
            ys=[r['values'][role]['wall_ms'] for r in cells]
            low=[y-min(r['values'][role]['per_process_wall_ms']) for r,y in zip(cells,ys)]
            high=[max(r['values'][role]['per_process_wall_ms'])-y for r,y in zip(cells,ys)]
            ax.errorbar(xs,ys,yerr=[low,high],color=color,marker=marker,linestyle=linestyle,
                        linewidth=1.7,capsize=3,markersize=5,label=label)
        ax.set_title(title,loc='left',fontweight='bold')
        ax.set_xticks([10,20,50]);ax.set_xlim(7,53);ax.set_ylim(bottom=0)
        ax.set_xlabel('Target UTF-8 input (decimal MB)');ax.set_ylabel('Parse wall time (ms)')
        ax.grid(axis='y',alpha=0.22)
    handles,labels=axes.flat[0].get_legend_handles_labels()
    fig.legend(handles,labels,loc='outside lower center',ncol=3,frameon=False)
    image=root/'docs/images/json-mixed-parse-fix-mac-arm64.png';image.parent.mkdir(exist_ok=True)
    fig.savefig(image,dpi=180)
    (out/'plots').mkdir(exist_ok=True)
    fig.savefig(out/'plots/json-mixed-parse-fix-mac-arm64.svg')
    plt.close(fig)
    print('PLOT',image)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--root',type=Path,required=True)
    args=parser.parse_args()
    plot(args.out.resolve(),args.root.resolve())


if __name__=='__main__':
    main()
