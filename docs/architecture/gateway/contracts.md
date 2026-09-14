# Gateway 冻结合同

_单子原文为权威。本页原收拢旧单 §六—§十六的 G0 合同；2026-09-13 起修订依据改为[Session V3 新版总装计划](../../../todos/持久Agent_Gateway接入SessionV3_常驻助理与可靠自动任务总装计划.todo)，并已随 V0 批次同步（过时口径删改、V0 裁决见 §11）。实现批次照本页为唯一真源；改动须回单子改，再同步本页。_

---

[文档首页](../../README.md) · [Gateway 首页](README.md) · [现状审计](audit-write-map.md)

## 1. ProcessMode 与激活闸

```cpp
enum class ProcessMode { Interactive, Pipe, AppServer, Gateway };
```

只有 `ProcessMode::Gateway` 能：起 AutomationScheduler；起 ChannelManager；开 Gateway control endpoint；持 workspace/channel account lock；接受 webhook；拉起 headless session；续 durable outbox。普通 CLI 不因 enabled job/channel 偷启 Gateway；用户手动开的 `/loop`、普通工具后台命令不受影响（新版 §四）。

## 2. 各账与唯一所有者（新版 §5.1 对齐）

| 账 | 唯一所有者 | 存什么 | 不存什么 |
|---|---|---|---|
| Gateway profile/boot | `GatewayProcess`（已落库） | 实例身份、配置版本、启动/停止、健康 | 会话正文、密钥明文 |
| Automation | `AutomationStore`（V1 起） | spec revision、时间计划、每次 occurrence、暂停/取消、触发结算 | 模型输出正文 |
| Channel ingress | `ChannelIngressStore`（已落库） | 原始来信/附件引用、平台去重键、准入与路由裁决 | 整份 session history |
| Work/受理 | 共用持久受理服务（`SessionService`，已落库） | workId、sourceRef、inputRef、状态、ownerEpoch、session/turn 引用 | 抄会话 history |
| Execution | V3 单写者（已落库） | 四角色消息、请求、Action、结果、上下文链、终态 | scheduler 的 next fire、平台 transport retry |
| Task execution | 与异步工具单共用服务（V5 起） | 跨 turn 作业、接管、结果引用、取消 | backend 指针、thread handle、重复的 child 正文 |
| Reply delivery | `DurableReplyOutbox`（V1 起） | 固定正文 artifact、来源引用、目标、发送尝试、回执、未知态 | 重新拼一份 Agent history |

注意：`src/app_server/outbox.hpp`（前端有界事件队列）与 `src/hooks/outbox.hpp`（hook pending/ack 账）都不是渠道 DurableReplyOutbox，名字相似也不能顶替。

Work 是统一执行意图，occurrence/ingress 是触发来源，两者经 sourceRef 关联，不各自另立"是否已执行"判据。Work 的运行投影核对 V3 开轮/终态；UI 快照随时可重建。

## 3. 跨账只传引用

V3 跨场事实引用沿五键 `sessionId/runId/seq/id/hash`；外层再带 `workspaceKey`，不能只留 event hash 或相对路径。

```json
{
  "workspace_key": "...",
  "session_id": "...",
  "run_id": "...",
  "event_id": "...",
  "event_hash": "..."
}
```

Automation occurrence 只记 `run_ref`。Ingress 只记 `turn_ref`。Outbox 只记 committed response artifact 与 event ref。谁也不抄 canonical payload——接纳前正文保存在 durable input artifact（`operations-inputs/`），不得只存 payloadHash 而不留可恢复正文。

受理次序（新版 §六）：

```text
验身份/权限/配额/输入 -> input 原件落稳 -> accepted(sourceRef, inputRef, hash) 落稳
  -> 向调用方或平台确认受理 -> 投内存 wake
```

派发次序（新版 §六；claim/绑定/开轮的执行器归 V1，栅栏先冻结）：

```text
source.accepted -> 幂等建立 work -> claim(ownerEpoch) 落稳
  -> 绑定预留 session/turn 身份与 workId（V3 gateway.work.bound）
  -> V3 输入/开轮事实提交 -> 按统一工具授权执行
  -> V3 终态/回复选择提交 -> 结算 work
```

回复次序（新版 §九）：

```text
turn 终态 -> reply 原件落稳 -> reply.selection.committed 落稳
  -> outbox 投影 -> adapter send -> delivery receipt durable
```

`deliveryId` 定式（新版 §5.2；恢复后仍指原已提交选择，不用新 sessionId 重新散列——resume 一次不多送一次）：

```text
replySelectionId + deliveryTarget + ordinal -> deliveryId
```

崩在 reply 原件与 outbox 投影之间时，RecoveryProjector 按稳定 `deliveryId` 从已提交选择补出同一条 outbox item——只作确定性投影，不重跑 Agent、不调模型。

## 4. 状态机冻结

### 4.1 Automation Job / Occurrence

Job 是长期规则（id/owner/schedule/prompt/agent binding/session policy/delivery/permissions/missed fire/enabled/revision）。Occurrence 是这一次该跑的事实（id/job_id/scheduled_at/state/claim lease/run_ref/delivery_ref）。不可拿 `run_count + next_due` 一行同时冒充两者。

```text
scheduled -> claimed(lease_id, owner_instance, expires_at)
          -> starting -> running(run_ref)
          -> succeeded | failed | cancelled | needs_review
```

调度类型：`at` / `after`（创建时折绝对）/ `every` / `cron`（五字段+timezone）/ `event`（只收内部显式事件或 authenticated webhook）。

错过策略：`Skip` / `CoalesceOnce`（默认，附 missed_count）/ `CatchUpBounded(N)`。默认不无限 catch-up。

claim 与 lease：单机也要有（防 Gateway 重启与手动 run-now 相撞）。恢复裁决五档：

- `claimed` 且无 `run.started`：lease 过期后可重新 claim；
- 有 `run.started`、没有模型请求：按 RecoveryDecision 再开新 run，引用旧 incomplete run；
- 已发送模型请求：不假装同一 attempt 可续，转 `InterruptedAfterRequest`；
- 已有 committed response：只补 outbox；
- 工具 started 无 terminal：`NeedsReview`；terminal 已齐：重建 occurrence terminal，不再跑。

### 4.2 Task（`DurableTaskStore`，G4）

| 崩前状态 | 重启裁决 |
|---|---|
| queued，尚未开 run | 可重新 admission，仍受最新硬安全闸与原 spec 限额 |
| starting，无 run.started | 回 queued，revision + 1 |
| running，无外部副作用 | 新 run 接续，并引用旧 incomplete run |
| model request sent，无输出终态 | interrupted，不重发同一请求；按 task policy 由新 turn 总结或人工续 |
| tool started，无 terminal | needs_review，绝不自动重跑 |
| waiting_children | 从 child durable states 重建；全终态才唤醒 parent |
| cancelling | 续取消与进程树清理；清不清楚则 terminal unknown |
| terminal | 只重建投影与未送达 completion，不再执行 |

mailbox 状态走：`queued -> claimed -> injected -> acknowledged`，旁路 `returned`。已写 `input.received` 才算 injected。ChildCompletion 只送直接父。

后台命令：起进程前落 durable intent、command hash、cwd identity、permission decision；记 pid + process start token 防 PID 复用；证不出同一进程记 `ProcessOwnershipUnknown`，不得凭 PID 杀。

### 4.3 Ingress（已按 `channel/ingress_store.*` 落库，此处冻结总装面）

```text
received -> authenticated -> persisted -> deduplicated -> routed
         -> queued -> claimed -> turn_started -> turn_terminal
         -> reply_enqueued -> completed
任一步 -> rejected | dead_letter | needs_review
```

去重键三级：provider_event_id（永久）→ message_id（永久）→ 指纹（短窗，只作去重推断，不冒充永久身份）。

ACK 规矩：Webhook 验签、最小解析、durable persist 成功后再 2xx；长连接 persist 后才推进 cursor；persist 失败不 ACK；queue 满时已 durable 的进 dead letter，未 durable 的不 ACK。

### 4.4 Delivery（Outbox）

```text
pending -> claimed -> sending -> delivered -> acknowledged
sending -> retry_wait | delivery_unknown | dead_letter
```

`sending` 后崩溃：平台支持 client id / receipt query 就先查再重发；不支持则标 `delivery_unknown`，可按策略重发，但账上明示“可能重复”。不宣称 exactly-once。

Q2 实装注记（QQ 渠道族，`gateway/reply_outbox.*` 的落地态名）：`pending -> sending -> sent`（本地族沿 V1 的 `pending -> delivered`，hash 不符 `flagged`）；`sending` 超时落 `delivery_unknown`（停自动重发）；平台明确拒绝/令牌失效/限频重试耗尽落 `failed`（`delivery_error` 带稳定码）。`claimed`/`acknowledged`/`retry_wait` 的细分档位留后续批次，语义并入 `sending`（在途）与重试节流账——升级时按本表补名，不推翻现账。

ReplyAssembler：只消费 committed `ServerEvent` / Trajectory projection；thinking、usage、secret、内部工具参数默认不出站；preview 与 committed 分账；delivery 失败不回头改 Agent history；同一 committed response 重建出相同 `delivery_id` 与相同 payload hash。

## 5. RecoveryAction 与失败矩阵

```cpp
enum class RecoveryAction {
    NoWork,
    StartNewRun,
    ContinueProjection,
    ContinueDelivery,
    PauseForApproval,
    NeedsReview,
    QuarantineCorrupt,
};
```

`BuildRecoveryDecision(ReplayState, DomainState)` 是纯函数：只读账，不执行动作。Gateway 恢复器按决定办事，并另落 recovery intent/result。

失败矩阵（§16 全表，冻结）：

| 崩溃/故障点 | 重启后裁决 | 禁止动作 |
|---|---|---|
| ingress persist 前 | 等平台重送 | 不 ACK |
| ingress persist 后、ACK 前 | 去重后 ACK，仍只排一轮 | 不开第二 turn |
| queued 后、claim 前 | 重新入公平队列 | 不改原 event id |
| claim 后、run.started 前 | lease 过期可重 claim | 不并发开两轮 |
| run.started 后、model.request 前 | 新 run 引旧 incomplete ref | 不 reopen 旧 Journal |
| model.request 发出、首字节前 | 记 interrupted；仅显式策略可新请求 | 不冒充同一 attempt 重试 |
| partial model output 后 | 保已成账片段，needs review/新 turn 续 | 不抹掉片段重发原请求 |
| tool planned 后、started 前 | 依工具幂等合同判 safe start | 不仅凭名字猜幂等 |
| tool started 后、terminal 前 | needs review / unknown | 绝不自动重跑 |
| tool terminal 后、result commit 前 | 从 terminal artifact/ref 修复 result 投影 | 不再执行工具 |
| response committed 后、outbox 前 | 确定性补 outbox | 不重跑模型 |
| outbox pending 后、send 前 | 续 send | 不重跑 Agent |
| send 后、receipt 前 | 查平台回执；查不到则 delivery_unknown | 不宣称 exactly-once |
| 磁盘满 | 停接新活，保控制面与诊断 | 不继续做外部副作用 |
| Journal hash 坏 | quarantine session/run | 不猜着读、不自动续 |
| adapter 反复崩 | 退避、熔断、NeedsLogin/Failed | 不重启风暴 |
| provider 429 | occurrence 按预算退避或失败 | 不重复已执行工具 |
| shutdown 超时 | 留 incomplete/unknown 与进程树证据 | 不假写 clean |

## 6. disabled 零副作用合同

Gateway 未显式启动时：

- 零 Channel sidecar；零 scheduler timer thread；零 webhook listener；零平台连接；零模型调用；零 Gateway state 写入；不抢 account/workspace lock；
- 可只读展示“已配置，Gateway 未运行”。

这条须有进程级测试，不能只测某个 `enabled` bool。`gateway status` 等只读命令在 Gateway 未运行时不得创建任何 Gateway 目录或文件。

## 7. 稳定错误码

```text
gateway.already_running      gateway.not_running        gateway.lock_stale
gateway.safe_mode            gateway.config_invalid    gateway.control_unreachable
gateway.shutdown_timeout

automation.store_unavailable automation.job_not_found   automation.revision_conflict
automation.claim_busy        automation.schedule_invalid
automation.timezone_invalid  automation.recovery_needs_review
automation.job_terminal      automation.import_conflict

task.store_unavailable       task.spec_mismatch        task.side_effect_unknown
task.process_ownership_unknown

channel.account_in_use       channel.credentials_missing
channel.ingress_store_full   channel.ingress_corrupt
channel.delivery_unknown     channel.outbox_full

recovery.source_corrupt      recovery.source_unsupported
recovery.action_forbidden
```

## 8. 容量、公平与时间（摘要）

队列各层有帽：ingress（每账号/每会话/全局）、automation occurrence（全局/每 job）、headless session 并发、TaskLedger alive/queued、outbox（每账号/全局）、dead letter 磁盘预算。帽满要么拒收且不 ACK，要么已 durable 后转 dead letter；不可静默 drop canonical event。

优先级：用户/Channel 直接来信 > 已到点 one-shot 与人工 run-now > 普通 recurring automation > heartbeat > maintenance。同级轮转；每 agent/session/account 有并发帽；Heartbeat 不能饿死真人消息。

时间：内部 deadline 用 monotonic clock；持久 schedule 用 wall clock + timezone；DST 重复/跳过有固定裁决与 fixture；系统时间倒拨不重放已 terminal occurrence；timer 睡到最近 deadline 或 wake signal，不 busy poll。

## 9. 安全与隐私（摘要）

平台 sender、webhook、sidecar frame 一律外部输入。Gateway 不把 SecretRef 解出的明文交给模型、Trajectory、日志、TaskSpec。外来消息只作 user/channel provenance，不可变 system/developer。控制 endpoint 默认 loopback/local IPC，并要求本机身份或 token。

## 10. G1 实现裁决（V0 修订）

单子未定死、G1 落地时定的三件（第 2 条 V0 已升级）：

1. **Control endpoint 首版形态**：本地文件控制通道（profile 目录内 `control.json` 状态快照 + `control/` 命令文件），零 socket、零端口。local-only 与“要求本机身份”由文件系统权限（user-only 目录）承担。`status --deep` 需要活探针时再升真 endpoint，届时本条修订。
2. **锁裁决（V0 升级：原子互斥 + ownerEpoch）**：`gateway.lock` 记 pid + 进程 start token（复用 `trajectory::CurrentProcessStartToken`/`ProbeLockHolder` 的身份核法）+ boot_id（实例令牌）+ owner_epoch（fencing 代号 = boot_id，一 boot 一 epoch）。取锁不再"先读后写"——头一步是 create-new（`wbx`）原子占位，双进程同时取锁由 OS 保证至多一只成功，互斥不依赖先后顺序；占位撞上才读账核身份（活拒/死清重试/读不懂保守拒）。陈旧锁 = 持有者死透或 PID 复用（token 对不上）；读不懂的锁（含旧 schema 无 owner_epoch 的锁文件）保守拒绝不删。双进程竞争测试见 §11.6。
3. **SafeMode 判定**：boot history 里连续非干净关机（无 clean shutdown 记录）次数 ≥ 3 即进 SafeMode：控制面照起、锁照取、业务面暂停、status/日志明示。干净关机即破连击，退出 SafeMode 无需额外仪式；显式 ack 口留给 V4 的 doctor。

## 11. V0 裁决：合同与持久受理底线（本页新增，回单子报备）

V0 批（2026-09-13）落定的合同。实现锚点见单内 §五/§六；本节是可直接对照的唯一真源。

### 11.1 身份定式（新版 §5.2）

```text
jobId + specRevision + scheduledSlot -> occurrenceId
accountId + providerEventId          -> ingressId（按平台规则补 conversation/thread）
sourceKind + sourceId                -> workId
workId                              -> session lineage + current ownerEpoch
logicalConversationId               -> current sessionRef（恢复后原子换代）
replySelectionId + target + ordinal  -> deliveryId
```

- sourceId/workId 全局带域，不拿 `op-1` 这类会话局部号跨场判重。
- occurrence 的 slot 用计划内时间（wall clock + IANA 时区，存 UTC slot），不用实际启动时间；时钟倒拨不能再造同一拍。
- 多次 resume 沿完整来源链查去重与未完成工作，不只读上一场。
- ownerEpoch 拦旧 worker 迟到提交；它不能撤回外部已发生动作，未知副作用仍须外部对账。Gateway profile 锁内 owner_epoch = boot_id；work 级 ownerEpoch 随 claim 落 Work 账（V1 接执行器）。

### 11.2 跨账栅栏与恢复窗口（新版 §六）

受理/派发/回复三段提交次序见 §3。无跨文件事务，靠稳定身份、先后栅栏和恢复对账补齐窗口：

- source 已接纳，work 未建：按 sourceId 补同一 work。
- work 已认领，V3 未开轮：核实没有旧执行，再开轮；不能凭租约过期认定未执行。
- V3 已开轮，work 还没记 turnRef：从预留身份 / 已注册的 `gateway.work.bound` 关联事实找到原轮，不能再派新轮。
- V3 已终结，work 未结算：补状态，复用输出，不调模型。
- accepted 写稳、队列未入：从账重建；duplicate 回原回执，原 work 仍可观察/推进，不"去重成功、任务失踪"。

### 11.3 schema 版本规矩与关联事实注册

- V3 envelope 新 kind 只许随 schema 纯追加（`EventKindV3` 注册表 + `schema3` 校验器 + schema 文档条目 + fixture），不改旧义。V0 补注册两枚 statusless 事实 kind：`gateway.work.bound`（work↔turn 绑定，恢复反查锚）、`reply.selection.committed`（回复选定，selectionId 恢复后不变）。
- SessionService 受理账（`operations.jsonl`）是服务层账，不进轨迹主账；schemaVersion 2 纯追加 `inputRef`（+重落行的 `originSessionId`/`originOperationId`）与 `operation.dispatched` 行，v1 旧行去重照旧、不重建 pending。

### 11.4 resolver 规则

会话目录一律走 workspace identity/index/resolver（`ResolveDirByWorkspaceKey`）解析实际目录；显示 slug、workspaceKey、实际目录分别处理，不拼目录名、不把显示名当身份。workspace-storage-v2 迁移前的旧 trajectories 平铺根口径废除。

### 11.5 PowerLoss 提交原语与原件保留（新版 §5.3）

- 关键领域提交（受理 accepted/dispatched、认领、执行授权、回复选择、发送回执）要求 PowerLoss 级提交：账行走 `JournalWriter::AppendLine(Durability::PowerLoss)`（fsync/FlushFileBuffers），原件走 `platform::AtomicWriteFile(WriteDurability::ProcessCrashDurability)`（文件 fsync + 目录 fsync + 原子替换）。只看 "flush" 字样不算数。
- 首版用追加事件与原子快照，可复用 JournalWriter 耐久原语；领域 schema 独立版本化，不假冒 V3 会话事件。
- 写盘失败即停受理/停执行：受理面 broken 传播（首笔落不稳后持续拒绝，回执不回成功），泵侧 dispatched 落不稳不取出、停泵报错。
- 索引/快照是派生物，损坏可重建；已提交原件丢失或中段断链则隔离，不跳过继续执行。只有未提交残尾按备份、修复意图、修复回执流程处理。
- 活 work、来源链、未送回复持有 artifact 保留引用；压缩、清理、归档不得删这些依赖。孤立原件（原件在、账行未落）可回收，不冒充受理。

### 11.6 V0 已验面与未验边界（如实分账）

已验（CI 可重复）：

- `tests/unit/runtime/test_session_service_durability.cpp`：受理次序（原件+账行+回执）、accepted 落稳入队前窗口重建、孤立原件不重排、dispatched 不重排、多跳来源链（A→B→C）、磁盘写失败拒受理与 broken 传播、v1 旧行兼容。
- `tests/unit/gateway/test_gateway_lock_race.cpp`：真起两只 racer 子进程（`tests/support/gateway_lock_racer.cpp`，跑生产同一份 `GatewayLock::TryAcquire`）抢同一把锁——至多一只持锁、真撞 RefusedAliveHolder、无坏锁误吞；ownerEpoch fencing（同进程不同 epoch 互不相认）；旧 schema 锁保守拒。
- `tests/unit/trajectory_v3/test_v3_gateway_contract.cpp`：两枚新 kind 的注册表、statusless、载荷合同正反例。

未验（后续批次真机验收，不以测试缝隙冒充）：

- 真拔电/断电一致性（fsync 档位是合同与调用面正确性，断电行为未上真机）。
- 真进程硬杀窗口（accepted/claim/开轮前后）：本批只注入账态；真起子进程在具名栅栏硬杀归 V1 主泵批。
- claim/开轮裁决执行器（V1 主泵未落，栅栏先冻结在 §11.2）。

锁面分账：`GatewayLock` 与 `channel::AccountLock`（V0 同步改 create-new 原子占位）都有双进程竞争册；会话锁 `trajectory::SessionLock` 本就是 `wbx` 原子创建，V0 未动也未加新竞争册。

## 12. V1 裁决：最短纵向闭环（2026-09-13，回单子报备）

V1 批落定的实现裁决（单子 §十 V1 五件事的落点）：

1. **主泵形态**：`gateway::GatewayWorkPump` 合同口（engine 层）+ `runtime::GatewayAutomationPump` 真装配（runtime 层）。同步单飞——每 `TickOnce` 至多一枚新执行 + 一轮恢复扫描 + 一轮 outbox 投递；stop 在 turn 边界生效（宽限内收不净如实记 `shutdown_timeout`）。取件排序复用 `SessionWorkScheduler`（`WorkKind::AutomationDue` 纯追加，与 goal/loop 同数值档）。收尾次序：`StopAccepting`（暂停接活）→ 主循环退出（摘 wake）→ `Close`（收执行器与领域 writer）→ 既有钩子/锁。
2. **AutomationStore**：`automation/jobs.jsonl` 纯追加事件账（job.created / occurrence.created / occurrence.claimed / occurrence.bound / occurrence.settled），`JournalWriter::AppendLine(PowerLoss)`，lazy 开写者。创建入口 = `control/` 命令文件（`gateway job add|run-now` CLI 落，泵消费即删）；`job list`/status 走只读投影 `ReadAutomationProjection`（零建目录零写盘）。occurrenceId = hash(jobId + revision + slot)，slot 用计划内时间。
3. **headless 装配**：`runtime::HeadlessExecutor`——SessionService 开场（V3 强制，v2 明报拒绝不双写）、受理/派发走 V0 修复后的账路、`gateway.work.bound` 落 V3（PowerLoss）、hook 四点、工具 fail closed（allow 名单）、ToolTraceHub 挂桥（补齐 AgentChannelEngine 的工具栅栏遗留）、预算三根硬线（AgentRuntimeProfile）、取消链（`AgentLoop::Run` 的 cancel 旗贯通到 wire 与工具）。回复选择冻结策略：`selectionId = "sel-" + turnId`，正文 = turn 内最后一条 assistant 的 text 块全文（`PlanReplySelection` 纯函数，执行路与恢复路同一份）；原件先落（`delivery/replies/`）→ `reply.selection.committed` 后提交（PowerLoss）。
4. **DurableReplyOutbox（本地）**：`delivery/outbox.jsonl`（item.enqueued/delivered/flagged）+ `replies/` 原件 + `out/<deliveryId>.txt` 发布。deliveryId = hash(selectionId + target + ordinal)。投递幂等：已发布核 hash 补回执不出第二份；hash 不符/原件缺失 → flagged（隔离）。
5. **status 分栏**：`ProbeStatusSections` 只读投影 work/execution/delivery 三栏，与 process 活探针分开报——进程 running 不冒充任务成功。

V1 恢复裁决（§八的 V1 面；重派/补跑归 V2）：occurrence claimed 未结算时——bound 行在且 V3 有 assistant 且无 selection → 补 selection（续原卷 `V3Writer::Continue` 追加事实行，不调模型）；selection 已在 → 补 outbox 投影与投递；V3 无 assistant（生成未完成）→ needs_review；bound 行不在（claim 后崩）→ needs_review（V1 保守：核实无旧执行后重派归 V2）。

V1 已验/未验分账见单子 V1 勾选；真起子进程在具名栅栏硬杀的冒烟仍未验（CI 册用"装配销毁重建 + 盘上调用计数"模拟，如实分账）。

## 13. V2 裁决：周期调度与可靠接管（2026-09-15 落，回单子报备）

V2 批落定的实现裁决（单子 §十 V2 五件事的落点）。调度引擎在 `src/gateway/automation_schedule.*`（纯函数零 IO），账面扩展全走 `automation/jobs.jsonl` 纯追加行（新行 type 见下），V1 旧行重放语义不变。

### 13.1 计划形态与拍点

- 三形态：`once`（V1 不变）/ `interval`（锚点等差：slot = anchor + k·interval，k≥1，纯 UTC，时区存而不参与运算）/ `cron`（五字段受限子集：`*`、`N`、`A-B`、`A-B/S`、`*/S`、逗号并列；英文名、`?`、`L/W/#`、`@词`、非五字段一律明拒不猜；dow 的 7 归一为 0）。
- 时区显式存储（IANA 名或 `UTC±H[:MM]` 固定偏移串），不拿进程本地时区当隐含值。内置规则表（`automation_schedule.cpp` FindZone）：UTC、Asia/Shanghai、Asia/Tokyo、America/New_York（US 2007+ 规则）、Europe/Berlin（EU 规则，过渡点按 UTC 01:00）+ 固定偏移串。认不出的名字 `automation.timezone_invalid` 明拒。不引 tzdata——libc++（macOS 腿）无 C++20 tzdb，手写规则表是三腿可编译的取舍；带 DST 的区只按现行规则算，不做历史考古。
- **DST 边界两裁**（测试钉，NY/Berlin 双区 + 固定偏移）：春跳缺口内的墙钟拍不存在——不补不挪，直接跳过（下一拍是缺口后第一个命中的墙钟）；秋拨重复时段的墙钟拍只取第一次出现（较早绝对时刻），第二次不再单独触发。拍点一律计划内 UTC 毫秒，occurrenceId 沿 `hash(jobId+revision+slot)` 定式。
- cron 校验含"两年内至少一拍"（Feb-30 这类永不命中的表达式创建即拒）。

### 13.2 misfire、合并、并发与队列帽

- misfire 政策：`coalesce`（默认）——停机/占用跨多周期时合并补一拍：occurrence 落最老一拍、`missedCount` 记覆盖范围（既无待办时新建；已有 scheduled 待办时 `occurrence.merged` 行并进，不建第二枚）；`skip`——迟到判定 = `now - slot > 宽限`（interval 宽限 = min(周期, 60s)，cron 固定 60s，泵轮询粒度量级）：迟到的拍不补不并、游标直进，最近一拍在宽限内仍算"当前拍"照跑（不算补），连最近一拍都超出宽限则全跳、下一拍等未来。
- 生成游标 `schedule_cursor` 只前进（`job.schedule_advanced` 行），时钟倒拨不重跑原 slot、不出新拍；cursor 是唯一"哪些拍已消化"的真源，occurrence 集合是派生事实。
- 同 job 不重叠：有 claimed 未结算的活儿不生成、不进游标（收口后余拍合并补）；最多留一份合并待办。
- 队列帽：全局 open（scheduled+claimed）occurrence 数达帽（默认 256，`AutomationStore::set_max_open_occurrences` 测试可调）停生成、游标不动、下轮重试（`stalled`），不无限 catch up。恢复扫描单飞沿用 V1（每 tick 至多一枚新执行）。
- `paused` 不补跑：pause 停生成停派发（已排待办原地等，不结算）；resume 游标直进到 resume 时刻，paused 窗口的拍不回填。

### 13.3 领域操作（CAS + 幂等键）

- `update/pause/resume/cancel` 一律带 `expectedRevision`（必须显式且相等；0 拒）与 `idempotencyKey`（同键同操作同任务回原回执，同键异操作/异任务 `automation.revision_conflict`）。照 GoalService 先例。
- update：`--prompt/--at/--every/--cron/--tz/--misfire/--deadline/--heartbeat` 出现即改；改周期（every/cron）即重锚（anchor/游标 = update 时刻，revision+1），**排队中的 occurrence 固定建账时的 revision，不随 update 挪**（occurrenceId 定式保证）。once 的 occurrence 建账即落，update 不挪它（改期走 cancel 重建）。
- cancel：终态。先停未来派发（scheduled 的 occurrence 就地结算 `cancelled`/`job_cancelled`，历史保留不删账），再处理在飞（claimed 的落 `occurrence.cancel_requested`——执行收完按事实结算，取消请求把 cancel 旗置位、执行在 turn 边界收场后按 `cancelled` 收口不判失败）；后续 update/pause/resume/run-now 一律 `automation.job_terminal` 拒。
- 命令面：CLI `gateway job add|run-now|list|read|update|pause|resume|cancel|import-loop`（写操作落 `control/` 命令文件由活 Gateway 消费，list/read 只读投影零写盘）。
- deadline：job 可带 `deadlineMs`；过线不生成、待执行 occurrence 在 claim 面结算 `cancelled`/`deadline_reached`（不判 failed、不再执行）。在飞硬掐沿用 V1 墙钟预算（同步泵 turn 边界生效，如实分账）。

### 13.4 恢复裁决（§八表的 V2 落面）

- **claim 后无开轮事实**（领域绑定行不在）：绑定先于 V3 `gateway.work.bound` 与一切模型/工具动作——绑定行不在 = 该 work 从未开跑。重派同一 occurrence（`occurrence.redispatched` 行，attempt+1，occurrenceId 不洗），本 tick 派发面即认领执行；attempt 帽（`kMaxAttempts=3`）到顶结算 `needs_review`/`redispatch_exhausted`；任务已取消结算 `cancelled`。
- **已绑定（开轮后）**沿 V1 裁决：V3 有 assistant 无 selection → 补 selection（同 selectionId，不调模型）；selection 在 → 补 outbox；无 assistant（模型请求可能已发）→ `needs_review`（未知副作用停住，不盲目重跑）；场对不上（stream 不存在/读不懂）→ `needs_review`。
- 多次 resume：settled 的 occurrence 永不重开；重派保 occurrenceId/attempts 递增；deliveryId 定式不变——resume 一次不多执行、不多送一份。
- heartbeat 任务恢复路同执行路：正文未变不投递（记观察账），补投只补有变化的。

### 13.5 heartbeat（观察与通知去重）

- job 带 `notifyOnChange`（CLI `--heartbeat`）：每次成功执行记 `occurrence.observed` 行（结果 sha256、是否变化、是否投递、是否更新已通知版本）；正文与上次已通知版本相同 → 不入 outbox，结算 `succeeded`/`unchanged_notification_suppressed`；不同 → 投递并在投递成后更新 `job.last_observed_sha`。
- 检查失败不能记成"无变化"：失败路永远投递失败通知（outbox 里 `notice-<occurrenceId>` 前缀的显式通知项，与 reply selection 分账），且不更新已通知版本。fresh/continuation：occurrenceId 定式与 session 策略无关（身份稳定）；`continuation` 需渠道路绑定（V3 线），本批创建面明拒——不假装支持。

### 13.6 /loop 显式导入

- 入口 `gateway job import-loop <来源会话id> "正文" --task loop-N --every 秒 [--idem 键]` → `control/job-import-loop-*.json` → 活 Gateway 消费 → `ImportLoop`：建 interval job（prompt 固定副本，不逐拍现读原 /loop 状态）+ `loop.imported` receipt 行（receiptId = `imp-` + hash(sessionId+taskId)，账上凭来源键反查）。
- 同来源（sessionId+taskId）幂等：再导回原 receipt，不建第二个 job——无 receipt 不暗搬、不双跑。结构上也不存在暗搬路：Gateway 不扫会话的 loop 状态，导入只走这条显式命令。原 /loop 状态只读留档（Gateway 不写它）；崩在"job 已建、receipt 未落"的极窄窗，凭 `--idem` 幂等键兜底（同键回原 job），裸重导可能出两个 job——如实记窗口，receipt 行落不稳属账 broken 一类。

### 13.7 新账行 type（纯追加，未知 type 读取侧跳过）

`job.updated`（fromRevision/toRevision/patch 键/cursorMs）、`job.paused`、`job.resumed`（cursorThroughMs）、`job.cancelled`、`job.schedule_advanced`（throughSlotMs/policy）、`occurrence.merged`（missedCount/throughSlotMs）、`occurrence.redispatched`（attempt/reason）、`occurrence.observed`（resultSha256/changed/delivered/updateLastObserved）、`occurrence.cancel_requested`、`loop.imported`。V2 已验/未验分账见单子 V2 勾选（放行门四条全过；真起子进程硬杀冒烟、断电一致性、真渠道投递仍属后续批次）。
