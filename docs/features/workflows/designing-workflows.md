# Workflow 设计指南

[Workflow](README.md) · [Schema 参考](../../reference/workflow-schema.md) · [运行时](../../architecture/workflow-runtime.md)

这页教人和 LubanCode 把任务画成 workflow。它讲设计原语，不把所有原语都冒充
YAML `type`。精确字段去查 [Schema 参考](../../reference/workflow-schema.md)。

## 先问：该不该做成 Workflow

适合 workflow 的差事有几样脾气：输入与产物说得清，步骤会复用，失败后要留账，
权限与预算能先定。只问一句、临场探索、下一步全靠未知发现的活，先让普通 Agent
办。别为画图而画图。

动笔前先手工走一份真实输入。写下四件事：

1. 输入从哪里来，缺字段时问谁。
2. 最终交付什么，怎样才算合格。
3. 哪些步骤会读写外部世界。
4. 失败、取消、超时、撞帽后往哪里收。

## 九类设计原语

| 原语 | 问自己的话 | LubanCode 落点 |
| --- | --- | --- |
| Node | 一步究竟办什么 | `tool`、`agent`、`llm`、`skill`、`template`、`transform` 等节点 |
| Flow | 哪一步接哪一步 | `entry`、`edges` 与 outcome |
| Batch | 同一套活要跑多少份数据 | `map`、`foreach`，需要时再接 `reduce` |
| Parallel | 哪些活互不依赖，能同时放行 | `parallel` / `join` 与汇合策略 |
| Branch | 结果不同，下一步是否不同 | outcome 边、`switch`、`fallback_to` |
| Shared | 下游要读上游什么 | Store 与 `${inputs...}`、`${nodes...}` 引用 |
| Looping | 是否要拿本轮结果决定再来一轮 | 有 `until`、软帽与硬帽的 `loop` |
| Nesting | 这段流程是否已经成熟，可整段复用 | `subflow` |
| Async | 是否要等一项长 I/O，同时仍能响应取消和总时限 | `async` 包一只普通执行节点 |

这些原语可以叠，但不要滥叠。先画最小可行图，再添分支、并发与恢复边界。

## Node：一步只担一门差事

节点 id 要说清产物，不要只写 `step1`。节点种类按职责选：

- 已有本机能力，用 `tool`。
- 需要多步工具循环，用 `agent`。
- 一次模型判断或结构化抽取，用 `llm`。
- 套用已有 Skill，用 `skill`。
- 纯排版用 `template`；纯数据搬运用已注册 `transform`。
- 要问人，用 `approval` 或 `ask_user`，不要拿 LLM 猜人的决定。

节点输入只读 Store。产物写进 `nodes.<id>.output`。副作用、重试与幂等在节点旁
声明，不能藏在 prompt 里算数。

## Flow：用 outcome 接路

Flow 是整张图，不是 `type: flow`。`entry` 指入口，普通 edge 写明 from、outcome
和 to：

```yaml
entry: draft
edges:
  - { from: draft, on: success, to: review }
  - { from: draft, on: error, to: failed }
```

一只节点同一 outcome 只接一条普通边。要同时放行多路，改用 Parallel。

## Batch：按数据项重复

一批文件、一列 URL、一组候选，都属于 Batch。数据项互不依赖时用 `map`；后一项
要读前一项，或外部服务怕并发时用 `foreach`。结果要归一份，再接 `reduce`。

Batch 的次数由数组长度决定。它不拿上一轮质量判断“还要不要再来”。这种反复归
Looping。

## Parallel：几路同时跑

Parallel 只收彼此独立的分支。若 B 必须读 A 的输出，就画顺序 Flow。

汇合策略要按业务选：

| 策略 | 何时收口 |
| --- | --- |
| `all` | 全部成功才算成功 |
| `all_settled` | 全部终态后收口，保留成功与失败账 |
| `any` | 任一路成功即可判成功 |
| `quorum` | 成功路数达到 N 即可判成功 |
| `race` | 名义上由最先终态的分支定结果 |

并发会争工具配额、模型限流与工作区文件。分支若会改同一文件，不算独立。
当前 runtime 仍会等全部分支归队，再评 `any`、`quorum` 与 `race`；不要拿这三种
策略当提前返回或省时手段。

## Branch：把判断写成明路

能靠执行结果区分的，直接接 `success`、`error`、`empty`、`skipped` 等 outcome。
要读结构化字段，才用 `switch`。条件只认受限比较，不执行脚本，也不 `eval`。

`fallback_to` 是能力缺失或节点失败后的明路。它不是暗中吞错。最终图上应看得见
失败去了哪里。

## Shared：先定数据合同

Shared 不是一只节点。LubanCode 用 Store 分区传值：

```text
inputs.<field>                 本次输入
nodes.<id>.output.<field>      节点产物
nodes.<id>.meta.<field>        attempt、错误与耗时等账
run.<field>                    workflow/run 元数据
```

整值引用保持 JSON 类型：

```yaml
input:
  items: "${nodes.search.output.items}"
```

设计时先列每只节点读什么、写什么。不要让下游从上游一段自由文本里猜字段。

## Looping：按结果反复，够了便停

普通回边会把后节点重新指回前节点：

```text
draft -> review -> revise -> review
                   ^          |
                   +----------+
```

这种圈没有独立轮次合同，LubanCode 拒绝。条件反复要用 `loop`：

```yaml
review_cycle:
  type: loop
  body: [draft, review]
  until:
    op: equals
    path: "${nodes.review.output.approved}"
    value: true
  min_iterations: 1
  max_iterations: "${inputs.review_limit}"
  hard_limit: 20
```

门槛命中便走 `success`；软帽用满走 `exhausted`。`max_iterations` 可由输入调，
`hard_limit` 必须写死在定义里。历轮结果进 `history`，上一轮进 `previous`。
loop body 会重复执行；有副作用便须幂等。

## Nesting：复用成熟子流程

`subflow` 适合复用已有、独立校验过的 workflow：

```yaml
publish:
  type: subflow
  subflow: publish-report
  subflow_version: 1.2.0
```

不要为少写三只节点就急着嵌套。子流程应有稳定输入、输出与版本边界。自指会被
拒绝；更深的递归也不该拿来冒充 Looping。`subflow_version` 当前只进定义与图示，
runtime 仍按 id 解析子图，尚不能靠它锁住实际版本。

## Async：等长 I/O，不等于并发分叉

`async` 包一只普通执行节点，把它放进工作线程。主调度进入 waiting 状态，轮询
取消与 workflow 总时限。body 完成后，产物再以 async 节点 id 落一份：

```yaml
wait_remote:
  type: async
  body: fetch_remote

fetch_remote:
  type: tool
  tool: remote_fetch
```

它不是 fire-and-forget，也不会让主 Flow 偷跑到下一节点。要同时展开多路，用
Parallel；要等待一只长 I/O 并守住取消、超时，用 Async。async body 中途被打断
后可能重放，有副作用须给 `idempotency_key`。

## 组合顺序

拿到需求，可按这条路想：

```text
先拆 Node
  -> 用 Flow 排先后
  -> 同一套活跑一批数据？Batch
  -> 几路互不依赖？Parallel
  -> 结果不同要择路？Branch
  -> 先定 Shared 数据合同
  -> 按质量反复？Looping
  -> 有成熟整段可复用？Nesting
  -> 有长 I/O 等待边界？Async
  -> 最后补预算、失败路、幂等与恢复
```

不是每张图都要九样俱全。两只顺序节点能办成，就别摆九层阵仗。

## 交付前查账

- 输入 schema 有类型；可调参数给合理 default，用户不必每次都填。
- 每只节点输入来自已完成节点；产物是结构化合同。
- 每条失败路有去处；不可用时是 fail、skip 还是 fallback 已写明。
- Parallel 分支不互踩；Batch 与 Looping 没混用。
- Looping 有 `until`、软帽、硬帽；普通边没有成环。
- Async 与重试、循环里的副作用有幂等键。
- `max_steps` 算进主图、loop body 与 async body；Batch 另查 `max_nodes` 与并发帽。
- 先跑 `validate`，再看 `graph`，最后才运行。

## 致谢

这套原语的整理受 [PocketFlow](https://github.com/The-Pocket/PocketFlow) 启发。
感谢 The Pocket 团队把 Node、Flow、Shared Store、Batch、Async、Parallel 与
“先设计、后编码”的路数讲得清楚。LubanCode 沿用的是这份简洁眼光，不照搬
Python API；本文字段、限制、journal、恢复与安全语义均以 LubanCode 源码和测试
为准。PocketFlow 的原始说明见其[官方文档](https://the-pocket.github.io/PocketFlow/)。
