# 会话、上下文与存档

[文档首页](../../README.md) · [上下文压缩机制](../context/compaction.md) · [命令参考](../../reference/commands.md) · [项目记忆](../../architecture/memory/design.md) · [安全模型](../../development/security.md) · [测试手册](../../development/testing.md) · [架构说明](../../architecture/README.md)

LubanCode 留一份只追加的 v3 会话原账，再分别投出完整聊天历史与模型当前上下文。Memory 另管跨会话知识。字段和接线边界见 [Session v3](../../architecture/session-v3.md)。

## 一场会话有什么

新会话默认写 v3。主账在 `~/.lubancode/workspaces/<workspace_key>/sessions/<sessionId>/<sessionId>.jsonl`，首行是 system 消息，往后只有两类行：

- `message`：system、user、assistant、tool 正文，带稳定身份、用途与来源。
- `event`：请求、工具执行、结果选用、hook、压缩等运行事实。

两类行共用递增 seq 与哈希链。流式片段分批保存，完整 assistant 在响应收口时定稿，不等整轮结束才写账。完整工具结果放 `artifacts/`，子代理另建 `subagents/<child>/<child>.jsonl`。

历史显示保留压缩前原文；当前上下文按已提交链选消息。二者不能混用。普通 flush 与断电落稳也分档，不把“写过文件”一概称作断电安全。

旧 v2 场仍认 `main.jsonl` 与 `session.json`，读取按源格式分派。只有显式设 `LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0` 才让新场回到 v2；这是待清理的过渡开关，不是新版使用前提。

## 会话归哪间 workspace

身份由统一裁决器计算（Git 公共目录 → 显式 marker → 项目 config → 启动 cwd 四级）：`/sessions` 列当前 workspace 的场，`--continue` 与裸 `/resume` 也只在当前 workspace 里找。主仓与 linked worktree 解出同一个 common git dir，归同一间 workspace——在 worktree 里开的会话，回主仓也列得出、续得上；项目记忆同理同树。两份独立 clone 路径不同，默认各立各的 workspace。

## 恢复与重放

```text
/resume
/resume 3
/resume 20260806-...
```

裸 `/resume` 在真终端打开全屏会话台账：

- **搜索**：输入即搜，命中标题、首句、session id 与目录；ASCII 不分大小写，中文按原字。搜索只筛内存，不因每敲一字重读盘。
- **筛选与排序**：`Tab` 轮换 Search / Filter / Sort 焦点，`←/→` 改选项。Filter 认 `Cwd | All`；Sort 认 `Updated | Created`。
- **浏览**：`↑/↓` 移动，`PageUp/PageDown` 翻页，`Home/End` 到头尾。换筛选后按 id 留住选中项，它消失了才落到最近一行。
- **查看态**：`Ctrl+T` 看所选会话的转录（v3 按 seq 游标翻页，v2 仍取头尾片段，`Esc`/`Ctrl+T` 收起回原行）；`Ctrl+E` 摊开选中场的长标题、目录、id、模型、消息数与创建/更新时间；`Ctrl+O` 在紧凑行与舒展行间切换，只改画法，不动筛选与选中。
- **恢复**：Enter 恢复所选场；Esc 原路返回，不改当前会话。台账里没有删除键——浏览不兼任碎纸机。

恢复是 **resume-as-new**：先验源场账（hash 链、父子边），折叠出有效对话与控制态，再开一间新 session（`start_reason=resume`，v3 通过来源引用反指源场；v2 仍用 manifest）接管对话。源场 Journal 永不重开追加——旧账封口存档，新账接着写，两边以事件引用互指，不存在“续写旧文件”的路径。崩溃在场半路（回合规了没收口、尾行撕裂）也按同一套规矩：验得过的前缀照折，悬空工具分档如实标注，不冒充执行过。

## 归档：日常收拾

> **V3-GAP-09：** 归档与取消归档目前仍要求 `session.json`，默认 v3 场缺该文件会报 `session.not_found`。下列步骤仅说明旧 v2 场行为；新版生命周期待接线，见[清理清单](../../development/v3-legacy-audit.md)。

不想要的场子先归档，转录仍留着：

```text
lubancode archive <id|标题>    # 顶层命令,归档任意一场
lubancode unarchive <id>       # 取消归档,搬回可续聊状态
/archive                       # 会话内:归档当前这场,成功后退出
/sessions archived             # 只读列表,看归档了哪些
```

- 归档按 session 状态图把场子转入 archived——只改 `session.json` 的 status，目录不搬、字节不动；lifecycle 记一笔 intent+result 回执。
- 归档后 `--continue`、`/sessions`、裸 `/resume` 一概略过它；`/sessions archived` 看得见，想续聊先 `lubancode unarchive <id>`。
- 会话内 `/archive` 先刷盘、封口再转态退出；后台子代理还在跑时拒绝。
- 引用认完整 id、唯一 id 前缀或唯一命中的标题；重名便列出短 id 叫你点明，绝不猜一场。

## 永久删除：另开明路

> v3 边界：现有删除实现仍从 `main.jsonl` 取封口状态与末 hash，尚未完整改到 v3 主账。下列墓碑与封口保证属于旧路径，不据此承诺 v3 删除闭环。

当真不要了，再显式删：

```text
lubancode delete <id|标题>           # 顶层命令,交互确认
lubancode delete <id|标题> --force   # 跳过确认——只给脚本,不可恢复
/delete                              # 会话内:删当前这场,确认后退出
```

- 确认屏写标题、完整 id、目录与「永久删除」；缺省取消，EOF、Esc、空答、别的答案都算取消，只有整行 `y`/`yes` 才动手。
- 回合正在流、后台子代理还在跑时拒绝；等它们收尾再删。
- 删除走 lifecycle 账：durable intent → 墓碑（`tombstones/<session_id>.json`，记末事件 hash 与原因）→ 删目录 → result 回执；活锁或 active session 拒绝。
- context artifact 按 hash 共用，可能被别的会话引用——删除不连坐删 blob；引用计数与 GC 另做。
- 归档不是删除，删除也不可逆。日常收拾先 archive。

## 标题

`/title` 查看或修改标题。v3 标题事件与持久恢复尚未接全，不能把内存改名当成已经写入 `session.title.applied`。标题不改 session id，也不重命名 JSONL；缺项见 [schema 第十节](../../architecture/trajectory-v3-schema.md)。

## Markdown 导出

```text
/export
/export docs/session-review.md
```

默认导到当前会话目录的 `exports/<id>.md`。v3 导出按当前链投影生成，hidden 正文默认不导；它不是完整历史时间线导出。压缩前原文仍在 JSONL，可从历史视图查看。项目记忆不混入导出；本轮临时召回包也不写入 history。

## 上下文由什么组成

模型上下文大体分四块：

1. 稳定系统提示：人格、工作方式、工具方针、协议段。
2. 运行环境：cwd、平台、项目指令、Skill 索引、延迟工具索引。
3. 工具 schema：当前已挂载工具的名称、说明与 JSON Schema。
4. 历史：用户、助手、工具调用与结果，以及压缩摘要。

现有 memory 路径把命中内容拼成本轮 `turn_context`，随尚未发送的用户消息尾部进入请求视图。它不进入永久 history，不随着内部工具来回越积越多；下一条外层用户消息再重算。召回持久桥仍依赖旧 recorder，默认 v3 场的完整接线见 [Session v3 边界](../../architecture/session-v3.md)。

## token 与缓存命中

后端把各 wire 的 usage 归一成统一口径：**输入**（未从缓存读取的部分）、**缓存命中**（从服务端缓存读取的部分）、**缓存写入**（个别协议有此概念）、**输出**。终端统计行与 `/context` 显示的"输入"是完整输入（输入+缓存命中+缓存写入），命中率按 token 计、分母只取输入——DeepSeek 报 49k 命中 + 1k 未命中就显示 50k 输入、98% 命中，既不是 1k 也不是 99k。

“缓存命中”不是 LubanCode 在本机缓存回答。它是模型服务报告：本次输入里有多少 token 复用了服务端 prompt cache。通常系统提示、工具 schema 与旧历史前缀越稳定，命中越多。不同服务是否支持、怎样计费，以服务端返回为准；没有字段时只显示能确认的总输入/输出，服务端没回报 usage 就写"未回报"，不拿 0 冒充真未命中。每次模型请求各落一笔逐步流水账（步号、请求 id、epoch、断因），整轮汇总按 token 求和。

## `/context`

裸敲展示系统提示、工具与历史的占用分解。带参数临时改本场窗口：

```text
/context 256k
/context 512k
/context 1m
/context 1000000
```

`k`、`m` 按十进制。这个值告诉压缩器何时该收历史，也供状态栏计算百分比。它不能凭空放大服务端真实窗口；provider 配错过大，最终仍会被远端拒绝。

## 自动压缩

v3 的手动和自动压缩共用 `RunV3Compact`。先选范围、查容量，再生成候选、校验、提交 `compact.applied`。提交落稳后替换当前内存 history，同场下一请求便使用新摘要与保留消息。

失败、取消或拒收不会采用候选。连续压缩会把旧摘要与后续历史一起纳入材料，新摘要取代旧摘要。超窗时按整轮退范围，不拆工具配对；退无可退便报错。压缩走 cheap 路由及其预算，未配时按路由规则回落 normal。

`/compact --dry-run` 在 v3 场尚未接线，会明报未执行。旧四分区双账算法仍供 v2 路径使用，不能拿它解释新版行为。详见[压缩指南](../context/compaction.md)。

## 观测账

压缩数字来自持久 `compact.applied` 及其请求、校验引用，历史分界线显示当时的前后 token，不在恢复时重算。模型实报与本地估算分列；缺 usage 不补零。

当前离线 `/usage` 读口仍枚举 v2 流，尚未完整接入 v3 主账与递归子账。终端活统计不能替代磁盘对账，也不能据空样本断言零消耗。迁移位置见[清理清单](../../development/v3-legacy-audit.md)。

## 压缩摘要的验收

摘要不再只靠"不少于 40 字"防呆。模型必须同时给出六栏 Markdown 存档和末尾一枚 JSON manifest（goal / constraints / open_items / next_action）。提交前程序逐项核：

- manifest 解析得动、goal 非空；
- 会话活动待办（todo 里 pending/in_progress 的条目原文）每一条都在 open_items 里——漏一项就拒收，旧历史不动；
- 工具 use/result 配对不破。

压缩后的新历史 = 存档 + 热区。热区按 token 预算（默认 12k）从最后一轮整轮保留起往前收；最新用户消息本身绝不丢，它所在的轮若超出预算，则轮头保留、其余按"工具调用+结果"消息组从尾收——压缩结果必须明显短于压缩前，不降反升时程序拒收换账并回执。同一轮里若无足够新内容（滞回带，默认 4k token），自动压缩不会再发摘要请求。

## 有损截断的显式告警

自动压缩之外还有一条保命索：单条工具结果巨肥（超过估算窗口的四分之一，token 轴口径）时尾部截断，防 read_file 吞大文件把 compact 的摘要请求也撑爆。这层兜底是有损的：真触发时终端会打一条醒目警告，说明截了哪些结果、完整流水仍在会话存档（`/export` 可查）——不让"语义压缩失败被截断救场"静默发生。

## 手工压缩

```text
/compact
/compact 保住 API 兼容性决定、失败测试和用户尚未回答的问题
/compact --dry-run
```

参数会并入本次压缩要求。适合长任务中途收束，或自动阈值尚未到、用户已知前文不再需要的场景。`--dry-run` 只算结构压缩的可回收量与钉住项，不动历史。

## 窗口防线

| 配置 | 单位 | 用处 |
| --- | --- | --- |
| `context_window` | token | 正常预算、状态百分比、自动压缩阈值、mid-turn projected 评估、保命索截断线（单条结果超窗口 25% 即截）；窗口未知时按 128k 兜底。 |

上下文保护只有 token 一条轴（旧的 `max_context_chars` 字节安全网已拆除：两把尺松紧随语言漂移，英文裁早、中文失守）。token 估算全库统一一把尺：ASCII 4 字符约 1 token，非 ASCII（中日韩等）每字约 1.5 token；旧版 `/3` 与 `/2` 两把尺已并轨。

## 工具输出与上下文

- `read_file`、`search`、命令与网页工具各自先限输出，免得一件工具吞掉整窗。
- **无损结构压缩（每轮请求自动生效）**：发给模型的视图里，冷区（最后一轮用户输入之前）的只读工具结果按精确键 + 内容指纹收账——同文件同内容读三遍只留一份正文加"累计出现 3 次"的引用；文件改版后再读，旧版标"已改版"并保头部预览，绝不与新版本合并；超长结果换 artifact 引用（头尾各留 256 字节预览与内容指纹）。副作用工具（`run_command`、`web_fetch` 等）一律不判重——命令文本相同不代表这次可以不跑，压缩只改展示视图，绝不跳执行。活历史与 session JSONL 一字不动；需要核查原文时从会话存档取回。
- `/compact --dry-run` 只算不动手：报告结构压缩可回收多少、哪些被钉住（最近热区、活动待办），不发请求、不改历史。
- 终端 transcript 可保留截断后的全文供 `Ctrl+E` 查看；真正回填模型的仍是工具结果。
- 子代理内部翻找不进入主 history，只把末尾结论带回。
- 延迟工具减少每轮 schema 体积；挂载后才开始计入工具预算。
- 图片会以协议 ImageBlock 进入当轮消息；文件本身不复制进 session 文本。

## 清理与删除的区别

| 动作 | 清什么 | 不清什么 |
| --- | --- | --- |
| `/clear` | 当前内存 history 与当前屏面 | 磁盘 session、项目记忆、配置 |
| `/memory forget` | 一条项目记忆移入 archive | 会话历史 |
| `/archive`、`lubancode archive` | 一场会话移出默认列表（`session.json` 转 archived，目录与字节不动，unarchive 可回） | 会话内容本身、记忆 |
| `/delete`、`lubancode delete` | 一场磁盘会话（永久，先确认） | 其他会话、记忆、artifact blob |
| `/worktree exit remove` | 工作树目录与分支（须满足安全条件） | 主仓库、共享项目记忆 |

## 并发与落盘

- session 事件由主会话顺序追加。
- 工具转录的屏幕快照另有锁；`Ctrl+O` 监听线程不直接遍历主线程正在改的 vector。
- 项目记忆写入只排 job，后台进程拿项目锁并用临时文件原子替换。
- 后台命令日志放系统临时目录，不写进 session 正文。

## 常见问题

### `--continue` 没恢复我想要的那场

它只找当前 cwd 最近一场。用 `/sessions all` 找 id，再 `/resume <id>`。

### `/context` 显示很满，服务端却没报错

百分比按当前配置窗口算。窗口配小会提前压缩，配大又可能晚于真实限制。先核对 provider/model 目录与服务官方窗口。

### 压缩后还能导出旧内容吗

能。session 是全量事件账，compact 只改后续请求使用的 history 形状。导出会标出压缩点。

### 项目记忆会污染会话吗

不会写入永久 history、JSONL 或 Markdown 导出。它只作本轮临时 `turn_context`，随用户消息尾部进入请求视图，且开头明写“只作线索”。

### 缓存命中为何忽高忽低

服务端缓存本就是尽力而为，但先把自家的账管住了再谈服务端。LubanCode 请求前缀守恒有三条规矩：

- **追加律**：已经发给模型的 system、工具表与旧消息不追改，新材料只往尾部添。默认工具往返里，后一份请求就是前一份的原样追加版（`tests/unit/api/test_request_prefix.cpp` 钉死，断前缀会被点名 `model_changed` / `system_changed` / `tools_changed` / `old_message_changed`）。动态材料（项目记忆召回、子代理名册、步数收尾提醒）随本轮用户消息尾部进请求，不再每回合改 system。
- **cache epoch**：一场会话不是只有一份前缀。换模型/换 provider、改工具表（如 tool_search 挂载）、compact、有损硬裁都显式开新 epoch 并记下断因——`/context` 显示当前 epoch，回合统计的逐步流水账记每一笔断因，不无名无姓地断。
- **首次定形**：工具结果第一次进请求视图时定形（超长首次就是预览，重复只自述引用，新版本只自述替代），此后这个 epoch 内一个字节不改；有损硬裁后的视图钉住，裁剪窗口不随回合滑动。compact 是唯一常规全量重写点，压完开新 epoch。

真没命中时也有两种账：前缀不等（客户端的断因，`DiffRequests` 说得出断在哪）与前缀相等但服务端未命中（服务端尽力缓存的失手）。opt-in 真机 e2e（`tests/manual/deepseek_e2e.cpp`，设 `DEEPSEEK_API_KEY` 才跑）把这两种分开报。LubanCode 只展示远端报告，不伪造命中数；服务端没回报 usage 时显示“未回报”，不拿 0 冒充。
