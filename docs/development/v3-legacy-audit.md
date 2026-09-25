# v3 旧设计清理清单

[文档首页](../README.md) · [当前 v3 实现](../architecture/session-v3.md) · [字段合同](../architecture/trajectory-v3-schema.md) · [文档规范](documentation.md)

核查日期：2026-09-11 首查；2026-09-23 复核翻新——T08/T11/T12/T15-B 销项 4 条入档，余 10 条逐条重验证据；2026-09-24 V3-GAP-01 销项入档、V3-GAP-02 复验销项（T07 迁移落码后补齐 service 集成测试账）；2026-09-25 V3-GAP-05 销项入档（workflow 域 v3 化棒二落地后移入）。基线为当前工作区源码（版本入口 `src/app/version.hpp`）。各轮都只查代码和文档、标记后续清理项，不删除实现或用户存档。下面都是静态检查结果，不冒充运行验收。

`V3-LEGACY` 标记沿用旧合同的代码或文档；`V3-GAP` 标记新版尚未接全之处。条目各有编号，清理时连同引用、测试与文档一起销项。不是所有 v2 都该删：插件 manifest v2、原生 ABI v2、JSON-RPC 2.0 与会话格式无关。

## 已销项

2026-09-16 的 T 系列单子销掉四条，2026-09-23 复核源码后入档；2026-09-24 销 V3-GAP-01 与 V3-GAP-02；2026-09-25 销 V3-GAP-05。销的是"新版缺口"；旧路退役另有在册条目的，仍留在下表追。

| 编号 | 销项提交与落点 |
| --- | --- |
| V3-GAP-02 | T07，提交 `4f127ce1`：OTLP 投影迁 v3 事件源。流发现走 `ProbeV3SessionStream` 分派（`src/telemetry/service.cpp:556` 一带：v3 场 `DiscoverV3Streams` 认 `<sessionId>.jsonl` 与递归 `subagents/*/<cid>.jsonl`，v2 布局仍发现 main.jsonl，两代主账并存报 `telemetry.session_format_conflict` 不猜）；投影走 `ProjectV3LedgerFile`（`src/telemetry/projector_v3.cpp`：六族 span、usage 吃 assistant message 唯一 owner、`model.usage.appended` 只作观察）；游标三件记到 v3 行粒度（messageId/eventId+lineHash+seq，`cursor.hpp` version 2）。2026-09-24 补验销项条件"重启补投、不重复累计"：unit service 册补 v3 夹具四案（行粒度游标/两代混投同活动不分裂/重启续投 metrics 不翻倍/cursor 丢失覆盖修复），integration 册 T07 已有根账+子账全链、collector 侧批 id 去重、截断尾停投与格式歧义案。旧 `Fold(const EventEnvelope&)` 路保留给旧档，退役归 V3-LEGACY-02 |
| V3-GAP-05 | 2026-09-24 销项（workflow 域 v3 化棒二）。三件事落地：其一，独立节点 session——`src/workflow/node_sessions.cpp` 的 `WorkflowNodeSessions`：llm/agent/skill 节点每次 attempt 开独立 v3 场（`workflow-runs/<runId>/nodes/<execId>/sessions/<sid>/<sid>.jsonl`），父 session 落既有 `subagent.spawn.requested/linked` 对（actionId=attemptId、taskArgs 带 nodeExecutionRef），子卷首行 systemMeta `cause=workflow_node`；usage 经 `/usage` 会话树递归可见（GAP-01 余项收口），非模型节点零伪造。其二，nodeExecutionId/attempt 消费与生产接线——生产 `/workflow run` 切 `account_root`（`src/app/commands/workflow_commands.cpp`，与旧 run 同根两代并存；subflow 经 `WorkflowExecutorContext` 传播），ListRuns 认 v3 布局。其三，失败恢复按 v3 账回放已有账路（ResumeAccount）补 kill-续跑用例钉死（`tests/unit/workflows/test_workflow_node_sessions.cpp` 案2：commit 落稳后注入崩溃，resume 不重跑不重开场）。旧桥清理：生产不再走 v2 编排桥/旧 RunJournal（旧 run 读取恢复保留）；runtime 的 v2 桥分支与 `tests/unit/workflows/test_workflow_trajectory_runtime.cpp` 留作旧路证据，退役归棒二 WorkflowService 收拢。测试册：node_sessions 四案（独立场+并账、kill-续跑、重试新场、零伪造） |
| V3-GAP-03 | T08，提交 `1ddc48bd`：召回/写入桥接 v3。`RecordRecallInjection` 在 v3 writer 在手时改走 `RecordRecallInjectionV3`（`src/app/memory_ledger_bridge.cpp:37`）——召回注入落 display=hidden 的正式 user 消息经 AdmitMessages 进链，`memory.recall.injected` 带 memoryId/revision/hash/messageRef，写入因果边落 `memory.save.requested`。残余：MemoryStore 版本/CAS/遗忘屏障仍归总设计 §4.71，挂 V3-GAP-08 伞下追踪；v2 场 recorder 老路退役归 V3-LEGACY-02 |
| V3-GAP-06 | T11，提交 `7352b867`：五域遗漏事实按域落 v3，早退分支拆除。T11-A 标题来源分家（`src/runtime/trajectory_session.cpp:1748`）、T11-B 审批档 `approval.mode.applied`（`src/trajectory/session_manager.cpp:1600`）、T11-C 环境快照 `session.environment.captured`（`trajectory_session.cpp:1129`）、T11-D 验证簿 `tool.verification.recorded/invalidated`、`tool.observation.late`（`src/runtime/trajectory_turn_bridge.cpp:1828` 起）、T11-E 容量裁决 `context.pressure.recorded`（`trajectory_turn_bridge.cpp:537`）。事实族进字段合同第二节 |
| V3-GAP-07 | T12-B–E，提交 `afd5e6bb`：compact 余项收口。干跑哑客户端不发模型（`src/app/commands/session_commands.cpp:610/967`）、midturn `parentTurnId` 递真实主轮号（1017–1021）、overflow 恢复（2614，`pre_send_overflow` 触发）、hard trim/预览降档 v3 收口（690/2622）。原条目末句"applied 后投影失败只报错、不挡后续请求"一并收口：T12-A `BlockV3Execution`（1146–1156）设会话级执行阻断，后续模型请求/新工具/自动续跑在准入处被拒。无残余 |
| V3-GAP-09 | T15-B，提交 `89060b07`：归档/删除接 v3 生命周期。v3 场归档状态落 lifecycle 账（`src/trajectory/session_manager.cpp:3932` 起分派），删除四段加引用核验，tombstone 一场只删一次；session.json 要求与 main.jsonl 推导封口只属 v2 场 |
| V3-GAP-01 | 本单，提交 `420210bc`：`ListSessionStreams` 两代并出——v2 布局件（main.jsonl/平铺 subagents/workflows）照旧，新增 v3 主账 `<id>.jsonl`（首行 schemaVersion 验明才收）与递归子 session 目录（visited 防环、深度封顶）；`ReadSessionUsage` 两种主账并存从拒读改并账：以 v3 为准，旧 main.jsonl/平铺子账按 v3 开账时刻（树内时间线最早一枚）划界只补前段、开账后一笔不计（防双计），并账/弃账各自点名，验不明（异版本/坏首行）仍按冲突拒读；workflows 编排账不并（V3-GAP-05 另管）。assistant usage owner 与 resume 源去重先前已由 T06（`594aff8d`）落地；空样本≠零消耗由坏账拒读/unknown 样本测试钉着。测试册 `tests/unit/accounting/test_v3_session_usage.cpp` 补两代并存四场。残余不归本条：telemetry 投影同型缺口归 V3-GAP-02（本日已销），insights 自家 v2 读面是 V3-LEGACY-02 |

## 待清理与待接线

| 编号 | 位置与证据 | 状态、替代方向与销项条件 |
| --- | --- | --- |
| V3-LEGACY-01 | `src/trajectory/v3/session_switch.cpp:13` `NewSessionV3WriteEnabled`（`session_switch.hpp:22` 声明）；`session_manager.cpp:902/2408` 分派，`:2199/2399` 一带 v2 场仍走 main.jsonl；`directory.cpp` 旧分支已收，v3 目录另建 | 旧读写仍可达，显式 `0` 还会新建 v2。后续退役写口、格式开关与旧专属测试；旧档读取另定边界，不把它当死代码直接删 |
| V3-LEGACY-02 | `src/trajectory/event.hpp`、`recorder.*`、`replay.*`；消费者仍在：`accounting/usage_projector.cpp:31`、`accounting/session_usage_reader.cpp:28`、`insights/friction_classifier.cpp:74`、`insights/prompt_auditor.cpp:1098`、`insights/integrity_gate.cpp:51`、`app/memory_ledger_bridge.hpp:25`（v2 回退路） | `EventEnvelope/EventScope/RecordRequest` 仍有消费者。逐个改到 v3 消息/事件与投影后再收旧接口；canonical JSON、哈希与耐久基础设施仍共用，不整目录删除 |
| V3-LEGACY-03 | `src/agent/compact.cpp:2239` `CompactTurnPartitioned`；调用仍在 `src/app/commands/session_commands.cpp:1333/2465/2491`、`src/tools/agent_tool.cpp:2269` | 旧四分区双账、archive/kept_indices 与 compact_v2 语义已非默认 v3 合同。保留旧场调用证据，待旧写口退役后清理；新版以 RunV3Compact、compact.applied、上下文链为准 |
| V3-LEGACY-04 | `src/agent/loop.cpp:2930` `tool_result_message.role = api::Role::User`；`:1323/2312/2571` 中断补配对仍用 User | 四角色枚举/adapter 已落地，生产内部容器尚未收敛。替成独立 Tool 时须联改 turn 切分、配对、回调、恢复和 wire 映射；不能只换枚举 |
| V3-LEGACY-05 | `src/hooks/` 下 `dispatcher.cpp`、`protocol.cpp`、`loader.cpp`、`outbox.cpp` 俱在；`docs/features/extensions/hooks.md` 的 legacy adapter | command hooks 与 Lua 中间件并存。按挂点迁移配置、权限、执行顺序、效果采用和恢复；Post/outbox 尚有调用，不能先删旧分派器 |
| V3-GAP-04 | 第一棒已清（2026-09-24）：`src/app_server/server.cpp` 的 `session_main_path` 按场格式分派（v3 场 `<sessionId>.jsonl`、旧场 main.jsonl，`SessionService::v3_format()` 裁决，不再硬拼）；thread/read、thread/resume 的 lastSeq 游标对齐本场段账行并亮 `sourceSessions` 来源链；thread/resume `startExecution=true` 经 SessionService resume-at-launch 真恢复（v3 源续接同 id、v2 源迁移新场；活场拒、坏源 `resume_source_rejected`），缺省仍只读——行为未变，"只读预览"口径保留在缺省档 | 部分销项。残余归 AppServer 2.0 单：完整 2.0 合同（session 命名空间、事件订阅游标、attach/pause/continue）仍是后续工作 |
| V3-GAP-08 | 总设计 §4.67–4.71；现行 goal/loop/memory 各域与命令入口 | 伞条。Goal、Loop、btw、Memory 新合同含独立实施清单；memory 域召回桥已由 T08 推进（见[已销项](#已销项) V3-GAP-03），其余各域状态照旧。旧功能可用不等于新增持久事务已接；按各自实施项推进，不批量删除现行功能 |

## 文档旧口径

| 页面 | 处理 |
| --- | --- |
| 根中英文 README、架构总览、会话指南 | 首查改成 v3 默认、同账两类行、执行中落账与双投影；保留已查明的过渡边界 |
| 用户压缩指南 | 按 v3 重写；旧四分区算法留在架构历史页并加醒目标记 |
| `architecture/context/compaction.md` | V3-LEGACY-03：旧算法参考，不再称现行主路 |
| `architecture/query-data-flow.md` | 更新角色边界、文件格式和落盘时点；保留实际仍存在的 User 工具容器说明 |
| `architecture/trajectory-v3-schema.md` | 更新默认状态、四角色实现与 usage/写入缺口；早期盘点标明历史范围 |
| Hook、AppServer、Workflow、Memory、Telemetry 专题 | 顶部注明当前接线边界，链接新版入口；旧合同有调用方，暂留参考 |
| `development/workspace-storage-v2/` 三页 | V3-LEGACY-01/02：历史迁移快照，不能据行号和阶段结论指导当前删除 |
| v3 `session_switch.hpp` 头注 | 清除"默认关""仅 1 开"等与函数相反的注释，保留现行开关准确语义 |

2026-09-23 复核另翻新四处散引：`features/sessions/README.md`（V3-GAP-09 销项后改注 lifecycle 路）、`features/context/compaction.md`（V3-GAP-07 销项后剩余改指 V3-LEGACY-03）、`architecture/memory/context.md` 与 `architecture/memory/design.md`（V3-GAP-03 销项后改注 T08 现状）。`architecture/memory/flow.md` 已于 2026-09-16 随 T08 先行翻新。

## 设计单的保留边界

对应设计单是新版设计总纲，早期验收记录保留日期，不删历史红项来伪造一次通过。D1/D2/D3 后续修复以当前源码和发行记录核实；§4.67–4.71 新增设计不能因总纲写"已完结"就算落地。LuaHook、AppServer、Workflow 关联单仍各有实施阶段。旧 workspace-storage-v2 快照已逐页标记，新设计单不因文件名含旧术语而整份作废。

## 清理顺序与验收

1. 先迁 usage、Telemetry、Memory、AppServer 与 Workflow 消费方，补 v3 原账夹具。
2. 再收敛内部 Tool 容器、旧 compact 和 command hook 挂点，验证请求 refs 与实际 wire 对齐。
3. 最后退役 v2 新写口及旧专属接口，按另行确定的旧档策略收读取侧；用户存档不随源码清理删除。
4. 每项检查生产调用、CMake、测试夹具、脚本与文档引用，不能拿一次 `rg v2` 结果批量删文件。

两轮文档校验均运行 `bash scripts/check_docs.sh` 与 `git diff --check`。实现清理时再跑相应单测及 `scripts/tests/v3_accept_matrix.py`。该矩阵使用本地假模型后端，不是摘要质量评测。
