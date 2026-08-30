# Channel Bridge v1 冻结

_阶段 0 冻结件。协议名、framing、method、错误码在此定死；阶段 1 的帧编解码与 router 照此实现。_

---

[渠道总览](README.md) · [channel.yaml 冻结](channel-manifest.md) · [消息合同冻结](message-contracts.md) · [配置层级冻结](configuration.md)

## 1. 协议常量

| 常量 | 冻结值 |
| --- | --- |
| 协议名 | `lubancode-channel/1` |
| 握手 `protocol_version` | `2026-08-29` |
| 帧 | 4 字节大端长度 + UTF-8 JSON 正文 |
| 单帧上限 | 8 MiB（媒体只传 ArtifactRef/文件路径/URL，不塞大段 base64） |
| 传输 | sidecar 的 stdin/stdout；stdout 只走协议帧，日志一律 stderr |

协议要办五件事：宿主启停 sidecar；sidecar 主动上报入站；宿主下发发送动作；双方交换状态与登录挑战；断线后按 delivery id、cursor、resume token 续上。这不是 MCP tools/call，不对外冒充 MCP。

## 2. framing 规矩

- JSON 正文须是 object。
- 字符串过 UTF-8 校验。
- request/response/notification 采用 JSON-RPC 2.0 形状。
- 两边都能发 request，也都能回 response。
- request id 在各自方向独立递增；响应只配本方向 pending。
- 陌生 method 回 `-32601`，不可静默吞。
- 协议错、超帽、连续坏帧：立即停 adapter，进入 backoff。
- 迟到响应、重复响应、陌生响应 id：丢弃并留诊断账，不崩宿主。

## 3. 握手

宿主先发：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "channel.initialize",
  "params": {
    "protocol_version": "2026-08-29",
    "channel_id": "qqbot",
    "account_id": "main",
    "state_dir": "...",
    "locale": "zh-CN",
    "host": {
      "name": "lubancode",
      "version": "0.x"
    },
    "requested_capabilities": {
      "inbound": ["text", "image", "audio", "file"],
      "delivery": ["send", "edit", "typing"]
    }
  }
}
```

sidecar 回：

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocol_version": "2026-08-29",
    "adapter": {"name": "openclaw-qqbot-bridge", "version": "1.0.0"},
    "capabilities": {
      "transports": ["websocket", "webhook"],
      "delivery": ["send", "native_stream", "typing"]
    },
    "account_state_version": 1
  }
}
```

规矩：

- 版本不认得就明败，报 `protocol_incompatible`，不重试。
- 有效能力取 manifest 声明、宿主请求、sidecar 回报三者交集。
- sidecar 回报的能力只能等于或窄于 manifest 声明，报得更宽视为协议错。

## 4. 宿主发给 sidecar 的 method

### channel.start（request）

登录检查后开始收消息。

```json
{ "method": "channel.start",
  "params": { "transport": "websocket" } }
```

result：`{ "started": true, "transport": "websocket" }`。

### channel.stop（request）

停收、flush spool/cursor、关连接。

```json
{ "method": "channel.stop", "params": {} }
```

result：`{ "stopped": true, "flushed": true }`。

### channel.health（request）

取连接、游标、积压、最近错误。

```json
{ "method": "channel.health", "params": {} }
```

result：`{ "state": "running", "connected": true, "cursor": "...", "backlog": 0, "last_error": null }`。

### channel.send（request）

发新消息或媒体。`client_id` 供平台幂等。

```json
{ "method": "channel.send",
  "params": {
    "conversation": {"kind": "group", "id": "group_openid"},
    "parts": [
      {"type": "text", "text": "..."},
      {"type": "file", "mime_type": "image/png", "local_path": "...", "size": 1024}
    ],
    "reply_to_message_id": "om_123",
    "client_id": "out-42"
  } }
```

result：`{ "provider_message_id": "om_456", "accepted": true }`。

### channel.edit（request）

改已发消息/卡片。正文是累计全文，不是 delta，除非平台明确要 delta。

```json
{ "method": "channel.edit",
  "params": {
    "provider_message_id": "om_456",
    "text": "累计全文",
    "client_id": "out-42"
  } }
```

result：`{ "edited": true }`。

### channel.typing（notification）

开关输入状态，丢了也无妨，不进 outbox。

```json
{ "method": "channel.typing",
  "params": { "conversation": {"kind": "direct", "id": "openid"}, "on": true } }
```

无 result。

### channel.react（request）

平台支持时加 reaction。

```json
{ "method": "channel.react",
  "params": { "provider_message_id": "om_123", "emoji": "ok" } }
```

result：`{ "applied": true }`。

### channel.login.begin（request）

开 QR/OAuth/设备码登录。挑战随后走 `channel.login.challenge` 通知。

```json
{ "method": "channel.login.begin", "params": { "mode": "qr" } }
```

result：`{ "begun": true }`。

### channel.login.cancel（request）

```json
{ "method": "channel.login.cancel", "params": {} }
```

result：`{ "cancelled": true }`。

### channel.logout（request）

吊销本地会话并停账号。

```json
{ "method": "channel.logout", "params": {} }
```

result：`{ "logged_out": true }`。

### channel.inbound.ack（request）

宿主耐久接收后确认 delivery id；sidecar 收到 ack 才可清理对应 spool。

```json
{ "method": "channel.inbound.ack", "params": { "delivery_id": "in-7" } }
```

result：`{ "acked": true }`。

### channel.inbound.nack（request）

明拒并给 retry/dead-letter 指示。

```json
{ "method": "channel.inbound.nack",
  "params": { "delivery_id": "in-7", "reason": "rate_limited", "retry": true, "dead_letter": false } }
```

result：`{ "nacked": true }`。

## 5. sidecar 发给宿主的 method

### channel.inbound（notification）

上报一枚已在 sidecar 暂存的入站事件。params 即 [ChannelInboundEvent](message-contracts.md) 的 JSON 投影，`delivery_id` 必带。用 notification 随后由宿主以 `delivery_id` 发 ack/nack，sidecar 在 ack 前保留本地 spool 并可按退避重发——这样双向 request 的长处理不会堵住读循环。

### channel.status（notification）

```json
{ "method": "channel.status",
  "params": { "state": "backoff", "reason": "transport_failed", "retry_at_ms": 1724700000000, "generation": 3 } }
```

`state` 取值：`connecting` `running` `degraded` `backoff` `stopped`。

### channel.delivery.receipt（notification）

平台异步送达、失败、撤回回执。

```json
{ "method": "channel.delivery.receipt",
  "params": { "outbound_delivery_id": "out-42", "provider_message_id": "om_456", "outcome": "delivered" } }
```

`outcome` 取值：`delivered` `failed` `recalled`。`failed` 时带 `reason` 与可选 `retry_after_ms`。

### channel.login.challenge（notification）

```json
{ "method": "channel.login.challenge",
  "params": { "kind": "qr", "payload": "...", "expires_at_ms": 1724700000000 } }
```

`kind` 取值：`qr` `url` `device_code`。

### channel.login.completed（notification）

```json
{ "method": "channel.login.completed",
  "params": { "account_summary": {"display_name": "...", "platform_user_id": "..."} } }
```

### channel.capabilities.changed（notification）

平台降级或权限改变。回报只可窄于握手所得，报得更宽视为协议错。

```json
{ "method": "channel.capabilities.changed",
  "params": { "capabilities": {"delivery": ["send", "typing"]} } }
```

### channel.fatal（notification）

不可自动恢复的账号错误。

```json
{ "method": "channel.fatal",
  "params": { "reason": "account_revoked", "detail": "..." } }
```

## 6. 错误码冻结

协议层错误走 JSON-RPC 2.0 标准整数码：

| code | 含义 | 规矩 |
| --- | --- | --- |
| `-32700` | Parse error | 坏 JSON。 |
| `-32600` | Invalid Request | 正文不是 object、缺 `jsonrpc`/`method`。 |
| `-32601` | Method not found | 陌生 method 必回此码，不静默吞。 |
| `-32602` | Invalid params | 参数缺字段、类型错。 |
| `-32603` | Internal error | 其余内部错。 |

domain 错误统一挂在 `-32000` 到 `-32099` 段，`message` 放稳定名，`data.detail` 放脱敏细节。稳定名冻结如下：

| 稳定名 | 场景 | 处置 |
| --- | --- | --- |
| `protocol_incompatible` | 握手版本不认得 | 明败，不重试 |
| `frame_too_large` | 单帧超 8 MiB | 立即停 adapter，进 backoff |
| `invalid_utf8` | 帧内坏 UTF-8 | 同上；连续坏帧同罪 |
| `invalid_frame` | JSON 解不开或非 object | 同上 |
| `unknown_delivery_id` | ack/nack 对不上已上报 delivery | 丢弃并留诊断账 |
| `not_capable` | 平台无 edit/react/native_stream 等能力 | 调用方回落，不重试 |
| `rate_limited` | 平台 429 | 尊重 Retry-After，delivery 不重跑 Agent |
| `permanent_reject` | 平台永久 4xx | dead letter，保错误码 |
| `transport_failed` | 发送层网络/5xx 错 | 退避重试 |
| `spool_write_failed` | sidecar spool 落盘失败 | Webhook 回 5xx；WS/poll 不推进 cursor |
| `login_required` | 凭据缺失或失效 | 账号转 `NeedsLogin`，停重试风暴 |
| `account_revoked` | 账号被平台吊销 | 不自动重试 |
| `spawn_failed` | sidecar 起不来 | 账号 `Fatal`，其余账号照跑 |
| `process_crashed` | sidecar 中途退出 | 重启计数，超限转 `Fatal` |
| `shutdown_timeout` | 停机宽限用尽 | 杀整棵进程树，释放账号锁 |

状态类 reason（`channel.status` 与账号状态机共用）另见 [配置层级冻结](configuration.md) 的账号状态机一节。

## 7. 关闭顺序

```text
ChannelManager 标 stopping
-> 停收新 turn
-> 等正在发送的请求收口（有上限）
-> channel.stop
-> sidecar flush spool/cursor
-> 关 stdin
-> 等进程退出
-> 超时杀进程树
-> 释放账号锁
```

析构、reload、Ctrl+C、服务停止都走同一条幂等路径。
