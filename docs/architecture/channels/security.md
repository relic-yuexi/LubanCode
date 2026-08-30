# 渠道威胁模型与数据保留

_阶段 0 冻结件。列出资产、敌手、入口与防线；定死哪些数据放哪、活多久、怎么清。_

---

[渠道总览](README.md) · [channel.yaml 冻结](channel-manifest.md) · [Bridge 协议冻结](bridge-protocol.md) · [消息合同冻结](message-contracts.md) · [配置层级冻结](configuration.md)

## 1. 资产与敌手

| 资产 | 敌手 | 入口 |
| --- | --- | --- |
| 本机文件与 shell | 远端任意 sender（含"授权用户"发的注入正文） | 平台消息驱动 Agent |
| 账号凭据（token/secret/cookie） | 恶意 Package、日志读者、进程转储 | sidecar 环境、state 目录、日志与 trace |
| 全局密钥环境变量 | 恶意 Package 递归继承环境 | sidecar spawn |
| 用户隐私（会话史、记忆、群成员名） | 非 owner 远端 sender | 记忆召回、session history |
| 服务可用性 | 平台重放、消息洪水、bot 回声环 | ingress 队列 |
| 内网（SSRF 面） | 远端 URL/媒体引用 | 媒体下载、webhook 回调 |
| 公网监听面 | 伪造 webhook、重放、超大 body | Webhook listener |

核心断语：**外部消息就是不可信用户输入**。过 allowlist 只证明"谁发的"，不证明"他说的都安全"。消息可驱动 Agent，但仍受 Agent 工具表、Channel 工具上限、ModePolicy、路径与沙箱、审批边界、Prompt injection 防线约束。授权用户发来的正常任务本就该驱动 Agent；正确做法是保住 provenance 与宿主权限，不靠正文警语冒充安全边界。不把外部用户文字当 system/developer 指令。

## 2. 威胁与防线对照

| 威胁 | 防线 |
| --- | --- |
| 远端正文注入驱动高危工具 | 工具权限五层交集（见第 3 节）；confirm 工具 fail closed；高风险工具不默认开放 |
| 恶意 Package 偷凭据 | Package trust 门；最小环境变量继承；密钥按声明注入，不递全环境；stdout 只协议、stderr 大小帽与轮转 |
| bot 自回声 / 跨 bot 往返 | 默认拒 `sender.is_bot`；记本 bot message id 过滤回声；同内容跨 bot 设 hop/digest 窗；Channel 到 Channel 转发须显式工具与目标 allowlist |
| Webhook 伪造与重放 | raw body 验签（不 parse 后重序列化再验）；时间窗与 replay id；body 上限、content-type、读超时；path 规范化；每 path+IP 限速；loopback bind 默认，公开监听须显式配置；trusted proxies 配置才认反向代理 client IP |
| sidecar 越权 | 不走 shell 字符串只传 argv；state_dir 单账号隔离；crash/超时/坏帧杀整棵进程树；Package 未信任不执行。首版 sidecar 仍拿当前用户 OS 权限，文档不虚称强沙箱 |
| 媒体投毒 | 文件名去路径段；临时文件放账号受控目录；MIME 声明与魔数复核；解压包默认不自动展开；远端下载防 SSRF（含重定向、私网、大小、超时）；病毒扫描留可插拔 Hook，未配置不声称扫过 |
| 平台重放与洪水 | 持久去重键三级退让；每账号/每 conversation 队列上限；每 sender 速率窗；同正文短窗；下载媒体总字节帽；dead letter 上限与保留期；队列满按传输类型回 429/503 或停读，不默丢 |
| 项目配置越权 | 层级冻结：项目只能收窄（见[配置层级冻结](configuration.md)） |
| 假 owner 自报身份 | `is_owner` 只由宿主 allowlist 推导；pairing 批准只认宿主看到的 sender id |
| 凭据泄露进日志/会话 | token、context token、cookie、QR token 不进 session/trace/日志；`secret` 明文兼容但 doctor 报 warning；日志 sender 默认 hash |

## 3. 权限交集与 fail closed

最终有效工具：

```text
主 Agent 工具
∩ Channel 全局上限
∩ Account 上限
∩ Binding 上限
∩ Conversation 上限
- deny 并集
```

每一层只可收窄。解析时保留来源账。

Channel Session 没本地用户守着键盘。现有 confirm 档遇上工具审批，首版必须 fail closed：

- 工具已在全局账号政策与 binding allowlist 明确允许：可执行。
- 工具须确认但没有远端审批能力：拒绝，并回一条短说明。
- deny 工具：直接拒绝。
- project policy 只能继续收窄。

不为"机器人好用"便把 confirm 偷换成 auto。远端按钮/卡片审批留二期；无 interaction 能力的平台仍 fail closed。模型不可提供 sender id，不可生成 approval id，不可改 hash。

## 4. 关联键与可观测上限

每条日志至少带可用者：

```text
channel_id  account_id  adapter_generation  delivery_id  provider_event_id
conversation_id  sender_id_hash  session_id  turn_id  outbound_delivery_id
```

sender 默认 hash；debug 也不随意打印正文与 token。正文不进 trace，只记大小、part 类型、digest 与关联 id。

## 5. 数据保留表

数值型默认保留期（spool 清理、dead letter 保留天数等）待实现阶段定档并写回本表；本表冻结数据类别、归属与清理规矩。

| 数据 | 存放 | 寿命 | 清理与脱敏规矩 |
| --- | --- | --- | --- |
| 原始平台 webhook/事件 | sidecar durable spool | 短期；host ack 且投递完结后按 retention 清 | 只在账号受控 state 根内清；诊断 artifact 须脱敏、限大小 |
| 入站事件 journal | `~/.lubancode/channels/<ch>/<acct>/ingress/journal.jsonl` | append-only，随 archived 状态保留 | 快照可重建；dead letter 上限与保留期见队列账 |
| dead letter | `ingress/dead-letter.jsonl` | 有上限与保留期 | 保错误码与 delivery id，不保整份正文 |
| outbox 账 | `outbox/journal.jsonl` | committed delivery 完结后归档 | 记 outbound delivery id、client id、回执 |
| adapter state（cursor/resume） | `adapter/state.json` | 常驻至账号删除 | 不含密钥 |
| 账号非密配置 | `account.json` | 常驻 | 版本化 |
| 登录凭据 | `credentials.json.enc` | 常驻；logout 吊销 | `0600`/DACL 当前用户；能接系统密钥库时改引用 |
| pairing 记录 | 账号 state | code 短期有效，过期清 | 存 hash 不存明文；限速 |
| 账号锁 | `locks/<ch>-<acct>.lock` | 实例存活期 | 记 pid/start/generation，不记密钥 |
| sidecar 日志 | `logs/adapter.log` | 轮转 | stderr 大小帽 |
| session history | 现有 sessions JSONL | 随会话 | 只存 Agent 真正看过的规范化内容；带 provenance，不存 raw event |
| trace | 现有 trace 账 | 随会话 | 正文不进 trace；token 一律打码 |
| 媒体临时文件 | 账号受控目录 | retention 到期清 | 只碰已验证状态根；远端下载过 SSRF 检查 |
| Package 源码 | Package 目录 | 安装期 | 与账号数据分家；升级卸载不删状态 |

## 6. 泄露禁令

以下内容不得出现在日志、session、trace、错误文案、诊断输出：

- access token、context token、cookie、QR token。
- 密钥明文（doctor 对 `secret` 明文来源给 warning）。
- 模型思考链（默认不出站，见[消息合同](message-contracts.md)）。
- 工具入参里的密钥与绝对路径（脱敏后才可出站）。
- 未脱敏的 raw 平台事件。

`/export` 可带 channel provenance 摘要，不带凭据与 raw event。
