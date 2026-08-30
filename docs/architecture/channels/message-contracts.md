# 消息合同冻结：ChannelInboundEvent、ReplyAction、MessageProvenance

_阶段 0 冻结件。入站事件、去重、状态机、来源账、出站动作在此定死；阶段 1 落 `src/channel/types.*` 时照此实现。_

---

[渠道总览](README.md) · [channel.yaml 冻结](channel-manifest.md) · [Bridge 协议冻结](bridge-protocol.md) · [配置层级冻结](configuration.md)

## 1. ChannelInboundEvent

统一入站事件。平台各家水管不同，进了 LubanCode 只剩这一份。

```cpp
struct ChannelInboundEvent {
    int schema = 1;
    std::string delivery_id;       // sidecar -> host 投递 id
    std::string provider_event_id; // 平台原始去重 id
    std::string channel_id;
    std::string account_id;
    std::int64_t received_at_ms = 0;
    std::int64_t provider_at_ms = 0;

    ChannelConversation conversation;
    ChannelSender sender;
    std::string message_id;
    std::optional<std::string> reply_to_message_id;
    std::vector<ChannelPart> parts;
    ChannelIngressHints hints;
};
```

conversation：

```cpp
enum class ConversationKind { Direct, Group, Guild, Channel, Thread };

struct ChannelConversation {
    ConversationKind kind;
    std::string id;
    std::optional<std::string> parent_id;
    std::optional<std::string> thread_id;
    std::string title;
};
```

sender：

```cpp
struct ChannelSender {
    std::string id;
    std::string display_name;
    bool is_bot = false;
    bool is_owner = false; // 只能由宿主 allowlist 推导，sidecar 声称不算
};
```

`is_owner` 由宿主 allowlist 推导，sidecar 声称不算——这份真值归宿主。

parts 首版九类：

```text
text  image  audio  video  file  link  mention  location  unsupported
```

`ChannelIngressHints` 承载入站辅助信号（mention 命中、命令前缀识别一类），字段集阶段 1 落 `src/channel/types.hpp` 时定档，本页不预支。

每个媒体 part 只带：

- 平台 media id。
- MIME、文件名、大小。
- sidecar 已下载的受控本地路径，或待下载引用。
- 可选 caption。
- SHA-256（已下载时）。

原始平台 JSON 默认不进 Agent history。诊断需要时，可写经脱敏、带大小帽的 sidecar debug artifact。

### 1.1 媒体怎样进 Agent

`api::Message` 的稳定模型输入以 Text/Image 为主。统一事件先收全类，不等于 Agent 已能原生理解。投影规矩：

| part | 投影 |
| --- | --- |
| text | `TextBlock`。 |
| image | 过下载、MIME、魔数、大小检查后转 `ImageBlock`/ArtifactRef。 |
| audio | 有受信 STT 能力才转录；否则只给文件名、MIME、大小与"未转录"说明。 |
| file | 交文档抽取工具或 Skill；未解析前不可把二进制冒充文本。 |
| video | 首版只存 artifact 与元数据；抽帧/转写另走受信工具。 |
| unsupported | 给模型一行稳定说明，不悄悄丢掉。 |

下载、转录、文档解析都要有独立工具权限和大小帽。ChannelAdapter 只搬运与规范化，不在 sidecar 里悄悄调用第三方模型。

## 2. MessageProvenance

现有 `api::Message` 只有 role 与 content。Channel 补宿主真账：

```cpp
enum class MessageOrigin {
    HumanTerminal,
    ExternalChannel,
    PeerSession,
    BackgroundCompletion,
    ToolResult,
    HostSynthetic,
};

struct MessageProvenance {
    MessageOrigin origin;
    std::string channel_id;
    std::string account_id;
    std::string sender_id;
    std::string conversation_id;
    std::string provider_message_id;
};
```

规矩：

- 由宿主生成，模型不可改。
- 落 session JSONL。
- 不原样塞进模型正文。
- provider request serializer 默认忽略这份宿主元数据，只由 Prompt/Tool 隐式上下文按需取安全子集。
- 供权限、审计、工具隐式上下文与回复关联使用。
- 老 session 没字段时回落 `HumanTerminal`/`HostSynthetic` 的兼容推断。

首期若不直接扩 `api::Message`，可先把 provenance 放 `TurnIngress` 与 session 事件行；但最终必须收进统一持久合同，不能永远靠 `[来自 QQ]` 这类文字标签猜身份。

## 3. 交付语义与去重键

首版定为：

```text
adapter -> host：at least once
host ingress -> Agent turn：at least once + 持久去重
Agent reply -> platform：幂等尽力；平台支持 client id 时用 client id
```

不许写"exactly once"。崩溃夹在模型副作用与状态落盘之间，仍可能重放。能做的是把窗口缩小、留账、让重复可识别。

去重键三级，从上往下退：

```text
1. channel_id + account_id + provider_event_id
2. channel_id + account_id + conversation_id + message_id
3. sender_id + normalized parts digest + provider timestamp bucket
```

第三级指纹只作短窗去重，不可冒充永久 id。Webhook 的 200 必须在 sidecar durable spool 落盘之后才回；落盘失败回 5xx，让平台重试。

## 4. 入站状态机

每次迁移只追加 journal。快照可重建，不拿一份可覆盖 JSON 当唯一真账。

```text
received
-> durable
-> authorized | rejected
-> routed
-> queued
-> running
-> replied | completed_without_reply
-> delivered | delivery_failed
-> archived
```

旁路终态：

```text
duplicate
rate_limited
unsupported
dead_letter
cancelled
```

WebSocket resume sequence 与长轮询 cursor 只在事件进 sidecar durable spool 后推进；host ack 后 sidecar 才可清理已上报 spool；未 ack 事件重连后再送，host 以 provider event id 去重。

## 5. ReplyAction

ReplyAssembler 的输出。它只吃 `ServerEvent`（`TurnStarted`、Text `ItemStarted/ItemDelta/ItemCompleted`、Tool `ItemStarted/ItemCompleted`、`UsageUpdated`、`TurnCompleted`），不另接 provider SSE，不读 Agent 内部 buffer。终端、App Server、Channel 看的是同一轮真账。

```cpp
enum class ReplyActionKind { Send, Edit, Typing, React, Upload };

enum class ReplyDurability { Preview, Committed };

struct ReplyAction {
    ReplyActionKind kind;
    ReplyDurability durability = ReplyDurability::Committed;
    std::string text;                 // Send/Edit 的正文；edit 发累计全文
    std::vector<MediaAttachment> media;
    std::string reply_to_message_id;  // 回复关联
    std::string outbound_delivery_id; // Committed 动作的账
    std::string client_id;            // 平台幂等 id
};
```

每轮生成三枚稳定关联 id：

```text
reply_route_id
outbound_delivery_id
provider_reply_to_message_id
```

同一 inbound 的多块回复都带同一 parent 账。平台支持 reply/quote 时，默认只首块引用原消息；可配置 all/off。

### 5.1 ServerEvent 默认投影

| 事件 | 默认出站 |
| --- | --- |
| thinking delta | 丢弃 |
| text delta | 进 coalescer |
| tool started | 不发；可选发脱敏进度 |
| tool completed | 不发；可选更新进度 |
| usage | 不发 |
| turn success | flush 正文 |
| turn failed | flush 已有正文，再发稳定错误提示 |
| turn cancelled | flush 已有正文，再发"已停止" |

模型思考、工具入参、密钥、绝对路径、stderr 不得直接进聊天平台。

### 5.2 三种回复模式

```text
final   等 TurnCompleted，一次或按上限分块发
block   按 min_chars / idle_ms / max_chars 合并后分块发
native  用平台原生 streaming/edit/card update
```

默认：平台有稳定 native streaming 用 `native`；只有普通发送用 `block`；LINE 这类 reply token 有时限且编辑弱的，按平台策略选 `final` 或受控 block。

coalescing 配置：

```json
{
  "reply": {
    "mode": "block",
    "min_chars": 200,
    "max_chars": 1800,
    "idle_ms": 1200,
    "max_updates_per_second": 1,
    "typing": true,
    "tool_progress": false
  }
}
```

规矩：

- `max_chars` 不得超过 manifest 与运行时能力交集。
- 优先在段落、换行、句号处切；实在不成才硬切 Unicode code point。
- 不劈 UTF-8，不劈 Markdown code fence；劈开时补 fence，下一块重开。
- native edit 发送累计全文，不发 delta，除非平台明确要 delta。
- 更新节流与最终 flush 分开；最终 flush 不因节流漏掉。
- 空正文不发空消息。

### 5.3 preview 与 committed 分账

| 动作 | 耐久要求 |
| --- | --- |
| typing on/off | best effort，不进 outbox |
| 工具进度提示 | best effort，可丢 |
| native stream 中间更新 | preview，只记采样 trace |
| block 模式已发出的正文块 | committed，进 outbox/receipt 账 |
| final 模式最终正文 | committed |
| native stream 最终 complete/update | committed |
| 文件、图片等媒体 | committed |

preview 失败只降级，不重跑 Agent。committed delivery 失败按 delivery id 重试。中间 native stream 崩了，若 turn 最终正文已经落 session，重启后可发一枚 final committed 更新；若 turn 自己没完成，只留失败账，不凭半截 delta 编造最终答案。
