#!/usr/bin/env python3
# Copyright 2026 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Derive the committed result tables and report from every formal process."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import statistics

FAMILIES = ['canada', 'citm_catalog', 'twitter', 'records12', 'records60', 'mixed_nested']
LABELS = {'canada': 'GeoJSON / Canada', 'citm_catalog': '目录 / CITM',
          'twitter': 'Twitter / Unicode', 'records12': '12 字段混合记录',
          'records60': '60 字段混合记录', 'mixed_nested': '混合嵌套 / 转义'}


def load(path):
    return json.loads(path.read_text())


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def generate(out, root):
    tags = ['branches-final', 'async-final', 'concurrent-final']
    results = {}
    processes = batches = 0
    for tag in tags:
        meta = load(out / 'results' / (tag + '-metadata.json'))
        assert meta.get('finished_at'), tag
        rows = [json.loads(line) for line in (out / 'results' / (tag + '-raw.jsonl')).read_text().splitlines()]
        for row in rows:
            assert row['output'][0]['validated_all_results']
            assert len(row['output'][0]['measurements']) == 9
        processes += len(rows)
        batches += len(rows) * 9
        results[tag] = load(out / 'results' / (tag + '-summary.json'))
    branches, asynchronous, concurrent = (results[t] for t in tags)
    assert (len(branches), len(asynchronous), len(concurrent)) == (36, 18, 12)
    assert (processes, batches) == (408, 3672)
    for label in ['baseline', 'current']:
        manifest = load(out / 'manifests' / (label + '.json'))
        assert digest(out / 'binaries' / (label + '-d8')) == manifest['binary_sha256']
    fixtures = load(out / 'fixtures/manifest.json')['fixtures']
    for fixture in fixtures:
        assert digest(out / 'fixtures' / fixture['path']) == fixture['sha256']
    expected = load(out / 'expected.json')
    validation = load(out / 'results/validation.json')
    selection = load(out / 'results/conformance-selection.json')
    for check in validation['conformance']:
        assert check['failures'] == 0
    sizes = []
    for size in [10, 20, 50]:
        item = {'requested_mb': size}
        for mode in ['parse', 'stringify']:
            selected = [r for r in branches if r['requested_mb'] == size and r['mode'] == mode]
            gm = math.exp(statistics.mean(math.log(r['speedup']) for r in selected))
            pair_gm = [math.exp(statistics.mean(-math.log(1 - r['paired_less_time_percent'][p] / 100)
                        for r in selected)) for p in range(4)]
            item[mode] = dict(geomean_speedup=gm, geomean_less_time_percent=100*(1-1/gm),
                              paired_geomean_speedups=pair_gm,
                              time_lower_range_percent=[min(100*(1-1/v) for v in pair_gm),
                                                        max(100*(1-1/v) for v in pair_gm)])
        sizes.append(item)
    concurrency = []
    for fixture in fixtures:
        if fixture['family'] not in ['records12', 'records60']:
            continue
        cells = {r['mode']: r['values']['current'] for r in concurrent if r['fixture'] == fixture['name']}
        concurrency.append(dict(fixture=fixture['name'], family=fixture['family'],
             requested_mb=fixture['requested_mb'], input_bytes=fixture['bytes'],
             serial_parse_4=cells['parse'], parse_async_4=cells['parseAsync'],
             async_less_time_percent=100*(1-cells['parseAsync']['wall_ms']/cells['parse']['wall_ms'])))
    summary = dict(version=1, date='2026-10-26', platform='Mac ARM64',
                   source_commits={label:load(out/'manifests'/f'{label}.json')['commit'] for label in ['baseline','current']},
                   binaries={label:load(out/'manifests'/f'{label}.json')['binary_sha256'] for label in ['baseline','current']},
                   source_pins=load(out/'source-pins.json'), build=load(out/'manifests/build-comparison.json'),
                   fixture_manifest=fixtures, validation=validation,
                   conformance_selection=selection, processes=processes, measured_batches=batches,
                   warmups_per_process=2, samples_per_process=9, pairs=4,
                   geomeans_by_size=sizes, branches=branches, async_single=asynchronous,
                   concurrent=concurrency,
                   negative_aggregate_rows=[r for r in branches if r['less_time_percent'] < 0],
                   negative_paired_median_rows=[r for r in branches if r['paired_median_less_time_percent'] < 0])
    evidence = root/'tools/json_large/evidence/mac-arm64-20261026.json'
    evidence.parent.mkdir(parents=True, exist_ok=True)
    evidence.write_text(json.dumps(summary, indent=2)+'\n')
    (out/'results/report-summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    archive = Path('/Users/james/projects/v8/v8-standalone-worktree-docs/2026-10-26/json-large-mac-arm64-6f5f120c7-n9dc_ije')
    text = [f'''# 10/20/50 MB JSON：Mac ARM64 分支性能对比

日期：2026-10-26。受测优化版本为 `12.6-jsonparse-async` 的
`{summary['source_commits']['current']}`，基线为本地 `12.6` 的
`{summary['source_commits']['baseline']}`。两者的 V8 版本号均为 `12.6.228.49`。
同形对象直接构建与后台栈伪共享修复已在第一次本地提交 `6f5f120c7` 中提交，并快进
合并回 `12.6-jsonparse-async`。本报告与测试工具随后单独本地提交。

**本表是两个分支整体的比较。** 优化分支含此前的解析、序列化、数字转换和异步支持等
多轮改动，不能把全部差异归因于最后一次同形对象优化。测试没有更新或移动本地 `12.6` 分支。

## 汇总结果

下表对每个体积的六类输入等权计算加速比几何平均。加速比为基线耗时/优化版耗时；
耗时降低为 `1 - 优化版/基线`。负值表示变慢。完整逐场景数据及全部负面结果在后文。

| 目标体积 | JSON.parse 加速比 | parse 耗时降低 | JSON.stringify 加速比 | stringify 耗时降低 |
|---|---:|---:|---:|---:|
''']
    for row in sizes:
        p, s = row['parse'], row['stringify']
        text.append(f"| 约 {row['requested_mb']} MB | {p['geomean_speedup']:.3f}× | {p['geomean_less_time_percent']:.2f}% | {s['geomean_speedup']:.3f}× | {s['geomean_less_time_percent']:.2f}% |\n")
    text.append('''
## 测试数据与网上来源

采用 [nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) 中常用的
`canada.json`、`citm_catalog.json`、`twitter.json`：分别提供数值密集的地理数据、
大量目录/映射对象，以及含 Unicode 的嵌套 API 响应。原始文件、README、许可证及 SHA-256
均保留，固定源提交为 `478d5727c2a4048e835a29c65adecc7d795360d5`。

公开种子先解析并紧凑化，再将完整子文档重复放入外层数组，按最接近目标体积的份数生成；
它们是明确标注的派生数据，不是原始小文件本身。原有类型与嵌套结构保留，解析过程仍需为
重复文档分别构造结果。生成记录的字段、ID 和部分字符串按序号变化。

额外生成的 12/60 字段记录包含整数、分数、负零、字符串、布尔值、null 和 12 位整数。
这两个名称在本轮代表新的混合类型 schema，与上一轮较小输入的同名场景不同。
混合嵌套场景包含多级对象/数组、字段类型变化、前缀形状变化、索引键、`__proto__` 数据键、
含转义的键、CJK、emoji、引号、反斜杠、换行、tab、NUL 和极小/极大有限数。
体积由这些结构化内容组成。

体积采用十进制 MB：1 MB = 1,000,000 个 UTF-8 字节。公开种子按完整文档扩展，实际体积
与目标的偏差最多约 4.51%，下表列出实际数值。API 接收的是解码后的 JavaScript 字符串，
UTF-16 长度与 UTF-8 文件字节数都保存在结果中。

| 输入族 | 实际约 10 MB | 实际约 20 MB | 实际约 50 MB |
|---|---:|---:|---:|
''')
    for family in FAMILIES:
        fs = sorted([f for f in fixtures if f['family']==family], key=lambda f:f['requested_mb'])
        text.append('| '+LABELS[family]+' | '+' | '.join(f"{f['bytes']/1e6:.6f} MB" for f in fs)+' |\n')
    text.append('''
50 MB 文件的完整遍历统计如下。深度包含叶节点，数字位模式和所有字符串内容均参与验证。

| 输入族 | 对象数 | 数组数 | 数字数 | 字符串值数 | 最大值深度 |
|---|---:|---:|---:|---:|---:|
''')
    for family in FAMILIES:
        count=expected[f'{family}-50mb']['tree']['counts']
        text.append(f"| {LABELS[family]} | {count['objects']:,} | {count['arrays']:,} | {count['numbers']:,} | {count['strings']:,} | {count['max_depth']} |\n")
    text.append('''
## 设备、构建和测量口径

- Apple M1 Pro，8 个 CPU 核心、16 GiB 内存，MacBookPro18,3，macOS 27.0.1 / build 26A434。
  测量期间读取的电源状态为 AC Power，系统未记录 thermal/performance warning。
- 两个版本均使用配置指定的 Apple clang 21.0.0（clang-2100.3.34.2）、MacOSX27.0 SDK、
  ARM64、部署目标 11.0、官方 Release `-O3`、无 ThinLTO/PGO、系统 libc++；
  i18n、Wasm、sandbox 关闭。基线在独立工作树原样构建，无兼容性源代码补丁。
- 用户 GN 参数文件逐字节相同；解析 SDK 相对路径后，实际 C/C++ 编译选项相同。
  优化分支多一条既有的 `HWY_BROKEN_TARGETS=HWY_ALL_SVE` 全局定义，完整解析后的配置保留。
- 两者运行参数统一为 `--expose-gc --thread-pool-size=4 --max-old-space-size=4096`。
  文件读取和 UTF-8 解码在计时外，JSON.parse/parseAsync 的字符串输入保留；
  JSON.stringify 的普通数据对象由 JSON.parse 在计时外构造。
- 不传 reviver、replacer 或 space。每次计时前显式 GC；计时内自然 GC 保留。
  每次结果均在计时后完整验证，并清理结果/Promise 数组，再进行下一次预计时 GC。
- 每个独立进程预热 2 次、计时 9 次，使用进程中位数；分支比较每项 4 轮相邻独立进程配对，交换
  基线/优化版执行先后；async 补充阶段对同一输入作 4 次独立进程重复。固定随机顺序种子
  20261026。主表为四个进程中位数的中位数。
  预热数据和所有慢样本也保留，不取最快一轮。
- 正式计时与编译、下载、绘图不重叠。CPU/RSS 附加指标属于整个进程，含读入、解码、
  校验和显式 GC，不作为纯 API 内存分配量。
- 分支对比共 288 个进程；当前分支的 async 单任务 72 个进程，四任务对照 48 个进程。
  总共 **408 个进程、3,672 个计时批次**。四任务行的延迟为整批完成时间，四个调用使用同一份已读入的输入字符串。

## JSON.parse：两个分支

单位 ms。正的耗时降低比例表示优化分支更快。

| 输入族 | 目标 MB | 12.6 | 12.6-jsonparse-async | 耗时降低 | 四轮配对降低比例范围 |
|---|---:|---:|---:|---:|---:|
''')
    for mode in ['parse','stringify']:
        if mode=='stringify':
            text.append('''
![JSON.parse latency](images/json-large-mac-arm64-parse.png)

## JSON.stringify：两个分支

单位 ms。输出为紧凑 JSON，完整 UTF-8 输出的 SHA-256 已跨版本核对。

| 输入族 | 目标 MB | 12.6 | 12.6-jsonparse-async | 耗时降低 | 四轮配对降低比例范围 |
|---|---:|---:|---:|---:|---:|
''')
        for family in FAMILIES:
            for size in [10,20,50]:
                r=next(r for r in branches if r['family']==family and r['requested_mb']==size and r['mode']==mode)
                v=r['values'];d=r['paired_less_time_percent']
                text.append(f"| {LABELS[family]} | {size} | {v['baseline']['wall_ms']:.3f} | {v['current']['wall_ms']:.3f} | {r['less_time_percent']:.2f}% | {min(d):.2f}% ～ {max(d):.2f}% |\n")
    text.append('''
![JSON.stringify latency](images/json-large-mac-arm64-stringify.png)

图中的阴影/误差范围表示四个进程中位数的最小/最大值，不是置信区间。

## 当前分支 JSON.parseAsync

原始 `12.6` 没有 JSON.parseAsync，因此这里只列当前分支的异步测量。
单任务总延迟包含 API 入口复制、排队、后台原生解析、前台对象构建和 Promise 完成。
入口耗时表示 API 调用返回 Promise 所用的时间。它与整体解析完成时间含义不同。

| 输入族 | 目标 MB | parse 单任务 ms | parseAsync 单任务 ms | async 入口 ms |
|---|---:|---:|---:|---:|
''')
    for family in FAMILIES:
        for size in [10,20,50]:
            p=next(r for r in branches if r['family']==family and r['requested_mb']==size and r['mode']=='parse')['values']['current']
            a=next(r for r in asynchronous if r['family']==family and r['requested_mb']==size)['values']['current']
            text.append(f"| {LABELS[family]} | {size} | {p['wall_ms']:.3f} | {a['wall_ms']:.3f} | {a['entry_ms']:.3f} |\n")
    text.append('''
上表 sync 与 async 来自各自独立的测量阶段，适合描述端到端延迟，不作为同进程配对的
因果对照。四任务补充实验在同一阶段分别测量串行调用四次 parse 和四个并发 parseAsync。

| 输入族 | 每个输入目标 MB | 四次串行 parse ms | 四个并发 parseAsync ms | async 整批耗时降低 | async 入口 ms |
|---|---:|---:|---:|---:|---:|
''')
    for r in concurrency:
        text.append(f"| {LABELS[r['family']]} | {r['requested_mb']} | {r['serial_parse_4']['wall_ms']:.3f} | {r['parse_async_4']['wall_ms']:.3f} | {r['async_less_time_percent']:.2f}% | {r['parse_async_4']['entry_ms']:.3f} |\n")
    text.append('''
## 保留的负面结果

下表列出汇总耗时增加的所有分支对比项，负值表示优化分支变慢；上文完整表还给出了
每项四轮中的负面样本范围。它们均保留在原始数据和提交的汇总 JSON 中。

| 输入族 | API | 目标 MB | 耗时降低（负为减速） | 配对变化中位数 |
|---|---|---:|---:|---:|
''')
    if summary['negative_aggregate_rows']:
        for r in sorted(summary['negative_aggregate_rows'], key=lambda r:r['less_time_percent']):
            text.append(f"| {LABELS[r['family']]} | {r['mode']} | {r['requested_mb']} | {r['less_time_percent']:.2f}% | {r['paired_median_less_time_percent']:.2f}% |\n")
    else:
        text.append('| 本轮无汇总减速项 | — | — | — | — |\n')
    text.append('''
JSON 形状、数字分布、字符串编码、对象数量和 GC 共同决定结果。几何平均只描述本轮六类
输入的等权汇总；单个场景仍应使用对应行。本轮对 50 MB 大文件的结果也不应直接套用
上一轮较小、schema 不同的同形对象微基准百分比。

## 正确性与公开测试集

- 18 个大型文件在两个分支上完整遍历，键顺序、类型、数字 IEEE-754 位模式（含负零）、
  字符串内容、数组长度和普通对象原型参与指纹；两支结果一致。
- 验证阶段为每个大型输入生成完整 canonical JSON，通过 stdout 临时流逐字节计算 SHA-256，
  两支的完整 UTF-8 输出字节数及 SHA-256 全部一致。
- 每次预热和计时后的解析结果均全树验证；每次序列化结果验证全字符串长度及两个 32 位
  摘要。验证阶段的完整输出 SHA-256 与测量阶段的摘要用途分别记录。
- [JSONTestSuite](https://github.com/nst/JSONTestSuite) 固定在
  `1ef36fa01286573e846ac449e8683f8833c5b26a`，下载 318 个 parsing 文件并核对 Git blob/SHA-256。
  其中 95 个 `y_` 必须接受、176 个适用的 `n_` 必须拒绝，另 22 个 `i_` 可由实现决定。
  基线 parse、当前 parse 和当前 parseAsync 的 271 个必选判断全部通过，22 个可选判断及
  其接受结果的全树指纹也全部相同，共 879 次案例执行。
- 另外 25 个文件含非法 UTF-8 原始字节，在严格解码阶段排除并保留清单。
  [ECMAScript JSON.parse](https://tc39.es/ecma262/multipage/structured-data.html#sec-json.parse) 接收经 ToString 得到的字符串，
  原始字节解码不属于这个 API 的语法测试；本报告没有把解码失败算成解析器正确拒绝。
- 当前代码提交前复跑了现有容器栈和同形对象回归套件，分别为 312/187 次比较，哈希与
  前一轮冻结结果一致。上一轮完整 Release/DCHECK 验证记录仍在原报告和归档中。

## 原始材料与复现

''')
    text.append(f'''- 可执行工具：[tools/json_large](/Users/james/projects/v8/v8-standalone/tools/json_large)。
- 提交的完整汇总：[mac-arm64-20261026.json](../tools/json_large/evidence/mac-arm64-20261026.json)。
- 原始实验目录：`{out}`。
- 外部归档：`{archive}`；`verification.json` 给出文件尺寸、SHA-256 和归档校验结果。
- `fixtures/` 保存全部 18 个实际 JSON 文件和体积/UTF-16 长度/来源/哈希清单。
- `downloads/` 保存固定源版本、许可证、公开案例和所有编码排除清单。
- `binaries/`、`manifests/`、`machine.json` 保存实际 d8、提交、构建参数、完整编译选项和设备信息。
- `results/*-raw.jsonl` 保存所有预热、计时、命令、执行顺序、进程 CPU/RSS 和负载；
  CPU/RSS 包含校验开销。`large-validation-raw.jsonl` 保存完整输出字节数和 SHA-256。
- `fetch.py` 与 `generate.py` 可重新取得固定源并生成文件；`run.py` 支持从解压后的归档运行。
  设置 `V8_JSON_ROOT` 指向仓库，使用新的 `--tag`，不要覆盖原始样本。

测试全部在本机 Mac ARM64 完成，结果不代表 Android、ARM32 或其他构建参数。
''')
    (root/'docs/json-large-mac-arm64.md').write_text(''.join(text))
    print(evidence)
    print(root/'docs/json-large-mac-arm64.md')
    print(json.dumps(sizes,indent=2))


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    args=parser.parse_args()
    generate(args.out.resolve(),args.root.resolve())


if __name__=='__main__':
    main()
