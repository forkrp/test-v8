# V8 14.4 JSON 优化定向回移

日期：2026-09-29。参考官方文章：[How we made JSON.stringify more than twice as fast](https://v8.dev/blog/json-stringify)。文中的上游倍数不是本分支的性能承诺。

## 范围与约束

实施基线：`cd44f49599fae8ea084646b4785e224f15bf6d5d`（V8 12.6.228.49）。
上游来源：本地 `origin/14.4.258.49-custom`，固定为 `e368b713b918f6a5ad0cf6db46c4a72c2f375b32`（V8 14.4.258.49）。

先回移官方同步 stringify / parse 优化，再让已有异步实现复用适合的能力。不升级整个引擎，不覆盖现有异步实现，不把同步序列化结果包装成伪异步，不让 worker 读取 V8 可变堆。保留通用语义回退。只本地提交，不推送。

## 实施顺序与验收

| 阶段 | 工作 | 验收证据 | 状态 |
| --- | --- | --- | --- |
| B0 | 冻结当前二进制、构建参数和四条路径基线 | 独立基线、SHA-256、原始 JSONL | 完成 |
| B1 | 官方 Dragonbox 数字格式化与带长度输出接口 | 保留旧接口；边界 / 随机 double 与冻结基线逐字节相同；同步 / 异步数值场景 | 完成：Release / DCHECK 随机数及边界 oracle 逐字节通过 |
| B2 | JSON fast-iterable 描述符元数据及完整生命周期 | 初始化、变更失效、复制、GC、生成布局与 snapshot 验证 | 完成：native、GC、snapshot 及 static-roots 生成验证 |
| B3 | 官方同步迭代快路径、字符特化、SWAR / SIMD、分段输出 | 快 / 慢路径对照、hooks 不重放、Unicode、深层 / 循环、平台回退 | 完成；性能不是所有输入都获益，见结果与例外 |
| B4 | 官方 parse 快速属性键匹配 | 同构 / 异构对象、重复 / 数字键、字段表示变化、reviver、错误位置 | 完成：基线 oracle 逐字节相同，保留异步构建器 |
| B5 | 异步采集 / worker / 物化的适配 | 保留真实 worker、external string 交付、GC / 终止 / isolate 销毁协议 | 完成：共用快遍历、原有采集缓存、带长度格式化和 SIMD 扫描 |
| B6 | 固定配置全矩阵、代码审核、文档与本地 DCO 提交 | 新旧同步 / 异步、1 / 4 / 16 任务、主线程时间、CPU / RSS、语义及定向 native 回归 | 完成：200 条聚合记录；本地 DCO 提交记录见 git log |

分阶段建立“旧同步 / 旧异步 → 新同步 / 旧异步 → 新同步 / 新异步”对照，不继续用旧同步作为新异步的唯一分母。代码优化若产生可重复回退，先核查原因，不能只报告有利场景。官方文章的倍数不作为本分支的性能承诺。

## Evidence → Finding → Path

| 源码证据 | 发现 | 回移方式 |
| --- | --- | --- |
| 14.4 `FastJsonStringifier` / `CanUseFastStringifier` | 独立无副作用快路径，复杂输入回退旧 stringifier | 保留本项目 `JsonStringifier<capture_mode>`，为同步新增官方快速入口 |
| `DescriptorArray::FastIterableState` / `ParseJsonObjectProperties` | parse 与 stringify 共用形状相关元数据 | 作为一个完整依赖单元迁移，不能只加标志位或只复制消费者 |
| `DoubleToStringView` / Dragonbox | 公共数值转换接口和算法均变化 | 新增带长度接口，旧 C-string 接口保留兼容，避免无关调用点大迁移 |
| `AppendStringSWAR` / `AppendStringSIMD` / Highway | 官方字符优化有平台依赖 | 追踪最小依赖闭包，保留安全尾部与标量回退 |
| `OutBuffer` / 当前 external-string 资源 | 同步分段缓冲最终有合并复制，异步已能直接交付连续存储 | 同步优先采用官方设计；异步不盲目引入额外整份复制，独立对照后选择 |

## 当前进度

本文件先于生产代码修改落盘。验证产物放在忽略目录 `out/json14-backport/`。基线包含 Release / DCHECK d8、定向 native 测试二进制、构建参数及 `baseline-sha256.txt`；四路径实测为 `baseline-parse.jsonl` / `baseline-stringify.jsonl`。另保存 200,000 个固定种子随机 double 加边界值，以及 JSON 错误 / Unicode 输出的冻结 oracle。

### 已实施的兼容性适配

- 新增 `DoubleToStringView` / `IntToStringView`；`DoubleToCString` 仍提供原来的 NUL 终止契约，其他既有数值转换接口不迁移。Dragonbox 头文件、原始许可证、GN 目标和来源元数据直接取自固定分支。
- 官方独立快路径放在 `json-stringifier-fast.cc`；复杂情况回退 `JsonStringifySlow`，原 `JsonStringifier<capture_mode>` 和异步入口保留。新增 `--json-stringify-fast-path` 开关用于快慢路径对照。
- Highway 只取官方 GN 构建所需源文件与递归头文件依赖闭包（45 个文件），不导入其测试、示例或整个 14.4 引擎。
- 适配旧 Handle、异常宏、字符串分派、字符类型别名和数字接口；不回移无关 API 变更。SWAR 非对齐加载改用本项目已有安全读取辅助函数。
- 描述符新增独立 flags 字段，不复用 / 修改 GC 状态位。沿用官方初始化和键变更失效；12.6 `SetDetails` 在不可枚举、非 data-field 或字段序号变化时失效，表示 / constness 泛化不清除有效标记。布局生成与 snapshot 完整重建。
- 官方 parse 属性循环保留当前 `BuildJsonObject` 及异步专用构建器扩展；只适配 token-helper 返回契约和编码谓词。

### 初版同步移植的验证与修正

冻结的 `sync-port-release` 已通过 200,000 个确定性随机 double 加 21 个边界值，以及错误位置 / 所有 65,536 个 UTF-16 code unit 的 oracle 逐字节对照；默认快路径、禁用快路径、stress-compaction 及两个 async API 回归均通过。

隔离同步测试（4 worker，20 轮，二进制反向顺序）中，4 次 stringify 的 records 为 24.519 → 17.842 ms，10M 普通字符为 85.087 → 24.115 ms，5M Unicode 为 56.270 → 31.532 ms。但高转义密度 UTF-16 长字符串为 12.639 → 13.595 ms，nested parse 为 35.825 → 37.546 ms。不能只报告收益：UTF-16 改为共享原有标量编码器处理转义尾部，普通前缀使用 SIMD；描述符失效也缩小为真正改变 JSON 键布局的变更。修正后已正常完整重建，最终复测结果见下文，不以这组中间数据代替验收结果。

### 异步复用边界

- 官方迭代遍历增加编译期 capture 模式：复用原 `JsonStringifyCapture` 的有界字符串身份缓存、native snapshot、packed 数值 span；不先生成同步 JSON 文本。
- 首次快路径尝试没有用户 JS 副作用；不支持时丢弃快照，再走原通用逻辑，getter / toJSON / replacer 不重放。
- worker 使用 Dragonbox 的带长度接口，省去数值文本的 strlen；纯 native SIMD 扫描与同步 Unicode 编码共享。4096 字符取消检查及跨块 surrogate-pair 保护保留。
- 泛型采集保留原来的「先命中身份缓存，再决定是否 Flatten」顺序；快速采集通过同一缓存与复制逻辑接收 no-GC 下的 flat string。不能为了复用接口而让热缓存路径多做 Flatten。
- 保留连续 native 输出与 external string 接管。官方同步分段缓冲不进入 worker，也不增加最终整份输出复制。
- parseAsync 原有 native key/value cache、共享字符串扫描和 Map-feedback 物化保留。官方新 key-loop 依赖 V8 描述符，不能放到 worker 访问堆；不再建立一套重复缓存。

### UTF-16 与输出缓冲的本地适配

官方 14.4 的 Latin1 路径已有 SIMD，UTF-16 路径仍主要是标量扫描与逐段复制。本次补充 UTF-16 向量分类；固定宽度 prefix copy 沿用原 NoExtendBuilder 的「可多复制、只提交有效前缀」原则，读写均受输入长度与预留容量约束。高转义密度或 surrogate 的尾部继续使用已有标量编码器，不新增第二套 surrogate 语义。

同步分段输出上限从 32K 调整到 256K **code units**，降低大量中间段的分配成本；UTF-16 段相应最多 512 KiB，不代表所有负载都降低内存。该分段策略不进入 async worker。对所有优化保留前后向、独立同步模式和强制通用遍历的对照；不能用若干收益样本宣称绝对最优。

## 验证范围

- macOS x64：正常 Release，以及优化构建加 `dcheck_always_on=true`，并非完整 Debug 配置。
- 200,000 个固定种子随机 double 加 21 个边界值；前 1024 个有限值还检查 fixed / exponential / precision 输出。与冻结旧引擎逐字节相同。
- JSON oracle 覆盖全部 65,536 个 UTF-16 code unit、数字 / Unicode / 转义 / 重复 / 特殊键及错误位置。另有 3000 个确定性异构值和 1500 个截断 / 非法 / 改写变体，sync 与 async 对照。
- 默认 / 禁用 stringify 快遍历 / stress-compaction，以及既有 parseAsync、stringifyAsync JS 回归。
- 24 项聚焦 native 测试：真实 worker 并行、主线程让出、GC、取消、终止、丢弃完成任务、isolate 销毁后的任务寿命、external string / realm、描述符复制与失效、向量尾部及 65,536 字符分类。新增测试证明 async 在任何 worker 编码之前已使用官方快遍历设置元数据，而不是先同步 stringify。
- 完整重建生成的布局和 snapshot。额外构建支持的 Intl + Wasm + 压缩指针 + sandbox 配置的 static-roots 生成器，重复生成两次结果一致；更新 `src/roots/static-roots.h`，再显式开启 static roots 编译 JSON 关键对象。
- 生成器使用本仓库 DEPS 的 ICU `98f2494518c2dbb9c488e83e507b070ea5910e95`，没有升级 ICU。验证下载和二进制仅存放在忽略目录。
- GN header dependency check、定向 C++ lint 和 `git diff --check` 通过；49 个未修改的第三方文件与固定来源分支逐字节一致，Highway 来源说明单独更新。

**认证边界：**没有宣称全量 V8 cctest、test262、ASan / TSan、其他架构 / 操作系统或生产 PGO 通过。static-roots 生成器成功启动及关键对象编译，不等同 Intl + static-roots 的完整 d8 行为认证。聚焦 cctest 使用原文档中的过滤构建清单，避开仓库既有全量 WASM SIMD 编译问题，不修改生产测试聚合逻辑。没有测量宿主 UI 帧率，也没有进行接近 String::kMaxLength 的实际巨量分配。

**线程与语义：**1 / 4 / 8 是平台 worker 配置，不是进程线程总数，也不是每个 JSON 自建独立线程。stringify 的任意 JS 对象图仍需要调用方线程同步观察；快路径不执行 JS，不支持时重走原泛型遍历，因此 hooks 不重放。parse 的结果对象仍在原 isolate 物化，不能把 VM 描述符或可变对象交给 worker。

## 最终性能结果

固定同机、无并行编译；4 worker 主矩阵每项 15 轮，1 / 8 worker 并发矩阵每项 9 轮。二进制与 sync / async 的执行顺序交替，每轮先 gc。表中 ms 是一批任务的端到端中位数，不是单个操作耗时。CPU 和峰值 RSS 是整个子进程，包含输入创建、结果校验、GC 和启动；不是纯 worker CPU。

以下均使用最终 `accepted-release`，不能拿历史旧同步版作为新异步的唯一分母。原始样本保存在 `out/json14-backport/accepted-*.jsonl`；`final-*`、`prefix-*` 等文件是中间候选，不是本次验收数据。

### parse：4 worker / 4 任务

| 输入 | 旧 sync ms | 新 sync ms | 旧 async ms | 新 async ms | 新 sync / 新 async |
| --- | ---: | ---: | ---: | ---: | ---: |
| records | 26.495 | 24.834 | 30.292 | 30.701 | 0.81x |
| numbers | 9.169 | 8.554 | 3.363 | 3.439 | 2.49x |
| strings | 8.938 | 8.657 | 7.896 | 7.666 | 1.13x |
| nested | 35.658 | 35.069 | 31.220 | 32.422 | 1.08x |
| varied | 19.554 | 18.770 | 15.850 | 15.857 | 1.18x |
| long_strings | 6.671 | 6.647 | 5.298 | 5.559 | 1.20x |
| unicode | 38.699 | 34.371 | 26.076 | 25.088 | 1.37x |

最后一列小于 1 表示新 async 仍慢于新 sync，不能解释为加速。

| 输入 | 旧 / 新进程 CPU s | 旧 / 新峰值 RSS MiB |
| --- | ---: | ---: |
| records | 1.993 / 2.041 | 191.0 / 194.5 |
| numbers | 0.344 / 0.339 | 77.8 / 74.9 |
| strings | 0.630 / 0.624 | 118.9 / 117.4 |
| nested | 2.570 / 2.541 | 184.2 / 192.8 |
| varied | 1.425 / 1.381 | 183.9 / 167.4 |
| long_strings | 0.334 / 0.341 | 97.4 / 108.1 |
| unicode | 2.310 / 2.270 | 308.2 / 270.7 |

### stringify：4 worker / 4 任务

| 输入 | 旧 sync ms | 新 sync ms | 旧 async ms | 新 async ms | 新 sync / 新 async |
| --- | ---: | ---: | ---: | ---: | ---: |
| records | 22.832 | 17.649 | 23.759 | 19.948 | 0.88x |
| numbers | 21.779 | 6.771 | 4.127 | 3.355 | 2.02x |
| integers | 10.476 | 6.428 | 3.149 | 2.944 | 2.18x |
| strings | 10.816 | 4.992 | 3.160 | 2.676 | 1.87x |
| nested | 18.216 | 14.397 | 20.836 | 19.860 | 0.72x |
| varied | 18.365 | 16.907 | 15.785 | 17.067 | 0.99x |
| long_strings | 11.985 | 13.323 | 6.119 | 6.208 | 2.15x |
| unicode | 31.412 | 18.435 | 23.257 | 19.833 | 0.93x |
| large_string | 80.390 | 22.166 | 7.299 | 3.302 | 6.71x |
| large_unicode | 53.415 | 20.651 | 6.295 | 3.381 | 6.11x |
| large_escaped | 29.188 | 13.376 | 5.086 | 5.151 | 2.60x |
| callbacks | 13.279 | 11.236 | 9.706 | 9.628 | 1.17x |
| tiny | 0.002 | 0.001 | 0.033 | 0.035 | 0.03x |

最后一列小于 1 表示新 async 仍慢于新 sync，不能解释为加速。

| 输入 | 旧 / 新进程 CPU s | 旧 / 新峰值 RSS MiB |
| --- | ---: | ---: |
| records | 1.055 / 0.924 | 187.8 / 184.3 |
| numbers | 0.622 / 0.341 | 91.9 / 86.5 |
| integers | 0.397 / 0.313 | 79.3 / 72.2 |
| strings | 0.365 / 0.259 | 147.6 / 117.7 |
| nested | 0.934 / 0.833 | 152.2 / 138.5 |
| varied | 0.904 / 0.875 | 166.9 / 181.0 |
| long_strings | 0.519 / 0.551 | 192.6 / 205.4 |
| unicode | 1.214 / 0.949 | 367.2 / 264.2 |
| large_string | 1.867 / 0.723 | 279.4 / 176.8 |
| large_unicode | 1.337 / 0.655 | 293.9 / 176.9 |
| large_escaped | 0.829 / 0.593 | 238.3 / 195.3 |
| callbacks | 0.646 / 0.586 | 87.4 / 80.0 |
| tiny | 0.047 / 0.047 | 16.9 / 16.8 |

调用方采集仍同步。最慢单次值是每轮四次调用中最长采集耗时的中位数，不是 p99。

| 输入 | 旧 / 新批次采集 ms | 新最慢单次采集 ms |
| --- | ---: | ---: |
| records | 20.339 / 17.676 | 4.883 |
| numbers | 0.920 / 1.079 | 0.381 |
| integers | 0.996 / 1.089 | 0.359 |
| strings | 2.135 / 1.627 | 0.520 |
| nested | 18.838 / 17.555 | 4.814 |
| varied | 13.236 / 14.642 | 3.895 |
| long_strings | 3.811 / 3.678 | 1.284 |
| unicode | 20.152 / 16.233 | 4.492 |
| large_string | 2.582 / 2.643 | 0.715 |
| large_unicode | 2.515 / 2.633 | 0.720 |
| large_escaped | 1.662 / 1.794 | 0.609 |
| callbacks | 8.334 / 8.563 | 2.289 |
| tiny | 0.022 / 0.026 | 0.010 |

### 高并发：16 任务，async-only

完整矩阵还包含 1 / 4 任务。这里展示 16 任务，并保留吞吐、CPU、RSS 的同机比较。

| API | worker | 输入 | 旧 / 新 async ms | 旧 / 新 CPU s | 旧 / 新 RSS MiB |
| --- | ---: | --- | ---: | ---: | ---: |
| parse | 1 | records | 123.797 / 124.249 | 2.573 / 2.602 | 439.3 / 457.8 |
| parse | 1 | numbers | 27.018 / 28.754 | 0.364 / 0.377 | 98.7 / 98.9 |
| parse | 1 | strings | 32.526 / 32.242 | 0.716 / 0.731 | 263.2 / 226.5 |
| parse | 1 | unicode | 169.140 / 164.945 | 3.350 / 3.393 | 614.5 / 554.9 |
| stringify | 1 | records | 87.705 / 73.127 | 1.433 / 1.230 | 318.8 / 300.7 |
| stringify | 1 | numbers | 49.305 / 36.197 | 0.592 / 0.476 | 191.0 / 194.6 |
| stringify | 1 | strings | 14.463 / 15.003 | 0.337 / 0.308 | 292.5 / 276.6 |
| stringify | 1 | unicode | 80.042 / 87.967 | 1.377 / 1.518 | 403.4 / 360.8 |
| stringify | 1 | large_string | 77.367 / 16.363 | 1.280 / 0.700 | 358.1 / 357.8 |
| parse | 8 | records | 100.543 / 109.295 | 3.264 / 3.363 | 456.9 / 403.1 |
| parse | 8 | numbers | 9.049 / 9.038 | 0.457 / 0.438 | 123.9 / 126.4 |
| parse | 8 | strings | 23.629 / 23.545 | 0.729 / 0.741 | 198.9 / 196.5 |
| parse | 8 | unicode | 104.650 / 97.046 | 3.497 / 3.374 | 544.2 / 499.0 |
| stringify | 8 | records | 86.970 / 71.638 | 1.402 / 1.183 | 293.3 / 261.4 |
| stringify | 8 | numbers | 9.006 / 7.481 | 0.618 / 0.515 | 169.9 / 166.9 |
| stringify | 8 | strings | 10.067 / 7.680 | 0.345 / 0.310 | 243.4 / 227.4 |
| stringify | 8 | unicode | 81.243 / 69.866 | 1.360 / 1.291 | 410.6 / 451.9 |
| stringify | 8 | large_string | 20.270 / 16.943 | 1.316 / 0.715 | 358.2 / 358.1 |

### 分阶段对照：4 worker / 4 任务

`sync-port-release` 仅包含初版同步回移；异步采集尚未复用官方快遍历。它也包含初版 UTF-16 / 描述符适配，不能视作最终同步基线。

| 输入 | 阶段 | sync ms | async ms | 采集 ms |
| --- | --- | ---: | ---: | ---: |
| records | baseline-release | 24.174 | 24.119 | 21.180 |
| records | sync-port-release | 18.053 | 24.777 | 21.729 |
| records | accepted-release | 17.650 | 20.239 | 17.612 |
| numbers | accepted-release | 6.669 | 3.384 | 1.171 |
| numbers | sync-port-release | 6.825 | 3.542 | 1.006 |
| numbers | baseline-release | 22.019 | 4.121 | 0.944 |
| unicode | baseline-release | 30.103 | 23.245 | 19.899 |
| unicode | sync-port-release | 18.213 | 27.272 | 24.017 |
| unicode | accepted-release | 18.891 | 19.684 | 16.652 |
| large_unicode | accepted-release | 21.960 | 3.590 | 2.867 |
| large_unicode | sync-port-release | 29.362 | 6.564 | 2.636 |
| large_unicode | baseline-release | 53.400 | 6.601 | 2.673 |

### 例外与结论

- 不能把 stringifyAsync 当作所有对象的自动替代。records / nested 等对象图可能仍慢于**更新后的同步版本**；主线程语义采集并没有消失。
- `long_strings` 是 2000 个不同、重复出现转义符的 UTF-16 长字符串。最终 4 worker / 4 任务矩阵中，同步 stringify 为 11.985 → 13.323 ms（耗时增加 11.2%），异步为 6.119 → 6.208 ms（增加 1.5%）。独立同步及强制泛型对照确认同步路径仍有性能退化；不以普通大字符串的收益掩盖该例外。UTF-16 SIMD、局部游标和段容量的尝试数据均保留在忽略目录。
- `varied` 异构对象的异步 stringify 为 15.785 → 17.067 ms（耗时增加 8.1%）；本轮没有消除所有异构对象的额外遍历 / 采集成本。这一输入应保留为后续优化的回归样本，不能宣称异步全面加速。
- 解析器的官方改动主要改善同步键扫描；parseAsync 已有 native key cache / shape feedback，本轮没有声称再取得同量级的异步解析加速。
- 几个百分点的正负差异、尤其 tiny 的微秒级计时，不作为普遍加速或统计显著性结论。真实输入仍需复测。
- `--no-json-stringify-fast-path` 可对照原通用遍历（同时关闭 async 快遍历），但不撤销 Dragonbox 或 worker 编码优化。

最终矩阵共有 200 条聚合记录；每条保留原始轮次延迟。Largest ASCII 为 10M code units，largest Unicode 为 5M UTF-16 code units，不把二者都误标为同样字节数。

### 二进制与生成文件 SHA-256

| 文件 | SHA-256 |
| --- | --- |
| baseline-release | `12dc91aa8a6b15c14339d0676cbbdc6f324334f84593cbf36a8eaa315063f78d` |
| sync-port-release | `79c30c2e95a89256f0a762e7fccece7fd84b2d549fd859f455173c2ef3331224` |
| accepted-release | `fef5d8d453353781afce35ec0710215b79314be796b4278934721e4522e40257` |
| accepted-dcheck | `60699a991121581a762d89fe8ed1b8ba5585b34bea9f821351032fb56924ff29` |
| static-roots-final.h | `e8f96942e4cbfba99f82b983154e9f73386d580d24acc87b85e94923df134675` |

## 复现与交付

基线源码与来源 SHA 见文首。使用独立 checkout 构建基线，不覆盖当前工作树；同机保留两份不可变二进制。构建参数保存为 `out/json14-backport/release-args.gn` / `dcheck-args.gn`。代表性命令：

```sh
rtk proxy ninja -C out/json-opt-release -j8 d8
rtk proxy ninja -C out/json-async -j8 d8
rtk proxy out/json-async/d8 --expose-gc test/json/json-upstream.js
rtk proxy out/json-async/d8 --expose-gc --stress-compaction test/json/json-upstream.js
rtk proxy out/json-async/d8 --expose-gc --no-json-stringify-fast-path test/json/json-upstream.js
rtk proxy out/json-async/d8 test/json/json-upstream.js -- numbers
rtk proxy out/json-async/d8 test/json/json-upstream.js -- oracle
rtk proxy out/json-async/d8 test/json/json-upstream.js -- differential
rtk proxy python3 test/json/parse-async-benchmark.py BASELINE FINAL --workers 4 --runs 15 --jobs 1 4
rtk proxy python3 test/json/parse-async-benchmark.py BASELINE FINAL --script test/json/stringify-async-benchmark.js --workers 4 --runs 15 --jobs 1 4 --cases records numbers integers strings nested varied long_strings unicode large_string large_unicode large_escaped callbacks tiny
```

聚焦 native 清单生成方式沿用 `docs/json-stringify-async.md`；对 `test/cctest/test-json-async.cc` 的全部 24 个测试逐一执行。static-roots 按 `tools/dev/gen-static-roots.py` 支持的 ptr-compression / Wasm / Intl 配置生成；该生成配置不依赖旧 static roots。

只提交源码、许可证、测试和文档；忽略的下载、编译器输出、二进制、基准原始样本不提交。没有推送远端。
