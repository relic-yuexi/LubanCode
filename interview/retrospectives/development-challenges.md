# 开发难题与故障复盘

_面向技术面试与项目答辩：不讲空泛的“遇到不少困难”，只讲现场、证据、根因、修法、验收与未还清的账。_

---

[面试深挖导航](../deep-dives.md) · [高频追问题库](../question-bank.md) · [Agent Loop、重试与恢复](../../docs/architecture/agent-loop/reliability.md) · [测试指南](../../docs/development/testing.md)

## 📋 面试官究竟在听什么

面试官问“开发中遇到过什么问题”，不只想听一桩 bug。他在看六件事：

1. 你能不能把“偶尔不对”压成稳定现场。
2. 你会不会先留证，再改代码。
3. 你能不能分清症状、诱因与根因。
4. 你修的是一处报错，还是一道系统边界。
5. 你拿什么证明修好了。
6. 你肯不肯说清尚未解决的部分。

一段答话，照这六拍走：

> 现场是什么；第一眼怀疑谁；怎样排除；根因落在哪条边界；修了哪几层；最后用什么回归钉住。

别一上来背源码。先讲冲突。比如：“合法中文进来，截断以后反倒成了非法 UTF-8。”这句话一落，问题便活了。

```mermaid
flowchart LR
    A[用户现场] --> B[最小复现]
    B --> C[分层埋点]
    C --> D[排除嫌疑]
    D --> E[根因边界]
    E --> F[局部修复]
    F --> G[边界兜底]
    G --> H[回归测试]
    H --> I[文档与欠账]

    accTitle: 一桩故障从现场走到复盘的路径
    accDescr: 先把用户现场压成最小复现，再用埋点排除嫌疑，找到根因边界，补局部修复和边界兜底，最后用回归与文档收账。
```

## 🔍 十件真实难题总览

下表都能在提交、源码或测试里对上。前九件已修。排队消息已补落盘、恢复与失败
回队，durable ack 仍在待办；不能把“有快照”说成“交付原子”。

| 难题 | 表面症状 | 真正根因 | 状态 |
| --- | --- | --- | --- |
| PowerShell 报错掐死会话 | 一条命令写错，下一轮 JSON 序列化崩 | 解析期错误走 GBK，坏字节混进 UTF-8 历史 | 已修 |
| 截断与 SSE 劈字 | 中文、emoji 偶发乱码或 `type_error.316` | 字节边界不等于 Unicode 码点边界 | 已修 |
| 并发流式请求永挂 | 主回合冻住，`/exit` 也收不回来 | 连接已建却无响应头，连接超时与低速超时都管不到 | 已修 |
| 后台代理偶发 SIGSEGV | Windows 好好的，Linux 第一枪就崩 | lambda 引用了已经退栈的局部字符串 | 已修 |
| 子进程启动失败误报 | 命令不存在却只报退出码 127 | 子进程先关掉了回传 `errno` 的管道写端 | 已修 |
| Windows 测试删目录失败 | WSL 全绿，MSVC 收尾报错 | `SessionStore` 还攥着 JSONL 文件柄 | 已修 |
| 查看态退场花屏 | 后台任务结束后整屏空白，像是卡死 | console 与 app 各记一本擦屏账，先后擦了两遍 | 已修 |
| 后台拒权没有告知 | 子代理烧了许多 token，最后才说写入失败 | 后台不能弹确认，却把系统拒绝说成“用户拒绝” | 已修 |
| 插件重复挂载与 Lua 竞态 | `/plugins` 重复记账；后台同拍调用可能撞 state | 发现、资源所有权与 registry wrapper 混成一步 | 已修 |
| 排队消息交付裂口 | 消息能落盘、能失败回队，却仍可能卡在“已取走、未成账”之间 | 活队列快照与目标 history 不是一笔原子提交 | 部分修，ack 待补 |

### AI 归因补记

旧稿只按技术根因分组，没记代码怎样来到仓里。这笔账不全。十件难题中，后台代理
那枚悬垂引用出自明确带 Claude 联署的提交 `0d3d706`。那次 AI 辅助改动给后台
拒权补通知，却把 `last_denial_hook_reason` 留在装配块里，又让两枚稍后执行的
lambda 按引用攥住它。人工审查、MSVC 编译和当时测试都没拦住。Linux 第一回调用
便 `SIGSEGV`，ASAN 报 `stack-use-after-scope`。

Git trailer 只能证明 AI 参与了这枚提交，不能证明具体哪一行由模型敲下。故归因
写作“AI 辅助变更引入，人工验收漏检”。修复提交 `0d44eb8` 也有 Claude 联署：
把变量提到 `RunTask` 函数体，再用回归守住回调寿命。AI 能引入 bug，也能帮着修；
合不合入、验收到哪一步，责任仍在人。

## 💥 难题一：一条 PowerShell 错命令为何能掐死整场会话

### 现场

用户在 Windows PowerShell 5.1 里发了带 `&&` 的命令。Shell 尚未跑到设置 UTF-8 输出编码那一行，解析器先报错。报错文本走系统 ANSI 代码页。中文 Windows 常见 CP936，也就是 GBK。

工具把这串字节塞进 `Tool::Result`。当轮还能显示。下一轮重发历史，`nlohmann::json::dump()` 发现非法 UTF-8，抛出 `type_error.316`。一条命令语法错，竟把整场会话砖死。

### 为何难查

- 病发晚了一拍。坏字节在工具阶段混入，崩溃却在下一次请求序列化。
- 正常命令都走 UTF-8。只有“脚本解析期错误”绕过编码设置。
- 错误看着像 JSON 库有病，根却埋在进程输出边界。

### 修法

第一层在 `src/tools/run_command.cpp` 治本。进程输出拿到手，先过 `SanitizeUtf8`。

第二层在 `src/agent/loop.cpp` 兜底。任何工具结果进 history 前再洗一遍。MCP、Hook、插件和文件工具都过这道门。

共享算法落在 `src/platform/text_encoding.cpp`：

1. 合法 UTF-8 原样放行。
2. Windows 上先按 ACP 重解，尽量保住原中文。
3. 仍不合法，就把坏字节换成 U+FFFD。

### 验收

`tests/unit/tools/test_tools.cpp` 留下 GBK 样本、垃圾字节、残缺序列与真实 PowerShell 解析错误。测试不仅查“没有崩”，还把结果再塞进 JSON，确认 `dump()` 不抛。

### 面试时怎样收口

> 我没在 JSON 层吞异常。先在命令输出处治本，再在统一的工具结果入账口兜底。这样新增工具忘了清洗，也不能污染整场历史。

## ✂️ 难题二：合法 UTF-8 为何会被自己截坏

前一桩修完后，又冒出一枚 `type_error.316`。这回上游没有坏字节。坏字节是程序自己造的。

### 两条根因

`TrimHistory` 为了压住超大工具结果，曾直接按字节 `resize`。三字节汉字若在刀口上，只留前一两个字节，合法内容便被砍坏。

结构压缩也会截 artifact 的头尾预览。旧实现按字节 `substr`，同样可能从汉字腰间下刀。

### 又一条近亲：SSE 劈开 emoji

网络块边界也不认字符。中转服务可能把四字节 emoji 拆成两枚 delta。若每枚 delta 直接送 UI，显示层会先见半个字符。

### 修法

- `Utf8PrefixBoundary` 把前缀刀口往前退，退到完整码点之前。
- `Utf8SuffixBoundary` 把后缀刀口往后推，越过悬空续字节。
- `Utf8DeltaGate` 暂扣疑似残尾，等下一块拼齐再放。
- 流正常结束、报错或被 ESC 打断，都要 `Flush`。拼不齐的残尾换成 U+FFFD，不能永远扣着。
- 三家 wire client 在请求 JSON 序列化处还留最后兜底。旧会话已有坏历史时，清洗后照发，不能让会话永远打不开。

### 一个要紧取舍

history assembler 可以攒原始块。相邻两块合起来，字符自然复原。UI 回调不成。它每来一块就要画，故须加 gate。两条路不能硬套一份策略。

### 验收

`tests/unit/api/test_unicode_boundaries.cpp` 穷举前后缀刀口，还模拟劈半 emoji 的流。每一枚 UI delta 都须合法，最终 history 又须还原完整字符。`tests/unit/cli/test_utf8_boundary.cpp` 再钉文件与 JSON 边界。

## ⏳ 难题三：连接超时、空闲超时都有，为何请求还能永挂

### 现场

主会话空闲时，后台子代理长轮询与主回流请求同拍发出。主请求回来，子代理那枚却进了 `cpr::Post` 再不返回。主流程随后冻住，退出也会被后台线程的无界 `join` 扣住。

### 旧判断为何失效

连接超时只管“连接建不起来”。现场里连接可能已被本机代理或 TUN 接住。

低速超时要见到传输阶段。若响应头一个字节也不到，它未必落锤。

这便露出一条空隙：连接已成，响应未始。

### 修法

- Anthropic、Chat、Responses 三个流式 client 都加 `request_hard_timeout_secs`。
- 用 `steady_clock` 记绝对期限，在 libcurl progress callback 里查墙钟。
- 默认 300 秒；配成 0 才显式关掉。
- `ABORTED_BY_CALLBACK` 本身分不清用户取消与硬超时，故另记 `hard_timeout_hit` 旁证，再分错误文案。
- 后台 AgentTool 析构不再无界等线程。到期仍收不回，台账收成终态，线程放走，进程先能退。

### 为何不用一枚普通总超时

普通 `CURLOPT_TIMEOUT` 会把一条仍在正常吐 token 的长回答拦腰砍断。硬墙钟确实也有这层风险，故给足默认值，还允许用户关掉。它守的是“进程不能永挂”这条底线。

### 验收

`tests/integration/process/test_network_timeout.cpp` 起一只裸 socket。它收下连接，连响应头也不回。三家 client 各跑一遍，须由 2 秒硬墙钟收口。低速超时故意设到 25 秒，用来证明落锤的不是它。

## 🧵 难题四：一枚悬垂引用为何只在 Linux 炸

### 现场

后台代理装配权限回调时，两枚 lambda 都按引用捕获 `last_denial_hook_reason`。这串变量却声明在一层 `else` 块里。块一收口，对象已析构。稍后 `sub_loop.Run()` 真调回调，便是 stack-use-after-scope。

MSVC 的栈布局没立刻踩坏，看着无事。Linux 上第一回调用就 SIGSEGV。ASAN 把根因钉在捕获生命周期。

### 修法

不必上 `shared_ptr`，也不必把状态塞成成员。只把变量提到 `RunTask` 函数体层级，让它罩住回调装配与整个 `sub_loop.Run()`。

### 这一题真正考什么

- lambda 捕获方式不等于对象生命周期。
- “某平台没崩”不等于没有未定义行为。
- ASAN 给的是犯罪现场，仍要顺着作用域解释为何会悬空。

源码落在 `src/tools/agent_tool.cpp`。后台拒权回归在 `tests/unit/agent/test_agent_tool.cpp`。

## 🔌 难题五：子进程明明没启动，为何被报成退出码 127

### 协议本意

POSIX 上父进程 fork 后，要分清两件事：

- `execvp` 成功，程序后来退出 127。
- `execvp` 本身失败，程序压根没起来。

实现为此备了一根 `exec_pipe`。写端带 `FD_CLOEXEC`。exec 成功，内核自动关写端，父进程读到 EOF；exec 失败，子进程把 `errno` 写回去。

### 根因

`RunProcessWithStdin` 的子进程清理循环把 `exec_pipe[1]` 也提前关了。exec 失败后再写 `errno`，自然写不进去。父进程只见 EOF，误把“没启动”看成“启动后退出 127”。

### 修法与验收

把 `exec_pipe[1]` 从关闭列表摘出来。成功路交给 `CLOEXEC`；失败路先写 errno，再关。

这件事会波及 Hooks exec form、MCP、LSP 与 PTC。`tests/integration/hooks/test_hooks_dispatcher.cpp` 钉 `spawn_failed`，`tests/integration/ptc/test_ptc_runner.cpp` 钉不存在的解释器须报 Spawn。

### 面试时可讲的取舍

> 退出码属于“程序已经启动”后的协议。spawn failure 属于进程边界。二者若混账，上层就会给错排障建议。

## 🪟 难题六：WSL 全绿，Windows 为何删不掉测试目录

App-server 整回合测试会落一场 session JSONL。断言完便 `remove_all`。WSL 上开着文件也能 unlink，测试一路绿。Windows 不准删除仍被占用的文件，收尾当场失败。

根因不是“Windows 比较麻烦”。测试忘了按产品生命周期关线程、析构 `SessionStore`、释放追加句柄。

修法有两层：

1. 先走 `thread/stop`，让运行时正经放柄。
2. 清理改用 `error_code` 形态。测试清理失败不该盖住前头真正的断言结果。

回归在 `tests/unit/app_server/test_app_server_turn.cpp`。

这题能带出一条工程经验：跨平台测试不只查业务结果，还要查资源释放语义。POSIX 允许的事，Windows 未必认。

## 🖥️ 难题七：终端为何像卡死，其实只是两本账互相擦

### 现场

用户正查看一只后台代理。任务完成，界面自动回 main。随后整屏空白，按键似乎也没反应。

### 根因

console 层记着 `view_body_top`，退场时先擦一遍。app 层又记着 `view_frame_top_`、行数与 viewport，`PrintViewedTranscript` 进门再擦一遍。

流式重铺、滚屏、静默回流都会让绝对行号漂。第一把擦子刚铺平现场，第二把按旧账又扫一遭，连新画的 main 帧与输入框一并抹了。

### 修法

擦除只留一本权威账。app 层只负责拼并打印查看帧，旧帧擦除归终端层。退场路径收成一拍：任务终态、结果投递、坞行退休、查看目标清空、main 重铺。

### 验收

普通单测只验不了真终端。`tests/manual/agent_stream_driver.cpp` 真起控制台，盯一只后台任务从运行到退场，再查：

- main 复位行仍在。
- composer 恰有一份。
- 已完成任务不再留坞。
- 退场后键入立刻回显。
- `/exit` 能正常收口。

## 🔕 难题八：后台代理没有确认框，为何还说“用户拒绝”

后台任务没有可交互终端。工具若需要确认，又没有预先放行，只能拒绝。旧实现把缺省文案“用户拒绝执行”原样送给子代理。用户从未见过确认框，却在最终报告里背了拒绝的名头。

更糟的一回，子代理反复尝试写文件，烧掉许多 token，直到交卷才提写入全被拒。真正的问题有两层：权限策略本身合理，告知与模型反馈却失真。

修法也分两层：

1. 拒绝发生时，立刻写入 `permission_denial_notices_`。main 会话取走后弹 toast，并把事件记入 transcript。
2. 给子代理的工具结果明说“后台未预放行，并非用户拒绝；重试同一操作不会成功”。这样模型该停就停，不再空转。

`tests/unit/agent/test_agent_tool.cpp` 查三件事：工具没有真执行；通知账里有归属；结果不再出现“用户拒绝执行该工具”。真终端驱动又查 toast 是否当场出现。

这桩问题值得在面试里讲。安全策略若只会挡，不会说，用户就分不清“系统不许”“Hook 拒绝”与“自己点了拒绝”。可审计性本就是权限系统的一半。

## 🧩 难题九：插件为何既重复挂载，又会撞同一只 Lua state

插件要同时给主会话与普通子代理使用。旧装配把“扫描、加载、适配、注册、打印”揉在一只 `MountPlugins` 里。主表调一遍，子表再调一遍。同一 DLL 便被 `LoadLibraryW` 两次，`PluginHost` 记两份，子表又从两份 manifest 包出重名工具；`/plugins` 也跟着重复报数。

Lua 走的是另一条险路。每个 registry 确有独立 `LuaTool`，可多只后台子代理共享同一张 sub registry。它们若同拍调用同一 Lua 工具，便会并发推拉同一只 `lua_State`。一眼看去“每工具独立 state”，仍没罩住“同一工具被多线程共享”。

修法分开两本账：

1. DLL 发现以完整路径幂等。两张 registry 各包 wrapper，模块只载一次。
2. 只有主表装配时打印并写 `/plugins` 账；子表静默装能力。
3. 每只 `LuaTool` 加 per-state mutex。同一工具串行，不同工具仍可并行。

`tests/integration/plugins/test_plugins.cpp` 先重复扫描真 DLL，断言 `PluginHost` 仍只留一份；又用八条线程同打一只带状态的 Lua 计数工具，结果须恰好覆盖 1 到 8，不重不漏。

这题能带出一句要紧话：

> wrapper 可以复制，底下资源未必能复制；资源可以共享，调用协议未必线程安全。装配代码须把定义、实例、所有权与展示账分开。

## 📬 难题十：排队消息有了账，为何仍不算可靠交付

旧现场已经补过一轮。面试时要把“已补的护栏”与“尚缺的 ack”分开讲。

### 旧现场

早先 `SessionSteeringQueue` 只活在内存。回合收尾取走队头，拿它开下一轮。若那轮
请求失败，消息已经出队；进程一退，session 也没有 queue 事件可恢复。

### 眼下已经补了什么

- 每条消息有稳定 id、目标、状态与 attempt 计数。
- 队列变动会向 session JSONL 追加整表快照；`/resume` 可重建。
- 自动发送失败会 `ReturnToFront()`；达到上限后留给用户处置，不死循环。
- 清场与退场会报未送条数和首条预览；子代理消失会留 `TargetGone`，不暗投 main。

### 还欠哪一刀

`TakeFirstAutoSendable()` 仍会把条目从活队列摘走；外层稍后才把它变成目标 user
message。两步之间若倒下，最后一份 queue 快照可能已不含它，history 又尚未成账。
反过来，目标已经收下，源 queue 的清账快照若没写成，恢复后又可能重送。

下一版须把状态写成 `pending -> inflight -> acked`。`pending` 与 `inflight` 各先写稳
再回显或投递；`acked` 的边界是目标耐久收下，不等模型跑完。主会话可用一条同时
携带 `queue_id` 与 user message 的 `queue.accepted` 事件原子收账。子代理分两本账时，
先写目标 receipt，再写源 ack；恢复靠 `queue_id + delivery_id` 对账补齐。

### 为什么不该仓促“自动重试”

排队消息可能触发写文件、发请求或启动命令。宿主若不知道上一轮是否已产生副作用，盲重发会重复执行。恢复协议须先定义 durable state 与 ack 边界。

当前代码在 `src/cli/queue_model.hpp`、`src/app/interactive_session.cpp` 与
`src/sessions/session_store.cpp`；内存状态机、queue 事件回放与失败回队已有测试。三态
receipt、崩溃点注入与 OS 级 flush 尚待补齐。

## 🧪 怎样把一次修复变成工程能力

上头十件事，看着七零八落，背后却有几条同一的规矩。

| 规矩 | 例子 |
| --- | --- |
| 不可信数据在边界收口 | 工具输出、SSE delta、JSON dump 前都守 UTF-8 |
| timeout 要封住全部状态 | connect、idle 与 hard wall clock 各管一段 |
| 生命周期须画到回调真正执行时 | lambda 引用、后台线程、SessionStore 文件柄 |
| UI 只留一本布局账 | 同一片屏不能由两层各擦一次 |
| wrapper 与底层资源分账 | DLL 模块幂等加载，Lua state 单独护并发 |
| 状态变更要有 durable ack | 排队消息不能“取走即消费” |
| 回归须复刻故障形状 | 裸 socket 装死、emoji 劈半、真控制台退场 |

一枚修复，至少应留下四样东西：

1. 最小复现或 fixture。
2. 能解释根因的短注释。
3. 一条失败前会红、修后会绿的测试。
4. 面向用户的错误文案或排障入口。

## 🎓 高频追问答法

### “最难的问题是哪一个？”

可答 UTF-8 系列。它横跨 shell 编码、工具结果、history、compact、SSE、JSON 与终端显示。难处不在写清洗函数，在于找全每一道会重新造坏字节的边界。

### “你怎么证明不是拍脑袋修的？”

报证据链：真实现场 → 最小复现 → 分层埋点或 sanitizer → 根因提交 → 针对故障形状的测试。别只说“跑了全量测试”。

### “有没有走过弯路？”

Unicode 现场先怀疑模型流与控制台，最后有一部分根因落在 `filesystem::path::string()` 的 ACP 窄口。先列嫌疑，再逐条排除，这不算丢脸。没证据就改，才算乱撞。

### “为什么修了两层甚至三层？”

源头修复保真，统一边界兜底保命，序列化前最后防线救旧数据。三层职责不同。若每层都吞同一种异常，又没有边界解释，那才是补丁堆叠。

### “还有没解决的问题吗？”

有。排队消息尚无耐久 ack。直接承认，再讲现状、风险、候选协议与验收。切莫把待办说成路线图已经落地。

## 🔗 证据索引

| 主题 | 源码 | 测试 |
| --- | --- | --- |
| UTF-8 清洗与边界 | `src/platform/text_encoding.cpp`、`src/agent/loop.cpp` | `tests/unit/tools/test_tools.cpp`、`tests/unit/api/test_unicode_boundaries.cpp`、`tests/unit/cli/test_utf8_boundary.cpp` |
| 三协议硬墙钟 | `src/api/anthropic/client.cpp`、`src/api/chat/client.cpp`、`src/api/responses/client.cpp` | `tests/integration/process/test_network_timeout.cpp`、`tests/unit/config/test_config.cpp` |
| 后台生命周期与拒权 | `src/tools/agent_tool.cpp` | `tests/unit/agent/test_agent_tool.cpp`、`tests/manual/agent_stream_driver.cpp` |
| POSIX 子进程 | `src/platform/process_posix.cpp` | `tests/integration/hooks/test_hooks_dispatcher.cpp`、`tests/integration/ptc/test_ptc_runner.cpp` |
| App-server 文件柄 | `src/app_server/server.cpp` | `tests/unit/app_server/test_app_server_turn.cpp` |
| 进程内插件 | `src/tools/plugin_loader.cpp`、`src/tools/lua_tool.cpp`、`src/app/tool_runtime.cpp` | `tests/integration/plugins/test_plugins.cpp` |
| 排队消息 | `src/cli/queue_model.hpp`、`src/app/interactive_session.cpp` | `tests/unit/agent/test_queue_model.cpp`、`tests/unit/agent/test_queue_keys.cpp` |

相关提交可从 `1953c3f`、`160f78a`、`c4a3dc6`、`f8f67c5`、`0d44eb8`、`4d2ef82`、`c850ec2` 往下追。它们不是装饰用编号。每一笔都能还原当时怎样定位、怎样改、怎样验。
