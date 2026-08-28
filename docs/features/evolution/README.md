# 自进化闭环（Evolution）

[文档首页](../../README.md) · [功能目录](../README.md) · [扩展](../extensions/README.md) · [Workflow](../workflows/README.md)

状态：契约冻结（阶段 0）。实现未落，命令与行为以实现后的程序为准。设计全文
见 `todos/Package驱动的自进化闭环设计.todo`；Package 清单与目录契约见
`todos/统一Package封装与组件挂载系统设计.todo`。

## 一句话

Package 是产物，自进化闭环才是机制。

LubanCode 从真实工作里提炼新本事，封成可查、可验、可卸的 Package Candidate，
交用户批准，灰度上岗，不济就回滚。候选、评测、批准、晋升、回滚合起来，才叫
自进化。Package 只装能力：不做评判，不偷偷改宿主，不替用户扩大权限。

## 闭环八步

```text
任务运行
  -> 收成败证据
  -> 找可复用做法
  -> 生成 Package Candidate
  -> 静态校验
  -> 回放与留出评测
  -> 用户批准
  -> 原子安装
  -> 小流量
  -> 观察、晋升或回滚
```

八步分说：①任务运行并收成败证据；②找可复用做法；③生成候选；④静态校验；
⑤回放与留出评测；⑥用户批准；⑦原子安装并小流量启用；⑧观察后晋升或回滚。
缺一步便不叫闭环：只有生成没有评测，是盲装；只有评测没有批准，是越权。

## 先分五档，别见什么都封包

| 落哪一档 | 判据 | 反例（不落这档） |
| --- | --- | --- |
| Memory | 一句话事实或偏好，用户明说。例：这个项目只用 `uv` | 一整套排查步骤（该进 Skill）；模型推断的偏好未审阅，不可冒充用户明说 |
| Skill | 可复用步骤，换项目仍大体能用。例：排查 provider 绑定 | 只此一次的临时操作；日期、账号、绝对路径焊死的做法 |
| Workflow | 稳定编排：步骤、输入、失败路、验收都说得清 | 分支说不清，每回步骤都变 |
| Agent + Prompt | 稳定角色：多次任务同一套模型角色、工具白名单、Skill 与 Prompt Profile | 一次任务就造一只新 Agent |
| Plugin / MCP | 新执行能力：现有工具压根办不了。例：读不了某种协议 | 现有工具能办，只是提示词没写好 |

推断出的偏好仍进候选箱，须由用户审阅。五档自上而下先试：能靠 Memory 办成
不起包；能靠 Skill 办成不添 Agent；现有工具能用，就不生 Plugin、MCP。

## 采集边界

首版只收已有、可追根的材料：

- `/record` 的 `events.jsonl`、目标口述、变量、验收与最后验证。
- Workflow run 的 definition、events、checkpoint 与最终产物摘要。
- `/goal` 的 objective、iteration、evidence 与终点判词。
- ToolTrace 中脱敏后的工具名、输入形状、outcome、error_code 与短摘要。
- 项目 Memory 中已接受的事实、偏好与 feedback。
- 用户当场说出的纠正、批准与拒绝。

首版不收：

- 模型长篇思考原文。
- 未脱敏的工具输入与输出。
- 只有 HTTP 2xx、没有产物证据的“成功”。
- 模型自己声称“我学会了”的话。
- 被用户拒绝过、内容未变的同类建议。

采集器只产 EvolutionObservation。它不生成 Package，也不决定晋升。

## 触发门槛

不可每跑一轮便生一只包。首版三种触发：

1. 用户明说：`/evolve propose <run|goal|recording>`。
2. 同类任务多次成功，系统只提示“发现可复用做法”，等用户点头再起草。
3. 用户反复纠正同一毛病，系统提出修订既有 Package 的候选。

自动提示至少满足五条：

- 有两次以上独立任务证据；或有一次用户明确要求沉淀。
- 输入、产物、验收大体同形。
- 不是偶然重试、临时路径、日期、账号或单机环境造成的成功。
- 没有同 fingerprint 的 pending/rejected 候选。
- 可说明它比现有 Memory、Skill 或 Package 多解决了什么。

触发门槛只是起草门，不是安装门。过了起草门，仍须走完评测与批准。

## 候选目录

进化出来的 Package 不得直接装进 active catalog。先落候选区：

```text
~/.lubancode/package-candidates/<package-id>/<candidate-id>/
  package/             候选 Package，只读看待
  evolution.json       演化来源与父版本（schema 1）
  eval-plan.json       评测计划（schema 1）
  eval-results.jsonl   只追加的结果账
  approval.json        批准或拒绝记录（schema 1）
```

规矩：

- `<candidate-id>` 形如 `cand-YYYYMMDD-NNN`，与 `evolution.json` 的
  `candidate_id` 一致。
- `package/package.yaml` 的 `version` 写候选瞄准的稳定版号；
  `evolution.json` 的 `candidate_version` 带预发布段。两者主次版本须一致。
- 候选目录与正式 Package 目录（`package-store/`、`packages/`）分开。失败候选
  不污染 PackageCatalog，也不占正式组件名字。
- 只有 staged/active 版本才进正式 Catalog。候选始终归 EvolutionCoordinator 管。

## 候选状态机

```text
observed -> drafted -> validated -> evaluated -> awaiting_approval
  -> staged -> canary -> active

任意非终态 -> rejected
canary / active -> rolled_back
```

状态只许由一枚 EvolutionCoordinator 改。CLI、TUI、Workflow、Agent 都不得各写
一套迁移规矩。每笔迁移落只追加事件账：谁改、何时、因何。

终态两枚：`rejected`、`rolled_back`。到终态不再迁移。

合法迁移，逐条列死：

| 从 | 到 | 触发 |
| --- | --- | --- |
| observed | drafted | 起草器产出最小候选包与演化账 |
| drafted | validated | Package doctor 与组件原生 validator 全绿 |
| validated | evaluated | 评测五道门跑完，结果入账 |
| evaluated | awaiting_approval | 评测材料齐备，提交批准页 |
| awaiting_approval | staged | 用户批准，content hash 复算一致 |
| staged | canary | canary 快照发给点名任务 |
| canary | active | 样本足够，改 active version |
| 任意非终态 | rejected | 用户拒绝；或起草前命中拒绝 fingerprint |
| canary | rolled_back | 灰度不合格，切回旧快照 |
| active | rolled_back | 线上不合格，切回旧版本 |

用法上，rejected 多用于未安装候选；已装版本的退出主路是 rolled_back。

非法迁移，一并定死：

- 跳步：`observed -> validated`、`drafted -> awaiting_approval`、
  `evaluated -> staged`、`staged -> active`。canary 不可省，批准不可绕。
- 回退：`validated -> drafted`、`canary -> awaiting_approval`、
  `active -> canary`。状态不回退。
- 从终态出发的一切迁移，如 `rejected -> drafted`、`rolled_back -> staged`。
- `observed -> staged / canary / active`。

候选内容改一字，content hash 便变，旧候选即作废。重来做新候选、记新
candidate-id；旧评测与旧批准不得沿用。

## evolution.json（schema 1）

`package.yaml` 只写身份、版本与兼容。来龙去脉另放 `evolution.json`，也可同步
写进宿主账本。逐字段：

| 字段 | 规矩 |
| --- | --- |
| `schema` | 必填，首版只认 `1` |
| `candidate_id` | 必填，与候选目录名一致 |
| `package_id` | 必填，与 `package/package.yaml` 的 `id` 一致 |
| `candidate_version` | 必填，SemVer 预发布段，如 `0.2.0-candidate.1` |
| `parent` | 有父写 `{version, content_hash}`；无父明写 `null`，不可假装升级 |
| `objective` | 必填，一句话说清这只想多解决什么 |
| `sources` | 五类稳定 ID：`run_ids`、`goal_ids`、`recording_ids`、`memory_ids`、`user_feedback_ids`。摘要须脱敏；整段会话、密钥、Cookie 一律不收 |
| `generator` | `provider`、`model`、`prompt_revision`。留账，日后复现得出 |
| `changes` | `components_added`、`components_changed`、`components_removed`、`permissions_added`、`tools_added` |
| `created_at` | ISO 8601 |

差异记法：

- `permissions_added` 一条一权：`process:<command>`、`network:<host>`、
  `env:<NAME>`（只记名，不记值）、`fs_read:<根>`、`fs_write:<根>`。根用命名
  根：`workspace`、`package_dir`、`package_data`、`tmp`。
- `tools_added` 记工具 wire 名：`plugin__<包命名空间>.<插件id>__<工具名>`、
  `mcp__<包命名空间>.<服务id>__<工具名>`。

## eval-plan.json（schema 1）

| 字段 | 规矩 |
| --- | --- |
| `schema` | 只认 `1` |
| `candidate_id`、`content_hash` | 绑定候选。hash 对不上，计划即作废 |
| `replay[]` | `source_id`、`task`、`workspace`（fixture 或隔离工作区）、`acceptance[]`。重跑它学来的任务，证明没忘本 |
| `holdout[]` | `task_id`、`task`、`acceptance[]`。至少一份未参与起草的同类任务 |
| `baseline` | `kind` 取 `parent` 或 `bare-agent`；`ref` 写父包引用或未挂载候选的普通 Agent；`metrics` 列对照指标 |
| `budget` | `max_tool_calls`、`max_tokens`、`timeout_ms`。越帽即 fail |

没有留出任务的候选，只可标 `experimental`，不可自动建议晋升稳定版。

## eval-results.jsonl（行 schema 1）

一行一条 JSON，只追加。末行写半截可丢，恢复时跳过不完整行。

| 字段 | 规矩 |
| --- | --- |
| `schema` | 只认 `1` |
| `seq` | 递增序号 |
| `gate` | `replay` / `holdout` / `baseline` |
| `task_id`、`candidate_id`、`content_hash` | 绑定候选与任务 |
| `outcome` | `pass` / `fail` / `skipped` |
| `metrics` | `success_rate`、`acceptance_rate`、`tool_calls`、`tokens`、`wall_clock_ms`、`permission_prompts`、`workspace_writes` |
| `baseline_ref` | 仅 `gate=baseline` 的行必填 |
| `unverified[]` | 没测到的写明，如 `network`、`real-service`、`multi-platform`。不许静默当测过 |
| `verdict` | 可省。评判模型的文字判词，只算一份证据 |
| `recorded_at` | ISO 8601 |

## approval.json（schema 1）

| 字段 | 规矩 |
| --- | --- |
| `schema` | 只认 `1` |
| `candidate_id`、`package_id`、`candidate_version`、`content_hash` | 批准只认当前 content hash |
| `tier` | `content-only` / `process-plugin-or-mcp` / `native-or-core-patch` |
| `status` | `awaiting_approval` / `approved` / `rejected` |
| `requested_at` | 提交批准的时刻，ISO 8601 |
| `decision` | `null`，或 `{decided_by, decision, decided_at, reason, fingerprint}`。`decided_by` 首版只认 `user` |

“批准这只候选”只认当时那只 content hash。文件变过，须重验、重批。拒绝账按
`fingerprint` 去重：内容未变的同款，不再重提，也不再劝装。

## 评测五道门

```text
Package doctor
  -> 组件原生 validator
  -> 来源任务回放
  -> 留出任务评测
  -> 与父版本或裸 Agent 对照
```

- 静态门：Package schema、SemVer、兼容范围；组件 schema、包内引用、路径与
  symlink 越界；密钥扫描、绝对路径、硬编码账号与环境依赖；工具、网络、进程、
  env 与权限差异；code-bearing 分类与信任材料。
- 来源回放：重跑它学来的任务。能在 fixture 或隔离工作区回放的，优先走确定性
  回放。
- 留出任务：至少一份未参与起草的同类任务，证明不是背答案。
- 基线对照：有父版比父版；无父版比未挂载候选的普通 Agent。

指标账：

- 任务成功率与验收通过率。
- 误报、漏报与不可恢复失败。
- tool calls、tokens、墙钟时间。
- 用户确认次数与权限请求次数。
- 工作区写入、外部副作用与回滚结果。
- 没测到的（网络、真实服务、硬件）写 `unverified`，不许静默当测过。

候选不能只因成功率多一分便晋升。权限陡增、耗时翻倍、token 暴涨，须把代价亮
给用户看。

评判规矩：生成模型可自查，不能独自盖章。最终结果由确定性检查、真实执行证据
与独立 Evaluator 合并。Evaluator 的文字判词只算一份证据，不压过测试与产物。
评测 Workflow 与被测 Workflow 须分开，免得自己给自己打分。

## 批准与权限

首版一律由用户批准晋升。批准页须亮出：

- Package id、候选版本、父版本与内容哈希。
- 从哪些任务和纠正里学来。
- 添、改、删了哪些组件。
- 评测样例、基线、结果与未测之处。
- 新增工具、进程、网络、env 与文件权限。
- 安装位置、灰度办法与回滚目标。

分三档：

| 档 | 待遇 |
| --- | --- |
| content-only | 批准后可直接进入 canary |
| process Plugin / MCP | 须另过 Package trust 与运行沙箱 |
| native Plugin / core patch | 不走自动晋升；交人工审查与发布 |

## 晋升、灰度与回滚

正式 Package 不原地改。版本化存放，用 active pointer 或 PackageSnapshot 选中
版本：

```text
~/.lubancode/package-store/
  moontide.provider-auditor/
    0.1.0/
    0.2.0/
```

晋升次序：

1. 候选复制到 staging，复算 hash。
2. 再跑 Package doctor。
3. 原子落入 version store。
4. 新会话或新任务拿到 canary 快照。
5. 老任务继续钉旧快照。
6. 观察到足够样本后，才改 active version。

首版灰度从简：只在用户点名 `/evolve use <candidate>` 时启用，不做复杂流量
路由。回滚只切回旧快照，不删除新版本，不抹评测账。候选已造成外部副作用时，
切版本只撤能力，不假装外部世界已复原；补偿另走步骤。

## 安全铁律

- 缺省不开自动起草，也不开自动安装。
- 项目内容不得因一次 checkout 就启动代码。
- 候选评测用隔离工作区；默认断网或沿用更窄权限。
- 评测不可读取正式密钥。需要真实服务时，须由用户单独批准。
- 候选不得扩大父 Agent 权限；真要扩大，批准页须单列。
- 不把用户私有路径、账号、token、Cookie、环境变量值写进候选。
- 不从模型思考原文提炼规则，只认行为、结果、用户反馈与可核验证据。
- 拒绝账须留 fingerprint，内容未变时不反复劝装。
- active Package 不原地改；会话钉快照。
- 自动生成的 native Plugin 不加载。首版只可产草稿，交人工构建与审查。
- LubanCode core patch 永远不走 `/evolve approve`。它须过 Git diff、构建、
  测试、review 与发布流程。

## 夹具

`tests/fixtures/evolution/` 收两只候选夹具：

- `candidate-content-only/`：content-only 候选。最小包（`package.yaml` +
  `skills/provider-binding-audit/SKILL.md`），评测三行入账，停在
  awaiting_approval。
- `candidate-code-rejected/`：带 process Plugin 草稿的候选。评测虽过，用户以
  未过信任门为由拒绝；拒绝账留 fingerprint。

夹具里 run、goal、recording 各 ID 一律占位；哈希用 `sha256:` 接 64 个 0 占位；
日期统一 2026-08-28。字段与本页逐一对得上。
