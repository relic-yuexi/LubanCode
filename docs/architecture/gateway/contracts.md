# Gateway 冻结合同（G0）

_单子原文为权威；本页把单子 §六-§十六的核心合同收拢冻结，实现批次照此为唯一真源。改动须回单子改，再同步本页。_

---

[文档首页](../../README.md) · [Gateway 首页](README.md) · [现状审计](audit-write-map.md)

## 1. ProcessMode 与激活闸

```cpp
enum class ProcessMode { Interactive, Pipe, AppServer, Gateway };
```

只有 `ProcessMode::Gateway` 能：起 AutomationScheduler；起 ChannelManager；开 Gateway control endpoint；持 workspace/channel account lock；接受 webhook；拉起 headless session；续 durable outbox。

## 2. 六本账与唯一所有者

| 账 | 唯一所有者 | 存什么 | 不存什么 |
|---|---|---|---|
| Gateway Registry | `GatewayRegistryStore` | instance、workspace/agent binding、启停与配置 generation | 会话正文、密钥明文 |
| Automation | `AutomationStore` | job spec、schedule、occurrence、claim、next fire、run ref | 模型输出正文 |
| Ingress | `ChannelIngressStore`（已落库） | provider event、去重键、路由状态、turn ref、dead letter | 整份 session history |
| Execution | `TrajectoryRecorder`（已落库） | input/model/tool/control/terminal canonical facts | scheduler 的 next fire、平台 transport retry |
| Task | `DurableTaskStore` | detached task spec、parent、state、mailbox refs、run refs | backend 指针、thread handle、重复的 child 正文 |
| Delivery | `OutboxStore` | reply artifact ref、target、attempt、receipt、dead letter | 重新拼一份 Agent history |

## 3. 跨账只传引用

```json
{
  "workspace_key": "...",
  "session_id": "...",
  "run_id": "...",
  "event_id": "...",
  "event_hash": "..."
}
```

Automation occurrence 只记 `run_ref`。Ingress 只记 `turn_ref`。Outbox 只记 committed response artifact 与 event ref。谁也不抄 canonical payload。

提交次序：

```text
入站：validate/auth -> ingress.received durable -> dedupe decision durable
      -> platform ACK -> queue claim durable -> open Agent turn

回复：model output committed -> reply artifact committed
      -> delivery.enqueued durable -> turn terminal
      -> adapter send -> delivery receipt durable
```

`delivery_id` 首版定式：

```text
hash(channel_account_id, conversation_id, turn_id, reply_ordinal, committed_event_hash)
```

崩在 reply artifact 与 outbox enqueue 之间时，RecoveryProjector 按稳定 `delivery_id` 从 committed response 补出同一条 outbox item——只作确定性投影，不重跑 Agent。

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

## 10. G1 实现裁决（本页新增，回单子报备）

单子未定死、G1 落地时定的三件：

1. **Control endpoint 首版形态**：本地文件控制通道（profile 目录内 `control.json` 状态快照 + `control/` 命令文件），零 socket、零端口。local-only 与“要求本机身份”由文件系统权限（user-only 目录）承担。G2 起 `status --deep` 需要活探针时再升真 endpoint，届时本条修订。
2. **锁裁决**：`gateway.lock` 记 pid + 进程 start token（复用 `trajectory::CurrentProcessStartToken`/`ProbeLockHolder` 的身份核法）+ boot_id（实例令牌，防同进程双 Gateway 互不相认）。陈旧锁 = 持有者死透或 PID 复用（token 对不上）；读不懂的锁保守拒绝不删。
3. **SafeMode 判定**：boot history 里连续非干净关机（无 clean shutdown 记录）次数 ≥ 3 即进 SafeMode：控制面照起、锁照取、业务面（G1 尚无）暂停、status/日志明示。干净关机即破连击，退出 SafeMode 无需额外仪式；显式 ack 口留给 G2 的 doctor。
