# 持久 Agent Gateway：总装合同

_本目录原是旧总装计划（G0 批次）的合同冻结文档。2026-09-13 起以Session V3 新版总装计划(见对应设计单)为唯一权威，本目录三篇已随 V0 批次逐项同步（旧存储路径、旧 Loop 恢复承诺、组件缺失判断等过时口径已删改；批号从 G0—G8 换为 V0—V6）。单子原文仍是权威，此处把核心合同提炼成实现批次可直接对照的唯一真源。_

---

[文档首页](../../README.md) · [架构首页](../README.md) · [冻结合同](contracts.md) · [现状审计与写盘图](audit-write-map.md) · [服务运维手册](runbook.md) · [渠道合同](../channels/README.md)

## 1. 这层是干什么的

LubanCode 已经能做事、能落账、能恢复。缺的是一间常开的值房：进程退了有人拉起，到点有人敲门，来信有人接，崩了能从账上裁决从哪道栅栏接。Gateway 是显式的进程形态（`lubancode gateway run`），不是配置文件里的一个开关。

入口边界：Gateway 接渠道消息与自动任务；AppServer 接自建应用的协议请求。两者复用会话执行能力，当前没有 Gateway → AppServer RPC 调用链。Channel Bridge 统一各平台适配器的消息合同，AppServer 协议统一应用客户端的请求与事件，两者不是同一套协议。

IM 入口仍在设计阶段：`lubancode im` 负责选平台、配账号，再用单账号启动计划调用共用 Gateway 启动服务；`gateway run` 保留所有已启用渠道与自动任务的行为。单账号范围、最近选择、无渠道行为和生命周期见 IM 设计修复单(见对应设计单)。这些待实现设计不代表当前命令已支持 IM 向导。

这里的单账号只限制沟通渠道，不关闭当前 profile 的自动任务。IM 是常驻助理的便捷入口；聊天交代任务、到点执行、结果送回、重启恢复才是完整目标。聊天侧任务创建和主动投递、周期调度、系统服务托管仍须按总计划逐项实现与验收。

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
lubancode gateway install [--profile <name>]   只管 supervisor(注册不 start)
lubancode gateway start   [--profile <name>]   经服务管理器拉起,不裸 spawn
lubancode gateway stop    [--profile <name>]   文件控制面 drain,不越权代杀
lubancode gateway restart [--profile <name>]   文件面 stop → 服务面 start
lubancode gateway status  [--json]             只读 probe + 领域三栏
lubancode gateway doctor  [--json] [--wait-ready <秒>] [--ack-safe-mode]
lubancode gateway logs    [--tail <行>]
```

`run` 是前台真进程。`install/start/stop/restart` 只管 supervisor。CLI 不另养一只暗 daemon。停止语义统一走文件控制面(投 stop 命令 → drain → 宽限;超时如实退 4 不代杀)——uninstall/restart 的收口同一条路。doctor 一项一码,退出码 0/1/2(全绿/有警/有病);`--wait-ready` 供 install 后验证与外部监控,超时如实退 1。三平台安装形态、运维手册与未验边界见 [runbook](runbook.md)。

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

新版（总装单 §5.3 提案；目录布局随 V1 起按批落定，下表是目标形状，不是"已存在"清单）：

```text
~/.lubancode/gateway/profiles/<profile>/
  gateway.json / gateway.lock / control/ / boot-history.jsonl
  automation/     # spec、occurrence 事实及可重建索引（V2 起）
  work/           # 受理/派发/接管引用、输入原件（V1 起）
  channels/       # 账号状态、ingress、adapter 状态引用（V3 起）
  delivery/       # 回复原件、发送事实、回执、dead letter（V1 起本地、V3 起渠道）

~/.lubancode/workspaces/<resolved-workspace-dir>/sessions/<sessionId>/
  <sessionId>.jsonl          # V3 主账（新场默认开启；只显式 0 回 v2）
  operations.jsonl           # 受理账（SessionService，三端共用）
  operations-inputs/         # 输入原件（durable input artifact）
  artifacts/
  subagents/...
```

会话位置一律走 workspace identity/index/resolver 解析实际目录，不按显示名手拼（见 §8-V0）。workspace-storage-v2 迁移前的旧 trajectories 平铺根口径废除。

状态根落位（应用根语义）：上表的 `~/.lubancode` 前缀即状态根——生产装配走 `gateway::DefaultGatewayRoot()`（`<状态根>/gateway`）与 `config::StateRootDir()`（workspaces 树），个人布局路径原样；设了 `LUBANCODE_HOME`/`LUBANCODE_DATA_HOME`（应用 Worker，合同 `docs/reference/capability-contract.md` §13.2）时整树随状态根落数据根（含 `gateway.json`——用户手写档与锁/账/日志同树，不拆双根）。`gateway status` 渠道栏的 channels 树同挂状态根下。

首版存储裁决不变：append-only event + 原子 snapshot/manifest，不先引 SQLite。spec 可原子换代；事实事件只追加，不可把 JSON 文件当共享可变 map 反复整份覆盖。关键受理、认领、执行授权、回复选择与发送回执要求 PowerLoss 级提交（§10-V0）。

## 7. 批次地图与当前状态

旧 G0—G8 已废，新版 V0—V6（见新版总装单 §十）。旧 G0/G1 的产出（本目录文档、GatewayProcess 骨架、CLI run/status/stop、测试册）折入新账：

| 批 | 内容 | 状态 |
|---|---|---|
| V0 | 合同与持久受理底线：文档翻新、work/occurrence/reply 身份与 schema 注册、SessionService 受理四病修复、锁的原子互斥与 ownerEpoch + 双进程竞争测试、PowerLoss 提交原语与原件保留规则 | 已落（2026-09-13，本目录三篇同步） |
| V1 | 最短纵向闭环：主泵、最小 AutomationStore（once/run-now）、共用 headless 装配、reply selection + 本地 DurableReplyOutbox | 已落（2026-09-13，PR #63；裁决见 contracts.md §12） |
| V2 | 周期调度与可靠接管（interval/cron、时区/DST、misfire、恢复裁决、heartbeat、/loop 导入） | 已落（2026-09-15，裁决见 contracts.md §13） |
| V3 | 渠道总装与真 transport（durable work 引用、sidecar、ACK 服从 ingress 耐久回执） | 待实现 |
| V4 | 服务安装与常驻运维（install/uninstall/start/restart/doctor/logs、三平台 supervisor 生成物、健康探针、关机未收净如实入账） | 已落（2026-09-15，裁决见 contracts.md §14；运维手册见 [runbook](runbook.md)；真实服务注册三平台真机未验，分平台列） |
| V5 | 长任务、Goal 与 Workflow（与异步工具单共用执行账） | 待实现 |
| V6 | 发布验收（回归、72h soak、容量扫点、安全、能力表） | 待实现 |
