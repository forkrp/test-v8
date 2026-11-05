# Android ARM32 JSON 真机验证

验证对象为 `f52ceb328b6d0d2d34f60a0584b158919b964ec1`
（`feat(json): dispatch ARM32 NEON kernels at runtime`）。开始验证时工作区干净，
功能代码已经本地提交。本轮没有修改引擎源码，也没有为手机重新编译二进制。

验证于台北时间 **2026-11-05 23:50:03 至 2026-11-06 00:00:32** 完成，
控制流程耗时约 10 分 28 秒。该时间包含 ADB、文件校验和测试调度，不是性能指标。

## 设备与产物

- 设备：OnePlus 6 / ONEPLUS A6003，序列号 `885841c1`，SoC 为 SDM845。
- 设备属性报告 Android 15、API 35，支持 `arm64-v8a,armeabi-v7a,armeabi`。
  原始 build fingerprint 等属性完整保留在证据 JSON 中。
- 本次运行的是 **ELF32 ARM** 程序和动态库，不是 ARM64 程序。
- 沿用上一轮模拟验证的冻结产物：`d8`、`cctest-json`、`load-smoke` 和
  `libv8-json-smoke.so`；对应二进制 SHA-256 与上一轮完全一致。
- 上传的 **341 个文件**在测试前、测试后均与本地清单逐项校验 SHA-256，全部一致。
- 使用设备互斥锁，未观察到其他 V8 测试任务重叠；结束后没有本轮遗留的运行进程。

设备现有 ADB 执行身份为 `uid=0(root)`，SELinux 为 `Enforcing`。
本轮没有更改 root、SELinux、CPU 调频或系统库配置，也没有安装 APK。

## 分派与编译层验证

三组测试使用同一份二进制，仅改变启动参数：

| 模式 | 启动参数 | 硬件 NEON | JSON 实现 | Maglev | TurboFan 实际优化检查 |
|---|---|---:|---|---|---|
| 自动选择 | 无额外参数 | 1 | NEON | 开启 | 通过 |
| 强制标量 JSON | `--no-json-simd` | 1 | 标量 | 开启 | 通过 |
| 限制 V8 可用 CPU 特性 | `--arm-arch=armv6` | 1 | 标量 | 自动关闭 | 通过 |

分派结果由原生 `JsonSimdRuntimeDispatch` 用例检查并打印。
`test/json/arm32-runtime-tiers.js` 检查 Maglev 能力判断，并通过优化状态位和精确
算术结果确认 TurboFan 实际完成了优化。

**第三组是软件限制测试。** 它限制 V8 可使用的 CPU 特性，不会移除这台手机的
NEON 指令能力，也不表示本产物支持 ARMv6 硬件。此次没有连接无 NEON 或仅 D16
的旧 ARMv7 真机；相关指令可用性证据仍来自此前的 QEMU 验证。

## 正确性结果

| 测试类别 | 自动 NEON | 强制标量 | CPU 特性受限 | 合计 |
|---|---:|---:|---:|---:|
| 原生测试 | 48/48 | 48/48 | 48/48 | 144/144 |
| Test262 执行 | 332/332 | 332/332 | 332/332 | 996/996 |
| 上游语料的 JSON API 对照 | 141,318/141,318 | 141,318/141,318 | 141,318/141,318 | 423,954/423,954 |
| 随机、边界和副作用差分 | 34,628/34,628 | 34,628/34,628 | 34,628/34,628 | 103,884/103,884 |
| 大文本差分 | 112/112 | 112/112 | 112/112 | 336/336 |
| 编译层检查 | 通过 | 通过 | 通过 | 3/3 |
| 同一 `.so` 的集成检查 | 通过 | 通过 | 通过 | 3/3 |

累计 **528,174 次 JSON API 结果对照全部一致**，另有原生测试和 Test262 执行。
以上是执行次数，不是去重后的独立输入数量。

每组原生测试包含 35 项 JSON 与 13 项 MessagePack 检查，覆盖保护页、字符宽度转换、
UTF-16、数字边界、外部字符串、移动 GC、并发 worker、取消、终止、isolate 生命周期，
以及此前暴露 libc++ 配置问题的异常注入用例。

上游语料来自锁定版本的 RapidJSON、yyjson、simdjson 和 nlohmann/json 测试数据。
每组运行 36,680 个 `parse`、36,680 个 `parseAsync`、33,979 个 `stringify` 和
33,979 个 `stringifyAsync` 用例。

额外差分检查数值位模式及负零、属性顺序和描述符、UTF-16、非法语法、
reviver/replacer/proxy 副作用及 GC。大文本组覆盖约 4 MiB 两侧和 16 MiB 以上的
文本、宽字符变体，以及文本四分点附近的非法字符插入；每组包含每个解析 API 的
53 个用例，以及每个序列化 API 的 3 个用例。

参照为此前冻结的原始 V8 12.6 Mac ARM64 结果。对照逐项比较结果文本，保留数值位、
UTF-16 和对象元数据；原生并发生命周期测试另行执行。

动态库检查使用 NDK 头文件编译的调用端，通过 `dlopen` 加载同一个 `.so`，验证
C++ 异常、V8 初始化、四个 JSON API、异步任务完成、JIT 算术、基础 Wasm、
V8 释放和 `dlclose`。三种模式均返回 `JSON embedder smoke status=0`。

## 证据与复验

- [机器可读的真机证据](../tools/json_arm32/evidence/oneplus6-arm32-20261106.json)
- [实现及此前指令模拟验证报告](json-arm32-runtime-dispatch-20261105.md)
- [构建与接入说明](../tools/json_arm32/README.md)

完整日志、输入清单、控制脚本和结果位于
`out/json-arm32-device-20261105-f52ceb328/`。目录名沿用测试开始日期，执行跨越了午夜。
其中 `validate-device.py` 保留完整命令和调度流程，`payload-manifest.json` 保留全部
上传文件的哈希，`device-results.json` 保留测试前后设备文件哈希及逐项返回码。
`external-*`、`differential-*`、`large-*` 中保存了完整对照结果和原始输出。

测试文件保留在手机的独立目录
`/data/local/tmp/v8-json-arm32-f52ceb328-20261105`，便于复验。本地
`run-{neon,scalar,restricted}-d8` 和对应 `cctest-json` 包装脚本可复用；完整控制器
保护已有证据目录，重新执行整套测试时应使用新的输出和设备目录。

本轮完成的是上述设备及执行身份下的 ARM32 原生正确性和兼容性验证。
无 NEON 旧真机、普通业务 APK/JNI 启动链、与应用其他原生 SDK 的 C++ 运行库共存，
以及设备性能测量均不在本次已验证范围内。
