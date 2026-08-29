# 自进化闭环（Evolution）

[文档首页](../../README.md) · [功能目录](../README.md) · [扩展](../extensions/README.md) · [Workflow](../workflows/README.md)

状态：阶段 0/1 已落（0.26.82），阶段 2 已落（Skill-only 候选起草与
propose/diff/reject），阶段 3 已落（评测与基线：静态门、确定性回放/留出、
基线对照、`/evolve test` 与 CI JSON），阶段 4 已落（批准与安装：
approve/use/promote/rollback、staging 原子落 version store、点名 canary、
会话钉快照与哈希失效三处对账），阶段 5 已落（组合包：同指纹簇攒够门槛起
草 Skill+Workflow[+Agent] 组合候选，静态门过不了就地降档 Skill-only，评测
与被测 workflow 分家，批准页亮复杂度代价）。代码型候选、自动建议未落，
命令与行为以实现后的程序为准。设计全文见
`todos/Package驱动的自进化闭环设计.todo`；Package 清单与目录契约见
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

阶段 3 的可执行口径（对上表的两处增补，只增不改）：

- 任务的 `acceptance[]` 元素两种形态：纯字符串 = 人工验收，确定性评测
  判不了，行行如实记 `skipped` + `manual-acceptance`；对象 =
  `{"kind": "..."}` 可执行检查，kind 认五种——`file_exists`（`path`）、
  `json_parses`（`path`）、`file_contains`（`path` + `text`）、`command`
  （`command`，按空白拆 argv 直接起进程，不经 shell 拼串，cwd=任务的
  workspace，超时吃 `budget.timeout_ms`）、`manual`。`path` 一律相对任务
  的 `workspace`。kind 白名单里没有（也不许有）"拿被测 workflow 跑一遍"
  这种 kind——认不得的 kind 整份计划拒解析（阶段 5 钉的评测分家规矩，
  见"组合包（阶段 5 落地）"）。
- `baseline` 可另带 `fixture`：一份基线确定性指标账 JSON（相对候选目录），
  字段 `schema`(1)、`kind`、`ref`、`task_id`、`metrics`（七项全字段）、
  `unverified[]`。没附 `fixture` 的基线只做静态对照，代价对照如实记缺。

## eval-results.jsonl（行 schema 1）

一行一条 JSON，只追加。末行写半截可丢，恢复时跳过不完整行。

| 字段 | 规矩 |
| --- | --- |
| `schema` | 只认 `1` |
| `seq` | 递增序号 |
| `gate` | `static` / `replay` / `holdout` / `baseline` |
| `task_id`、`candidate_id`、`content_hash` | 绑定候选与任务 |
| `outcome` | `pass` / `fail` / `skipped` |
| `metrics` | `success_rate`、`acceptance_rate`、`tool_calls`、`tokens`、`wall_clock_ms`、`permission_prompts`、`workspace_writes` |
| `baseline_ref` | 仅 `gate=baseline` 的行必填 |
| `unverified[]` | 没测到的写明，如 `network`、`real-service`、`multi-platform`。不许静默当测过 |
| `verdict` | 可省。评判模型的文字判词，只算一份证据 |
| `recorded_at` | ISO 8601 |

阶段 3 的增补（只增不改）：`gate` 另认 `static`（静态门也是一道门，
发现即 error 记账）；行可带扩展字段 `checks[]`（逐项检查：`kind`、
`detail`、`outcome`、`note`）、`findings[]`（静态门的密钥/绝对路径发现：
`kind`、`path`、`line`、`detail`，不回显密钥原文）、`notes[]`（预算越帽、
夹具缺失一类的人话）。必填字段一概不动。

阶段 5 的增补（只增不改）：行可带扩展字段 `complexity`（静态门行携带）：
`shape`（`combination`/`skill-only`）、`has_workflow`、`has_agent`、
`components`、`minimal_components`、`extra_components`、`files`、
`minimal_files`、`extra_files`——组合包比最小 Skill 包多出的组件数与维护
面，照实记账（见下"组合包（阶段 5 落地）"）。

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
    0.1.0/            版本目录 = 一份完整包拷贝
    0.2.0/
    channels.json     已装版本账 + active/canary 指针（schema 1，原子替换）
    install-log.jsonl 只追加的 store 事件账（install/canary/promote/rollback）
```

晋升次序：

1. 候选复制到 staging（包目录下 `.staging/<版本>`），复算 hash——与批准
   绑定的对不上即停晋升，staging 清走，正式 store 不动。
2. 再跑 Package doctor（静态门复用 `AnalyzePackage` + 密钥/绝对路径扫描）。
3. 原子落入 version store（`rename` 成正式版本目录；同版本已存在且内容
   不同即拒，不原地改）。
4. 新会话或新任务拿到 canary 快照（`/evolve use` 点名）。
5. 老任务继续钉旧快照。
6. 观察到足够样本后，才改 active version（`/evolve promote`）。

首版灰度从简：只在用户点名 `/evolve use <candidate>` 时启用，不做复杂流量
路由。回滚只切回旧快照，不删除新版本，不抹评测账。候选已造成外部副作用时，
切版本只撤能力，不假装外部世界已复原；补偿另走步骤。

阶段 4 的落地口径（对上册的增补，只增不改）：

- **批准页**：`/evolve approve <候选id>` 一枚命令出材料并批——先递
  `evaluated -> awaiting_approval`（评测材料齐备，提交批准页），再验三道门
  （哈希绑定、档位分类、评测账在），装 store 后落
  `awaiting_approval -> staged`。装失败的候选停在 awaiting_approval，
  清障重批即恢复；同版本同哈希已装则幂等跳过。
- **哈希失效三处对账**：①评测——计划哈希对不上拒评（阶段 3 已落）；
  ②批准——当前哈希与批准账或评测行的哈希对不上即拒批，指路重做候选；
  ③store——staging 复算对不上停晋升；canary/promote/rollback 切指针前
  复算；会话装配折 PackageSnapshot 时再复算，对不上即拒挂并指路
  （重装该版本或 `/evolve rollback`）。store 内文件被手改，下次启动看得见。
- **会话钉快照**：装配一次定终身——`PackageMountInput.store_candidates` 收
  evolution store 的选中版本（scope=Store，同 id 遮蔽次序 dev > project >
  store > user > official），`BuildPackageMount` 折成会话快照；store 里后续
  promote/rollback 改的只是 channels 指针，不动在跑会话的快照，旧版本目录
  一枚不删，在跑任务照旧读得到。
- **回滚语义**：`/evolve rollback <包id>` 无参切父版（演化账 `parent` 的
  版本；无父即撤下——包不再挂载，版本与账保留）；`/evolve rollback <包id>
  <版本>` 切指定版（须在 store 已装账里）。canary/active 状态的候选落
  `rolled_back`（状态不回退；此后再点名切回某版是 store 层的指针操作，
  候选账不动）。
- **命令面**：`/evolve approve|use|promote` 收候选 id，`/evolve rollback`
  收包 id（可带版本）；`/package list` 的账面并进 store 选中版本
  （`store` 过滤词），发现不等于挂载——tamper 的在列，挂载侧拒。

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

`tests/fixtures/evolution/` 收三只候选夹具：

- `candidate-content-only/`：content-only 候选。最小包（`package.yaml` +
  `skills/provider-binding-audit/SKILL.md`），评测三行入账，停在
  awaiting_approval。
- `candidate-code-rejected/`：带 process Plugin 草稿的候选。评测虽过，用户以
  未过信任门为由拒绝；拒绝账留 fingerprint。
- `candidate-eval-smoke/`：阶段 3 的评测形状样板（`cand-20260828-003`）。
  `eval-plan.json` 的 replay/holdout 各带夹具工作区与对象式可执行验收
  （`file_exists`/`json_parses`/`file_contains`），人工字符串验收混排；
  `baseline.fixture` 指向 `fixtures/baseline-bare.json`（裸 Agent 指标账）。
  内容哈希用 64 个 0 占位——真跑评测会因哈希对不上拒评，这是有意的：
  执行型测试在 `tests/unit/evolution/test_evolution_eval.cpp` 里用临时目录
  现算哈希现写计划，不在仓库夹具上留一改就失效的硬哈希。

夹具里 run、goal、recording 各 ID 一律占位；哈希用 `sha256:` 接 64 个 0 占位；
日期统一 2026-08-28。字段与本页逐一对得上。夹具不写真实密钥、账号、绝对
路径；验收命令只放跨平台无害的（文件存在/JSON 可解析一类，不起危险进程）。

## 观察账（阶段 1 落地）

采集器只产 `EvolutionObservation`，落只追加的 JSONL 观察账：

```text
~/.lubancode/evolution/observations/
  observations.jsonl   一行一条观察（schema 1），只追加
  rejected.jsonl       一行一条被拒 fingerprint，只追加
```

`observations.jsonl` 行字段：`schema`(1)、`id`(`obs-` 前缀，由来源类型 +
来源 ID 决定，重采同一条账 id 不变)、`source`(`run`/`goal`/`recording`/
`tooltrace`/`memory`/`user_feedback`)、`source_id`、`source_ref`(原始账文件)、
`summary`(脱敏一句话)、`outcome`(`success`/`failure`/`partial`/`unknown`)、
`fingerprint`、`details`(来源专属脱敏结构账)、`evidence[]`(指回原始账的
引用，不抄正文)、`created_at`(来源账本时间，可空)。坏行/半截行读取时跳过，
不废整账。

同类指纹口径：recording 收目标口述 + 验收 + 工具名序列；run 收 workflow id +
终态 + 节点序列；goal 收 objective + 最终判词；tooltrace 收工具名 + error_code；
memory 收 kind + 标题。日期、URL、绝对路径归一成 `<date>`/`<url>`/`<path>`
占位；时间戳、路径、来源 ID、入参原文、内容哈希、时长、模型名一概不进指纹。
口径版本在指纹前缀 `v1|` 里，改口径即换版本，老账自然失配。

脱敏复用录制件与 journal 的既有打码器（`RedactSecrets`/`SanitizeToolInput`
同规矩）：观察全文查无 token、Cookie、authorization、私钥。模型思考原文
压根不收——适配器只读各家账本的白名单字段，会话消息正文一处不读。

命令面（阶段 1）：`/evolve status`（扫五路账本采观察、落账、报账面）、
`/evolve list`（按指纹聚类列账，可按来源过滤）、`/evolve show <观察id>`
（看一条观察，指回来源 ID 与原始账文件）。三条都只读观察；不生成 Package，
不装任何东西。`rejected.jsonl` 的去重门在阶段 2 接 `/evolve reject` 时启用，
阶段 1 由 store 把守：被拒 fingerprint 不再重复进观察账。

## 候选起草（阶段 2 落地）

`/evolve propose <recording-id|observation-id>` 把一场 `/record` 录制变成
最小 content-only Package Candidate（阶段 5 起簇够门槛可升组合档，见下
"组合包"节）：SKILL.md 复用现有 skill drafter
（偶然值抽象、失败重试折叠），另补一节"排错"收录连败无成功的稳定失败路；
`package.yaml` 只写最小五字段，须过 manifest 严格解析。候选落
`~/.lubancode/package-candidates/<包id>/<候选id>/`（目录形状如上"候选目录"
节，另加只追加的 `state.jsonl` 迁移账），不进 PackageCatalog、不进四层扫描
目录——`/package list` 看不见它。状态机唯一写口是 `EvolutionCoordinator`：
propose 落 `observed -> drafted` 首行，`/evolve reject <候选id> [理由]` 落
`rejected` 并把指纹写进 `rejected.jsonl`（同类不再进观察账，也不再被劝）。
`/evolve diff <候选id>` 与父版或空对照，列新增文件与 SKILL 正文摘要；
`/evolve list` 的账面同时列观察簇与候选区，`/evolve show` 认 `cand-` 起头
的候选 id，回指来源观察。整包内容哈希照 Package 阶段 1 的盘点算法复算，
进 `approval.json` 与 `eval-plan.json`；评测与批准是阶段 3/4 的事。

## 评测与基线（阶段 3 落地）

`/evolve test <候选id>` 跑评测五道门，结果只追加进 `eval-results.jsonl`，
状态经 `EvolutionCoordinator::Test` 迁 `drafted -> validated -> evaluated`
（静态门全绿才 validated，五道门入账才 evaluated；评测行有 fail 不挡
迁移——失败记在账上，状态如实反映"跑完了"）。五道门各自的实现与口径：

- **静态门（doctor + 扫描）**：复用 `package::AnalyzePackage` 做 schema、
  SemVer、组件原生 parser、包内引用与路径越界诊断，不另写一套；另补
  密钥扫描（键形态 `token:`/`api_key=` 一类与裸 `sk-` key，占位值
  `[已打码]`/`{{…}}`/`<…>` 不冤枉）与绝对路径扫描（盘符、UNC、
  `/home/`、`/Users/`、`~/`，URL 不冤枉）——扫候选包全文含 SKILL 正文，
  发现即 error 记账，静态门不过则状态停在 drafted。doctor 没喂当前
  LubanCode 版本，兼容范围检查没做，行里记 `unverified: compat-range`。
- **来源回放与留出**：确定性优先——不起真模型。任务的"执行"由夹具携带
  产物、检查器逐项验收（`file_exists`/`json_parses`/`file_contains`/
  `command`）。夹具放候选目录下（`fixtures/…`，相对路径引用），或
  `tests/fixtures/evolution/`。人工验收（字符串项）如实记 skipped。
  workspace 给了却不在盘上 = 没测（`fixture-missing`），不是测砸。
- **基线对照**：父版在比父版（CI `--baseline <父包目录>` 另做静态对照与
  父版哈希对账），无父比裸 Agent——对照的另一边是基线夹具里的确定性
  指标账（七项指标 + 基线自己的 unverified）。基线行 outcome 只判
  "候选确定性结果是否低于基线"，判不了（无夹具/候选无可执行检查）就
  skipped，不硬判。
- **指标账的诚实口径**：`tool_calls` 只数确定性检查里真起了的进程
  （command 项），`tokens` 恒 0（不起模型），`wall_clock_ms` 为检查执行
  实测，`workspace_writes` 为检查前后快照对比的新增/改动文件数，
  `permission_prompts` 为 0（非交互）。这是"确定性代跑"的账，不是模型
  回合的账；没起模型、真实服务、多平台没测到，行行写 unverified
  （`model-in-the-loop`、`agent-metrics`……）。
- **独立 Evaluator 只在确定性证据之后**：首版 Evaluator 是结构化的
  "确定性结果汇总 + 未测之处清单"（`BuildDeterministicVerdict`），不接
  真模型；行里的 `verdict` 字段留给后续的模型判词，判词权重永远低于
  测试与产物。

`/evolve show` 的候选页带评测摘要：通过几项、没测什么、比基线贵多少
（tool calls/tokens/墙钟/确认/写入五路 delta）。CI 非交互入口：

```text
lubancode evolve test <候选目录> --baseline <父包目录> --json
```

stdout 吐 JSON（候选身份、静态门逐项、各门结果与检查账、汇总
`totals`/`unverified`/`cost_vs_baseline`、确定性判词文本），退出码：
全过 0、有 fail 1、夹具/计划缺失 2。评测引擎与 `/evolve test` 同一枚
`EvolutionCoordinator::TestDir`，写口只有它。

预算（`budget`）在任务行上判：tool calls 或墙钟越帽，该行即 fail
（note 记数）。重跑 `/evolve test` 账照追加（seq 续号），状态不非法
迁移；候选内容改一字，计划哈希对不上即拒评——重做候选，不沿用旧账。

## 批准与安装（阶段 4 落地）

`/evolve approve <候选id>` 出批准页并装架。页面材料照 §十 清单：Package
id、候选版本、父版与内容哈希；来源（run/goal/recording/memory/user-feedback
的稳定 ID）；添改删组件；评测汇总（通过几项、没测什么、比基线贵多少）与
任务样例；新增工具与权限差异（content-only 明写"无"）；安装位置、灰度
办法（`/evolve use`）与回滚目标。批准只认当前 content hash——文件变过，
批准与评测一并作废，重做候选。code-bearing 候选（带 Plugin/MCP 或有
工具/权限差异）首版明拒，指路 Package trust 流程；native/core patch 永不
进 `/evolve approve`。

## 组合包（阶段 5 落地）

阶段 2 的 propose 只出最小 Skill-only 包。阶段 5 升级 `/evolve propose`：
先按观察账聚**同 fingerprint 簇**（点名场在前，账上同指纹的独立任务随后，
簇帽八场；录制件已删或没录完的不进簇），再交起草器判两把尺。

**提炼门槛两把尺**（"同形怎么判"的口径）：

| 尺 | 判什么 | 口径 |
| --- | --- | --- |
| 尺一（Workflow 档） | 稳定编排 | 簇 ≥2 场独立任务，且各场**成功路折叠序列**同形：`tool_call` 事件按连续同名折叠（与观察指纹同一折叠口径）后只数**最终成功**的步子，各场的工具名序列（名字、次数、次序）完全一致；步数 ≥2（单步的稳定做法归 Skill，不算编排） |
| 尺二（Agent 档，更严） | 稳定角色 | 尺一之上，各场**全场工具面**也同形：含失败尝试在内出现过的每一个工具名都一致 |

编排看成功路，角色看整个工具面——失败重试里摸过的工具也是这只角色的
习惯，面不同就不封同一只 Agent。同一场里"失败→成功"的重试折进同一步
（不算稳定失败）；连败不附成功的工具不在成功路上，进失败路。

**组合包形状**（够门槛才出；形状照官方 `examples/packages/browser/`）：

```text
package/
  package.yaml                     最小五字段(id 仍 evolve.<slug>)
  skills/<slug>/SKILL.md           阶段 2 同款(含"排错"节)
  workflows/<slug>-flow/workflow.yaml   节点=成功路折叠步(type: tool),
                                       失败路写进头注释与 description
  agents/<slug>-agent.yaml              尺二过门才添:tools.allow 照观察
                                       到的实际面,skills.preload 预装包内
                                       Skill(包内短名,挂载层折 canonical)
```

偶然值照 §8.2 抽：各场同值（抽象后）的入参留字面量，异值的提成 workflow
输入 `${inputs.step<N>_<key>}`（各场示例进 description）；各场不一致的
嵌套入参不焊死，注记在 workflow 头注释。失败路（阶段 2 已抽的稳定失败
模式，簇内并账）不排进成功链——编排只走成功路，连败的工具以头注释与
description 记下"遇此先换路"，排错细节在同包 SKILL 的"排错"节。验收用
可执行检查器：组合候选的 eval-plan replay 带 `file_exists`/`file_contains`
一类对象式检查（workspace=候选目录，只查包形状），人工口述照列。

**降档路径（不硬塞）**：组合件落盘后立刻过静态门（`AnalyzePackage` 的
引用闭合、canonical 名、无越界 + 密钥/绝对路径扫描）。过不了就地删掉
`workflows/` 与 `agents/`、降回 Skill-only，诊断进 `state.jsonl` 迁移账与
propose 回执（`prompt_revision` 记 `evolution-stage5-downgraded`），演化账
按降档后的形状记组件。降档后的包必须真过静态门，不带病落盘。

**评测 Workflow 与被测 Workflow 分家**：候选包里若带 workflow，评测计划
的执行**不得用被测 workflow 自己跑**。评测执行只有阶段 3 的确定性检查器
（`file_exists`/`json_parses`/`file_contains`/`command`），workflow 组件只做
静态校验 + 来源回放的夹具。acceptance 的 kind 白名单里没有"跑被测
workflow"这种 kind，认不得的 kind 整份计划拒解析——这道门在解析层钉死。

**复杂度代价栏**：评测账的静态门行带 `complexity` 扩展字段——组合包比
最小 Skill 包多出的组件数（skills+workflows+agents 对 1 件）与维护面
（文件数对 2 个）。`/evolve show`、`/evolve test`、批准页、CI JSON 都照实
亮。**不是组件越多越容易晋升**：组合候选的批准页多一行"复杂度代价"，
组件多出的部分须由编排的收益来换，评测与批准照常走五道门。

**命令面**：`/evolve propose <recording-id|observation-id>` 照旧一个入口——
簇够两把尺的门槛出组合候选（回执亮形状、簇大小、组件清单），否则照旧
Skill-only（回执明写"最小 Skill-only 包(默认答案)"）。`/evolve diff` 分档
展示（文件带 `[skill]`/`[workflow]`/`[agent]` 标签，workflow 摘要列节点链
与失败路，Agent 摘要列工具面与预装 Skill）；`/evolve show` 亮形状与复杂度。

验收钉子（单测 `tests/unit/evolution/test_evolution_stage5.cpp`）：同形
两场 → 组合候选（workflow 过 parser 与结构校验、Agent 过 parser、整包过
静态门、diff 见 workflow+agent）；单场或组合不稳 → Skill-only；悬空引用
的组合 → 降档 Skill-only 带诊断；评测计划只带确定性检查器（分家）；
复杂度栏组合 > 最小档。
