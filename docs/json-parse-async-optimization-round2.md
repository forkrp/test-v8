# JSON.parseAsync 第二轮优化计划与验收记录

日期：2026-09-28。基线：`bec433731dda91bbac2926f568584ae01682c765`。

**最终状态：本轮实现与定向验收完成。**对象低并发完成时间改善，16 任务普通对象总耗时基本持平；没有新增同文档流水线或内存配额，也不承诺任意输入快于同步解析。最后交付数据统一见 `verified-*` 记录及本文最终结果。

本轮优先降低对象密集输入的完成时间和额外 CPU 工作，而非增加 worker 数或扩大主线程时间片。先保留基线二进制、采样当前 Release，再分步实施、同配置比较；只有通过正确性和性能检查的改动才保留。本文先于代码修改落盘，执行结果在本轮结束时回填。不保证任意输入快于同步解析，也不将微基准当作实际 UI 延迟。

## 范围与现状

- 保持 API、错误与 reviver 语义；不改线程池默认值，不增加依赖，不自动提交或推送。
- 现有优化包括 12 字节记录、deque 消费回收、数组预分配、批量数值写入、小子树局部句柄、Map feedback、原对象构建器及 64 槽短字符串复用；不重复实现。
- 当前路径：调用线程 ToString / Flatten / 输入复制 → worker 完整解析到原生记录 → 调用线程分片构建 → 原 reviver → Promise 完成。单文档两个阶段仍串行。
- 上轮 Release 普通对象单任务异步 12.180 ms、同步 7.195 ms；四任务异步 34.724 ms、同步串行 26.269 ms。此处仅是历史基准，不是本轮测量。
- 上轮后台 UTF-16 预解码已经因额外复制、内存和耗时回退撤回，本轮不重复该方案。

## 目标与执行顺序

| 目标 | 实施或评估范围 | 验收条件 | 初始状态 |
| --- | --- | --- | --- |
| R1 建立当轮证据 | 固定原二进制与参数，采样对象场景的主线程 / worker；核对构建器和所有调用路径 | 可定位热点，明确采样与计时差别，基线不可被覆盖 | 开始 |
| R2 降低后台控制开销 | 常见属性 / 标点状态合并，减少重复检查；必要时改用顺序记录读取 | 不删除语法验证，错误信息与位置一致，取消可响应；端到端及 CPU 对照 | 排队 |
| R3 降低对象材料化开销 | 针对实测热点缩短记录、临时属性和现有构建器间的路径；保留 Map / GC 安全 | 普通对象或不同内容对象有可复现收益，不扩大切片预算 | 排队 |
| R4 评估更大架构变更 | 根据测量判断按在途字节限流、同文档流水化是否值得 | 无证明不引入新全局调度器、自定义 arena 或跨线程可变队列；记录采纳或延后理由 | 排队 |
| R5 回归与交付 | 语义差分、错误边界、并发、GC、终止与销毁、同步控制组、吞吐与内存 | 定向测试通过，保留 / 撤回实验和限制可复现，文档与最终源码一致 | 排队 |

## 实验规则

1. 对照使用相同 GN 参数，关闭 DCHECK 的 Release；语义与安全另用 DCHECK 构建。JSON 所用 user-visible 池固定 4 或 8 个 worker，报告同时提交的文档数量。
2. 对象、数值、转义字符串、嵌套、不同内容、长字符串、Unicode 均检查；重点为 1 / 4 任务对象场景，另测 8 worker / 16 任务压力。
3. 每轮只改一个主要因素；先筛选，最终使用多轮中位数和反向顺序复核。主要场景重复出现超过约 5% 回退时，修正或撤回，不用混合不同配置数据解释。
4. 除完成时间，保留进程 CPU、峰值 RSS、前台任务最大耗时和语义结果。进程 CPU / RSS 包含启动与输入准备，采样比例不等同精确阶段耗时。
5. 不能以关闭 GC、写屏障、取消检查、语法检查或增大 2 ms 软预算换性能；独立大分配、字符串与 reviver 超预算的既有限制须继续披露。
6. 构建不与性能计时同时运行；输出与候选二进制放在忽略的 `out/json-opt2/`。本机是 macOS x64，不外推其他平台；完整 cctest 聚合构建的既有 WASM SIMD 对齐问题与定向测试分开报告。

## 初始证据 → 发现 → 路径

| 证据 | 发现 | 对应路径 / 动作 |
| --- | --- | --- |
| `docs/json-parse-async-optimization.md` 性能表 | 低并发对象场景仍有额外耗时，数字批量路径已获益 | R1 后聚焦 R2 / R3，不盲目加线程 |
| `src/json/json-parser-background.cc` 的 Parse / ReadString | 属性名、冒号、值和分隔符经过多个循环；取消与空白处理反复进入 | 检查融合常见状态能否减少控制成本，保留错误边界 |
| `src/json/json-parser-async.cc` 的 BuildSmallValue / Close | 使用原 BuildJsonObject，但仍先填充临时属性栈 | 采样并核对 builder 契约后再选择最小优化 |
| `src/json/json-parser-background.h` 的 NodeAt / DiscardBefore | deque 支持前缀回收，但逐记录寻址尚可评估 | 优先标准容器顺序遍历，若无收益则保留原实现 |
| `src/json/json-parser-async.cc` 的 JsonParseAsync / worker 完成发布 | 全量输入复制及整文档发布限制阶段重叠，任务总内存未被配额限制 | R4 明确成本与收益门槛，不直接引入复杂流水线 |

## 验证入口

现有脚本无需安装新框架：

```sh
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt2/baseline-release out/json-opt-release/d8 --runs 11 --workers 4
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt2/baseline-release out/json-opt-release/d8 --mode async --runs 9 --workers 8 --jobs 16 --cases records numbers varied
rtk proxy out/json-async/d8 --expose-gc --stress-compaction test/json/parse-async.js
rtk proxy out/json-async/cctest-json-async test-json-async/JsonParseAsyncConcurrentWorkers
```

## 执行记录

计划已先行落盘，基线二进制及 SHA-256 已保存到 `out/json-opt2/`。

### 已完成的实现与筛选

- R1：对基线 Release 连续解析普通对象数组，用 macOS `sample` 采样 5 秒，未同时编译。前台热点包含 `BuildSmallValue` / `BuildJsonObject` / `JSDataObjectBuilder`，worker 包含 `ParseJsonInBackground` / `ReadString` / `Append`。调用栈保存在 `baseline-profile.txt`，它定位热点而不是精确阶段 CPU 计时。
- R2：融合属性名后必需的冒号处理，移除独立 `kColon` 状态；父容器计数与标志共用一次寻址。字符串扫描按键 / 值编译期特化，复用同一份验证实现，使值路径不必保留数字索引键的运行时分支。根据采样中的 `Append` 热点，使用既有 `V8_INLINE` 消除逐记录 helper 调用，不更换记录容器。
- R3：小对象 / 小数组的原语直接走既有 `Primitive`，递归仅用于容器。前台改用标准 deque 的顺序迭代器，在每片起点重建，回收后不访问旧迭代器。没有改变 12 字节记录、GC 根、写屏障或 2 ms 软预算。
- 已分别保存 `state-release`、`primitive-release`、`iterator-release`、`scanner-release` 和逐轮结果；所有候选均通过现有与新增 JS 语义测试。筛选不代替最终回归，后续已执行反向复测、独立模式与最终安全检查。

### 不引入的复杂性

- 不删除原对象构建器的临时属性缓存：`BuildFromIterator` 会在确定 Map 后通过 `RevisitValues` 再次读取值；直接消费记录仍须缓存已分配的值以保持唯一 HeapNumber、GC 根及失败回退。此次先减少递归和记录寻址，不为省去一个已有 SmallVector 重写 builder 接口。
- 不新增全局内存调度器：当前任务队列确实不是字节配额，但调度前就已同步复制输入。仅限制运行 worker 无法约束全部待处理副本；完整配额需要同时设计排队、复制、取消和公平性。当前无足够证据证明它优于本轮的小改。
- 不实现同文档流式发布：它可能重叠扫描和构建，却不自动消除两者 CPU 工作；当前预分配依赖最终计数、结束偏移。没有通过跨线程提前发布、扩容或放松语义来换取单项速度。此限制仍保留，不标成已经解决。

### 同步数值控制组的布局回退

验证期间发现同步数值数组回退，不能因未改同步语法就忽略。4 次轮换进程顺序、每进程 20 轮，基线进程中位数的中位数为 8.970 ms，首个属性状态候选为 9.527 ms，未对齐的完整候选为 9.577 ms。该差异可重复，不再按单次噪声处理。

未对齐候选的符号检查显示原 `JsonParser<uint8_t>::ParseJsonNumber` 地址不变，而共享 `StringToDouble(Vector<const uint8_t>)` 的入口从 `0x10032b800` 移到 `0x10032c750`（二进制符号地址，非 ASLR 后地址）。后者不再按本机 64 字节缓存行对齐。仅使用已有 `ALIGNAS(64)` 宏对 x64 / GNU 模式编译器的该入口对齐，不改转换算法、不加运行时分支；其他架构及 MSVC 模式不启用该注解。

同一轮三方控制组：基线 9.204 ms、未对齐候选 9.828 ms、对齐候选 8.619 ms。该实验支持本机存在布局敏感性；不是所有平台对齐必然加速的证明。对齐候选随后经过完整验收，原实验记录保留在 `alignment-control.jsonl` 与 `numeric-control.jsonl`。

也对照了 32 字节对齐：同步数值为 8.893 ms，64 字节为 8.516 ms，基线为 9.216 ms；32 字节没有改善不同内容对象的控制组，故撤回。最后加入有实际热路径依据的 `Append` 内联，筛选中相对未内联 / 已对齐版本，独立异步普通对象单任务 10.219 → 9.688 ms、数值单任务 2.349 → 2.164 ms、不同内容对象单任务 6.660 → 5.820 ms。最终交付选择为 64 字节对齐加 worker 内联，而非额外填充代码或调整线程数。

## 最终配对结果

最终交付二进制为 `inline-release`（与 `out/json-opt-release/d8` 哈希相同）。`final-*` 和 `accepted-*` 是保留的中间候选记录，不能当作最后交付数据；最后验收文件统一使用 `verified-*`。

本机 macOS x64，相同 GN 参数 Release，关闭 DCHECK、i18n 和 sandbox。JSON 所用 user-visible 池固定 4 个 worker；任务数是同时提交的独立文档数，不是将单文档拆成多个 worker。下表为 15 轮中位数，单位 ms；同步列是新版串行完成同样份数的时间。

| 输入 | 任务数 | 基线异步 | 新版异步 | 耗时变化 | 新版同步 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 普通对象 | 1 | 12.021 | 10.143 | -15.6% | 6.839 |
| 普通对象 | 4 | 34.489 | 32.047 | -7.1% | 25.648 |
| 数值数组 | 1 | 2.332 | 2.171 | -6.9% | 2.466 |
| 数值数组 | 4 | 3.462 | 3.326 | -3.9% | 8.731 |
| 转义字符串数组 | 1 | 1.813 | 1.956 | +7.9% | 1.483 |
| 转义字符串数组 | 4 | 8.260 | 7.635 | -7.6% | 9.300 |
| 嵌套对象 | 1 | 13.229 | 11.764 | -11.1% | 7.466 |
| 嵌套对象 | 4 | 36.012 | 34.616 | -3.9% | 35.073 |
| 不同内容对象 | 1 | 6.948 | 6.157 | -11.4% | 3.815 |
| 不同内容对象 | 4 | 19.001 | 17.480 | -8.0% | 19.702 |
| 长转义字符串 | 1 | 2.540 | 2.575 | +1.4% | 1.350 |
| 长转义字符串 | 4 | 5.717 | 5.779 | +1.1% | 6.651 |
| Unicode 对象 | 1 | 12.106 | 10.334 | -14.6% | 6.621 |
| Unicode 对象 | 4 | 29.708 | 27.178 | -8.5% | 37.245 |

普通对象四任务异步仍比新版同步慢约 24.9%；没有宣称消除两阶段的全部成本。

### 反向顺序复测

交换二进制执行顺序，4 worker、20 轮。保留全部场景，不只挑选改善项。

| 输入 | 任务数 | 基线异步 ms | 新版异步 ms | 耗时变化 |
| --- | ---: | ---: | ---: | ---: |
| 普通对象 | 1 | 11.818 | 9.739 | -17.6% |
| 普通对象 | 4 | 34.546 | 31.844 | -7.8% |
| 数值数组 | 1 | 2.274 | 2.089 | -8.2% |
| 数值数组 | 4 | 3.423 | 3.320 | -3.0% |
| 转义字符串数组 | 1 | 2.075 | 1.724 | -17.0% |
| 转义字符串数组 | 4 | 8.145 | 7.572 | -7.0% |
| 嵌套对象 | 1 | 12.405 | 10.885 | -12.3% |
| 嵌套对象 | 4 | 36.162 | 34.281 | -5.2% |
| 不同内容对象 | 1 | 6.594 | 5.590 | -15.2% |
| 不同内容对象 | 4 | 18.638 | 17.389 | -6.7% |
| 长转义字符串 | 1 | 2.225 | 2.166 | -2.6% |
| 长转义字符串 | 4 | 5.494 | 5.572 | +1.4% |
| Unicode 对象 | 1 | 11.730 | 10.478 | -10.7% |
| Unicode 对象 | 4 | 28.934 | 26.577 | -8.1% |

### 16 任务与资源检查

固定 8 个 JSON worker，同时提交 16 份文档，独立 async 模式。CPU 秒和峰值 RSS 是整个基准子进程，包含启动、输入准备和显式 GC；不是仅解析函数，也不能跨不同轮数比较累计 CPU。每一行的基线 / 新版轮数相同。

| 输入 | 轮数 | 基线→新版 ms | 耗时变化 | CPU 秒（基线→新版） | 峰值 RSS MiB（基线→新版） |
| --- | ---: | ---: | ---: | ---: | ---: |
| 普通对象 | 15 | 110.259 → 109.912 | -0.3% | 5.536 → 5.227 | 474.5 → 476.2 |
| 数值数组 | 15 | 9.055 → 8.955 | -1.1% | 0.707 → 0.678 | 127.8 → 127.5 |
| 不同内容对象 | 15 | 61.048 → 57.726 | -5.4% | 3.224 → 2.967 | 399.7 → 410.4 |
| 普通对象 | 20 | 111.206 → 111.108 | -0.1% | 7.616 → 7.046 | 569.4 → 517.4 |
| 数值数组 | 20 | 9.118 → 8.690 | -4.7% | 0.916 → 0.856 | 130.6 → 133.1 |
| 不同内容对象 | 20 | 59.592 → 58.436 | -1.9% | 4.207 → 3.867 | 425.5 → 412.9 |

普通对象 16 任务两轮总耗时基本持平，虽然基准进程 CPU 分别下降约 5.6% 和 7.5%，不能把 CPU 变化当作同幅度完成时间收益。高并发与内存收益须按场景判断；没有增加线程、降低保留结果数量或声称 RSS 一律下降。

### 独立同步控制组

4 worker 配置、4 份串行解析、20 轮，避免混合模式中的异步任务影响判断。不同内容对象约 +4.2%、Unicode 约 +3.8% 的剩余差异如实保留，不宣称同步各场景都加速。数值控制组的可重复回退已修正。

| 输入 | 基线 ms | 新版 ms | 耗时变化 |
| --- | ---: | ---: | ---: |
| 普通对象 | 25.430 | 25.583 | +0.6% |
| 数值数组 | 8.962 | 8.658 | -3.4% |
| 转义字符串数组 | 9.081 | 8.884 | -2.2% |
| 嵌套对象 | 35.260 | 34.611 | -1.8% |
| 不同内容对象 | 18.685 | 19.461 | +4.2% |
| 长转义字符串 | 6.694 | 6.562 | -2.0% |
| Unicode 对象 | 35.210 | 36.565 | +3.8% |

### 主线程诊断

DCHECK 原生时序测试交替执行基线与新版，各 5 个独立进程；不是上面的 Release 配置。worker_wait 是前台等待时长，不是后台 CPU。每项独立取中位数，不能要求分项之和等于总中位数。

| 指标 | 基线中位数 | 新版中位数 |
| --- | ---: | ---: |
| 初始调用 ms | 0.895 | 0.907 |
| 前台等待 ms | 7.416 | 6.257 |
| 前台累计 ms | 31.244 | 31.123 |
| 前台片数 | 26.000 | 26.000 |
| 每次运行最长片段 ms | 3.387 | 3.409 |
| 总完成时间 ms | 39.442 | 38.230 |

五次运行最大单片：基线 3.450 ms，新版 3.440 ms。2 ms 软预算没有扩大；这些样本不是硬实时保证，也没有验证宿主 UI 帧延迟。

## 完成状态与限制

| 目标 | 状态 | 交付 |
| --- | --- | --- |
| R1 | 完成 | 原二进制 / SHA-256、无并行编译的 Release 采样、固定线程对照 |
| R2 | 完成 | 属性状态合并、键 / 值扫描特化、热 Append 内联、共享数值入口对齐 |
| R3 | 完成 | 原语免递归、标准 deque 顺序读取；复用原对象构建器与 Map 检查 |
| R4 | 评估完成，未引入 | 字节配额与同文档流水线成本仍需独立验证；不是已实现功能 |
| R5 | 完成定向验收 | Release / DCHECK 语义与 GC、10 项原生用例、GN 依赖、静态检查和重复基准 |

新增回归覆盖属性名 / 冒号之间的 4096 字符空白、Unicode 与错误消息，以及跨块 / 跨片的形状变化、重复键、索引键、double 可变存储不共享。10 项原生用例为 CompactRecords、SnapshotTableGrowth、YieldsAndSurvivesGC、RejectsWithoutThrowing、ConcurrentWorkers、PrimitiveArraysGC、TaskTiming、Termination、TasksOutliveIsolate、QueuedWorkerOutlivesIsolate。

JSON 两个修改文件的 cpplint 按仓库既有过滤规则通过；conversions.cc 的既有 SharedStringAccessGuardIfNeeded 非 const 引用诊断经 HEAD 源码核对后，仅对该文件另排除 runtime/references，该行未修改。GN 头文件依赖和 git diff --check 通过。

本轮没有重跑完整 cctest 聚合套件，没有验证其他平台、Bazel、sandbox 或生产 PGO 构建。此前 WASM SIMD 对齐导致的聚合构建问题不在此修改范围。单个大分配、输入 Flatten / 复制、GC 和 reviver 仍可能产生长任务；总在途内存没有硬配额。

## 最终复现与证据

以下命令使用本机保留的构建产物；换机器需从基线提交与最终源码分别以相同 GN 参数构建。聚焦 cctest 的本地 Ninja 清单位于 out/json-async/json-async-tests.ninja，不等同完整套件。

```sh
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt2/baseline-release out/json-opt-release/d8 --runs 15 --workers 4
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt-release/d8 out/json-opt2/baseline-release --runs 20 --workers 4
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt2/baseline-release out/json-opt-release/d8 --mode async --runs 15 --workers 8 --jobs 16 --cases records numbers varied
rtk proxy python3 test/json/parse-async-benchmark.py out/json-opt2/baseline-release out/json-opt-release/d8 --mode sync --runs 20 --workers 4 --jobs 4
rtk proxy out/json-async/d8 --expose-gc --thread-pool-size=4 --stress-compaction test/json/parse-async.js
rtk proxy out/json-async/cctest-json-async test-json-async/JsonParseAsyncConcurrentWorkers
```

所有最终原始样本在 out/json-opt2/verified-*.jsonl、verified-latency.json，验证输出在 verified-*.log。final-provenance.json 保存基线核对、GN 参数、最终源码与二进制 SHA-256。中间候选与被撤回的 32 字节对齐保留为实验记录，不作为交付实现。

源代码、测试与本文留在项目中；二进制 / 采样 / 原始日志留在忽略的 out/。提交与推送遵循后续单独授权，以 Git 历史和远端状态为准。
