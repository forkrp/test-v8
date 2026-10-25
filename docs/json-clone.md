# JSON.clone：普通数据对象的原生深拷贝

本地扩展，基于 V8 12.6.228.49、源码基线 `cb637b6d8`。接口采用 JSON 往返的数据语义，为可直接复制的普通数据树省去 JSON 文本中间表示。

```js
const copy = JSON.clone(original);

// 在未包含此扩展的运行时中兼容部署：
const cloneData = JSON.clone ?? (value => JSON.parse(JSON.stringify(value)));
```

`JSON.clone` 是不可构造的同步函数，`length` 为 1，在 `JSON` 上不可枚举、可写、可配置。它是 V8 内部 C++ builtin，不是 JavaScript 封装，也不是 ECMAScript 标准接口。本次增加 JS 入口及内部 `JsonClone` 函数，没有增加公开的 `v8::JSON` C++ ABI。

## 数据语义

目标是使用原生 JSON 算法执行 `JSON.parse(JSON.stringify(value))` 时得到的数据。新增接口直接调用内部算法，不通过可被业务替换的 `JSON.parse` / `JSON.stringify` 属性。

| 输入情况 | JSON.clone 的结果 |
|---|---|
| 普通对象、数组及嵌套容器 | 每次出现都产生新的容器，修改副本不影响输入 |
| 同一子对象被引用多次 | 各处分别复制，副本之间不保留原来的共享引用关系 |
| 字符串 | 值保持不变；不可变的内部字符串存储可以共享 |
| 对象中的 undefined、函数、Symbol 值 | 对应属性被省略 |
| 数组中的 undefined、函数、Symbol 值及普通空洞 | 变为 null；空洞上的继承属性仍按 JSON 规则读取 |
| NaN、Infinity、-Infinity | 变为 null |
| -0 | 变为 +0 |
| Date | 默认变为 JSON 日期字符串；仍遵循用户定义的 toJSON |
| getter、toJSON、Proxy | 调用次数和顺序遵循现有 JSON 算法；失败的快路径不执行用户代码 |
| Symbol 键、不可枚举属性 | 省略 |
| 属性描述符、原型、不可扩展状态 | 与 JSON.parse 的新普通对象/数组一致 |
| 循环引用 | 抛出与现有 JSON 路径相同的 TypeError |
| BigInt | 默认抛出 TypeError；自定义 toJSON 仍由 JSON 算法处理 |
| 顶层 undefined、函数、Symbol | JSON.stringify 返回 undefined 后解析失败，抛出 SyntaxError |

这与 `structuredClone` 的语义不同。后者保留循环和共享引用、NaN、undefined、Date 等可克隆类型。本扩展适合迁移已经使用 JSON 往返的业务。

这些是数据和可观察属性访问的兼容规则。内存不足、最大 JSON 字符串长度、栈余量等资源边界不保证与文本往返完全一致；直接复制本来就不构造那段 JSON 文本。

## 直接复制的范围

首版采取保守条件：

- 对象使用当前 builtin 所属 realm 的初始 `Object.prototype`，构造器为初始 Object，拥有 fast properties，没有索引元素、拦截器、访问检查、嵌入字段或自定义 toJSON；它本身不能是原型对象、已废弃 map 或不可扩展对象。
- 对象的所有自身属性都必须是字符串键、普通可枚举/可写/可配置的数据字段。遇到符号键、访问器、特殊描述符等情况整体回退。
- 数组使用当前 realm 的初始 `Array.prototype` 和 Array 构造器，只有标准、可写的 length 属性，元素种类为 `PACKED_SMI_ELEMENTS`、`PACKED_DOUBLE_ELEMENTS` 或 `PACKED_ELEMENTS`。
- 当前初始原型链没有影响 JSON 处理的属性。跨 realm、自定义原型、null 原型、字典对象、稀疏或其他 holey 数组等走回退。
- 直接复制的叶值是字符串、布尔、null、Smi 和无需 JSON 归一化的有限 Number。需要省略属性或改变表示的值使整次操作回退。
- 原生复制递归最多 128 层，并检查实际 C++ 栈余量。更深的输入继续走现有 JSON 算法；128 不是公开接口的最大深度。

因此，plain object 不等于一定命中快路径。上述限制为第一版的实现边界，性能表同时保留了回退输入。

## 实现要点

实现位于 `src/json/json-clone.cc`。快路径在禁止 JavaScript 执行的作用域内遍历对象，仅使用原生字段读取。对象及数组通过 `Factory::CopyJSObject` 复用已验证的 map，随后替换需要深拷贝的子容器。

`CopyJSObject` 是浅复制，不能直接作为深拷贝使用。本实现为双精度属性字段分配新的可变 HeapNumber box，避免修改副本数值时影响输入；数组的可变元素存储也独立复制。对 copy-on-write 的对象元素数组，在替换子引用前先分离 backing store。不可变字符串和安全的 copy-on-write 存储无需物理重复。

复制后还会清除输入已有的 identity hash，让副本按需生成自己的散列值，避免同一输入的大量副本在 Map/Set 中产生可避免的散列冲突。

对象先检查完整属性布局，再访问子对象，避免明知末尾存在 getter 时仍复制前面的整个大子树。所有跨分配点存活的对象都由 Handle 保护，子对象分配后重新取得堆指针，支持移动 GC。

循环检查只比较当前祖先链，不用全局 visited 集合去复用结果。这样既能识别循环，又保留 JSON 对重复引用逐次复制的行为。循环、未覆盖形状和特殊值均在不调用业务代码的情况下退出快路径，然后执行一次原生 stringify + parse。真实异常或终止请求直接传播，不再回退。

长遍历每 1024 个工作单元检查执行终止；处理 `kNoGC` 中断，避免在复制中运行用户回调。数组存储的原生批量复制本身不是分片任务。这个接口仍同步占用调用 isolate 的线程，不能把可移动 V8 堆对象指针交给后台线程直接复制。

内部开关 `--no-json-clone-fast-path` 可以关闭直接复制，用同一接口执行原生 JSON 回退，便于语义差分验证。

## 验证与性能

2026-10-25 完成最终核验。实现和原生测试与 2026-10-23 的已验证版本逐文件匹配；本次补齐文档，并为基准加入可选批次时长校准，没有修改引擎实现。

- Release 的 14 项运行全部复测通过，包括默认、强制回退、移动 GC 三种 clone 模式（每种 193,656 条检查）、5 项 clone 原生检查、小栈回退及既有 JSON 回归。
- DCHECK + 堆验证的默认、强制回退、移动 GC 三项运行也全部复测通过。
- 普通对象、数组、共享引用、可变数值字段、copy-on-write、identity hash、getter/toJSON/Proxy、终止和 JSON 特殊值均有定向检查。
- 当前配置为 Apple M1 Pro / macOS ARM64、Release `-O3`，无 LTO/PGO，关闭 Intl/Wasm/sandbox；未验证 Android、全量 V8/test262 或 sanitizer。

正式复测使用同一冻结 Release 二进制，对 clone、JSON 往返和 d8 serializer 三种方法按轮次轮换顺序。11 种输入、4 轮、每进程 9 个计时批次，共 132 个独立进程、1,188 个批次。计时前校准批次目标为 20 ms，降低快速场景的计时量化误差；实测最短批次 18.125 ms。各进程记录的 batch 数量可不同，表格均换算为每次操作耗时。

单位 ms；加速比为 JSON 往返耗时 / JSON.clone 耗时。

| 输入 | JSON 往返 | JSON.clone | 加速比 | 四轮加速比范围 |
|---|---:|---:|---:|---:|
| small_record | 0.000550 | 0.000180 | 3.06× | 3.04–3.07× |
| records_10000 | 5.295900 | 1.781250 | 2.97× | 2.95–3.03× |
| nested_1000 | 0.457053 | 0.222090 | 2.06× | 2.05–2.07× |
| packed_ints_100000 | 1.271424 | 0.033568 | 37.88× | 37.05–38.40× |
| packed_doubles_100000 | 4.315125 | 0.127593 | 33.82× | 33.18–34.98× |
| ascii_strings_1m | 0.909442 | 0.044213 | 20.57× | 20.32–20.79× |
| unicode_strings_1m | 1.839731 | 0.043305 | 42.48× | 42.41–42.51× |
| mixed_shapes_10000 | 6.859375 | 2.720850 | 2.52× | 2.51–2.60× |
| dictionary_fallback | 0.225375 | 0.226107 | 1.00× | 0.99–1.00× |
| sparse_fallback | 0.092489 | 0.093292 | 0.99× | 0.99–1.00× |
| late_getter_fallback | 10.083333 | 10.062500 | 1.00× | 1.00–1.00× |

这些收益来自省去文本往返，尤其是共享不可变字符串存储。它们不是任意对象都能获得的加速；三类回退输入约为 1×，不把小幅波动作为收益。

2026-10-23 原始样本保留在 `out/json-clone-20261023/`，包含部分更短计时批次；上表只使用本次校准后复测，未混合两套测量。最终验证、脚本/源码/二进制哈希和原始样本位于 `out/json-clone-finalize-20261025-qtiiwknn/`。仓库外证据归档位于 `/Users/james/projects/v8/v8-standalone-worktree-docs/2026-10-25/json-clone-final/`。

复现入口：

```sh
out/host/d8 --allow-natives-syntax --expose-gc test/json/clone.js
out/host/d8 --allow-natives-syntax --expose-gc --no-json-clone-fast-path test/json/clone.js
out/host/d8 --allow-natives-syntax --expose-gc --stress-compaction test/json/clone.js

out/host/d8 --expose-gc test/json/clone-benchmark.js -- clone records_10000 9 20
out/host/d8 --expose-gc test/json/clone-benchmark.js -- json records_10000 9 20
out/host/d8 --expose-gc test/json/clone-benchmark.js -- serializer records_10000 9 20
```

基准的 `serializer` 指 d8 暴露的 `ValueSerializer` / `ValueDeserializer` 往返，包含该 d8 绑定创建及复制 ArrayBuffer 的成本。它不是浏览器或 Node 的 structuredClone 基准，不能据此声称所有 structuredClone 实现都更慢。

测量脚本将输入构造、正确性校验、预热和每个批次前的显式 GC 排除在计时之外；计时循环中自然触发的 GC 包含在内。每次只保留一个结果对象。纯数据输入检查输出与 JSON 往返一致。

## 来源

- 当前 checkout 的 `src/objects/value-serializer.cc`、`include/v8-value-serializer.h` 已提供结构化克隆所用的二进制序列化设施，但它们并不提供本接口的 JSON 数据语义。
- [V8：JSON.stringify 快路径](https://v8.dev/blog/json-stringify) 说明无副作用路径及字符串处理优化。当前分支已经回移较新的 JSON 实现，本扩展直接省去文本往返。
- [ECMAScript JSON 算法](https://tc39.es/ecma262/multipage/structured-data.html#sec-json.stringify) 定义现有 JSON 转换行为。
- [WHATWG structured clone](https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal) 定义与 JSON 往返不同的克隆语义。
