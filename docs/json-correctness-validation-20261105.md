# JSON 正确性扩展验证与 ARM32 SIMD 兼容性审查

日期：2026-11-05。分支：`12.6-jsonparse-async`。原始参考版本：`cd985f42b1e54328dfa52a01149d9350701df5b8`。

已按要求先本地提交上一轮验收结果：`780a253a5ef13559a5011e46ac2cc6ef52a79365`（`perf(json): accelerate long escaped string decoding`）。之后继续本轮正确性工作；下述指针修复和新测试是提交后的工作区改动。本次没有执行 push。

## 结论与实际修复

本轮最终 12 个差分阶段、**84,038 条原版参考记录、373,592 次候选逐条对照**全部一致，未发现 JSON 输出或错误语义偏差。Release、DCHECK、ASan 各 60 项既有及新增检查通过。

静态审查发现序列化的 Latin1 SIMD 循环存在越界指针构造：首个转义字符位于块前部时，原循环先推进到该字符之后，再减去一个 stride，会暂时形成 `chars` 之前的指针；尾部判断也可能构造超过输入尾端的指针。没有观察到由此造成的错误 JSON 输出，但这种指针运算存在 C++ 未定义行为风险。

`src/json/json-stringifier-fast.cc` 已改为索引推进，并用 `length - position >= stride` 判断剩余输入。只在已证明完整向量可读时构造块指针，保留原转义、复制和标量尾部算法。没有增加分配、线程或 GC 策略。本轮没有重新测量该修复的性能。

## 独立对照与测试矩阵

解析结果通过独立编码器记录 binary64 位模式、UTF-16 码元、属性顺序和描述符、数组空洞以及标准/null/其他原型分类，不通过当前 `JSON.stringify` 来判断当前 `JSON.parse` 是否正确。序列化输出逐码元比较，错误名称/消息及 getter、Proxy、toJSON、replacer、reviver 的调用轨迹也参与对照。

| 阶段 | 配置与范围 | 结果 |
| --- | --- | --- |
| 主边界与随机矩阵 | Release、DCHECK+GC 压力/堆验证、ASan+GC 压力、主机便携字符串路径、关闭 stringify 快速路径 | 每配置 parse/parseAsync 各 7,970 条；stringify/stringifyAsync 各 9,344 条；完全一致 |
| 大输入 | Release、DCHECK+GC 压力/堆验证、ASan+GC 压力、主机便携路径 | 每配置解析各 53 条、序列化各 3 条；完全一致 |
| 补充随机种子 | 两个独立种子各 10,000 次生成；其中一个种子再经 ASan+GC 压力 | 每配置解析各 13,334 条、序列化各 20,000 条；完全一致 |
| 数字格式化 | 200,000 个随机 IEEE-754 值和 21 个边界值；另含固定/指数/精度输出 | 原版、Release、DCHECK、ASan 的 201,041 行输出逐字节一致 |
| 既有检查 | Release / DCHECK / ASan 各 60 项，其中 34 个原生 JSON 测试 | 全部通过 |
| 最新守护页测试 | Release、DCHECK、ASan、主机便携实现各 3 项 | 全部通过 |

计数表示执行的记录对照次数；不同构建复用相同语料，不能解释为同样数量的独立输入。数字格式化和既有数字后端测试另行记录，没有与主矩阵混加。

覆盖包括：全部 65,536 个 UTF-16 码元、孤立/成对代理项、16 字节 lane 与 31/32/33、63/64/65、127/128/129、4095/4096/4097 等边界；负零、次正规数、溢出/下溢、整数精度边界、长数字及既有独立构造的精确中点；重复键、转义同名键、索引键、`__proto__`、缓存冲突与宽对象；深层结构、畸形/截断输入、回调抛出任意值、重入、快路径中途回退、调用后修改源对象及 GC。

大输入语料精确位于 `4 MiB - 32`、`4 MiB + 32`、`16 MiB + 32`，包含字符串内部结构符号、奇偶反斜线、前导空白和 UTF-16 变体，并在潜在分片位置附近插入非法控制字符，比较并行/串行结果及错误恢复。

原生 `JsonScannerGuardPages`、`JsonNumberGuardPages`、`JsonExternalStringGuardPages` 将输入尾部紧贴不可读内存页。扫描器遍历所有 UTF-16 值和 16 个 lane 位置；外部字符串覆盖转换宽度、转义密度、截断 Unicode/反斜线及错误报告。这样可暴露普通容器尾部余量容易掩盖的越界读取。

既有测试还包含单/双 worker 配置、增量标记/压缩、异步完成任务丢弃、终止、任务晚于 isolate 销毁、realm 与资源寿命。两套解析 API 的 JSONTestSuite 各 293 条与原版逐条一致。

## ARM32：编译回退与设备兼容是两个问题

Android API 等级不能替代 CPU 能力检测。NDK 的说明区分出厂搭载新系统的设备与旧设备升级；构建指南明确提到升级后的旧设备仍可能没有 NEON。[NDK NEON 文档](https://developer.android.com/ndk/guides/cpu-arm-neon)、[NDK 构建维护指南](https://android.googlesource.com/platform/ndk/+/master/docs/BuildSystemMaintainers.md)。

当前 ARM32 构建使用 `-mfpu=neon`，JSON 中的 `__ARM_NEON` 条件是编译期选择。V8 的 `CpuFeatures::ProbeImpl` 还会合并编译器保证的特性，因此该构建中的 `CpuFeatures::IsSupported(NEON)` 不能作为独立的实际设备能力检测。只在 JSON 入口加 `if`，无法保证全库及其依赖不会在其他位置执行 NEON。

本轮验证结果：

- Android ARM32/NEON 与 ARM64：修改的运行时代码和原生测试翻译单元均编译通过。
- ARM32 无 NEON：用 `-mfpu=vfpv3-d16` 并移除编译器 NEON/D32 保证，八个 JSON/数字翻译单元编译通过；预处理宏确认没有 `__ARM_NEON`。
- 主机便携路径：关闭显式 NEON 字符串分支并选择 Highway EMU128，完成四 API 差分及原生守护页验证。主机仍是 ARM64，编译器可能自动向量化，不能把这项测试称为无 NEON CPU 的运行验证。

若正式支持无 NEON 旧 ARMv7，可靠方案需要整个 V8 及依赖以非 NEON 指令集为基础，单独编译可选 NEON 内核并检测实际 CPU 后分派，同时核对 snapshot/JIT 和目标设备。当前默认 ARM32 产物仍要求 NEON，本轮没有加入会造成错误安全感的 JSON 局部动态判断，也没有改变发行产物的指令集要求。

## 证据边界与复现

本机运行环境为 Apple M1 Pro / macOS ARM64。ASan 是独立诊断构建，启用 DCHECK；完整参数和二进制哈希见证据。当前没有连接 Android 设备，因此 Android 结果限于编译，完整无 NEON 产物及真实旧设备尚未验收。未执行完整 test262、TSan、生产 sandbox/i18n/PGO 配置或接近最大字符串长度的巨量分配测试。

测试通过增加了对这些输入和构建的信心，不能证明任意输入均无缺陷，也不应直接替代目标设备和宿主应用的发布验证。

两个早期大输入 DCHECK 进程因测试语料构造产生过多临时字符串而由本任务主动终止，日志明确为 `Received SIGTERM signal`；它们保留为诊断，不计入通过结果。随后减少测试构造的临时分配，在相同 GC 压力和堆验证参数下重跑最终矩阵。测试工具还检查预期有效输入是否意外抛出脚本异常，避免把测试脚本自身的问题当作一致性通过。

使用说明：[tools/json_correctness/README.md](../tools/json_correctness/README.md)。
机器可读证据：[mac-arm64-20261105.json](../tools/json_correctness/evidence/mac-arm64-20261105.json)。
完整本地证据：`out/json-correctness-20261105/`，最终阶段以 `audited-verified-` 为前缀；保留压缩原始输出、stderr、构建日志、源码快照、补丁、工具链命令和所有诊断记录。

核心 GN 与 cctest 头依赖检查、修改行 cpplint 和空白检查通过。此次新增修复和测试保留在工作区，没有追加提交。
