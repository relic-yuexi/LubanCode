---
name: lubancode-workflow
description: 设计、审查或修改 LubanCode Workflow 时使用；把自然语言任务拆成有界、可校验、可恢复的图，选择 Node、Batch、Parallel、Branch、Shared、Looping、Nesting 与 Async，并生成或修订 workflow 包。普通一次性 Agent 任务不必使用。
---

# LubanCode Workflow

官方文档根固定为 `<技能目录>/../../docs`。设计或审查 workflow 时，先读
`features/workflows/designing-workflows.md`；要写 YAML，再读
`reference/workflow-schema.md`；碰到 Async、恢复、取消或 journal，再读
`architecture/workflow-runtime.md`。用真实绝对路径打开，不把占位文字当目录。

先确认用户要的是可复用流程，不是一场临时探索。根据任务复杂度问必要问题；只问
会改变输入、产物、权限、失败路或图形的岔口，不固定问几轮。

设计时交付一张短账：

1. 输入与最终产物。
2. 节点表：每只节点的职责、种类、读值、写值、外部能力。
3. Flow：入口、outcome、失败路与收口。
4. Store 数据合同。
5. 预算、权限、副作用、幂等与恢复边界。

按任务选原语，不求凑齐：

- 一步一责用 Node；先后关系由 Flow 的 edge 表达。
- 同一套活跑数组用 Batch：独立项选 map，有依赖或怕限流选 foreach，归并用 reduce。
- 多路互不依赖才用 Parallel；先选 all、all_settled、any、quorum 或 race。当前 runtime 会等全部分支归队，不把 any/quorum/race 当提前返回。
- 结果择路用 outcome、switch 或 fallback，条件只读结构化 Store。
- 按本轮质量决定是否再来，用 Looping。普通 edge 不得回环；loop 必有 until、软帽与定义硬帽。
- 成熟、独立、已有输入输出合同的整段流程才用 Nesting/subflow；当前 `subflow_version` 尚不约束 resolver，须另验目标定义。
- 一只长 I/O 要等待并响应取消、总时限时用 Async。Async 不是 fan-out，也不是 fire-and-forget；多路同时跑仍用 Parallel。

写包前查真实能力：已有工具、Skill、transform、workflow 与宿主 executor 叫什么，
便写什么。不得杜撰能力。包落在
`<project>/.lubancode/workflows/<id>/` 或 `~/.lubancode/workflows/<id>/`，
`workflow.yaml` 直接住根，prompt 与模板用安全相对路径。

若用户只要设计，给预览与图便停，不落盘。若用户要创建或修改，先查目标目录；
已有定义不擅自覆盖。落盘后至少运行：

```text
/workflow validate <id>
/workflow graph <id> ascii
```

能从当前环境跑真实 workflow 时，再用一份最小输入验成功路与失败路。不能运行便
明说验到了哪一层。

副作用节点若会 retry、loop 或 async 重放，必须有稳定 `idempotency_key`。
`max_steps` 要算进主图、loop 与 async body；Batch 另守 `max_nodes`。最终 `result`
只引用每条成功收口路径都有的产物。
