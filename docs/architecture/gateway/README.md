# 持久 Agent Gateway：总装合同

_本目录是《持久 Agent Gateway 自动任务与可靠恢复总装计划》的合同冻结文档（G0 批次落）。单子原文为权威；此处把散在单子里的核心合同提炼成实现批次可直接对照的唯一真源。_

---

[文档首页](../../README.md) · [架构首页](../README.md) · [冻结合同](contracts.md) · [现状审计与写盘图](audit-write-map.md) · [渠道合同](../channels/README.md)

## 1. 这层是干什么的

LubanCode 已经能做事、能落账、能恢复。缺的是一间常开的值房：进程退了有人拉起，到点有人敲门，来信有人接，崩了能从账上裁决从哪道栅栏接。Gateway 是显式的进程形态（`lubancode gateway run`），不是配置文件里的一个开关。

一条生产水路：

```text
OS Supervisor
  -> lubancode gateway run
  -> durable ingress / durable automation
  -> SessionWorkScheduler
  -> HeadlessSessionHost
  -> Agent + RunTurn
  -> Trajectory Journal
  -> durable outbox
  -> channel / webhook / local delivery
```

## 2. “持久”分五层（放行门）

| 层 | 承诺 | 首版放行门 |
|---|---|---|
| P1 状态持久 | 已接受的输入、job、运行事实、回复、投递状态可从盘重建 | 进程重启后 state hash 与崩前 durable prefix 一致 |
| P2 触发持久 | 进程活着时到点必醒；进程死后由 OS supervisor 拉起，再扫 due | 不依赖终端、UI 或用户按键 |
| P3 执行持久 | 能从安全栅栏继续；不能安全继续时明标 unknown/needs review | 不盲重跑已开始的未知副作用 |
| P4 投递持久 | 已提交回复进 outbox；发送失败可续，不重跑模型 | turn 后、send 前杀进程，重启只续 send |
| P5 运维持久 | 有 install/start/stop/status/doctor/logs，坏配置不重启风暴 | 三平台各有受支持的监督路径 |

少一层，便只可说“有落盘”或“有定时器”，不可说“持久 Agent”。

## 3. 五条定案

1. `Gateway` 是显式进程形态。普通 `lubancode` 不因读到配置便起线程、开端口、连平台或烧模型 token。
2. Trajectory 仍是一次 Agent 运行的 canonical fact ledger。Automation、Ingress、Outbox、Task 各守自己的领域账，不抄模型正文，不另造第二份会话真值。
3. 重启恢复的是已提交事实与可判状态，不是 C++ 调用栈。`running` 不等于可重跑；见过未知副作用，便停在 `NeedsReview`。
4. 外送失败只重试 delivery，不重跑 Agent。平台来信重复只走去重，不重复开 turn。
5. 先打通一条假渠道 + 一只自动任务 + 一场 headless turn。杀进程验过，再接 QQ、微信，再谈规模。

## 4. 依赖方向与线程规矩

```text
platform adapters
    -> channel protocol/runtime
        -> gateway work contracts
            -> headless session runtime
                -> agent

automation scheduler
    -> gateway work contracts

agent/runtime facts
    -> trajectory

trajectory reply projection
    -> outbox
        -> channel delivery
```

`agent`、`trajectory`、`workflow` 不反向 include Gateway、QQ、微信或 OS service installer。

线程规矩（§5.2 冻结）：

- Channel transport thread 只解帧、验签、落 ingress、投 wake。
- Timer thread 只算最近 deadline、投 wake。
- Outbox worker 只取已提交 delivery、调用 adapter、落 receipt。
- Agent turn 只由 Gateway 主调度面开；同一 session 单飞。
- Recorder 仍按 run 单写。任何 worker 不跨 writer 直写别人的 Journal。
- shutdown 先停止接活，再摘 wake，再收 turn，再关 outbox/adapter，最后释放 workspace/account lock。

## 5. 进程形态与命令

```cpp
enum class ProcessMode {
    Interactive,
    Pipe,
    AppServer,
    Gateway,
};
```

只有 `ProcessMode::Gateway` 能：起 AutomationScheduler、起 ChannelManager、开 Gateway control endpoint、持 workspace/channel account lock、接受 webhook、拉起 headless session、续 durable outbox。

```text
lubancode gateway run     [--profile <name>]   前台真进程
lubancode gateway install [--profile <name>]   只管 supervisor
lubancode gateway start   [--profile <name>]
lubancode gateway stop    [--profile <name>]
lubancode gateway restart [--profile <name>]
lubancode gateway status  [--json] [--deep]
lubancode gateway doctor  [--json]
lubancode gateway logs    [--follow]
```

`run` 是前台真进程。`install/start/stop/restart` 只管 supervisor。CLI 不另养一只暗 daemon。

Gateway 进程退出码（G1 实现裁决，冻结）：

| 码 | 含义 |
|---|---|
| 0 | 干净关机（boot history 落 clean shutdown 记录） |
| 1 | 未预期错误 |
| 2 | `gateway.already_running`（锁被活进程持有） |
| 3 | `gateway.config_invalid`（profile 配置坏，稳定退出防重启风暴） |
| 4 | `gateway.shutdown_timeout`（关机超限，账上明示，不假写 clean） |

`gateway status`：0 = 运行中；1 = 未运行/陈旧锁/锁读不懂（探测结论进 stdout 与 `--json`）。`gateway stop`：0 = 已停（含"本来就没在跑"与"非干净退出已如实入账"）；4 = 等待超时（不越权代杀）；1 = 锁读不懂不敢投命令/命令文件写不进。

## 6. 目录布局

```text
~/.lubancode/gateway/
  profiles/<profile>/
    gateway.json
    gateway.lock
    control.json
    boot-history.jsonl
    logs/
    automations/
      jobs/<job_id>/spec.json
      events/<shard>.jsonl
      occurrences/<occurrence_id>.json
    tasks/
      events/<shard>.jsonl
      snapshots/
    channels/
      <channel>/<account>/
        account.json
        account.lock
        ingress/
        outbox/
        dead-letter/
        adapter-state/
```

Agent 运行事实仍住 `~/.lubancode/workspaces/<workspace_key>/sessions/<session_id>/`。Gateway 不把 Trajectory 搬进自己的目录。

首版存储裁决：append-only event + 原子 snapshot/manifest，不先引 SQLite。spec 可原子换代；事实事件只追加，不可把 JSON 文件当共享可变 map 反复整份覆盖。

## 7. 批次地图与当前状态

| 批 | 内容 | 状态 |
|---|---|---|
| G0 | 现状冻结与总合同（本目录文档 + 渠道单对账） | 已落（2026-09-01） |
| G1 | 前台 Gateway 骨架与控制面（GatewayProcess/profile/lock/control/status/boot history/SafeMode 骨架） | 已落（2026-09-01：`src/gateway/` 四件 + `lubancode gateway run\|status\|stop` 子命令 + `tests/unit/gateway/` 16 例 + 真机冒烟） |
| G2 | AutomationStore、Scheduler 与系统服务 | 待实现 |
| G3 | Headless Session 与 Trajectory 收口（BuildRecoveryDecision） | 待实现 |
| G4 | DurableTaskStore 与 detached work | 待实现 |
| G5 | ChannelManager 宿主化与 durable ingress 接线 | 待实现 |
| G6 | ReplyAssembler、Outbox 与可靠投递 | 待实现 |
| G7 | 首只真渠道与生产运维 | 待实现 |
| G8 | 故障扫点、容量与发布 | 待实现 |
