#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Plot measured per-workload latencies; ranges are process minima/maxima."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--summary', type=Path, required=True)
    parser.add_argument('--dest', type=Path, required=True)
    parser.add_argument('--vector-dest', type=Path)
    args = parser.parse_args()
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    data = json.loads(args.summary.read_text())
    families = [('canada', 'GeoJSON / Canada'), ('citm_catalog', 'Catalog / CITM'),
                ('twitter', 'Twitter / Unicode'), ('records12', '12-field mixed records'),
                ('records60', '60-field mixed records'), ('mixed_nested', 'Mixed nested / escapes')]
    args.dest.mkdir(parents=True, exist_ok=True)
    if args.vector_dest:
        args.vector_dest.mkdir(parents=True, exist_ok=True)
    for mode in ['parse', 'stringify']:
        figure, axes = plt.subplots(2, 3, figsize=(15, 8.3), sharex=False)
        for axis, (family, title) in zip(axes.flat, families):
            rows = sorted([r for r in data['branches'] if r['family'] == family and r['mode'] == mode],
                          key=lambda r: r['requested_mb'])
            x = [r['input_bytes'] / 1e6 for r in rows]
            for label, color, title_label in [('baseline', '#0072B2', '12.6'),
                                               ('current', '#D55E00', '12.6-jsonparse-async')]:
                y = [r['values'][label]['wall_ms'] for r in rows]
                low = [min(r['values'][label]['per_process_wall_ms']) for r in rows]
                high = [max(r['values'][label]['per_process_wall_ms']) for r in rows]
                axis.plot(x, y, '-o', color=color, linewidth=2, markersize=4, label=title_label)
                axis.fill_between(x, low, high, color=color, alpha=0.14, linewidth=0)
            axis.set_title(title, fontsize=11)
            axis.set_xlabel('UTF-8 input (decimal MB)', fontsize=9)
            axis.set_ylabel('API latency (ms)', fontsize=9)
            axis.set_xticks([10, 20, 50])
            axis.set_ylim(bottom=0)
            axis.grid(alpha=0.22)
            axis.spines['top'].set_visible(False)
            axis.spines['right'].set_visible(False)
        figure.suptitle(f'JSON.{mode}: Mac ARM64 / 4 independent process pairs', fontsize=15, y=0.99)
        figure.text(0.5, 0.945, 'Median of process medians; shaded range = min/max process median; 9 measured calls per process',
                    ha='center', fontsize=10, color='#444444')
        handles, labels = axes.flat[0].get_legend_handles_labels()
        figure.legend(handles, labels, loc='lower center', ncol=2, frameon=False)
        figure.tight_layout(rect=(0, 0.055, 1, 0.925), h_pad=2.2, w_pad=2)
        name = f'json-large-mac-arm64-{mode}'
        figure.savefig(args.dest / (name + '.png'), dpi=160, bbox_inches='tight')
        if args.vector_dest:
            figure.savefig(args.vector_dest / (name + '.svg'), bbox_inches='tight')
        plt.close(figure)
    print('matplotlib', matplotlib.__version__)


if __name__ == '__main__':
    main()
