# 配置层级冻结：项目只能收窄

_阶段 0 冻结件。渠道是全局网络能力，层级规矩比普通模型配置更严；本页定死谁能开、谁能收、密钥放哪、状态放哪。_

---

[渠道总览](README.md) · [channel.yaml 冻结](channel-manifest.md) · [Bridge 协议冻结](bridge-protocol.md) · [消息合同冻结](message-contracts.md) · [威胁模型](security.md)

## 1. 配置结构

全局 `~/.lubancode/config.json`：

```json
{
  "channels": {
    "qqbot": {
      "enabled": true,
      "default_account": "main",
      "accounts": {
        "main": {
          "enabled": true,
          "transport": "websocket",
          "app_id": "123456",
          "secret_env": "QQBOT_CLIENT_SECRET",
          "dm_policy": "allowlist",
          "allow_from": ["owner_openid"],
          "group_policy": "allowlist",
          "group_allow_from": ["group_openid"],
          "require_mention": true,
          "agent": "general-purpose",
          "reply": {
            "mode": "block",
            "tool_progress": false
          }
        }
      }
    }
  }
}
```

## 2. 层级规矩

四层各守各的口：

```text
全局 config：可启用账号、给凭据、定最大权限
项目 config：只可绑定本项目 Agent、收窄 sender/group/tool/reply 策略
环境变量：只供密钥值，不替项目开启账号
CLI 临时参数：只可停用或收窄，不可悄悄放宽
```

项目级 config 无权把 Channel 从关闭改成开启。陌生项目不能写一份 `.lubancode/config.json`，便替用户启动微信、读取 QQ token、开放公网 Webhook。

## 3. 激活决策

普通启动 `lubancode` 时 Channel 必须完全不运行。真正启动一个账号须同时过四道闸，再查凭据与锁：

```text
Package 已安装且已信任
&& channels.<id>.enabled == true
&& accounts.<id>.enabled == true
&& process_mode == Gateway
&& credentials_ready
&& account_lock_acquired
```

判断写成一只权威函数，不在 CLI、ChannelManager、Package mounting 三处各猜一遍（QQ 接入单 Q0 起实装于 `src/channel/activation.hpp`；渠道库不反向依赖 package，信任态以 `ChannelTrustState` 快照传入）：

```cpp
ChannelActivationDecision ResolveChannelActivation(
    ChannelProcessMode process_mode,          // InteractiveCli | OneShot | AppServer | Gateway
    const ChannelTrustState& trust,           // 已安装且已信任
    const std::optional<ChannelUserConfig>& channel,  // nullopt = 没有 channels 配置
    const std::string& channel_id,            // 只进诊断文案
    const std::string& account_id,
    const ChannelCredentialState& credentials,
    const ChannelLockState& lock);
```

决策码冻结：

```text
DisabledByDefault   没有 channels 配置
NotGatewayMode      进程不是 Gateway 形态
PackageUntrusted    Package 未安装或未信任
ChannelDisabled     channels.<id>.enabled != true
AccountDisabled     accounts.<id>.enabled != true
CredentialsMissing  凭据缺失或解析失败
AccountInUse        账号锁被另一实例持有
Ready               唯一可启动渠道执行载体的状态（受管子进程或进程内直连，Q1 定案）
```

只有 `Ready` 才能启动渠道执行载体。执行载体形态按渠道实现定案（QQ 已于 Q1 定案进程内直连，见接入单 §十五；未来其他渠道仍可走受管子进程），决策合同传输无关。其余状态只供 `/channels`、doctor 与日志展示，不产生后台线程和网络副作用。"Package 已安装且已信任"一闸：进程内直连的渠道实现（无渠道包）退化为"渠道实现内置受信"（装配侧恒信任，QQ 即此），不另造假包概念。

默认行为表：

| 启动方式 | 配置 enabled | 是否启动 Channel |
| --- | ---: | --- |
| `lubancode` | 无/false | 否 |
| `lubancode` | true | 否，只显示 `gateway not running` |
| 单发/管道模式 | true | 否 |
| App Server 普通模式 | true | 否 |
| `lubancode gateway run` | false | 否 |
| `lubancode gateway run` | true，且信任/凭据/锁齐 | 是 |
| `lubancode gateway start` | true，且信任/凭据/锁齐 | 是 |

`/channel login` 是唯一例外，但它只可起一只短命 setup helper：完成 QR/OAuth/凭据校验便退出，不开收消息循环，不留下监听端口。`/channel start` 若当前没有 Gateway，不可就地拉起账号，只给引导。

## 4. 密钥来源

首版只收三种：

| 来源 | 字段 | 规矩 |
| --- | --- | --- |
| 文件 | `secret_file` | 用户明确指定的**绝对路径**文件，须做权限与越界检查。QQ 首版推荐。 |
| 环境变量 | `secret_env` | 值从宿主环境取，配置只记名字。 |
| 登录产物 | （平台登录后写入） | 放 Channel 私有 state，`0600`/DACL 当前用户。 |

可兼容 `secret` 明文，但 `/channel doctor` 必须给 warning；日志、trace、session、错误文案一概打码。

**解析优先级（QQ 接入单 Q0 起冻结）**：`secret_file` > `secret_env` > `secret` 明文。高优先级来源**配置了但无效**（文件读不出、权限不安全、内容为空）时明报稳定错误，**不静默降级**到低优先级来源——降级只会把"配置错了"藏成"碰巧能用"。

`secret_file` 读取规矩（它不是任意 JSON 凭据文件）：

- 内容即 AppSecret 原值；至多剥掉末尾一处换行（`\n` 或 `\r\n`），其余字节一概不动。
- 拒绝空值、拒绝内部控制字符、拒绝超限文件（上限 8 KiB）。
- 路径必须绝对；按 canonical 解析（符号链接/重解析点解析到真实目标再验）；目标须是常规文件、归属当前用户，且无组/其他用户读写位（POSIX）或 DACL 无其他账户读权（Windows）。

密钥交接：不进 argv、不进模型输入、不进会话、不进 trace、不进错误与普通日志。凭据只进入获准的渠道执行载体——QQ 已定案进程内直连（Q1，接入单 §十五），凭据根本不出宿主进程：无专用启动管道，无子进程环境继承，`SidecarEnvAllowlist` 不消费。未来某渠道若定案受管子进程，则按本节原口径另补"经不落日志的专用启动管道交付、环境走白名单"的实装与冻结件修订。首版不生成 `credentials.json.enc`；接上 OS 密钥库/DPAPI 后再谈登录存储。

## 5. 状态目录

```text
~/.lubancode/channels/
  locks/
    qqbot-main.lock
  qqbot/
    main/
      account.json           非密配置与版本
      credentials.json.enc   登录产物；能接系统密钥库时改用引用
      ingress/
        journal.jsonl
        dead-letter.jsonl
      outbox/
        journal.jsonl
      adapter/
        state.json           cursor、resume seq、sync buf
      logs/
        adapter.log
```

源码与数据分家。Package 升级、卸载，不得顺手删账号状态。

状态根落位（应用根语义）：上树挂在渠道账号状态根 `channel::DefaultChannelsStateRoot()` 下——个人布局即 `~/.lubancode/channels` 原样；设了 `LUBANCODE_HOME`/`LUBANCODE_DATA_HOME`（应用 Worker，合同 `docs/reference/capability-contract.md` §13.2）时随状态根落数据根（`<数据根>/channels`）。

## 6. pairing 不是平台登录

两件事不可混：

- 平台登录：证明这只 adapter 能代表哪个 bot/account。
- 用户 pairing：决定哪个远端 sender 可以驱动 LubanCode。

未知 DM 在 `dm_policy=pairing` 下：

```text
收到消息
-> 不进 Agent
-> 生成一次性 pairing code
-> 回固定配对提示
-> 本地 /channel pairing approve 后才放行后续消息
```

pairing 记录须带 channel、account、sender id、创建时间、过期时间与尝试次数。code 用加密随机数，短期有效，存 hash 不存明文。重复申请限速。批准只认宿主看到的 sender id，不认模型文字里自报身份。

## 7. 策略字段与默认值

```text
dm_policy: pairing | allowlist | open | disabled
group_policy: allowlist | open | disabled
require_mention: true | false
allow_bots: false
tools: {"allow": [...]|[], "deny": [...]}     # 渠道段与账号段都可写
```

默认值：

- DM：`pairing`。
- Group：`allowlist`。
- Group mention：`true`。
- 其他 bot：拒绝。
- 非官方个人账号自动化（如 Zalo Personal）：默认整体 `disabled`，须明写风险确认。

`tools` 上限字段（QQ 接入单 Q0 起）：**allow 未设置与 `allow: []` 语义分开**——未设置 = 本层不添上限；`[]` = 本层禁用全部工具。每一层只可收窄，deny 永远压过 allow。渠道段写渠道上限，账号段写账号上限，binding 写会话上限，合并规矩见 §8。

**QQ 首版模板**（`MakeQqTemplateAccount()`，逐字段显式，不改全渠道默认值迁就 QQ）：

```json
{
  "transport": "websocket",
  "dm_policy": "pairing",
  "group_policy": "disabled",
  "allow_bots": false,
  "require_mention": true,
  "reply": {"mode": "final"},
  "tools": {"allow": ["read_file", "search"]}
}
```

`tools.allow` 是核过注册名的最小只读名单（`read_file`、`search` 均为现有注册工具名）。缺省不等于"所有免确认工具都是只读"；动态 tool_search、插件、MCP、子 Agent 的工具名都不在这份名单里，五层交集自然拦下，扩不出上限。首版不开放任意 shell。

准入次序——先鉴权，后建 session。不通过准入的消息，不建 session，不召回记忆，不调用模型：

```text
event
-> account enabled
-> bot/self-loop check
-> DM/group policy
-> sender allowlist
-> group allowlist
-> mention/reply trigger
-> command policy
-> route binding
-> session resolve
```

## 8. session key 与 binding

session key：

```text
channel:<channel_id>:<account_id>:<kind>:<conversation_id>
```

thread 平台：

```text
channel:<channel_id>:<account_id>:<kind>:<conversation_id>:thread:<thread_id>
```

群聊可配 scope：

```text
group                 全群一场 session（默认）
group_sender          每位 sender 一场
group_thread          每条 thread 一场
group_thread_sender
```

切 scope 不迁旧 history；新 key 开新场，旧绑定留档可查。

binding 形状：

```json
{
  "bindings": [
    {
      "agent": "ops-agent",
      "match": {
        "channel": "qqbot",
        "account": "main",
        "conversation": {"kind": "group", "id": "group_openid"}
      },
      "policy": {
        "tools": {"allow": ["read_file", "search"], "deny": ["shell", "write_file"]}
      }
    }
  ]
}
```

匹配从具体到宽：

```text
conversation+thread
-> conversation
-> account
-> channel
-> default Agent
```

同档命中两条，报冲突，不按文件次序碰运气。

**Agent 选择与工具权限分两本账（QQ 接入单 Q0 起）**：Agent 取最具体的那条 binding（同档冲突整事件拒绝）；工具上限则**收集所有命中 binding**，与渠道上限、账号上限做交集，再减各层 deny 并集：

```text
有效工具 = Agent 已有工具
         ∩ 渠道 tools.allow ∩ 账号 tools.allow ∩ 每条命中 binding 的 tools.allow
         - （渠道 ∪ 账号 ∪ 所有命中 binding 的 tools.deny 并集）
```

每层 allow 未设置 = 不添上限；设了（含空名单）= 只许名单内。**具体 binding 抹不掉宽层 deny**——conversation 档 binding 放行的工具，仍会被它命中的 account 档 binding 或渠道/账号段的 deny 拦下。有效策略与来源账（哪些层出了手）随路由决策冻结带进执行：准入在执行前重验，本轮用的策略版本以执行时重验的决策为准；已开始的工具沿现有取消边界收场，不假称副作用能撤回。须确认（needs_confirm）的工具只有一条生路：至少一层显式 allow 列了它、且五层交集后仍可用——见 [security.md](security.md) §3。

记忆与多用户隔离首版默认：

```text
owner DM       可按 binding 明开 project/user memory
非 owner DM    user memory 关闭；project memory 关闭
group          user memory 关闭；project memory 关闭
```

要给某个工作群开 project memory，须在全局账号授权与具体 binding 两处都明写。项目 config 只能在全局授权范围内收窄。召回 query provenance 必须带 sender/conversation；缓存不能跨安全域复用。

## 9. 账号状态机

```text
Disabled
-> Validating
-> Starting
-> Authenticating
-> Connecting
-> Running
-> Degraded
-> Backoff
-> Stopping
-> Stopped
```

不可恢复终态：

```text
Misconfigured
TrustRequired
NeedsLogin
Fatal
```

状态迁移都带：

- channel/account。
- timestamp。
- stable reason code。
- 脱敏 detail。
- retry_at。
- generation。

reason code 复用 [Bridge 协议](bridge-protocol.md) 第 6 节的稳定名，另加状态机专属：`misconfigured`（manifest/schema/config 错）、`trust_required`（Package 未批）。

## 10. 重启与退避

```text
1s, 2s, 4s, 8s, 16s, 30s, 60s
```

加 10% jitter。成功稳定运行一段后归零。以下不自动重试：

- manifest/schema 错。
- trust 未批。
- 凭据缺失。
- 账号被吊销。
- 协议版本不兼容。
- 状态迁移失败。

## 11. 账号锁

同一 `channel_id + account_id + credential fingerprint` 只许一只本机实例持有。锁文件记 pid、start time、generation，不记密钥。假死锁须核进程存活再清，不可见锁便直接删。

## 12. reload

reload 走 diff：

- 只改 reply/policy：新 turn 用新快照，不重连 adapter。
- 改 transport/credential：优雅重启该 account。
- 改 Package digest/protocol：先起新 generation，握手成功后切流，再停旧 generation。
- 新 generation 起不来：保旧 generation，报 reload failed。

首版可先 stop-old/start-new，但须把消息空窗与重投写进测试。不可装作原子热切已经完成。
