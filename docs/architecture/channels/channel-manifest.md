# channel.yaml schema 1 冻结

_阶段 0 冻结件。字段、类型、取值、校验规矩在此定死；阶段 1 的 parser 照此实现，实现不得放宽。_

---

[渠道总览](README.md) · [Bridge 协议冻结](bridge-protocol.md) · [消息合同冻结](message-contracts.md) · [配置层级冻结](configuration.md)

## 1. 地位

`channel.yaml` 是 ChannelPlugin 的 manifest，与工具 Plugin 的 `plugin.json` 并排两类组件，互不扩写。不给 `plugin.json` 硬添 `lifecycle: daemon`、`inbound`、`accounts` 等字段——一份 manifest 不背两套生命周期。

```text
plugins/foo/plugin.json       一次工具调用
channels/qqbot/channel.yaml   常驻消息渠道
```

schema 只收 `1`。改字段须升 schema 号并写迁移，不许悄悄改语义。

## 2. 完整示例

```yaml
schema: 1
id: qqbot
name: QQ Bot
description: Connect QQ Bot accounts through the official QQ Bot API.

runtime:
  kind: process
  command: node
  args:
    - ${channel_dir}/runtime/dist/index.js
  protocol: lubancode-channel/1
  startup_timeout_ms: 15000
  shutdown_timeout_ms: 5000
  requires:
    executables:
      - name: node
        version: ">=20"

capabilities:
  transports: [websocket, webhook]
  conversations: [direct, group, guild, thread]
  inbound: [text, image, audio, video, file, mention, reply]
  outbound: [text, image, audio, video, file, reply]
  delivery: [send, edit, native_stream, typing]
  login: [credentials, qr]

limits:
  text_chars: 2000
  media_bytes: 20971520
  outbound_requests_per_minute: 60

state:
  format: 1
  migrator: ${channel_dir}/runtime/dist/migrate.js
```

## 3. 字段规矩

### 3.1 顶层

| 字段 | 类型 | 规矩 |
| --- | --- | --- |
| `schema` | int | 只收 `1`。其余报错。 |
| `id` | string | 全局渠道 id，小写 kebab-case。同一时刻只许一份已挂载实现占用同一 id。 |
| `name` | string | 展示名。 |
| `description` | string | 一句话说明。 |

### 3.2 `runtime`

| 字段 | 类型 | 规矩 |
| --- | --- | --- |
| `kind` | string | 首版只收 `process`。native-library Channel 首版不做。 |
| `command` | string | 可为 Package 内冻结 executable，或 `requires.executables` 明列的裸命令。不走 shell。 |
| `args` | string[] | 只许 `${channel_dir}` 一个占位符。 |
| `protocol` | string | 首版只收 `lubancode-channel/1`。 |
| `startup_timeout_ms` | int | spawn 到 initialize 完成的上限，缺省 15000。 |
| `shutdown_timeout_ms` | int | stop 到杀进程树的宽限，缺省 5000。 |
| `requires.executables[]` | list | 每项 `name` + 可选 `version`。 |

占位符与命令的信任规矩：

- `command`、`args` 只许 `${channel_dir}` 占位，不走 shell 字符串，只传 argv。
- 裸命令在信任时解析绝对路径并记 fingerprint；后来 PATH 指到另一份 executable，须重新批准。
- Package 内 command canonical 后必须留在 Package 根内。

### 3.3 `capabilities`

声明，不是授权。运行时握手所得能力只能等于或窄于声明（见 [Bridge 协议](bridge-protocol.md) 握手）。各数组取值冻结如下：

| 字段 | 取值集 |
| --- | --- |
| `transports` | `websocket` `webhook` `long_polling` `sdk_events` |
| `conversations` | `direct` `group` `guild` `channel` `thread` |
| `inbound` | `text` `image` `audio` `video` `file` `link` `mention` `reply` `location` `unsupported` |
| `outbound` | `text` `image` `audio` `video` `file` `reply` |
| `delivery` | `send` `edit` `native_stream` `typing` `react` |
| `login` | `credentials` `qr` `oauth` `device_code` |

声明某项 capability，conformance suite 便须加那项测试；没能力就不得硬报支持。

### 3.4 `limits`

宿主上限。sidecar 握手报得更小就取更小，报得更大不放宽。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `text_chars` | int | 单块文本上限。 |
| `media_bytes` | int | 单个媒体上限。 |
| `outbound_requests_per_minute` | int | 出站请求速率帽。 |

### 3.5 `state`

| 字段 | 类型 | 规矩 |
| --- | --- | --- |
| `format` | int | 账号 adapter state 版本，从 `1` 起。 |
| `migrator` | string | 可选。升级迁移器，支持 `${channel_dir}`。 |

### 3.6 manifest 不收什么

- 不放账号 token。
- 不放用户 allowlist。
- 不放 webhook 公网 URL。
- 非官方个人账号自动化（如 Zalo Personal）必须声明 `risk: unofficial_personal_account`，首次启用须本地交互确认；该字段是唯一的附加风险标注，其余未知字段一律报错。

## 4. 校验与报错

- 未知字段报错。
- 类型错、路径越界、坏占位符报错。
- 错误信息带类型、行、列、来源（builtin/user/project/package 哪一份文件）。

## 5. 发现与挂载

Package inventory 识别第七类组件 `Channel`：

```text
channels/<local-id>/channel.yaml
```

canonical id：

```text
<package-id>:<channel-local-id>
```

对外渠道 id 由 `channel.yaml:id` 给出（如 `qqbot`）。同一时刻只许一份已挂载实现占用同一渠道 id；冲突按 Package 原子挂载失败，不临时挑一份。

发现不等于运行。Channel 含网络、账号、密钥与常驻进程，发现后须依次过十关，任何一关失败都不留半只 adapter 或半份路由：

```text
discover
-> static parse
-> dependency check
-> package trust
-> channel enablement
-> account credentials resolve
-> account lock
-> sidecar spawn
-> initialize
-> start
-> running
```

## 6. 会话钉快照

一枚 Channel turn 开始时钉住：

- Package digest。
- Channel manifest digest。
- adapter protocol version。
- account config revision。
- policy revision。
- Agent definition snapshot。
- Prompt/Skill/Tool snapshot。

reload 只影响后来的 turn。正在跑的一轮不得中途换 adapter、政策或 Agent。
