# 使用线程池并行解析 JSON

`JSON.parseAsync(text[, reviver])` 是本分支的**非标准 V8 扩展**，返回 Promise。
语法扫描、结构解析和数字转换在线程池执行；结果对象仍在调用方 isolate 的线程构建。
解析器原有优化见 [第三轮优化记录](json-parse-async-optimization-round3.md)；后续官方同步基线回移及最新对照见 [V8 14.4 JSON 回移记录](json-upstream-backport.md)。
**不是把整段同步 `JSON.parse` 延后执行，也不是整条路径都脱离主线程。**

**接入前提：**宿主必须提供工作线程、non-nestable foreground task 调度，以及
Promise microtask checkpoint。缺少工作线程或 non-nestable 支持时返回 rejected Promise，
不偷偷退回同步解析。多个调用可并行扫描，但不保证任意输入的端到端耗时都低于同步解析。

## 调用方式

```javascript
const [users, orders] = await Promise.all([
  JSON.parseAsync(usersText),
  JSON.parseAsync(ordersText),
]);
```

使用 `try / catch` 或 `.catch()` 处理失败。输入的 ToString 在调用时发生；转换异常、
语法错误和 reviver 异常通过 Promise 拒绝返回。执行终止不转换为普通拒绝。
语法错误由现有 JsonParser 生成，保留错误类型和位置消息。不可调用的 reviver 被忽略。
函数不可作为构造函数，JSON 对象上的属性不可枚举，函数 length 为 2。

支持双字节字符、转义、孤立代理项、重复键、数字键顺序、`-0`、`__proto__` 自有数据属性，
以及 reviver 的删除、this、调用顺序和 context.source。最终结果遵循 Promise resolution：
例如 reviver 返回的 thenable 会被采用。本扩展不改变同步 `JSON.parse` 或 `v8::JSON::Parse` 的 API 和语义。

## 线程与数据流

| 阶段 | 执行位置 | 工作内容 |
| --- | --- | --- |
| 输入准备 | 调用方线程 | ToString、展平字符串、复制到不可变的原生字符缓冲区 |
| 解析 | V8 Platform 工作线程池 | 校验 JSON、扫描字符串与转义、转换数字、记录容器边界和元素类型 |
| 结果构建 | 调用方 isolate 的前台任务 | 用既有 JsonParser 构建器创建 JS 对象，分片保存 GC 根 |
| reviver 与完成 | 调用方线程 | 既有 internalizer 执行回调，resolve / reject Promise |

每份输入是一个独立工作任务，复用平台线程池，不为每次调用创建线程或额外 isolate。
单份文档内部不再细分多个工作线程。并发上限由宿主线程池决定。任务登记的互斥锁
仅用于注册、结束和查询，不包住解析过程，也不把多个 worker 串行化。

工作线程只持有原生内存，不读写 V8 Handle、JSObject、Map 或调用方堆。扫描结果是
每条 12 字节的前序记录：容器结束位置/计数、数字值或字符串位置/标志。reviver 另需
起止位置和主线程快照。普通对象与小型嵌套结构复用 `BuildJsonObject` / `BuildJsonArray`；
无 reviver 的大数组按元素类型预分配并直接填充，double 数组不先创建临时 HeapNumber。
深度超过 16 或超过 128 条记录的结构走可暂停的非递归路径。

为什么不直接在线程池调用原 `JsonParser::Parse`？它会操作调用方 isolate 的堆、
分配 JS 对象并更新 Map，不能与调用方 JS 并发执行。给同一个 isolate 加 Locker 会
把工作重新串行化；另建 isolate 再序列化/反序列化也不能消除主线程对象重建。
当前方案不做整棵 JS 对象的序列化往返，但仍有原生中间结果与主线程分配成本。

## 响应性、内存与吞吐边界

前台构建每片最多推进 4096 步；普通数组最多 128 个元素一批，批次仍计入步骤预算，
在跨过检查阈值时检查约 2 ms 的软预算。小子树一次完成，
大结构保存进度后重新投递 non-nestable task。**2 ms 不是硬性最长任务时间。**
以下工作仍可能形成长任务：

- 输入对象的 ToString、字符串展平和整份输入复制。
- 单个巨大字符串的主线程分配/解码、大数组初始分配、大对象最终构建、GC、错误位置计算。
- reviver 遍历和用户回调，以及 Promise resolution 和 await 后的用户代码。

因此扫描量/数字转换占比高的输入更容易获得并行收益；小对象创建占比高的输入仍可能
总耗时增加。要求解析后还有大量计算时，应把后续计算也放在工作线程自己的 isolate，
只回传最终的小结果，而不是承诺任意巨大对象图都能无成本跨线程交付。

每个任务在扫描期间保留一份原生输入，worker 完成后即释放该副本；前台使用强引用保护的原始 V8 字符串。原生记录使用标准 deque 按块增长，避免连续大数组反复扩容复制；每个前台片段结束后回收已消费的前缀，不把整份记录与完整结果一直同时保留到结束。

后台标记 Smi，前台无需再做通用 double-to-Smi 判断。最多 64 个已解析短字符串在同次解析内复用；哈希碰撞须比较真实内容，不同内容退回原路径，不替换已有槽。UTF-16 和单字节字符串都覆盖。该缓存不跨调用、不跨 isolate；不是无界字符串驻留池。

无 reviver 时另有独立的 64 槽属性键表：对已验证的非空、无转义、非索引短键，按层级与属性序号选槽，并严格比较全部字符和结束引号，命中时替代重复扫描。宽对象超过前 64 个属性不再探测。前台小子树构建复用 GC 根保护的内部化键，但继续执行原对象构建器的 Map / 字段表示检查。键和字符串值不共享槽，碰撞不覆盖旧键；原生节点及字符串描述符仍为 12 字节。
普通与缓存对象构建入口编译期特化同一份实现，避免给同步 `JSON.parse` 增加每对象的缓存选择分支。

Managed 管理状态生命周期，状态在调用、worker 完成和前台回收边界调整 external memory 估计。worker 正在增长的记录尚未逐次反馈给调用方 GC；估计不含 deque 块余量和分配器管理开销，也不是内存配额。宿主/调用者仍须限制输入大小和在途调用数，不能因接口返回 Promise 就无界提交大文档。

## 原生宿主需要做什么

项目版本头为 V8 12.6.228.49。自定义 Platform 必须实现工作线程调度、
`NumberOfWorkerThreads()`、`NonNestableTasksEnabled()` 和 non-nestable task 投递。
前台任务必须在所属 isolate 的线程执行；投递不得同步内联或嵌入正在运行的 JS。

默认 libplatform 的宿主可以在自己的事件循环每轮安排：

```cpp
// platform / isolate 已初始化；仍须遵守宿主既有 Locker 约定。
v8::Isolate::Scope isolate_scope(isolate);
if (v8::platform::PumpMessageLoop(
        platform, isolate, v8::platform::MessageLoopBehavior::kDoNotWait)) {
  v8::MicrotasksScope::PerformCheckpoint(isolate);
}
// 返回宿主事件循环，让输入、UI 等工作运行，不要忙等 Promise。
```

`Isolate::HasPendingBackgroundTasks()` 现在包含 JSON 解析及其未完成构建，供宿主保持
事件循环存活；不能仅凭当前前台队列为空就退出。d8 已消费这个接口并驱动前台任务与
microtask checkpoint。d8 的 setTimeout 不等价于真实 UI 帧调度验证。

## 实现与销毁安全

- `src/json/json-scanner.h`：同步/异步共享的字符分类表和无转义字符串扫描原语。
- `src/json/json-parser-background.{h,cc}`：无 V8 堆访问的语法状态机和紧凑原生记录；语法状态机本身仍独立。
- `src/json/json-parser-async.cc`：线程池投递、分片构建、GC 根、异常与 Promise 生命周期。
- `src/json/json-parser.h`：授权复用原构建器、字符串与错误方法，声明任务查询/取消接口。
- `src/api/api.cc`、`src/execution/isolate.cc`：在途任务查询和销毁前取消。
- `src/builtins/builtins-json.cc`、注册表和 bootstrapper：保留原生 JS 入口。

每片重新创建短生命周期 JsonParser，借用原有 GC 后字符指针刷新逻辑，不跨任务保存
裸字符指针。PersistentHandles 保存源字符串、Promise、native context 和活动构建栈；
完成的数组元素直接放入数组，不为每条记录永久保留单独句柄。对象通过原生快速构建器
写入，不触发继承的 setter。任务切换时恢复发起调用的 realm，结束时恢复宿主 context。

输入取消标志允许长扫描停止；isolate teardown 先标记取消，再 CancelAndWait，
最后释放 Managed 状态和 GC 根。尚未执行的 worker/foreground task 通过 CancelableTask
跳过执行，任务析构不访问已销毁的 isolate。没有 JS 层 AbortSignal 或取消 API。

前一版本附带的 ObjectTwoHashTable 新键计数修复仍保留：否则 reviver 快照表可能无法
正确扩容。这不是本轮新增的并行机制。

## 可复现验证

在已配置好的 V8 构建目录中，从仓库根目录运行：

```sh
ninja -C out/json-async d8 cctest
out/json-async/d8 --expose-gc test/json/parse-async.js
out/json-async/d8 --expose-gc --stress-compaction test/json/parse-async.js
out/json-async/d8 --expose-gc test/json/parse-async-benchmark.js -- 5
```

C++ 用例在 `test/cctest/test-json-async.cc`，使用
`out/json-async/cctest test-json-async/用例名` 单独运行。测试控制真实前台队列，
覆盖跨 realm、暂停期间 GC、终止、无工作线程/不支持前台调度，以及 isolate 销毁后的任务。
`JsonParseAsyncConcurrentWorkers` 使用双 worker 屏障：断言两个实际工作线程均非调用方
线程且同时在途，并验证期间主线程仍能执行 JS 和 GC，不依靠耗时猜测并行。

JS 检查包括固定种子差分样本、1280 个并发输入变异检查、短字符串碰撞、扫描块边界、语法错误消息、Unicode、
reviver、深宽结构、数值数组和原型语义。吞吐基准以同步连续执行 N 次对比
`Promise.all` 的 N 次提交；两边均保留全部结果，显式 GC 后计时并交替执行顺序，包含异步初始调用、
排队、解析、构建和 Promise 完成，不仅测 worker 自身。

### 优化前历史结果（a7a56c174）

macOS x64，`is_debug=false`、`dcheck_always_on=true`，关闭 i18n 和 sandbox。
下表为同一脚本每项 5 轮的中位数，单位 ms；同步列为串行完成全部 N 份，
异步列为并发提交全部 N 份并等待全部结果。各项输入大小见基准脚本输出。

| 输入 | 任务数 | 同步总耗时 | 异步总耗时 | 异步变化 |
| --- | ---: | ---: | ---: | ---: |
| 普通对象数组 | 1 | 35.174 | 41.431 | +17.8% |
| 普通对象数组 | 4 | 141.798 | 154.590 | +9.0% |
| 数值数组 | 1 | 7.511 | 4.494 | -40.2% |
| 数值数组 | 4 | 29.870 | 7.314 | -75.5% |
| 转义字符串数组 | 1 | 4.409 | 5.839 | +32.4% |
| 转义字符串数组 | 4 | 26.050 | 23.531 | -9.7% |
| 嵌套对象数组 | 1 | 45.081 | 48.581 | +7.8% |
| 嵌套对象数组 | 4 | 191.899 | 186.126 | -3.0% |

**不能宣称吞吐普遍达标：**4 份数值数组约快 4.1 倍，但普通对象数组约慢 9.0%。
这个结果验证了真实线程池收益及其边界，不是任意数据的加速保证；尚未在宿主 UI 中测量帧延迟。

已通过 d8 语义/差分自检、`--stress-compaction`、9 项定向 C++ 测试、GN 头文件依赖检查、
按仓库配置过滤 header_guard/include_what_you_use 的 cpplint，以及 `git diff --check`。
原始输出位于 `out/json-worker-*.log`。

完整 cctest 聚合目标仍被原有 WASM SIMD 测试 `alignof(SIMD256NodeObserver) <= 8` 断言阻断。
本次没有改这些测试；使用本地生成的聚焦 Ninja 清单链接 `out/json-async/cctest-json-async`
执行上述 9 项测试，不代表完整 cctest、其他平台、Bazel 或 sandbox 构建通过。
当前构建不支持可写的 `--verify-heap`；未宣称完成该选项验证。

原生任务时序用例的单次诊断输出（包含初始调用，worker_wait 是前台等待而非 worker CPU 时间）：

```text
JSON.parseAsync 2.4 MB: sync=42.649 ms call=0.887 ms worker_wait=9.360 ms foreground=41.441 ms slices=26 max_slice=3.671 ms total=51.689 ms
```

最慢前台片段包含分配、GC 和调度噪声；上述软预算不能用来承诺硬实时上界。

### 后续优化记录

- [第一轮优化与验收](json-parse-async-optimization.md)：紧凑记录、分块回收、批量材料化与短字符串复用。
- [第二轮优化与验收](json-parse-async-optimization-round2.md)：后台状态与字符串扫描特化、原语免递归、前台顺序读取、数值热入口布局控制，以及相对 `bec433731` 的 Release 对照。
- [第三轮优化与验收](json-parse-async-optimization-round3.md)：有界属性键复用、重复键扫描融合、固定线程数并发对照，以及流水线 / 复制 / 内存调度的取舍。
