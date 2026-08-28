# Workflow Schema 参考

[参考手册](README.md) · [使用指南](../features/workflows/README.md) · [设计指南](../features/workflows/designing-workflows.md) · [运行时](../architecture/workflow-runtime.md)

这页收 `workflow.yaml` 的现行契约。设计取舍见设计指南；内部调度见运行时页。
权威实现位于 `src/workflow/definition.*`、`parser.*` 与 `validator.*`。

## 包与顶层字段

```yaml
schema_version: 1
id: review-report
version: 1.0.0
name: 审核报告
alias: review-report
description: 起草、审核并交付报告
enabled: true

inputs:
  type: object
  required: [topic]
  properties:
    topic: { type: string }
    review_limit: { type: integer, default: 5 }

entry: draft
limits:
  max_concurrency: 4
  max_nodes: 64
  max_steps: 128
  timeout: 10m
  tool_calls: 100
  tokens: 120000

nodes: {}
edges: []
result: {}
```

`id` 只认小写字母开头的小写字母、数字与 `-`。alias 可用 Unicode 文字、
数字、`-`、`_`，但不能带空白、斜杠与控制符。输入 schema 当前核对
`type`、`required`、`properties` 与 `default`；命令行按声明类型把具名参数还原成
JSON 值。

## 节点种类

| `type` | 必要字段 | 产物或用途 |
| --- | --- | --- |
| `tool` | `tool` | 调已注册工具 |
| `agent` | `task`；可选 `role`、`allowed_tools`、`step_limit`、`model_role` | 跑完整 Agent 工具循环 |
| `llm` | `prompt`；可选 `output_schema` | 单次模型调用 |
| `skill` | `skill` | 装载一份 Skill |
| `template` | `template` | 安全模板渲染 |
| `transform` | `operation` | 已注册纯数据变换 |
| `approval` | `input` | 经 InteractionBroker 等批准或拒绝 |
| `ask_user` | `input` | 经 InteractionBroker 取回答 |
| `subflow` | `subflow`；可选 `subflow_version` | 调另一份 workflow |
| `async` | `body` | 在工作线程跑一只普通执行节点并等待 |
| `parallel` | `branches`、`join` | 同时跑多路并汇合 |
| `join` | `branches`、`join` | 显式汇合 |
| `map` | `items`、`body` | 数组逐项展开，可并发 |
| `foreach` | `items`、`body` | 数组逐项顺次执行 |
| `reduce` | `reduce_body`；可选 `initial` | 稳定次序归并结果 |
| `switch` | `conditions` 或 `default_to` | 按结构化值择路 |
| `loop` | `body`、`until`、`max_iterations`、`hard_limit` | 有界条件循环 |
| `checkpoint` | 无 | 显式保存 Store 快照 |
| `end` | 无 | 收终态并构造 `result` |

`Flow`、`Batch`、`Branch`、`Shared`、`Looping`、`Nesting` 是设计原语；其中
有些对应多种节点或整张图，不是额外 `type`。

## 公共节点字段

```yaml
input: { topic: "${inputs.topic}" }
retry:
  attempts: 3
  backoff: exponential
  initial: 1s
  max: 10s
  jitter: true
on_unavailable: fallback
fallback_to: backup
checkpoint: true
side_effects: true
idempotency_key: "${run.run_id}:publish"
```

`on_unavailable` 取 `fail`、`skip` 或 `fallback`。副作用节点若会重试，或被
loop/async 重放，必须声明稳定幂等键。包内 `prompt`、`task`、`template` 引用
不得越出 workflow 目录。

## Edge 与 outcome

```yaml
edges:
  - { from: draft, on: success, to: review }
  - { from: draft, on: error, to: failed }
```

常见 outcome 有 `success`、`error`、`empty`、`skipped`、`joined`、
`cancelled`、`exhausted`。一只节点同一 outcome 只许一条普通边。普通 edge
不得成环；需要条件反复时用 `loop`。

## 条件

`switch.conditions` 与 `loop.until` 共用受限条件：

| `op` | 含义 |
| --- | --- |
| `exists` / `not_exists` | 值存在或不存在 |
| `equals` / `not_equals` | 与 JSON 字面量相等或不等 |
| `gt` / `lt` | 数值比较 |
| `contains` | 数组或字符串包含字面量 |
| `starts_with` | 字符串前缀 |
| `non_empty` | 数组或字符串非空 |

条件只读 Store，不执行任意表达式。

## Parallel

```yaml
fanout:
  type: parallel
  branches: [search_a, search_b, search_c]
  join: quorum
  quorum: 2
  max_concurrency: 3
```

`join` 取 `all`、`all_settled`、`any`、`quorum`、`race`。`quorum` 须给
大于零且不超过分支数的 N。当前 runtime 会等全部分支归队后再评策略；
`any`、`quorum` 与 `race` 尚不提前返回。

## Batch

```yaml
process_items:
  type: map
  items: "${nodes.collect.output.items}"
  body: process_one
  max_concurrency: 4

process_in_order:
  type: foreach
  items: "${nodes.collect.output.items}"
  body: process_one
```

`items` 必须解析成数组。控制节点拥有 body；body 不再接普通 edge，也不能同时
归另一只 map、foreach、loop 或 async。

## Loop

```yaml
review_cycle:
  type: loop
  body: [draft, review]
  until: { op: equals, path: "${nodes.review.output.approved}", value: true }
  min_iterations: 1
  max_iterations: "${inputs.review_limit}"
  hard_limit: 20
```

`min_iterations` 与 `max_iterations` 可写正整数或精确的 `${inputs.xxx}`。
`hard_limit` 只能写定义内正整数。`until` 必须读取本 loop body 的输出。
成功输出含 `completed_iterations`、`previous`、`last`、`history`、
`condition_met` 与 `exhausted`。

## Async

```yaml
wait_remote:
  type: async
  body: remote_call
```

body 只收普通执行节点，不收控制节点。运行时另开工作线程，主调度停在 waiting
态，守取消与 workflow 总时限。body 的成功产物复制到 async 节点名下。它不是
后台放飞，也不是 fan-out。

## Subflow

`input` 显式映射为子 workflow 输入，父图不会把整本 Store 暗中递下去。子图成功
时，其 `result` 成为 subflow 节点产物；失败以稳定错误码回父图。`subflow_version`
当前会解析、序列化并显示在图上，resolver 仍只按 id 取定义，尚未执行版本约束。

## Store 引用

支持的主要路径：

```text
${inputs.topic}
${nodes.search.output.items}
${nodes.search.meta.attempt}
${run.run_id}
```

字符串若整段只有一枚引用，解析后保持 JSON 类型；引用混在文字里时才转成文本。
节点只能读取拓扑上已经完成的产物。`result` 同样从 Store 解析；若成功分支没有
产出它所引用的节点，最终构造会失败。

## 校验入口

```text
/workflow validate <id>
/workflow graph <id> ascii
/workflow doctor
```

runtime 开跑前还会再验一次定义，不能靠绕过命令行让坏图上路。
