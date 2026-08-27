# Workflow 运行时

[架构总览](README.md) · [设计指南](../features/workflows/designing-workflows.md) · [Schema 参考](../reference/workflow-schema.md) · [app-server](../features/app-server/README.md)

这页解释 workflow 从 YAML 到终态怎样走，重点是 Store、调度、等待、journal 与
恢复。用户命令见 [Workflow 指南](../features/workflows/README.md)。

## 定义链

```text
workflow.yaml
  -> yaml-cpp 解析
  -> WorkflowDefinition 强类型 AST
  -> 归一化 JSON 与内容 hash
  -> ValidateDefinition
  -> WorkflowRuntime
```

parser 只负责语法与默认值。validator 守节点字段、引用、能力、图、预算、路径、
幂等与控制 body 所有权。runtime 开跑前重验定义，`validate` 与 `run` 不分两套
规矩。

主要实现：

- `src/workflow/definition.*`
- `src/workflow/parser.*`
- `src/workflow/validator.*`
- `src/workflow/runtime.*`
- `src/workflow/store.*`
- `src/workflow/journal.*`

## 调度

主调度从 `entry` 起步。普通节点交给按 `NodeKind` 装配的 executor，取得 outcome
后沿 edge 走。控制节点由 runtime 自己解释：

| 控制结构 | 调度办法 |
| --- | --- |
| `switch` | 按声明次序查条件，首中即走 |
| `map` | 按数组展开 body，可并发 |
| `foreach` | 按数组顺次跑 body |
| `parallel` / `join` | 开分支任务，按 join 策略收口 |
| `loop` | 每轮顺次跑 body，轮末查 until |
| `async` | 工作线程跑 body，主调度等待并轮询取消、时限 |
| `checkpoint` | 显式落 Store 快照 |
| `end` | 解析 result，收成功终态 |

主图、loop body 与 async body 共吃 `max_steps` 总账。map/foreach 的数据展开另守
`max_nodes`，parallel 分支链各有步数 guard。工具调用、token、并发与总时限也
不因进入控制节点另开免单账。

## Store

Store 是一场 run 的数据真值：

```text
inputs                    运行输入
nodes.<id>.output         节点产物
nodes.<id>.meta           attempt、耗时与错误等元数据
run                       workflow、version、run_id 等元数据
```

普通节点输出原子提交。同一节点重复提交会被挡住。loop 与控制节点因逐轮更新上下文，
走受控 overwrite 入口。并发分支各写自己名下，`WorkflowRunSummary::nodes` 另用锁
守住并发改账。

## Parallel 与 Async

两者都可能用线程，却不是一回事。

```text
Parallel: 一次放行多条独立分支 -> join 收口
Async:    一只长 I/O body 在工作线程跑 -> 主 Flow 原地等待
```

当前 Parallel 会等全部 worker 归队，再评 join 策略。`any`、`quorum` 与 `race`
还不是提前返回合同。

Async 进入 `waiting_io`，写 `node_waiting` 事件。外部取消或总时限到达时，runtime
把取消信号递给 body，再等线程归队。故而 body executor 必须合作检查取消；C++
线程不能靠 runtime 安全强杀。中断后恢复可能重放未完整提交的 body，副作用必须
幂等。

## Loop 与普通环

普通 edge 若成环，validator 报 `cycle_detected`。Loop 不是普通回边：它把 body、
停止条件、最少轮次、用户软帽与定义硬帽收在一只控制节点里。

每轮结束，runtime 写：

- 本轮 body 各节点输出；
- `previous`、`last` 与完整 `history`；
- `condition_met` 与 `exhausted`；
- `loop_iteration_completed` 事件；
- 一份完整轮次 checkpoint。

条件命中并满足最少轮次，走 `success`。软帽用尽，走 `exhausted`。

## Journal 与恢复

```text
workflow-runs/<run-id>/
  manifest.json
  definition.json
  events.jsonl
  checkpoints/<seq>.json
```

事件只追加并 flush。载荷入盘前按敏感键与文本规则打码。checkpoint 先写临时件，
再原子换名，故恢复不捡半份 Store。

Resume 先读定义快照与内容 hash，再重放已完成节点。普通已完成节点不重跑；loop
会读最新完整轮次继续判定。副作用节点仍须自己提供幂等键，journal 不能替外部
系统撤回一封已经发出的邮件。

## 宿主边界

workflow 层不拥有会话。Tool、LLM、Agent、Skill、Approval、AskUser 与 Subflow
都由宿主装 executor 或 Broker。定义里出现一种节点，不等于每条宿主入口都已经
装好它。缺装配时应明报 `not_configured`，不可悬死或偷换成另一种执行。

终端入口在 `src/app/commands/workflow_commands.*`，app-server 只投影 run 快照与
增量事件。两处应共用同一 runtime、Store 与 journal 合同。
