# 使用 JSON.parseAsync 避免整份文档连续占用线程

这是本分支的**非标准 V8 扩展**，不是浏览器通用 API。它返回 Promise，
把解析和对象构建分成多个前台任务；**不是后台线程解析，也不保证所有任务都小于 2 ms**。

**接入前提：**宿主必须支持 `TaskRunner::PostNonNestableTask`，持续调度前台任务，
并执行 Promise microtask checkpoint。只执行 JS 脚本或只清空 microtask 队列不能推进解析。
不支持 non-nestable task 时，Promise 会以 `Error` 拒绝，不会退回同步解析。

## 调用方式

```javascript
async function loadJson(text) {
  try {
    return await JSON.parseAsync(text);
  } catch (error) {
    console.error('JSON 解析失败', error);
    throw error;
  }
}
```

`JSON.parseAsync(text[, reviver])` 与 `JSON.parse` 使用相同的输入字符串转换、
数字/字符串词法解析和 reviver internalizer。输入转换在调用时发生；转换异常、
语法错误、reviver 异常通过 Promise 拒绝返回。执行终止不会被转换为普通拒绝。
无效 JSON 的错误类型及位置消息沿用现有解析器。函数不可作为构造函数，属性不可枚举。

支持两字节字符、转义与孤立代理项、重复键、数字键顺序、`-0`、`__proto__` 自有数据属性，
以及 reviver 的删除、`this`、调用顺序和 `context.source`。不可调用的 reviver 被忽略。
最终结果遵循 Promise resolution：例如 reviver 返回的 thenable 会被采用。

## 调度与性能边界

解析器每片最多推进 4096 个语法步骤/空白字符，每 64 步检查一次约 2 ms 的软预算，
未完成时重新投递 non-nestable foreground task。对象属性、数组元素在解析时逐项创建，
**不会在结束后再同步反序列化整棵树**。空白很长、嵌套很深时也能在步骤之间让出线程。
调度公平性取决于宿主：宿主需要在这些任务之间给 UI、输入和其他工作运行的机会。

以下工作仍可能形成长任务：

- 输入对象的 `toString`，以及首次任务中的 cons string 展平。
- 单个超长字符串/数字的扫描、解码、转换，单次大分配、容器扩容、GC，错误位置计算。
- **reviver 遍历和用户回调仍同步执行**；大数据且对响应性敏感时不要传 reviver。
  为 reviver 维护的数组快照最终转换也不是分片的。
- `await` 后用户自己执行的同步工作，以及 Promise resolution 触发的用户代码。

因此适用于包含大量常规对象/数组元素的大文档，不是严格实时保证。调用者仍应限制
不可信输入大小及同时进行的解析数。没有额外 worker isolate、线程池、序列化副本或新依赖。
调度与逐项属性创建会增加总吞吐成本；这个 API 优先降低连续占用，不承诺比同步解析更快。

## 原生宿主需要做什么

项目基于 V8 12.6.228.49。V8 提供引擎、Platform 和任务接口，不提供通用 UI 事件循环。
自定义 Platform 需要正确实现 `NonNestableTasksEnabled()` 和 `PostNonNestableTaskImpl()`，
在对应 isolate 的线程上执行任务，不得同步内联执行投递的任务，也不得嵌套进正在执行的 JS。

使用默认 libplatform 的宿主可以在自己的事件循环每轮安排以下步骤：

```cpp
// platform / isolate 是宿主已初始化的对象。应在 isolate 所属线程执行；
// 使用 Locker 的宿主还需遵守其既有锁约定。
v8::Isolate::Scope isolate_scope(isolate);
if (v8::platform::PumpMessageLoop(
        platform, isolate, v8::platform::MessageLoopBehavior::kDoNotWait)) {
  v8::MicrotasksScope::PerformCheckpoint(isolate);
}
// 返回宿主事件循环，让输入、UI 等任务运行，而不是忙等 Promise。
```

d8 已经驱动前台任务和 microtask checkpoint，因此可以直接运行本文测试。
d8 的 `setTimeout` 在前台任务队列排空之后才处理，不能作为 UI 帧调度的等价证明。
本扩展不改 `v8::JSON::Parse`，后者仍同步；宿主也不能假设一个未完成的 Promise
会自动保持其事件循环存活。

## 实现位置与生命周期

| 位置 | 职责 |
| --- | --- |
| `src/init/bootstrapper.cc`、`src/builtins/builtins-definitions.h` | 在各 realm 的 JSON 对象上注册原生内置函数 |
| `src/builtins/builtins-json.cc` | 提取参数并进入异步调度；原同步内置函数不变 |
| `src/json/json-parser-async.cc` | 保存语法状态、分片构建对象、投递任务、settle Promise |
| `src/json/json-parser.h` | 仅授权异步状态类复用现有词法与错误处理方法 |
| `include/v8-internal.h` | 为 Managed 状态分配独立 external pointer tag |
| `src/objects/objects.cc` | 修正 ObjectTwoHashTable 新键插入后的元素计数，使增量 reviver 快照能正确扩容 |

每片重新创建短生命周期的 `JsonParser<Char>`，保存的是字符偏移而不是跨任务的裸字符指针。
片内沿用现有 GC epilogue 指针更新机制。`PersistentHandles` 保持源字符串、Promise、
原始 native context 和活动嵌套栈可达；栈槽按深度复用，不为每个已完成元素保留独立根。
每步局部 `HandleScope` 及时释放临时句柄，避免整份文档的临时句柄累积。

结果通过 `CreateDataProperty` 写入，绕过继承的 setter，包括 `__proto__`。
每个任务恢复发起调用的 native context，结束时恢复宿主先前的 context。
`CancelableTask` 阻止 isolate 销毁后的执行；`Managed` 在 isolate teardown 时释放解析状态，
任务析构函数不访问可能已经销毁的 isolate/GlobalHandles。

## 验证

在已配置好 V8 的构建环境中，从仓库根目录执行：

```sh
ninja -C out/json-async d8 cctest
out/json-async/d8 --expose-gc test/json/parse-async.js
out/json-async/cctest test-json-async/JsonParseAsyncYieldsAndSurvivesGC
out/json-async/cctest test-json-async/JsonParseAsyncRejectsWithoutThrowing
out/json-async/cctest test-json-async/JsonParseAsyncTermination
out/json-async/cctest test-json-async/JsonParseAsyncTasksOutliveIsolate
out/json-async/cctest test-json-async/JsonParseAsyncTaskTiming
out/json-async/cctest test-json-async/JsonParseAsyncSnapshotTableGrowth
```

JS 自检涵盖语义差分、固定种子随机样本、错误消息、Unicode、reviver、深层/宽层结构、
GC，以及“大任务先提交、小任务后提交但先完成”的让步检查。
C++ 测试额外控制真实前台任务队列，验证跨 realm 恢复、暂停期间 GC、终止、
不支持任务调度的宿主，以及 isolate 销毁后继续运行/销毁已取消任务的安全性。

### 本次本机验证结果

macOS x64，`is_debug=false`、`dcheck_always_on=true`，关闭 i18n 和 sandbox：

- d8 构建成功；上述 JS 自检及额外 `--stress-compaction` 运行均通过。
- 六项定向 C++ 测试均通过，包括快照表扩容/重复键覆盖和已处理异常不泄漏到宿主。
- 新增 C++ 文件通过 depot-tools cpplint；GN 头文件依赖检查及 `git diff --check` 通过。
- 完整 cctest 聚合构建被既有 WASM SIMD 测试的 `alignof(SIMD256NodeObserver) <= 8`
  断言阻断。本次没有修改这些文件；使用仅保留 cctest 基础设施和本功能测试的本地
  Ninja 清单，链接出 `out/json-async/cctest-json-async` 执行六项测试。
  这不是完整 cctest 套件通过的声明。相关清单、构建日志和逐项测试日志保留在 `out/`。
- 当前构建没有启用可写的 `--verify-heap`，因此没有宣称通过该选项的验证；
  暂停期间主动 GC、GC 压缩压力以及 DCHECK 检查已实际执行。

`JsonParseAsyncTaskTiming` 使用约 2.4 MB、100000 个普通对象的数组；
五次独立进程测量的中位数如下，不能外推为硬性帧时间保证：

| 指标 | 中位数 |
| --- | ---: |
| 同步 `JSON.parse` 一次连续执行 | 42.973 ms |
| `JSON.parseAsync` 初始调用返回 | 0.096 ms |
| 前台解析任务数 | 245 |
| 每次运行的最慢解析任务 | 2.846 ms |
| 所有异步解析任务总耗时 | 78.771 ms |

测量原始数据位于 `out/json-async-timing.json`。改善的是单次连续占用，
不是总解析耗时；宿主真实 UI 的调度效果仍需在其事件循环内验证。
