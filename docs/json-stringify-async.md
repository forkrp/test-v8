# JSON.stringifyAsync 设计、目标与验收

后续的官方同步优化回移及异步复用见 [V8 14.4 JSON 回移记录](json-upstream-backport.md)。本文性能数字保留为引入 stringifyAsync 时的历史基线，不应作为当前 HEAD 的同步分母。

日期：2026-09-28。本分支已有 JSON.parseAsync；本轮目标是新增真正使用平台工作线程的 `JSON.stringifyAsync(value[, replacer[, space]])`，而不是把同步 stringify 包在 Promise 中。性能目标用同机固定线程、独立基线和端到端测量验收，不承诺未经验证的“所有输入最快”。

**验收结论：**大字符串、数值数组和相应并发负载有显著收益；任意对象的语义采集仍同步。4 worker / 4 任务的数值数组约快 5.00 倍，10 MB 无转义字符串约快 11.41 倍，但嵌套对象批次慢约 16.2%，并消耗更多 CPU / RSS。不要把该 API 当作所有输入的自动替代品，也没有测量宿主 UI 的帧率。

## 语义与线程边界

任意 JS 对象可能含 getter、toJSON、replacer、Proxy、循环引用和执行期间的属性变更。它们必须在调用方 isolate 上按原 stringify 的顺序执行。工作线程不能读取该 isolate 的 JSObject、Map、String 裸指针，也不能持有这些指针绕过 GC。

首选方案是**同步语义采集、异步原生编码**：复用原 JsonStringifier 遍历，在调用返回前采集本次实际观察到的原生值 / 字符快照；worker 执行转义、数字格式化与结果缓冲区构建，前台只交付最终字符串和 Promise。调用后的对象修改不改变已采集的输出，回调不会在 worker 或以后重新执行。采集本身仍有主线程成本，不宣称任意对象图都没有长任务。

结果交付优先采用 V8 原生 external string 所有权转移，避免再次整份复制；先验证长度、资源生命周期、编码及失败路径。不存在安全的“直接让线程池运行原同步 stringify”：原实现会读取可变堆、触发 JS，并参与 GC。

## 目标

| 编号 | 工作 | 完成证据 |
| --- | --- | --- |
| S1 | 冻结同步 / parseAsync 基线，追踪已有 stringifier 和任务生命周期 | 完成：修改前 `aa2706c5edc10c245891d49576ce2c6956b67380`；独立 baseline 二进制、构建参数和 SHA-256 |
| S2 | 最小改造原 stringifier，生成纯原生输入；复用转义和数值格式化 | 完成：`stringify-async.js` 语义 / 事件顺序 / 640 项固定种子差分；冻结原同步实现的逐字节对照 |
| S3 | 真实平台 worker 编码，安全完成、取消和销毁 | 完成：9 项新增原生检查，含真实双 worker 屏障、JS / GC、realm、终止、丢弃任务和 isolate 销毁 |
| S4 | 优化批量数字数组、重复键 / 字符串、缓冲区和结果交付 | 完成：1 / 4 / 8 worker，1 / 4 / 16 任务；wall、入口占用、CPU / RSS、同步控制和反向复测 |
| S5 | 回归 parseAsync，记录边界、有效 / 撤回方案与复现步骤 | 验证完成：Release / DCHECK / 压缩 GC、全部 20 项定向原生检查、lint、GN 依赖；本地提交记录以 Git 的 Signed-off-by 为准，不推送 |

不新增 JSON 专属线程池，不把 getter 等用户 JS 搬到后台，也不先同步产生完整 JSON 再假装异步。若原生采集方案不能形成实测收益，将调整实现，而非降低验收范围。不会推送远端。

## Evidence → Finding → Path

| 已有证据 | 发现 | 实施路径 |
| --- | --- | --- |
| `src/json/json-stringifier.cc` 的 Serialize_ / ApplyToJsonFunction / ApplyReplacerFunction | 语义访问与文本写出混合，且有 NEED_STACK 重试约定 | 保留遍历逻辑，采集模式必须同步清空重试前的原生输出 |
| SerializeFixedArrayWithInterruptCheck / SerializeStringUnchecked_ | 现有数字数组快路径和转义逻辑可复用 | 优先批量原生快照、共享纯字符编码，而非重写对象枚举 |
| `src/json/json-parser-async.cc` 的 Managed、CancelableTask、任务登记 | 已有明确的任务完成 / 销毁协议 | 沿用所有权和调度方式，扩展 pending / cancel 查询 |
| Factory::NewExternalStringFromOneByte / TwoByte | 可以交付原生字符资源而不重复创建完整文本副本 | 验证资源转移与 GC 后存活，避免裸指针或释放后访问 |

## 使用与限制

以下代码放在 async 函数或支持顶层 await 的 ES module 中：

```javascript
const result = await JSON.stringifyAsync({value: 1.25, text: '文字列'}, null, 2);
const inputs = [{id: 1}, {id: 2}];
const batches = await Promise.all(inputs.map(value => JSON.stringifyAsync(value)));
```

这是本分支新增的非标准 API，不是 ECMAScript 或通用浏览器承诺。参数和文本输出遵循现有 `JSON.stringify`；顶层 `undefined`、函数、Symbol 产生 fulfilled `undefined`，不是字符串。循环引用、无自定义 toJSON 的 BigInt、getter / replacer 抛出的异常成为 rejected Promise，保留原异常对象；执行终止不是普通 Promise rejection。

- **调用入口仍可能阻塞。** getter、toJSON、replacer、Proxy trap 和对象遍历都在调用期间完成；这些用户代码也可能无限循环。对象密集、短字符串或小请求不保证加速。若要求任意对象图的严格帧预算，需要把对象的创建和所有权一开始就放到独立 isolate，而不是让本 API 不安全地读取共享可变堆。
- **并发是平台线程池并发，不是一调用一线程。** 每个有文本结果的调用提交一个 native worker。宿主还须持续执行前台 non-nestable tasks 和 Promise microtasks；无 worker 或无该类前台任务支持时，在读取输入 / 执行 hooks 之前拒绝。没有新增 JSON 专属线程池。
- **输入语义是调用时观察到的序列。** 调用后再修改对象不影响本次结果；调用过程中 hooks 修改后续属性，仍采用原同步遍历规则。不是提前深拷贝整棵对象图，也不会重放 hooks。
- **不要无界 `Promise.all`。** 每个在途调用保留独立原生快照 / 输出缓冲区。重复字符串缓存只在本次调用有效且大小固定；没有全局无界内容缓存。内存统计是估计，不是配额或背压；宿主应限制在途任务数。

## 实现与所有权

1. 原 `JsonStringifier<capture_mode>` 继续决定属性次序、遗漏、循环、replacer、gap、rawJSON 和 NEED_STACK 重试。同步和采集共用一份遍历代码，编译期区分模式，移除同步热路径上的新 runtime 分支；capture adapter 只替换文本写出端。NEED_STACK 同时清空原生记录和缓存。
2. worker 输入仅包含 owned native 字符数组、数值数组和 16 字节记录。最多 7 字节的标点 / 短整数结构文本放入记录空隙，避免额外记录和小范围复制。记录使用 `std::deque`，避免扩大整块记录数组时反复复制。
3. 64 个键 / 64 个值的 caller identity hints 在 GC epilogue 清空，裸地址不进入 worker。worker 的 128 项编码缓存通过原生 span 的种类、offset、length 再校验，不把哈希相同当作内容相同。
4. packed double 数组批量复制后复用 `DoubleToCString`。packed Smi 数组保留 int32 并复用 `IntToCString`；小于 16 项的 Smi 数组保留原短字面量路径，避免额外 span 成本。转义例程与同步实现共享，UTF-16 lone surrogate 和跨块 surrogate pair 保持一致。
5. worker 结果作为 external one-/two-byte string 交付，不再向 V8 堆整份复制。无须转义的根字符串由 worker 检查后直接接管已采集的 native 字符数组，进一步避免第二份 native 输出副本；有转义或 surrogate 时走共享编码器。资源只有在 factory 成功接收后才移交；空串、失败或取消由 native owner 释放。缓冲区几何增长可能留下容量余量，V8 字符串长度统计不等于实际 malloc 容量。
6. isolate 注册表强持有任务状态，保证 PersistentHandles 在 isolate 线程释放。完成任务切回创建时 native context；isolate teardown 先设置 native cancellation 并释放 roots，再 CancelAndWait。worker / 延迟任务销毁不访问已销毁的 V8 堆。宿主若丢弃完成任务，Promise 不会凭空完成，root 在 isolate teardown 清理。

长度按原 `String::kMaxLength` 校验，采集阶段只检查输出下界，worker 检查实际转义后长度。转义分块不切断 surrogate pair。取消在记录和有界循环间检查，不把一次大 memcpy、分配或 GC 宣称为硬实时可中断操作。

## 已完成的优化实验

同机 Release、4 个平台 worker、每项 9 轮中位数；ms 为整个批次 await 完成耗时，不是单独 worker CPU 时间。这里是逐步优化对照，最终同步对照和生命周期验收另列。

| 实验 | 场景 / 任务数 | 修改前 ms | 修改后 ms | 决策 |
| --- | --- | ---: | ---: | --- |
| 紧凑前缀 + deque | records / 4 | 42.034 | 26.454 | 保留；该轮峰值 RSS 从 257.7 降至 193.5 MiB |
| 紧凑前缀 + deque | nested / 4 | 42.296 | 26.071 | 保留；删除大量纯标点记录 |
| 紧凑前缀 + deque | varied / 4 | 31.125 | 18.245 | 保留 |
| int32 数组 + 小 Smi 数组直接结构文本 | integers / 4 | 5.441 | 3.319 | 保留；该轮峰值 RSS 从 90.2 降至 75.0 MiB |
| int32 数组 + 小 Smi 数组直接结构文本 | nested / 4 | 25.573 | 24.056 | 保留；无需把短整数往返转换为 double |
| 根字符串直接接管，反向 15 轮 | large_string / 4 | 10.118 | 7.306 | 保留；RSS 从 356.3 降至 287.8 MiB |
| 根字符串直接接管，反向 15 轮 | large_unicode / 4 | 9.635 | 6.471 | 保留；RSS 从 355.9 降至 287.7 MiB |
| 编译期模式 + 固定 7 字节前缀复制 | nested / 4 | 25.049 | 22.391 | 保留；去除不必要的 runtime 分支及变长小复制 |
| 字符缓存热 / 冷代码强制拆分，15 轮 | nested / 4 | 20.786 | 21.662 | 撤回；其他 4 任务场景仅小幅变动，不能证明整体更优 |

证据：`out/json-stringify-async/prefix-benchmark.jsonl`、`integers-benchmark.jsonl`。第一版对象数组明显慢于同步，不作为验收结论；紧凑记录解决了主要新增采集开销，但原遍历成本仍存在，不隐瞒对象场景的剩余串行比例。

单任务 ASCII 根字符串接管没有稳定改善 wall time（反向实验 5.228 → 5.379 ms），保留它的依据是并发 wall / RSS 的重复收益，不把所有指标写成加速。缓存拆分实验保存在 `cache-inline-benchmark.jsonl`，不在最终实现中。

## 最终性能

环境：macOS 15.4，Intel Core i7-14700KF，64 GiB 内存；本机报告 28 logical CPU。Release 为非 debug、无 DCHECK、无 i18n、无 sandbox、系统 libc++、Apple Clang / Xcode、本地 `-O3`，没有生产 PGO。详细参数和基线位于 `out/json-stringify-async/{environment.txt,release-args.gn,dcheck-args.gn,baseline-sha256.txt}`。测量时没有同时编译；输入创建、一次同步 expected 生成、显式 GC 和结果相等检查在计时区间外，字符串可能已经被预热 / flatten，首次生产调用还可能承担这些成本。

以下都是**批次总耗时中位数**，单位 ms；capture 为提交该批所有调用的主线程时间，不是后台时间。`Promise.all` 批量提交期间没有自动让出主线程，不能把 4/16 次 capture 总和误称为一次短片段。

### 4 worker，11 轮

| 场景 | 同步 1 任务 | 异步 1 任务 | capture 1 | 同步 4 任务 | 异步 4 任务 | capture 4 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| records：10 万个短对象 | 6.496 | 8.841 | 6.354 | 23.533 | 24.770 | 21.452 |
| numbers：12 万个 double | 5.526 | 3.427 | 0.311 | 21.540 | 4.304 | 1.173 |
| integers：20 万个整数 | 2.652 | 2.535 | 0.297 | 10.580 | 3.213 | 1.095 |
| strings：5 万个重复转义字符串 | 3.080 | 1.558 | 0.687 | 12.034 | 3.567 | 2.512 |
| nested：4 万个嵌套对象 | 5.124 | 7.737 | 5.513 | 18.938 | 22.008 | 19.850 |
| varied：3 万个混合对象 | 5.202 | 7.068 | 4.362 | 18.944 | 17.118 | 13.993 |
| long_strings：2 千个不同长串 | 3.044 | 3.161 | 0.962 | 11.728 | 6.340 | 3.980 |
| unicode：7 万个 Unicode 对象 | 8.220 | 7.762 | 5.775 | 30.300 | 24.624 | 20.506 |
| large_string：1 千万 Latin1 码元 | 22.666 | 5.452 | 0.721 | 84.571 | 7.409 | 2.571 |
| large_unicode：5 百万 UTF-16 码元 | 16.270 | 4.381 | 0.700 | 55.274 | 6.374 | 2.583 |
| large_escaped：150 万转义混合码元 | 7.790 | 3.161 | 0.381 | 30.953 | 6.739 | 2.393 |
| callbacks：2 万个 getter/toJSON 对象 | 3.594 | 3.940 | 2.373 | 13.076 | 9.480 | 7.982 |
| tiny：单个短对象 | 0.002 | 0.017 | 0.006 | 0.002 | 0.043 | 0.034 |

证据：`final-w4.jsonl`；包含全部样本、最长单次入口占用和输出长度，tiny 的微秒级比值不能作为稳定吞吐结论。

### 8 worker，16 任务，9 轮

| 场景 | 同步 ms | 异步 ms | 总 capture ms | 吞吐比 |
| --- | ---: | ---: | ---: | ---: |
| records | 97.270 | 88.831 | 85.817 | 1.10x |
| numbers | 87.007 | 9.044 | 4.519 | 9.62x |
| integers | 41.748 | 7.594 | 4.431 | 5.50x |
| strings | 46.265 | 9.723 | 8.618 | 4.76x |
| nested | 76.194 | 80.386 | 77.922 | 0.95x |
| varied | 77.468 | 58.209 | 55.676 | 1.33x |
| unicode | 134.773 | 99.756 | 96.379 | 1.35x |
| large_string | 341.643 | 20.660 | 15.778 | 16.54x |
| large_unicode | 230.997 | 19.330 | 15.618 | 11.95x |
| callbacks | 52.353 | 33.106 | 31.629 | 1.58x |

超过 worker 数的速度比不是超线性线程加速证明：两条路径的复制次数、批量数字处理和内存分配方式不同。`final-w1.jsonl` 的单 worker / 4 任务中，numbers 22.071 → 12.645 ms、large_string 83.786 → 19.548 ms，说明部分收益来自算法和复制减少；records 23.641 → 24.638 ms，说明有明确的串行采集下限。

### CPU、内存与同步控制

相同最终二进制，4 worker / 4 任务 / 15 轮，以独立进程按场景交替运行 sync / async。CPU 是 `wait4` 的整个子进程 user+system 秒数，RSS 是整个子进程峰值 MiB；均包含输入准备、expected、GC 和校验，不是纯 worker CPU 或单次调用分配量。

| 场景 | 同步 CPU s | 异步 CPU s | 同步 RSS MiB | 异步 RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| records | 0.505 | 0.658 | 112.9 | 190.3 |
| numbers | 0.397 | 0.277 | 55.9 | 74.4 |
| integers | 0.224 | 0.222 | 54.4 | 65.8 |
| strings | 0.244 | 0.184 | 83.2 | 121.7 |
| nested | 0.420 | 0.577 | 71.4 | 133.2 |
| varied | 0.444 | 0.558 | 96.0 | 159.4 |
| unicode | 0.635 | 0.630 | 175.3 | 236.7 |
| large_string | 1.459 | 0.543 | 220.9 | 128.7 |
| large_unicode | 1.023 | 0.427 | 221.3 | 128.8 |
| large_escaped | 0.563 | 0.353 | 145.3 | 170.8 |
| callbacks | 0.338 | 0.352 | 50.3 | 75.4 |

证据：`resources-final.jsonl`。**对象快照有实质内存和 CPU 成本**；大量轻量对象或严格内存预算优先保留同步 API / 调整应用的数据所有权，而不是无界增加并发。

修改前 baseline 与最终二进制的同步专用控制为每项 20 轮、反向二进制顺序。多数 4 任务场景在约 ±2% 内；callbacks 12.540 → 13.061 ms（+4.2%）、long_strings 11.681 → 12.265 ms（+5.0%），strings 11.280 → 10.799 ms（-4.3%）。因此不宣称原同步路径完全零回退；编译期分流消除了新增逐值分支，但编译布局和共享转义改造仍须在目标生产配置复测。证据：`final-sync-control.jsonl`；早期 runtime 分支版本的 callbacks 曾有约 5–10% 回退，因而没有保留该结构。

## 正确性和验收边界

- Release / DCHECK 均通过 `stringify-async.js`（1 和 4 worker）、4 worker 的 `--stress-compaction`、既有 `parse-async.js`。差分测试覆盖 omission、BigInt/toJSON、循环、深层重试、replacer、gap、Proxy、稀疏数组及继承 getter、抛出任意值、重入调用、GC、调用后变更、全 65,536 码位、surrogate 分块边界、整数阈值及 4,000 元素边界。
- 20 项定向原生测试全部通过：原有 parseAsync 11 项，新 stringifyAsync 9 项（NativeBounds、ConcurrentWorkers、ResourceAndRealm、Unsupported、Termination、TaskTiming、DroppedCompletion、TasksOutliveIsolate、QueuedWorkerOutlivesIsolate）。双 worker 屏障确认确实不在主线程执行，并允许主线程在等待时执行 JS / GC。
- 终止测试实际发现并修复了 external-string 快速完成绕过待处理 termination 的问题：前台 completion 先处理 stack-guard interrupt，再决定交付 / 拒绝 / 保留 termination。
- 修改前冻结二进制与最终同步实现对全码位、长串、缓冲边界、数值数组、gap、循环及 rawJSON 的输出逐字节相同。不是仅以修改后的同步路径作为唯一 oracle。
- GN header 依赖、cpplint 1.6.1（仓库风格过滤 header_guard / include_what_you_use）和 `git diff --check` 通过。未新增 V8 运行时依赖。

最终 DCHECK 原生诊断（本机原生测试平台默认 **16 worker**，与手动设为 4/8 的 Release benchmark 不同）：10 MB 根字符串 capture 0.866 ms、等待 4.592 ms、前台交付 0.016 ms、总计 5.475 ms。等待不是 worker CPU，单次诊断不是 p99 / 硬实时承诺。日志：`native-tests-final.log`。

本次不是全量 V8 测试认证：当前裁剪仓库的全量 cctest 聚合仍有先前记录的 WASM SIMD 对齐编译问题，使用聚焦 Ninja 清单链接上述单个测试源。未验证其他架构 / 操作系统、sandbox、i18n、Bazel、生产 PGO、完整 test262、宿主 UI 帧延迟、ASan / TSan 或接近最大 V8 字符串长度的实际巨量分配；NativeBounds 覆盖的是无需巨量分配的长度 / 预取消边界。

## 复现命令

在项目根目录、已有该项目工具链与上述构建参数的前提下运行；所有路径都位于项目内。`baseline-release` 是实现前冻结副本，不能用改动后的二进制覆盖它。

```sh
rtk proxy ninja -C out/json-opt-release -j8 d8
rtk proxy ninja -C out/json-async -j8 d8
rtk proxy ninja -C out/json-async -f json-async-tests.ninja -j8 cctest-json-async
rtk proxy out/json-opt-release/d8 --expose-gc --thread-pool-size=4 test/json/stringify-async.js
rtk proxy out/json-async/d8 --expose-gc --thread-pool-size=4 --stress-compaction test/json/stringify-async.js
rtk proxy out/json-async/d8 --expose-gc --thread-pool-size=1 test/json/stringify-async.js
rtk proxy out/json-async/d8 --expose-gc test/json/parse-async.js
rtk proxy out/json-async/cctest-json-async test-json-async/JsonStringifyAsyncConcurrentWorkers
rtk proxy out/json-async/cctest-json-async test-json-async/JsonStringifyAsyncTaskTiming
rtk proxy out/json-async-tools/gn check out/json-opt-release //:v8_base_without_compiler
```

```sh
rtk proxy python3 test/json/parse-async-benchmark.py out/json-stringify-async/final-release --script test/json/stringify-async-benchmark.js --workers 4 --runs 11 --cases records numbers integers strings nested varied long_strings unicode large_string large_unicode large_escaped callbacks tiny
rtk proxy python3 test/json/parse-async-benchmark.py out/json-stringify-async/final-release --script test/json/stringify-async-benchmark.js --workers 8 --jobs 16 --runs 9 --cases records numbers integers strings nested varied unicode large_string large_unicode callbacks
rtk proxy python3 test/json/parse-async-benchmark.py out/json-stringify-async/final-release out/json-stringify-async/baseline-release --script test/json/stringify-async-benchmark.js --mode sync --workers 4 --runs 20 --cases callbacks records nested strings numbers integers varied long_strings unicode large_string large_unicode large_escaped
rtk proxy python3 test/json/parse-async-benchmark.py out/json-stringify-async/final-release --script test/json/stringify-async-benchmark.js --mode async --workers 4 --jobs 4 --runs 15 --cases records numbers integers nested large_string large_unicode
rtk proxy python3 test/json/parse-async-benchmark.py out/json-stringify-async/final-release --script test/json/stringify-async-benchmark.js --mode sync --workers 4 --jobs 4 --runs 15 --cases records numbers integers nested large_string large_unicode
```

保存全部 JSONL 样本而非只挑最快一次。输出中的 `code_units` 是码元数，UTF-16 输入不能直接当作相同字节数；`max_capture_ms` 是每轮最长入口耗时的中位数，不是全局最大值。解释资源对照时必须使用同轮数、同模式计数、同输入与独立进程，不能把同时跑两种算法的进程 CPU 与只跑一种算法的进程 CPU 相比。

### 重新生成聚焦原生测试清单

若 `json-async-tests.ninja` 尚不存在，在正常 GN 生成、`d8` 构建完成之后执行。这里只在忽略的 out 目录过滤测试 object 输入，不修改生产 BUILD.gn、不删除其他测试源，也不声称全量 cctest 通过。正常完整构建环境也可直接使用标准 `cctest` 可执行文件运行相同测试名。

```sh
rtk proxy python3 - <<'PY'
from pathlib import Path
import re

d = Path('out/json-async')
keep = {'value-helper.o', 'heap-utils.o', 'print-extension.o',
        'profiler-extension.o', 'trace-extension.o', 'test-json-async.o'}
def filtered(text):
    lines = text.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if (line.startswith('build ./cctest:') or
                line.startswith('build obj/test/cctest/cctest_sources.stamp:')):
            lines[i] = re.sub(
                r'obj/test/cctest/cctest_sources/[^\s]+\.o',
                lambda m: m[0] if Path(m[0]).name in keep else '', line)
    return ''.join(lines)

(d / 'json-async-tests.ninja').write_text(
    (d / 'build.ninja').read_text().replace(
        'subninja toolchain.ninja', 'subninja json-async-toolchain.ninja'))
(d / 'json-async-toolchain.ninja').write_text(
    (d / 'toolchain.ninja').read_text().replace(
        'subninja obj/test/cctest/cctest.ninja',
        'subninja json-async-cctest.ninja').replace(
        'subninja obj/test/cctest/cctest_sources.ninja',
        'subninja json-async-cctest_sources.ninja'))
(d / 'json-async-cctest.ninja').write_text(
    filtered((d / 'obj/test/cctest/cctest.ninja').read_text()).replace(
        'target_output_name = cctest', 'target_output_name = cctest-json-async'
    ).replace('build ./cctest:', 'build ./cctest-json-async:'))
(d / 'json-async-cctest_sources.ninja').write_text(
    filtered((d / 'obj/test/cctest/cctest_sources.ninja').read_text()))
PY
rtk proxy ninja -C out/json-async -f json-async-tests.ninja -j8 cctest-json-async
rtk proxy python3 - <<'PY'
from pathlib import Path
import re
import subprocess

names = re.findall(r'TEST(?:_WITH_PLATFORM)?\((Json\w+)',
                   Path('test/cctest/test-json-async.cc').read_text())
for name in names:
    subprocess.run(['out/json-async/cctest-json-async', 'test-json-async/' + name],
                   check=True, timeout=90)
print('PASS native tests:', len(names))
PY
```
