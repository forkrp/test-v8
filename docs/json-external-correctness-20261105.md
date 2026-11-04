# JSON 上游测试接入与正确性验证

日期：2026-11-05。运行时代码基线：`215f79bf08e2466661ca13bff4b64553bb1d0bad`，分支 `12.6-jsonparse-async`。

## 目标与验收

本轮目标是将独立标准断言和知名 JSON 库积累的边界输入接入可复现工具，补充已有的自建差分、守护页和异步生命周期验证。验收要求：固定上游提交与文件哈希；明确记录语义适配和未覆盖输入；原版 V8 12.6 通过标准/独立预期检查；当前五种配置通过全部适用测试，四个 API 与原版输出一致；工具能识别故意注入的错误。

上述验收已完成。**未发现本轮输入上的引擎结果不一致。**本轮修改的是测试、导入工具和文档，没有修改运行时代码。已重新核对上一轮资格验证的源码哈希，复用与这些源码匹配的冻结二进制，没有重复构建或进行性能测量。

## 固定来源

| 来源 | 固定提交 | 本轮实际接入范围 |
| --- | --- | --- |
| [TC39 Test262](https://github.com/tc39/test262) | `d11543772591a5c24b3229e6195a74c73e56032e` | JSON 目录全部 166 个测试文件，原始断言和所需 harness；每个文件分别以 sloppy/strict 独立进程执行 |
| [RapidJSON](https://github.com/Tencent/rapidjson) | `297637bc0a0f00dc504dbc1a47ebdaa577f8ae19` | Reader/Writer 的 204 条字面量宏输入，加 `ScanWriteUnescapedString` 输入，共 205 条来源记录 |
| [yyjson](https://github.com/ibireme/yyjson) | `ebb8d7e5aaa51bea4edefb39ee73a235066b0023` | 22 个数字文本文件的全部非空、非注释行，共 30,289 条来源记录 |
| [simdjson-data](https://github.com/simdjson/simdjson-data) | `351949906abde446f0314bf79606fb5d884f5be7` | `jsonchecker` 下 JSON 文件；1,855 个 UTF-8 可解码输入进入校验，另 28 个明确排除 |
| [nlohmann/json_test_data](https://github.com/nlohmann/json_test_data) | `a015e5be2d5dfffcf98d00d9df39c9285efc7887` | roundtrip、json_tests 和 Big List of Naughty Strings，共 562 条来源记录 |

simdjson-data 使用 simdjson 提交 `37a1d727eaab785805c670a94a951a65fb7262e5` 的 `dependencies/CMakeLists.txt` 所指定的版本。锁文件保留该声明。共锁定 2,205 个文件，包含测试、harness、许可证和来源说明；这不是独立测试用例数量。

另生成 5,110 条来源记录，用于导入字符串的边界位置、截断和大数组变形。全部来源记录按 UTF-16 输入内容去重并保留来源列表，最终得到 **36,680 条唯一用例**。这个去重范围是本轮语料，不表示它们全部没有出现在以前的测试中，也没有将上游 C/C++ 测试的全部控制流移植过来。

## 预期如何建立

- Test262 使用上游标准断言，覆盖 parse/stringify 的可观察语义以及当前已支持的 raw JSON/source access。全部 166 个文件均执行，没有按功能跳过。它们测试同步标准 API，没有机械替换成异步 API。
- 普通文本由 Python 独立严格解析，数字转换到 binary64，并按 ECMAScript 的索引键顺序调整对象模型。结果比较保留负零、数值位模式、UTF-16 码元、对象顺序和数组结构；d8 侧额外检查原型、属性描述符与数组空洞。
- RapidJSON 中 99 个直接使用十进制期望字面量的 double 用例、8 个 UTF-8 字符串用例，也与上游显式预期核对。其余输入同样接受独立预期校验。静态提取包含上游 `#if` 非活动分支中的字面量，不能解释成 RapidJSON 当前实际执行了这些分支。
- serializer 输入由 tagged model 直接构造，没有调用待测 parser。字符串转义使用额外的小型独立编码器验证；其他输出逐条与原版 `JSON.stringify` 对照，不要求采用其他 C/C++ 库的数字输出拼写。
- 数字溢出、64 位整数、孤立 UTF-16 代理项、扩展数字格式等按 ECMAScript 语义适配。例如 RapidJSON 拒绝的溢出数字在这里可以产生 Infinity，yyjson 的十六进制或非有限数字扩展在标准 JSON 文本中应被拒绝。错误比较采用 `SyntaxError` 类别，不检查各实现的错误消息措辞。

48 个深层输入超出了 Python 预期生成器的递归限制。这些输入仍参与 parse/parseAsync 与原版的差分，但没有独立语义判定，也没有用来构造 serializer 输入。JS 结果编码采用迭代遍历，避免编码器自己的递归限制掩盖解析结果。另有 28 个非法 UTF-8 字节文件被排除；输入到 JS JSON API 的是 UTF-16 字符串，本轮没有采用替换字符解码来改变这些字节输入。

每条排除或适配均在本地 `prepared-complete/manifest.json` 中记录；机器可读证据也完整保留了 28 个排除文件和 48 个仅差分输入的清单。

## 验证结果

原版参考为未经本分支优化的 V8 12.6 提交 `cd985f42b1e54328dfa52a01149d9350701df5b8`。以下执行使用同一份固定语料和驱动。

| 配置 | Test262 执行 | parse / parseAsync | stringify / stringifyAsync | 结果 |
| --- | ---: | ---: | ---: | --- |
| 原版 12.6 | 332 | parse：36,680 | stringify：33,979 | 独立断言通过，生成参考输出 |
| 当前 Release | 332 | 各 36,680 | 各 33,979 | 全部通过，逐条一致 |
| DCHECK + stress-compaction + verify-heap | 332 | 各 36,680 | 各 33,979 | 全部通过，逐条一致 |
| ASan + stress-compaction | 332 | 各 36,680 | 各 33,979 | 全部通过，逐条一致 |
| 主机便携字符串路径 | 332 | 各 36,680 | 各 33,979 | 全部通过，逐条一致 |
| Release 关闭 stringify 快速路径 | 332 | 各 36,680 | 各 33,979 | 全部通过，逐条一致 |

合计 **1,992 次 Test262 执行、70,659 条原版语料记录、706,590 次候选逐条对照**。这些执行复用输入，不能解释为同样数量的独立语料。五种候选配置的 `completed` 和 `all_passed` 均为 true；所有最终进程 stderr 为空。

36,680 条用例包括 33,979 条 serializer 输入、2,653 条独立判定的语法错误和 48 条仅解析差分输入。导入字符串的变形覆盖 15/16/17、31/32/33、63/64/65 码元位置；这表示逻辑字符串位置，实际地址对齐仍由已有原生守护页测试负责。

大输入从 RapidJSON 的扫描回归字符串构造，精确落在 `4 MiB - 32`、`4 MiB + 32` 和 `16 MiB + 32`。每种大小包含正常数组和在中部字符串中注入控制字符的错误数组，共 6 条。正常解析逐项核对所有字符串及元素属性描述符；序列化与独立构造的完整目标文本比较。损坏输入必须抛出 SyntaxError。

工具的 **11 项自检通过**，包括数值中点、负零、严格语法、重复键与 Unicode、C 字面量解码、深层编码，以及故意提供错误预期、修改/缺失对照行、错误 manifest、错误 model 和不存在的 API。这些负向控制用于确认测试工具能够失败。

初次 Test262 宿主适配使用 `Realm.create`，导致两个跨 Realm 测试在两种模式下出现 4 次 `no access` 失败。改为 d8 的 `Realm.createAllowCrossRealmAccess` 后，原版和全部候选完整重跑通过。初跑日志及此前较小范围的成功记录保留为诊断，不计入上表。

## 复现与边界

- [运行与导入说明](../tools/json_correctness/README.md#pinned-upstream-suites)
- [固定来源和 SHA-256](../tools/json_correctness/external-sources.json)
- [导入及执行工具](../tools/json_correctness/external.py)
- [四 API 校验驱动](../test/json/external-corpus.js)
- [机器可读证据](../tools/json_correctness/evidence/external-mac-arm64-20261105.json)
- 完整本地档案：`out/json-external-20261105/`。最终语料为 `prepared-complete`；资格验证结果为 `reference-complete`、`release-complete`、`dcheck-complete`、`asan-complete`、`portable-complete`、`fallback-complete`。

`fetch` 是唯一访问网络的步骤，后续生成与运行可离线进行。文件被修改时校验会失败；来源提交、工具、语料、运行时源码和二进制哈希均保留。原始上游文件下载到本地缓存，没有随本提交重新分发；缓存保留对应许可证及来源说明，聚合数据需按其具体来源核对许可。

结果限于本机 macOS ARM64 和上述构建。主机便携路径不是 ARM32 无 NEON 设备验证；本轮没有改变 ARM32 构建要求，也没有新增设备运行结论。这里执行的是 Test262 的 JSON 子集和选定上游输入，不是完整 Test262，也不是各 C/C++ 库的完整原生测试。现有守护页、异步取消、GC 和 isolate 生命周期测试仍需保留。这些通过结果增加了覆盖范围，不构成任意输入均正确的证明。
