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

### 相对时间提醒

`create_reminder` 的时间参数三选一：`delay_seconds`（相对延时）、`at_ms`（UTC 毫秒时刻）、`cron`（周期）。例如“五分钟后提醒我取报告”传 `{"description":"取报告","delay_seconds":300}`，宿主以这条消息的接收时间加 300 秒。重试沿用同一时刻，不把提醒越推越晚。延时须为正整数，最长十年；成功回执提供 `dueAtMs` 和可直接读取的 `dueAtUtc`（ISO 8601 UTC 时间）。

Gateway 的 system 只放工作目录等稳定说明，不注入动态时间。需要当前日期或时刻时，模型调用只读工具 `get_current_time`，取得 `now_ms`、ISO 8601 `utc` 和 `timezone: UTC`；时间只通过工具回执进入历史。QQ 新账号模板默认允许此工具；已有账号若设置了 `tools.allow`，须在各层允许名单补上 `get_current_time`。相对提醒直接传 `delay_seconds`，无需 shell 权限，也不让模型猜时间戳。

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

只有 `Ready` 才能启动渠道执行载体。执行载体形态按渠道实现定案（QQ 已于 Q1、飞书于 F1 定案进程内直连，见各家接入单；未来其他渠道仍可走受管子进程），决策合同传输无关。其余状态只供 `/channels`、doctor 与日志展示，不产生后台线程和网络副作用。"Package 已安装且已信任"一闸：进程内直连的渠道实现（无渠道包）退化为"渠道实现内置受信"（装配侧恒信任，QQ/飞书即此），不另造假包概念。

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

密钥交接：不进 argv、不进模型输入、不进会话、不进 trace、不进错误与普通日志。凭据只进入获准的渠道执行载体——QQ（Q1）与飞书（F1）已定案进程内直连，凭据根本不出宿主进程：无专用启动管道，无子进程环境继承，`SidecarEnvAllowlist` 不消费。飞书 AppSecret 与 QQ AppSecret 同一套来源规矩与泄露禁令（引导请求体里的 AppSecret 不进任何错误文案）。未来某渠道若定案受管子进程，则按本节原口径另补"经不落日志的专用启动管道交付、环境走白名单"的实装与冻结件修订。首版不生成 `credentials.json.enc`；接上 OS 密钥库/DPAPI 后再谈登录存储。

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
  "tools": {"preset": "ask"}
}
```

模板默认"操作前询问"档：查询与提醒工具预授权，写文件/命令/发文件进审批带（执行前经渠道按钮问用户，见下）。预设展开的 `allow` 是核过注册名的名单（`read_file`、`search` 均为现有注册工具名）。Q5 起多了三枚聊天侧任务工具（`create_reminder`/`list_reminders`/`cancel_reminder`，Gateway 装配注册，见 `runtime/channel_automation`）：只对过了配对/准入的会话可用——未配对 sender 进不了模型，拿不到工具；落账走 automation 域命令与归属闸（任务只归创建者查询/取消），不碰文件系统。缺省不等于"所有免确认工具都是只读"；动态 tool_search、插件、MCP、子 Agent 的工具名都不在预设名单里，五层交集自然拦下，扩不出上限。任意 shell 不预授权，只在审批带内可申请。

**企业微信智能机器人首版模板**（`MakeWecombotTemplateAccount()`，W1；字段同 QQ 模板，只改凭据语义——`app_id` 存 BotID，`secret_env` 指长连接专用 Secret）：

```json
{
  "transport": "websocket",
  "app_id": "替换为 BotID",
  "secret_env": "WECOMBOT_SECRET",
  "dm_policy": "pairing",
  "group_policy": "disabled",
  "allow_bots": false,
  "require_mention": true,
  "reply": {"mode": "final"},
  "tools": {"allow": ["read_file", "search", "create_reminder", "list_reminders", "cancel_reminder", "get_current_time"]}
}
```

用户侧准备（管理后台智能机器人开 **API 模式选长连接**，拿 BotID + Secret——长连接专用密钥，与回调模式的 Token/AESKey 互斥，二选一）。密钥规矩全沿 QQ：`secret_file` > `secret_env` > inline 明文，setup 向导落受管 `secret_file` 时会清掉 env 引用与旧明文。首版范围：文本进出（voice 回调的平台转写文本进正文；image/file/video 落占位说明），出站 markdown（≤20480 字节 UTF-8 自动分段），群聊映射支持但模板默认禁用；媒体收发、模板卡片、流式、enter_chat 欢迎语、主动推送归 W2。回复限频单会话 30 条/分钟、1000 条/小时（发送线程记账排队）。

**飞书首版模板**（F1 起，`MakeFeishuTemplateAccount()`；照 QQ 模板五可选项对齐，`secret_env` 预指 `FEISHU_APP_SECRET`——手写配置的默认密钥来源，向导存了受管 `secret_file` 后由提交侧清掉此引用）：

```json
{
  "channels": {
    "feishu": {
      "enabled": true,
      "default_account": "work",
      "accounts": {
        "work": {
          "enabled": true,
          "transport": "websocket",
          "app_id": "cli_xxxxxxxx",
          "secret_env": "FEISHU_APP_SECRET",
          "dm_policy": "pairing",
          "group_policy": "disabled",
          "allow_bots": false,
          "require_mention": true,
          "reply": {"mode": "final"},
          "tools": {"allow": ["read_file", "search", "create_reminder", "list_reminders", "cancel_reminder", "get_current_time"]}
        }
      }
    }
  }
}
```

飞书密钥规矩全沿 Q0（`secret_file` > `secret_env` > `secret` 明文；高优先级来源配置了但无效时明报不降级）。用户侧准备：open.feishu.cn 建企业自建应用、事件订阅选"使用长连接接收事件"、订阅 `im.message.receive_v1`、开通发消息权限、发布应用版本；首版仅国内域，larksuite 后置。

Q6 起各层 tools 段另有 `approve`（可申请审批带，presence 合同与 `allow` 同款：键在=本层参与，未写=不参与）：名单内的须确认工具**不预先授权**——模型可见（看不见无从申请），执行前经渠道按钮问用户（QQ 键盘卡，`channel.approval.requested/resolved` 落 V3），允许才执行这一次；拒绝/超时/取消都不执行，超时默认拒绝不默认放行。`deny` 永远赢：hard deny 不可被按钮覆盖。有效审批带 = 各显式层 `approve` 的交集（与 `allow` 同构）。旧配置零层声明仍为空带。新 QQ 向导默认保存 `tools.preset: "ask"`，展开为常用只读/提醒预授权和写文件/命令/发文件审批带。`readonly` 只开放查询，`auto` 预授权常用工具；preset 与 allow/approve 互斥，deny 仍优先。旧账号可用 `lubancode channel setup qqbot --permissions` 选择模式，无须列工具名。

**Q7 菜单/面板/命令绑定**（QQ 接入单 §十三；账号段可选字段，不写零行为变化）：

```json
{
  "menu": {
    "publish": true,
    "items": [
      {"name": "帮助", "type": "send_message", "send_message": "/帮助"},
      {"name": "官网", "type": "link", "link": "https://example.com"},
      {"name": "更多", "type": "menu", "sub_menu_items": [
        {"name": "设置", "type": "send_message", "send_message": "/设置"}
      ]},
      {"name": "搜索偏好", "type": "switch", "switch_id": "search", "switch_default": true}
    ],
    "panel": {
      "enabled": true, "scope": "c2c", "target_type": "all", "remark": "主面板",
      "items": [{"name": "查看任务", "desc": "查看我的定时任务", "type": "command"}]
    }
  },
  "commands": [
    {"match": "/帮助", "action": "help"},
    {"match": "/文件说明", "action": "file_help"},
    {"match": "/查看任务", "action": "list_reminders"},
    {"match": "/整理", "action": "prompt",
     "prompt": "请整理我当前的待办并给出摘要。", "require_tools": ["read_file"]}
  ]
}
```

- **发布是显式开关**：`menu.publish` / `panel.enabled` 不开就不碰平台——安装与普通启动不自动覆盖用户在开放平台配好的菜单/面板。发布在 Gateway tick 里做：先读远端，远端与本地已发布摘要一致才写（只有我们改过）；远端被人工改过 → 冲突，不覆盖，给本地可见提示。面板按备注里的所有权前缀（`lubancode:<渠道>:<账号>:<用户备注>`）认领复用，不反复创建耗额度。官方限速：菜单写 5 QPM、面板写 10 QPM、读 30 QPM、目标更新 60 QPM，发布器各接口独立滑窗。
- **点击不等于执行**：官方 `send_message`/`command` 点击只把文本填入聊天输入框，用户发送后成为普通 C2C 消息——照常走 ingress 账、准入、路由与五层工具闸；`hints.command` 是识别位（`/命令 [参数]`），不是分派。旧式快捷菜单的 type=12 互动不发布不解码；消息内按钮的 type=11 归 Q6（`channel/qq` 的互动位与键盘回调），不在此列。
- **命令绑定**：`match` 去首尾空白后整串等值才命中（用户改过、带参数的不命中，照常进模型）；`action` 四路——`help`/`file_help`/`list_reminders` 是宿主控制命令（零模型直答，`list_reminders` 走 Q5 任务桥归属闸，只看自己的任务）；`prompt` 把预设输入当作用户发言进模型，`require_tools` 逐名过本轮冻结策略，名单外就地拒——**菜单不扩权，绑什么工具就得什么权限**。`target_type=specific` 的 c2c 面板按已配对用户关联对象，撤销配对后重同步时移除；面板可见与 `only_admin` 都不是宿主授权。
- `menu.items` ≤10、子菜单 ≤5（不嵌套）、`panel.items` ≤20、`link` 须 `https://`、名称长度按平台字符口径（一个中文汉字算 2 字符）校验。


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

## 13. 连接状态与 TLS 信任根（QQ 连接诊断单 §三/§四）

### 13.1 连接状态（QQ session 独占平台连接状态）

平台连接状态的唯一权威在适配器的网关 session（QQ 独占）；宿主只透传与输出结构化快照，不另养一份猜测状态。快照字段（`src/channel/qq/qq_adapter.hpp` 的 `ConnectionSnapshot`）：`connected`（只在 READY/RESUMED 后成立；断线、停止立即 false）、`thread_alive`（网关线程存活，不等于在线）、`stage`（`fetching_token` → `fetching_gateway_url` → `connecting`（TCP/TLS/WebSocket）→ `identifying`（Identify/Resume）→ `connected`，收口 `stopped`）、`last_failure`（stage + 稳定 error_code + 脱敏 detail + 失败时间；退避事件不改写它）、`failure_history`（连接成功后归档最近 8 笔，当前清空）、`retry_count` 与 `next_retry_at_ms`。

`gateway run` 默认打印开始连接、首次失败、在线、断线与停止；同一稳定码的重复失败 30 秒窗内合并（超窗补一条带累计数），原因变化立即显示。跨进程只读口：Gateway 每 tick 发布脱敏快照 `<渠道状态根>/<channel>/<account>/connection-status.json`（带 boot ID、pid、更新时间；`lubancode channel status <渠道> <账号>` 读它并校验进程存活与 60 秒新鲜度——进程死/快照过期/未连接都退非零，不凭 PID 宣告成功，快照不是连接状态权威）。

零泄露：连接诊断不打印 AppSecret、token、Authorization、原始响应体或带敏感参数的 URL；detail 来自各层稳定账，宿主输出前再过一道控制字符折叠与限长清洗。

### 13.2 TLS 信任根

wss 的 TLS 验证恒 REQUIRED，无降级开关，失败不改走明文。信任根解析（`src/channel/transport/tls.hpp` 的 `ResolveChannelTrustRoots`）：

- **显式信任锚**（装配 seam `ChannelGatewayWiring::Options::ca_pem`，测试位）：调用方全权指定，不回退平台来源；解析不出证书时明报（装配诊断一行"信任根不可用"），连接时报 `tls_trust_store_empty`。**未接入用户配置**——若未来开放配置须接全配置解析、优先级与文档（QQ 连接诊断单 §四原话）。
- **平台默认（Windows）**：从系统证书库（Root/Ca，CurrentUser+LocalMachine，剔除 Disallowed 显式不信任）导出信任根喂 mbedTLS，并在 mbedTLS 握手验证里接 Windows 链构建与 SSL 策略校验（`CertGetCertificateChain` + `CertVerifyCertificateChainPolicy`，覆盖链信任/有效期/用途/系统不信任策略）；系统裁决为链信任权威（可走 AIA 拉中间证书，本地导出子集做不到），系统拒则握手拒，系统过只放行"链不可信"误报位。主机名与有效期/EKU 由 mbedTLS 内置验证（`mbedtls_ssl_set_hostname` + verify flags）承担，与系统判定双保险不互盖。SDK 兼容口径：`CERT_CHAIN_PARA`/`CERT_CHAIN_POLICY_PARA` 只写 `cbSize`（部分 SDK 展开集缺 `dwUrlRetrievalTimeout`/`pvExtraPara` 成员），不传 `pvExtraPara`——SSL 主机名校验不依赖它。用户无须下载 PEM、造 `/etc/ssl` 目录或设环境变量。
- **平台默认（Linux/macOS）**：探测系统 PEM 路径（`/etc/ssl/cert.pem` 等），行为与既有版本一致；测试 CA 注入不受影响。

稳定错误码：`tls_trust_store_empty` / `tls_trust_store_load_failed` / `tls_cert_expired` / `tls_cert_hostname_mismatch` / `tls_cert_not_trusted` / `tls_cert_policy_rejected` / `tls_cert_verify_failed` / `tls_handshake_timeout` / `tls_handshake_failed`。HTTP 取令牌/查地址（cpr/libcurl 栈）与 WSS（mbedTLS 栈）是两段独立信任路径，分别报错分别验收，HTTP 成功不冒充 WSS 成功。

Windows 证书库导出与 SSL 策略校验（`#ifdef _WIN32` 块）在 CI 的 Windows 车道只编 main 不跑测试——真机行为归 Q3 Windows 发布包复测，单内如实标"未验"。
