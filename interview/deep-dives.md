# 面试深挖导航

[文档首页](../docs/README.md) · [高频技术面试追问题库](question-bank.md) · [开发难题与故障复盘](retrospectives/development-challenges.md) · [求职项目手册](portfolio.md) · [架构说明](../docs/architecture/README.md) · [Query 数据流](../docs/architecture/query-data-flow.md)

这页不替产品文档重抄字段。它替面试答辩排路：先说结论，再画数据流，随后交代取舍、失败路、测试证据与现存欠账。面试官从哪一层往下钻，都能顺着链接摸到源码。

## 先把项目说准

LubanCode 是一只 C++23 终端 coding agent。它把三家模型协议翻进一套中立消息，把模型给出的工具调用交给本机控制器，再把结果成对送回模型。会话、上下文、项目记忆、MCP、Skill 与 Hook 都围着这条主循环生长。

一句话架构：

```text
终端输入
  -> prompt / 临时上下文装配
  -> 中立 Request
  -> Chat / Responses / Anthropic adapter
  -> 流式中立事件
  -> assistant 消息
  -> 工具执行与结果回填
  -> 下一次模型请求，或本轮收束
```

答题时先守住四个词：

| 词 | 准话 |
| --- | --- |
| `turn` | 一条外层用户任务，从入队到最终收束。 |
| `step` | turn 内一次模型请求与一条 assistant 回包。 |
| `tool call` | assistant 回包里一枚带 id、名字与入参的调用。 |
| `session` | JSONL 事件账，不等于模型眼前的 history。 |

## 十二条深挖路线

| 面试官从哪里问 | 先答什么 | 深读 |
| --- | --- | --- |
| Agent Loop 怎么转 | turn 里循环 step；无工具则收口，有工具则顺序执行、成对回填 | [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)、[Query 数据流](../docs/architecture/query-data-flow.md) |
| 失败会不会重试 | 主模型 transport 不自动重试；续跑、工具纠错、session 恢复各走各路 | [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md) |
| 模型与 Schema 怎么管 | 端点判可用、目录补元数据、用户配置压默认；三种 Schema 分账 | [模型、Provider 与 JSON Schema 深挖](../docs/architecture/providers/schema.md)、[Provider 目录](../docs/features/providers/catalog.md) |
| 开发中遇到什么问题 | 先讲现场与证据，再讲根因边界、分层修法、回归与未结欠账 | [开发难题与故障复盘](retrospectives/development-challenges.md) |
| 上下文怎么管 | 四本账分开；长内容先在源头限流，再逐级压缩 | [上下文、长文本与记忆深挖](../docs/architecture/memory/context.md)、[Context 压缩算法深挖](../docs/architecture/context/compaction.md) |
| 大文件怎么读 | 搜索先定位，`offset/limit` 分页，1 MiB 单次封顶；不是一口吞 | [文件读取与命令执行深挖](../docs/architecture/tools/file-commands.md) |
| 命令怎么跑 | shell 语义、确认、超时、输出上限、杀进程树、后台台账各管一层 | [文件读取与命令执行深挖](../docs/architecture/tools/file-commands.md) |
| 单工具、多工具怎么处理 | wire 各异，中立块相同；多枚现按出现次序执行，结果同批回填 | [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[工具调用流程](../docs/architecture/tool-calling-flow.md) |
| MCP 怎么接 | 长命 stdio 子进程，一行一帧 JSON-RPC；握手、发现、包装、确认、调用 | [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md) |
| 插件有什么用 | 可信本地函数不必硬搭服务；Lua 走脚本，C ABI 接原生库，二者都同进程 | [进程内插件系统深挖](../docs/architecture/extensions/plugin-runtime.md)、[扩展指南](../docs/features/extensions/README.md) |
| Skill 怎么触发 | 启动只注名称与摘要，命中任务后才按名加载正文；三级覆盖 | [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[扩展指南](../docs/features/extensions/README.md) |
| Hook 怎么守边界 | 事件发射、匹配、信任、并发执行、固定归并；事后 Hook 不能回滚副作用 | [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[Hooks 流程](../docs/architecture/hooks-flow.md) |

## 答一题的五步法

### 1. 先定边界

不要一上来报类名。先说这层管什么，不管什么。

比如上下文：

> history 管模型眼前的连续推理；session 留完整流水；memory 留跨会话事实；artifact 留可追回的大结果。压缩只动其中一部分，不能混叫。

比如 MCP：

> MCP 只把外部工具接进本地注册表。工具是否安全，不能由 MCP 名字担保，还要走确认、Hook 与结果清洗。

### 2. 再走一遍成功路

用一件具体任务说。比如“读 `README.md` 后跑测试”：

```text
用户消息
-> step 1：模型调用 read_file
-> 本机读文件，回 ToolResult
-> step 2：模型调用 run_command
-> 确认、起进程、捕获输出，回 ToolResult
-> step 3：模型整理结论
```

这比“系统支持 tool use”有用。每一步都有数据形状，也有责任边界。

### 3. 把失败路摆出来

至少挑三类：

- 输入坏：JSON 解析失败、字段类型错、路径越界。
- 外部坏：进程起不来、MCP 死掉、Hook 超时、文件不是 UTF-8。
- 状态坏：工具调用与结果失配、压缩摘要漏待办、后台任务退出码拿不到。

说清“失败后哪本账仍可信”。例如 compact 失败，history 不动；命令超时，已捕获输出仍回填；MCP 响应迟到，pending id 已删，迟到包丢弃。

### 4. 讲取舍，不背算法名

可用这几组：

| 取舍 | 当前选择 | 代价 |
| --- | --- | --- |
| 多工具执行 | 顺序执行 | 换来确认、Hook 与副作用顺序可审计；真实等待占比尚未量 |
| 协议中立层 | 消息流 + 宿主执行工具 | 能接三家现有 wire；承不住 provider 自持执行状态机 |
| 记忆检索 | BM25 + 路径/符号硬命中 | 没有语义向量召回；本地、可解释、易失效 |
| session | JSONL 追加 | 查询不如数据库灵活；崩溃后容易保住完整前缀 |
| MCP 分帧 | newline-delimited JSON-RPC | 简单；服务器 stdout 不能混日志 |
| Skill 装载 | 索引常驻、正文按需 | 降低常驻上下文；首次使用多一枚工具调用 |
| 长工具结果 | artifact + 预览 | 多一层存储与追回逻辑；不必把整块塞进窗口 |

### 5. 最后给证据

证据按这次序报：

1. 核心源码入口。
2. 一条成功测试。
3. 一条失败或边界测试。
4. 现存欠账。

只报“有很多测试”没有分量。要说“`tests/unit/agent/test_loop.cpp` 钉住 ESC 后当前工具收尾、后续工具补成对结果；`tests/unit/tools/test_tools.cpp` 钉住 2 MiB 输出上限与杀树”。

## 高频追问速答

横向题目先翻[高频技术面试追问题库](question-bank.md)。下面只留最常撞上的几问。

### 模型目录是不是照搬 OpenCode

不是。OpenCode/Models.dev 可作元数据分层与覆盖链的同类参照；LubanCode 没消费 Models.dev，也不兼容 OpenCode config schema。仓内直接可证的来源是 Codex model-catalog，provider 目录与 C++ parser 按本项目三 wire 另写。

### 为什么不把全部历史都发给模型

窗口、费用与缓存都会吃不消。更要紧的是，大段重复日志会淹没任务状态。LubanCode 先在工具处限输出，再把请求视图里的重复只读结果折成引用；冷结果可进 artifact 与微摘要；历史到线才做全局 compact。每层都有原文去处，不是一刀删掉。

### 请求失败会不会自动重试

当前不会。三家 client 每枚 step 只发一次 HTTP/SSE attempt。已经流出正文、tool use 或跑过副作用工具后，盲重试会重复输出、打乱 id，甚至把命令跑两遍。系统保住请求前已成账的 history 与 session，再明报失败。普通 transport 断流时，屏上 partial text 不进 history；ESC 才会专门收起半截消息并补打断账。`max_tokens` 后追加宿主标记另开 step，算 continuation，不算 retry。

### 中立层能承住全新交互范式吗

不能说满。当前抽象押的是线性 `StreamEvent` 最终收成一条 `Message`，工具又由宿主执行。若新协议只把声明式状态机当输出，宿主仍掌握校验、权限和调度，可以扩 `ProgramBlock` 与执行器。若 provider 自己推进状态、调度副作用，`MessageAssembler` 会先失效，`AgentLoop` 随后失效。那该另开执行协议，不能把状态转移硬扮成一串 tool call。

### compact 时 system prompt 会怎样

主会话 `system_prompt_` 不参与摘要，也不被改写。compact 另发一枚请求，吃专门的总结 system；验收成功后只换 history。项目指令、cwd 或模型显式变化时，应用可重建主 system，那是环境变更，不是 compact。

### 很长的单条用户输入怎么办

这正是难点。episode 分层不能把一轮从中劈开，最新用户轮又受热区保护。若单轮自身已超过硬限，程序明确拒绝，叫用户拆输入、先落文件再搜索，或另开会话。不能假装 compact 能救任何巨型输入。

### 多个工具为何不并发

有些只读调用可以并发，现版却统一顺序跑。确认框、Hook、终端转录与副作用次序因此只有一条账。代码已有 `RunOneTool` 整链耗时与整轮墙钟字段；前者还夹着 Hook 和确认，不是纯工具 I/O。真实任务里“工具等待占多少”尚未汇总，故不能报成本比例。若将来并发，至少要先做依赖分析、只读能力标注、并发确认策略、结果稳定排序与取消传播；不能见到多个 JSON 块就直接开线程。

### 单二进制优势兑现了吗

2026-08-30 本机 `0.26.127` Windows Release 主程序为 `11.04 MiB`。`--version` 51 次点测，中位 `211.03 ms`，P95 `853.49 ms`。这只量短命令起进程、输出、退出，不等于交互 ready，也不是受控冷启动。完整发行包还带 skills、文档与安装脚本；“主程序单二进制”和“发行包单文件”不能混叫。

### BM25 参数调过吗

`k1=1.5`、`b=0.75`，取常用档，没有扫参。这里的一篇“文档”其实是一张结构化 `MemoryEntry`：标题、摘要、scope、关键词、路径和 evidence 符号；不是整份源码。现有固定问句量了最终召回，却没报 `doc.len` 分布，也没隔离硬命中对 BM25 参数的遮蔽。故不能说“按代码语料调过”。

### LaTeX 盒子怎样向上冒高度

盒子只存 `rows/width/baseline`；ascent 是 baseline，descent 是余下行数。分式线就是新盒 baseline，`HBox` 按各盒最大 ascent/descent 平移，所以左右文本对齐分数线。嵌套分式先产完整子盒，再把总行数与 baseline 交给父层。括号没有字号档：一行用普通字符，多行用 Unicode 顶、中、底件按实际行数拼。

### compact reduce 到哪一层停

冷区按外层用户输入与 `todo_write` 分 episode，超预算再按整轮切，块数随 token 预算变化。reduce 相邻两两并，最多四轮。四轮后若仍超窗，现版没有本地拒绝，仍发终稿请求；服务端拒绝后旧 history 不动。这是当前终止收口的欠账。

### cancel flag 用什么内存序

轮核与多信号合并口用 release store / acquire load；若干下游裸 `load()` 是默认 `seq_cst`。都安全。单纯停止旗用 relaxed 也够，acquire/release 只有在 true 同时发布别的非原子状态时才必要。`seq_cst` 也不能让阻塞 syscall 自动醒来，更不能替多字段状态机做事务。

### ACP 转码成功等于文字正确吗

不等于。当前先验 UTF-8；非法才试 ACP，转完再验 UTF-8 合法。那只验结构，不验语义。`C2 A1` 在 CP936 是“隆”，在 UTF-8 是“¡”；它本身合法 UTF-8，现版会直接取“¡”。可靠办法是让进程、Hook、HTTP 或文件边界明示编码；来源未知时留标签与原始摘要，不靠猜。

### 排队消息怎样画 durable ack

三态足够起笔：`pending -> inflight -> acked`。入队先写稳 pending，再回显；dispatcher
先写稳带 lease 的 inflight，再投递；目标把 `queue_id` 与 user message 同笔落账，才算
acked。ack 只认目标耐久收下，不等模型答完。目标 receipt 已写、源 ack 未写时，恢复
按 `queue_id + delivery_id` 对账补齐；次序倒过来会丢消息。现版已有 queue 快照、resume
与失败回队，尚无这笔原子收据。

### 不可信 Lua / DLL 怎样隔离

核心不再 `luaL_newstate` 或 `LoadLibrary`。一件插件占一只受限
`lubancode-plugin-host`，用带长度、版本、request id 与字节帽的 IPC 收发。文件权限取
`manifest 声明 ∩ 工具策略 ∩ 用户批准 ∩ worktree`，发短命 grant；插件只向 broker
报 grant 与相对路径，由核心规范路径、亲手开文件、分块回字节。OS 还须默认封住
工作区、网络、环境与继承句柄。少了 OS 拒权，capability RPC 只是一纸约定。

### 不用真终端怎样钉住 Ctrl+O 补画

纯测 `FormatTranscriptItems` 不够。要把生产 Ctrl+O 分支抽成无平台 action，注入
`SnapshotSource`、`RenderSink` 与固定 width。先在紧凑档完成一枚历史子工具，再调同一
action；断言它取一次快照，按“擦 footer、废锚点、写详细帧、重画 footer”落笔，且
旧参数与结果各出现一次。旧实现若只打印“详细模式”，这条测试当场见红。ConHost
残影另归真终端驱动。

### 若重来先砍什么

先砍块级 LaTeX。它好玩，也好演示；可 coding agent 先要读准、改对、验完。首版让
公式退回普通文本，把 parser、盒式布局与终端对齐的工时还给 session 恢复、durable
queue 和默认 CI。三协议、compact 仍咬着“不绑一家”与长会话主线；LaTeX 不在主线上。

### 为什么不直接改 OpenCode / Codex

只求生产力，直接用现成项目更划算。自建的目标还包括 C++ runtime、原生终端、跨平台
进程与 C ABI 练习；这是学习项目的 build 理由，不是商业 TCO 已胜。Git 只证明早期
提交借鉴 Codex model-catalog；OpenCode 是后来的同类对照。更难看的账也要认：仓根
没有 `LICENSE`，README 却写 Apache-2.0。来源注释有了，许可证闭环没有。

### 最大遗憾是什么

太早把项目当作品集，太晚把它当日用工具。功能越铺越多，连续 dogfooding、coverage、
默认终端 CI、durable ack 与许可证却落在后头。它们不是五项偶然欠账，是同一股偏心：
爱做看得见的新增，轻看枯燥收口。若重来，核心闭环站住便冻功能，先拿真实任务和外部
用户排路线图。

### JSON Schema 是不是安全边界

不是。schema 只描述输入形状。路径范围、命令危险度、权限、Hook、OS 账户能力另有边界。现版还有一项明确欠账：Hook 给出的 `updatedInput` 会过统一 schema 复检；模型原始入参主要靠 provider 结构化调用与各工具自己的参数检查，尚未在 `RunOneTool` 入口统一验 schema。

### MCP 工具和内置工具有何不同

进入 `ToolRegistry` 以后，上层看见的接口一样：名字、说明、schema、确认属性、`execute`。不同之处藏在执行背后：内置工具直接调 C++；MCP 工具把入参转成 `tools/call` JSON-RPC，经长命 stdio 子进程拿结果。MCP 默认确认，服务端返回的非文本 content 现版只给“不支持”占位。

### Skill 和 Hook 有何不同

Skill 给模型一份做事说明，靠模型理解后调用普通工具。Hook 是宿主在生命周期边界直接起外部命令，可阻断、改参或添上下文。前者影响“模型想怎么做”，后者约束“宿主准不准做”。两者不能互相冒充。

### 记忆何时触发

读：每条外层用户消息到来时，若 `enabled/use` 开着，按当前问题重算一次；同一 turn 内各 step 沿用这份召回包。写：用户显式记、模型调 `memory_save`，或 turn 收尾抽取。收尾抽取又受 `learn=off/review/auto` 分流。

### Hook 多只同时表态怎么办

命中的同步 handler 并发跑，收齐后按定义次序归并。权限固定是 `deny > ask > allow > 无表态`；附加上下文全收；改参取最终允许时的最后一份。不能拿“谁先返回”决定权限。

## 不该说满的话

- 不说“支持任意编码文件”。`read_file` 只收 UTF-8，可剥 BOM；NUL 与非法 UTF-8 会拒绝。
- 不说“多工具并行”。主工具循环现版顺序执行。
- 不说“完整支持 MCP 一切 content 类型”。现版只拼 text，其他类型给占位。
- 不说“向量记忆”。现版是本地 catalog、BM25 与硬命中。
- 不说“Hook async 已落地”。现版只解析、展示并记 `skipped_async`。
- 不说“所有工具参数统一过 JSON Schema”。原始入参尚未走宿主统一校验。
- 不说“后台命令跨平台完全同语义”。POSIX 探活后拿不到准确退出码，异常退出时收尾也弱于 Windows Job Object。
- 不说“模型请求会自动重试”。当前网络错、`429`、`5xx` 与流错误都直接返回；continuation 另算。
- 不说“每一级 compact 都同样强验收”。严格 manifest 与待办守恒在终稿；map 与中间 pair merge 较松。
- 不说“compact 会无限递归直到装下”。reduce 最多两两归并四轮，四轮后的本地超窗拒绝尚未补。
- 不说“BM25 参数按代码语料调过”。现版取常用值，固定尺子没有做 `k1/b` 消融。
- 不说“ACP 转成合法 UTF-8 就证明解码正确”。合法只是一道字节结构检查。
- 不说“排队消息已经 exactly-once”。现版有快照与失败回队，没有 durable receipt。
- 不说“Lua pure 画像或调用前确认等于沙箱”。Lua 与 DLL 仍在宿主进程。
- 不说“已经接入 OpenCode 或 Models.dev”。当前目录是 LubanCode 自有格式，仓内直接记载借鉴 Codex model-catalog。
- 不说“许可证已经处理妥当”。README 的 Apache-2.0 链接眼下指向不存在的 `LICENSE`。
- 不说“模型 capability 都会自动拦截不兼容请求”。若干字段当前只解析、展示或留作后续接线。

## 源码证据总索引

| 题目 | 入口 | 关键测试 |
| --- | --- | --- |
| 主循环与多工具 | `src/agent/loop.cpp` | `tests/unit/agent/test_loop.cpp` |
| 重试、取消与会话恢复 | `src/app/turn_runner.cpp`、`src/agent/session_store.cpp` | `tests/unit/agent/test_loop.cpp`、`test_session_store.cpp` |
| 模型目录与 Provider schema | `src/config/model_catalog.cpp`、`provider_catalog.cpp` | `tests/unit/config/test_model_catalog.cpp`、`test_provider_catalog.cpp` |
| 中立消息与三协议 | `src/api/types.hpp`、`src/api/*/request.cpp` | `tests/unit/api/test_chat_request.cpp`、`test_responses_request.cpp`、`test_anthropic_request.cpp` |
| 大文件读取 | `src/tools/read_file.cpp` | `tests/unit/tools/test_tools.cpp`、`test_utf8_boundary.cpp` |
| 命令与进程树 | `src/tools/run_command.cpp`、`src/platform/process_*.cpp` | `tests/unit/tools/test_tools.cpp`、`test_background_tasks.cpp` |
| context / compact | `src/agent/context*.cpp`、`compact.cpp`、`microcompact.cpp` | `tests/test_context*.cpp`、`test_compact.cpp`、`test_microcompact.cpp` |
| memory | `src/memory/project_memory.cpp`、`src/app/memory_extract.cpp` | `tests/unit/memory/test_project_memory.cpp`、`test_memory_retrieval.cpp` |
| MCP | `src/mcp/client.cpp`、`transport.cpp`、`mcp_tool.cpp` | `tests/integration/protocols/test_mcp_client.cpp`、`test_mcp_tool.cpp` |
| 排队与 durable ack | `src/cli/queue_model.cpp`、`src/app/interactive_session.cpp`、`src/sessions/session_store.cpp` | `tests/unit/agent/test_queue_model.cpp`、`tests/unit/sessions/test_session_store.cpp` |
| Ctrl+O 转录重铺 | `src/cli/console_input.cpp`、`src/cli/tool_display.hpp`、`src/cli/transcript.cpp` | `tests/unit/cli/test_transcript.cpp`；toggle action 单测待补 |
| Lua / DLL 插件 | `src/runtime/plugin_lua_host.cpp`、`src/tools/lua_tool.cpp`、`src/tools/plugin_loader.cpp` | `tests/integration/plugins/test_plugin_lua.cpp`、`test_plugins.cpp` |
| Skill | `src/tools/skill_loader.cpp`、`skill_tool.cpp` | `tests/unit/config/test_skills.cpp` |
| Hook | `src/hooks/dispatcher.cpp`、`protocol.cpp` | `tests/unit/hooks/test_hooks.cpp` |

面试前再读一遍[求职项目手册](portfolio.md)。那页管项目介绍、故事与演示；本页管追问时怎样把技术账说透。
