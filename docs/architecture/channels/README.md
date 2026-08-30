# 渠道接入架构：ChannelPlugin 与 Channel Runtime

_本目录是多渠道消息接入的合同冻结文档（设计单阶段 0）。此处只定合同，不写平台代码；实现批次照此为唯一真源。_

---

[文档首页](../../README.md) · [架构首页](../README.md) · [channel.yaml 冻结](channel-manifest.md) · [Bridge 协议冻结](bridge-protocol.md) · [消息合同冻结](message-contracts.md) · [配置层级冻结](configuration.md) · [威胁模型与数据保留](security.md)

## 1. 这层是干什么的

LubanCode 要接 QQ、微信、飞书这类聊天平台，缺的不是一只新工具，也不是一条 SSE，是一层常驻的 Channel Runtime。平台来信唤醒 Agent，Agent 回话交还平台，ChannelPlugin 管这条来回路。

一条消息走三条水路，各段各有主人：

```text
入站水路（platform -> Agent）
  聊天平台
    -> ChannelAdapter（sidecar 进程，管平台 SDK/鉴权/传输）
    -> Channel Bridge v1（channel.inbound 通知）
    -> ChannelIngressStore（耐久落账、去重）
    -> ChannelRouter（鉴权、绑定、定 Agent 与 session）
    -> SessionWorkScheduler（排队，同 conversation 单飞）
    -> ChannelSessionHost（无终端开一轮 turn）

turn 水路（Agent 内部，与终端同一条）
  ChannelSessionHost
    -> SessionRuntime + Agent + RunTurn
    -> TurnEventAdapter
    -> ServerEvent（中立事件，唯一出账）

出站水路（Agent -> platform）
  ServerEvent
    -> ReplyAssembler（合并、分块、脱敏投影）
    -> DeliveryDriver（send/edit/typing，重试与回执）
    -> Channel Bridge v1
    -> ChannelAdapter
    -> 聊天平台
```

Reviewer 只看这三条水路，就该说得出：平台 SDK 全在 sidecar，C++ 内核只认统一事件；Agent 一字不改，换平台只换 adapter；模型流与平台流分账，SSE 只是模型侧一条水管，不进 Channel 合同。

## 2. 四界分账

ChannelPlugin 不冒充现有任何一件。边界如下，谁也不许跨界：

| 名称 | 管什么 | 不管什么 | 生命周期 |
| --- | --- | --- | --- |
| 工具 Plugin（`plugin.json`） | 一次工具调用，起进程、算一遍、回一份 JSON、退出 | 常驻连接、主动上报、多账号 | 一次调用一只进程 |
| MCP | Agent 主动调用外部工具的协议 | 接收平台来信、聊天账号、消息回执 | 会话期客户端 |
| ChannelPlugin（`channel.yaml`） | 常驻消息适配器：平台传输、账号、入站规范化、出站投递 | 工具调用语义、Agent 配置 | 随账号常驻 |
| Package | Channel 组件的发现、校验、信任与挂载 | 收消息、开 Agent turn | 安装期 |
| App Server | 外壳连内核的控制面协议（终端/Web/桌面看会话） | 外部消息面 | 前端连接期 |
| Peer（`src/peers/`） | 本机 LubanCode 进程间会话字条 | 公网平台账号、平台协议 | 进程期 |

App Server 与 Channel 是两道不同的缝，可共用 `SessionRuntime` 与 `EventSink`，谁也不包谁：

```text
Desktop/Web -> App Server ----+
                              +-> Session Host -> Agent
QQ/WeChat   -> Channel Runtime-+
```

现有 process plugin 接不住渠道，不是参数问题，是生命周期压根儿不对：stdin 会被关，协议只收一份 stdout，WebSocket、35 秒长轮询、Webhook Server、心跳重连都无处安放。MCP 也接不住：`src/mcp/client.cpp` 只接带整数 `id` 的响应，且其业务语义是工具与资源，不是聊天账号与会话准入。不为省一只协议壳把两套领域硬拧在一起。

## 3. 组件名词表

| 名称 | 管什么 | 不管什么 |
| --- | --- | --- |
| Channel | 一类平台，如 `qqbot`、`weixin` | 不代表某个账号 |
| Channel Account | 一份登录身份与凭据 | 不决定 Agent 全局配置 |
| ChannelPlugin | 一类可安装渠道实现 | 不直接持宿主 Agent |
| ChannelAdapter | ChannelPlugin 某个账号的运行实例 | 不拼系统提示词 |
| ChannelManager | 启停 adapter、收状态、管账号锁与退避 | 不执行模型步骤 |
| ChannelBridge | C++ 宿主与进程 sidecar 的双向协议 | 不对外冒充 MCP |
| ChannelIngressStore | 入站去重、耐久状态、dead letter | 不保存整份 session history |
| ChannelRouter | conversation -> session/agent/policy | 不碰平台 SDK |
| ChannelSessionHost | 无终端运行一场 session | 不处理平台协议 |
| ReplyAssembler | ServerEvent -> 渠道出站动作 | 不泄露 thinking |
| DeliveryDriver | 调 adapter 发送、编辑、上传、重试 | 不重新跑 Agent |

产品界面可统称"渠道插件"。代码里仍把 `PluginTool` 与 `ChannelPlugin` 分开，免得生命周期、权限和错误码串账。

## 4. 总架构与依赖方向

```text
                          +----------------------+
                          | PackageCatalog       |
                          | trust / mount / pin  |
                          +----------+-----------+
                                     |
                                     v
+----------------+       +-----------+-----------+
| QQ / WeChat /  |<----->| Channel sidecar       |
| LINE / Feishu  |       | SDK + auth + transport|
+----------------+       +-----------+-----------+
                                     | Channel Bridge v1
                                     v
                          +----------+-----------+
                          | ChannelManager       |
                          | lifecycle / accounts |
                          +----------+-----------+
                                     |
                                     v
                          +----------+-----------+
                          | ChannelIngressStore  |
                          | durable / dedupe     |
                          +----------+-----------+
                                     |
                                     v
                          +----------+-----------+
                          | ChannelRouter        |
                          | auth / bind / policy |
                          +----------+-----------+
                                     |
                                     v
                          +----------+-----------+
                          | SessionWorkScheduler |
                          | single-flight / FIFO |
                          +----------+-----------+
                                     |
                                     v
                          +----------+-----------+
                          | ChannelSessionHost   |
                          | SessionRuntime+Agent |
                          +----------+-----------+
                                     |
                          TurnEventAdapter / EventSink
                                     |
                                     v
                          +----------+-----------+
                          | ReplyAssembler       |
                          | coalesce / chunk     |
                          +----------+-----------+
                                     |
                                     v
                          +----------+-----------+
                          | DeliveryDriver       |
                          +----------------------+
```

依赖只朝一个方向走：

```text
platform sidecar -> channel protocol -> channel runtime -> session runtime -> agent
agent events -> reply assembler -> channel protocol -> platform sidecar
```

`agent`、`sessions`、`runtime` 不反向 include 某一家平台 SDK。落进仓里，渠道宿主代码住 `src/channel/`，平台实现不进 `src/channel/platforms/`，全住 Package（见 [channel.yaml 冻结](channel-manifest.md)）。

## 5. 启动闸与激活决策（速览）

Channel 是全局网络能力，默认整层不运行。普通 `lubancode` 启动时，即使配置里已有 `enabled: true`，也只许读配置、列状态，不得拉 sidecar、开 WebSocket、做长轮询、监听端口。

启动一个账号须同时过四道闸，再查凭据与账号锁：

```text
Package 已安装且已信任
&& channels.<id>.enabled == true
&& accounts.<id>.enabled == true
&& process_mode == Gateway
&& credentials_ready
&& account_lock_acquired
```

判断收进唯一权威函数 `ResolveChannelActivation`，决策码冻结在 [配置层级冻结](configuration.md)。只有 `Ready` 才 spawn sidecar。`/channel login` 是唯一例外，且只许起短命 setup helper，不开收消息循环。

## 6. 进程形态

目标形态是 `lubancode gateway`：常驻入口，持 ChannelManager、Headless Session registry、SessionWorkScheduler、Agent/Package/Tool catalogs，可选同进程再挂 App Server。过渡形态可在现有交互进程里挂 ChannelManager 做试跑壳，但须用显式测试参数（如 `--dev-host-channels`），验收完成后删掉，不与 Gateway 真值长期并存。

## 7. 交付语义速览

```text
adapter -> host：at least once
host ingress -> Agent turn：at least once + 持久去重
Agent reply -> platform：幂等尽力；平台支持 client id 时用 client id
```

不许写"exactly once"。崩溃夹在模型副作用与状态落盘之间，仍可能重放；能做的是缩小窗口、留账、让重复可识别。去重键、状态机、ReplyAction 细账见[消息合同冻结](message-contracts.md)。

## 8. 冻结件索引

| 冻结件 | 文档 | 版本 |
| --- | --- | --- |
| channel.yaml schema | [channel-manifest.md](channel-manifest.md) | schema 1 |
| Channel Bridge | [bridge-protocol.md](bridge-protocol.md) | `lubancode-channel/1`（protocol_version `2026-08-29`） |
| 统一事件与回复 | [message-contracts.md](message-contracts.md) | ChannelInboundEvent schema 1 |
| 配置与激活 | [configuration.md](configuration.md) | 层级规矩 v1 |
| 威胁模型与数据保留 | [security.md](security.md) | v1 |

改这些合同须先改本目录文档，再动实现；实现与文档不合，以文档为准，要么改实现，要么明改文档并升版本。

## 9. 实现批次索引

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 0 | 冻结合同（本目录文档） | 已落地 |
| 1 | 纯合同代码与假 sidecar：`src/channel/types.*`、channel.yaml parser、ComponentKind 追加 Channel、帧编解码、双向 router、`fake-channel-sidecar`、错误码 | 待实现 |
| 2 | ChannelManager 与入站耐久：状态机、journal、去重、队列背压、pairing、`/channels` 命令 | 待实现 |
| 3 | Headless Session 与路由：TurnIngress、provenance、router、session host、ChannelTurn | 待实现 |
| 4 | ReplyAssembler 与 outbox：final/block/native、分块、preview/committed 分账 | 待实现 |
| 5 | QQ Bot 参考适配器 | 待实现 |
| 6 | WeChat 参考适配器 | 待实现 |
| 7 | Webhook 共用底座（LINE/Feishu/Zalo/WeCom callback） | 待实现 |
| 8 | 其余 WebSocket/轮询渠道 | 待实现 |
| 9 | Gateway 服务化 | 待实现 |

## 10. 兼容承诺

- 现有工具 Plugin manifest version 与一次一进程语义不变。
- MCP Client 不因本单接渠道通知。
- Package `ComponentKind` 只追加 `Channel`，前六类旧值与旧行为不动。
- 老 config 没 `channels` 时零行为变化，不起线程，不开端口。
- 老 session 没 provenance 时可恢复（回落兼容推断）。
- 终端主会话的消息泵、peer、goal、loop 不因 Channel 默认改优先级。
- App Server 事件 schema 若新增 channel provenance，字段必须可选，老客户端忽略即可。
