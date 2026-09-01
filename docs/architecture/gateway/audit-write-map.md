# G0 现状冻结审计与真实写盘图（2026-09-01 工作树）

_本页是 G0 批次的审计账：逐项审既有件、画出当前真实写盘图（每本账的 writer 与 direct write call site）、列断口。开每一批前都须重新查源码；本页“已有”不等于已过真机常驻验收。_

---

[文档首页](../../README.md) · [Gateway 首页](README.md) · [冻结合同](contracts.md)

## 1. 审计基线

- 工作树：`main` 于 2026-09-01（含 workspace P0-0/P0-1/P0-2、channel 阶段 2/3）。
- 单子《总装计划》落笔于 2026-08-31；其后 channel 阶段 2（513f3b55）与阶段 3（e0762a2f）落库。单子 §2.1/§2.2 的“眼下真相”表已按本页更新。

## 2. 逐项审

### 2.1 SessionRuntime（`src/runtime/session_runtime.*`）

P0-2 起恒开一场 `TrajectorySessionLedger`（本类持有 Recorder 所有权）；开账失败由 `trajectory_open_error()` 报，装配层须让会话启动失败，不回退旧 SessionStore（禁 dual-write）。`--continue` 走 resume-at-launch（`start_reason=resume`，source Journal 只读，永不 reopen append）。workspace 四级身份由装配层裁决后整份递进。**结论：会话账单一真值已立；无常驻能力（断口 A 确认）。**

### 2.2 Trajectory resume（`src/trajectory/session_manager.*`、`replay.*`）

`ResumeAsNew` 七步（验账→checkpoint/折叠→悬空分档→新场开张）；`RecoverWorkspace` 以 Journal 可证事实为准重建 session.json、续办换账崩溃；closed 硬门（`UnterminatedStreamsInSession`）。悬空工具已有分档（`ReplayDanglingTool`、`ToolExecutionUnknown`——unknown 副作用不重跑）。**结论：单本账内恢复闭环成立；“按副作用分档的统一恢复裁决器”（`BuildRecoveryDecision`）未立（G3）。**

### 2.3 Goal / Loop restore

- Goal：`GoalSessionWiring`（`src/app/wirings/goal_session_wiring.cpp`）的 ledger sink 是 `MakeSessionLedgerSink(SessionStore&)`——折进旧 SessionStore。P0-2 后旧档不建档（`EnsureBegun` 恒 `Disabled`），**goal 事件当前实际没有 durable 落点**；`RestoreFromArchive` 只读得到 P0-2 之前的老档。恢复后默认暂停。
- Loop：`LoopScheduler` 是内存真值（timer thread 只发 wake、due 队列单飞、错过合并），事件行经装配层折进旧 SessionStore——同上，**P0-2 后无 durable 家**。timer 依附当前进程（断口 A）。
- 两者在 Trajectory schema 里没有领域事件位（只有通用 control 事件）。

### 2.4 Workflow Resume（`src/workflow/runtime.*`、`journal.*`）

节点终态后 checkpoint；`Resume(run_dir)` 从 checkpoint 接续；RunJournal 事件 append+flush、入盘脱敏。**结论：账内恢复成立；没有全局到点触发与进程监督（G2）。**

### 2.5 TaskLedger（`src/tools/task_ledger.*`）

运行时真值接口：TaskRecord（快照/inbox/消息账/活度账/墙钟三信号）+ 递归派工（WaitingChildren/Completing 活态）+ 完成送达三去向（ForegroundCaller/MainTurnContext/ParentTaskInbox，绝不跨级飞 main）。监督器 `AgentSupervisor`（`src/runtime/agent_supervisor.*`）会话级单线程：墙钟期限统一登记、健康拍 500ms、睡眠甄别；自动重派永不做。**结论：内存账完备；`tasks_` 与线程句柄进程死后不续（断口 B，G4 建 DurableTaskStore）。**

### 2.6 BackgroundTaskRegistry（`src/tools/background_tasks.*`）

进程级单例；watcher 持原生句柄收尸（不凭 PID 猜），树杀（Windows Job / POSIX 组），退出码未知不借 0 冒充。**结论：进程内正确；跨 Gateway 重启不接管（G4 按 §11.4 裁决：intent + pid + start token，证不出同一进程记 `ProcessOwnershipUnknown`，不得凭 PID 杀）。**

### 2.7 Channel（阶段 1-3；`src/channel/*`）

- 阶段 1（0887a13）：types、channel.yaml schema 1 严格 parser、Package ComponentKind Channel、4-byte frame codec、双向 JSON-RPC router、`tests/support/fake_channel_sidecar`、稳定错误码。**勾选与源码逐项对账相符。**
- 阶段 2（513f3b55）：账号状态机+generation+退避+锁（AccountLock：pid+start_time+instance_token，核存活再清）、ChannelIngressStore（append-only journal + 三级去重 + dead letter + replay 容错）、每账号每会话队列与背压、pairing、IdleWake 接线（ChannelWakeCoordinator 小口，不反向 include runtime）。
- 阶段 3（e0762a2f）：TurnIngress/Provenance、ChannelRouter 与 binding、ChannelSessionHost（复用 SessionRuntime/Agent/RunTurn；单飞+限额；无审批 fail closed）。
- **关键事实：ChannelManager 目前没有任何生产宿主。** 交互进程 `/channels` 只读配置侧（“本进程没挂 ChannelManager”），sidecar 真进程（bridge_process）与 outbox（阶段 4）未做。**Gateway 将是第一个宿主（G5）。**

### 2.8 App Server（`src/app_server/*`）

控制面与远端前端（stdio/WS），复用 SessionRuntime 底子；BoundedOutbox 是内存出站事件队列，不是 delivery 账。**结论：不自动等于 Gateway，不管公网渠道账号（单子定案不变）。**

## 3. 真实写盘图（writer → 落盘 → direct write call site）

```text
~/.lubancode/
  workspaces/<workspace_key>/                 唯一项目持久化根（P0-2）
    workspace.json                            workspace::manifest.cpp（SessionManager::RegisterCheckout）
    sessions/<session_id>/
      main|subagent|workflow-*.jsonl          trajectory/journal.cpp（TrajectoryRecorder 单写者）
      blobs/、checkpoints/、session.json      trajectory/recorder.cpp、session_manager.cpp
      lifecycle/<op>/intent.json|result.json  trajectory/session_manager.cpp（RunLifecycleOp）
      tombstones/<session_id>.json            trajectory/session_manager.cpp（WriteSessionTombstone）
      session.lock                            trajectory/session_lock.cpp（owner: pid+start token）
    index/（可重建投影）                       trajectory/session_index.cpp（derived，可删重建）
  workflow-runs/<run_id>/                     workflow/journal.cpp（Runtime 持有；events/checkpoint/manifest）
  channels/<channel>/<account>/               channel/ingress_store.cpp（journal.jsonl/dead-letter.jsonl）、
                                              channel/account_lock.cpp（locks）、pairing、account_state
                                              【生产无宿主：当前只有测试装配写入】
  gateway/                                     【G1 起建立；此前零目录零写入】
  sessions/（旧 SessionStore）                 sessions/session_store.cpp——P0-2 起零写（历史档只读）
  memory/、insights/、config.json 等           外围账，不属本单六本账（memory 归 P0-3 另单）
```

内存-only 的账（无落盘）：TaskLedger、BackgroundTaskRegistry（子进程日志除外）、LoopScheduler、GoalCoordinator（事件 sink 指向已停写的旧 SessionStore）。

## 4. 断口再认定（对单子 §2.2 的更新）

| 断口 | 2026-09-01 现状 |
|---|---|
| A 有恢复没有常驻 | 确认。`--continue` 要人起进程；LoopScheduler/IdleWake 的 timer 都住在交互进程里；channel 无宿主进程。 |
| B 有会话账没有服务级任务账 | 确认且加重：goal/loop 事件在 P0-2 后连 durable 落点都没有（sink 指向停写的旧 SessionStore）。TaskLedger/BackgroundTaskRegistry 各自内存表。 |
| C 有 Channel 协议没有 Channel Runtime | 半解：阶段 2/3 件（Manager/IngressStore/锁/路由/Headless 会话）已落库，但无宿主进程、无 sidecar 真进程、无 outbox（阶段 4）。 |
| D 能重放事实不能一概重做动作 | 确认。Trajectory 有悬空工具分档与 unknown 终态，但统一恢复裁决器（`BuildRecoveryDecision`）未立。 |
| E 组件状态有几本，所有权未总收 | 半解：Trajectory 已是唯一 Session 账（P0-2）；六本账中 Registry/Automation/Task/Delivery 四本未建。 |

## 5. G0 对账结论

- 渠道单《多渠道消息接入与常驻ChannelPlugin设计》阶段 1 勾选逐项核过：types/yaml parser/ComponentKind/frame/router/fake sidecar/错误码七项全有源码对应，勾选属实（阶段 2/3 亦实）。无需改勾；渠道单状态行已注记本页对账。
- 本单《总装计划》§2“眼下真相”按本页第 2-4 节更新（channel 三行由“无”改“件已落库、无宿主”）。
