# 高频技术面试追问题库

_面向 LubanCode 项目答辩：每题先给一口短答，再指出面试官真正想验什么与该翻哪处源码。_

---

[面试深挖导航](deep-dives.md) · [求职项目手册](portfolio.md) · [开发难题与故障复盘](retrospectives/development-challenges.md) · [模型、Provider 与 Schema 深挖](../docs/architecture/providers/schema.md) · [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)

## 📋 怎么用这份题库

别背长答案。每题按四拍答：

1. 先定边界：这层管什么，不管什么。
2. 走成功路：给一件具体任务与数据形状。
3. 报失败路：哪一步会坏，坏后哪本账可信。
4. 讲取舍：为何这样做，现存欠账是什么。

若面试官不追，二十秒收口。若他往下钻，再报源码、测试与 corner case。

> 📌 **答题规矩:** 不会的边界直说“当前没做”。一个诚实欠账，常比一套空泛“高可用设计”更有分量。

## A 组：逼你交数字

下列数字取自 2026-08-30 的仓库与本机 Release 测试。固定语料评测、真机故障、
日常使用账各算各的，不能揉成一团。

### 1. compact 前后，token 到底降了多少？

**先答：** 目前还没有一组能拿来证明收益的成功对照实验。我能交出的真机数字，
反倒是一条失败账：旧热区算法曾把约 `70.8k` 压成 `73.7k`，多出 `2.9k`，
约涨 `4.1%`。病根是 mid-turn 长工具循环全挤在最后一轮，旧算法把整轮原样留下，
又在前头添了一份存档。现版自动路径发现新史不短，便拒收，原 history 不动。

这条数字只证明“旧算法会反涨，新闸能挡住”。它不能证明压缩后任务不跑偏。
眼下没有同任务、同模型、同输入的 FULL/compact 多轮对照；也没有一份真实长会话
同时记下压缩请求 `input_tokens`、`output_tokens`、耗时、后续任务判分。故这四个数
都不能编。

“摘要漏待办就拒收”已有确定性回归。此次本机跑 `Compact:*` 与
`CompactHierarchical:*`，共 `22` 条用例、`105` 个断言，全过。可生产拒收率仍是
未知。成功的 `compact_v2` 事件会记 `pre_tokens`、`post_tokens`、分块数与 manifest；
失败只在当场报错，没有耐久的 attempt/reject 分母。没分母，便算不出拒收率。

**面试官若追“下一步怎么量”：** 给每次尝试落一条 `compact_attempt`，记估算前后
token、provider 输入输出 usage、耗时、接受或拒绝及理由。再冻结一组多轮任务，
每轮查活动待办、文件改动、测试结果与最终判分。至少重复五次，方能报中位数、
P95、拒收率与跑偏率。

证据：[`compaction.md`](../docs/features/context/compaction.md)、
[`session_commands.cpp`](../src/app/commands/session_commands.cpp)、
[`test_compact.cpp`](../tests/unit/agent/test_compact.cpp)。

### 2. BM25 召回质量怎么评？误召回长什么样？

**先答：** 现在不再是“一条数据也没有”。中文固定尺子有 `100` 条项目记忆、
`88` 条应命中问句、`39` 条不应命中问句。此次实跑结果如下：

| 指标 | 实测 |
| --- | ---: |
| Recall@1 | `100%` |
| Recall@3 | `100%` |
| Precision@3 | `88.35%` |
| 负例误命中率 | `2/39 = 5.13%` |
| 注入字节 P50 / P95 | `269 / 547` |

两条误命中很具体：问“依赖注入是什么设计模式？”，词法检索撞上
`fact.zh-dep-vcpkg`；问“git 怎么撤销上一次提交”，撞上
`preference.zh-small-pr`。前一条把“依赖”当成包依赖，后一条把 git 操作泛词
拉向提交偏好。另一本中英混排尺子有 `34` 条记忆、`43` 条正问、`33` 条负问；
Recall@1/@3 都是 `100%`，Precision@3 是 `97.73%`，负问零误召回。它仍有一条
过注入：“MergeConfig 配置合并的顺序”除正确项外，又捎上 hooks 配置。

边界也要当场说：这些是固定 fixture，不是线上自然流量。它能防检索器改坏，
不能替代真实使用日志。是否加语义检索，要看自然问句里的语义漏召回、跨表达失败
与误注入成本；眼下这份数据只够说明词法基线有多准，尚不够证明向量检索没用。

证据：[`test_memory_retrieval_zh.cpp`](../tests/unit/memory/test_memory_retrieval_zh.cpp)、
[`corpus_zh.json`](../tests/fixtures/memory_retrieval/corpus_zh.json)、
[`test_memory_retrieval.cpp`](../tests/unit/memory/test_memory_retrieval.cpp)。

### 3. CI 覆盖率多少？真终端回归靠什么兜底？

**先答：** 行覆盖率、分支覆盖率都没采，故百分比未知。不能拿“测试很多”冒充
coverage。当前 CMake 注册 `268` 个 CTest 项；主 doctest 程序列出 `3692` 条用例。
GitHub Actions 跑 Windows、Ubuntu、macOS 三条腿。2026-08-30 本机现有 Release
构建实跑 `267/268` 通过。唯一红项是 `browser.mcp.selftest` 的崩溃收口分支：
它内部 `195 PASS / 1 FAIL / 2 SKIP`，旧 page id 回了 `browser.internal_error`，
没按预期报 page 已失效。故这一版不能答“全绿”。

真终端另有 `15` 只 `*_driver.cpp`。它们会开真实 Windows 控制台、注键、刮屏，
查 composer、footer、面板、滚屏与残影。它们全是 `EXCLUDE_FROM_ALL`，默认 CTest
不编也不跑。眼下兜底分三层：纯状态机与渲染边界进默认 CTest；
`agent_stream_driver`、`viewport_driver`、`history_search_driver` 等用进程内假
provider 穿过真 `lubancode.exe`；改终端路径时，再点名构建并手跑对应驱动，
报告落盘。

这套替代手段能挡住不少回归，却不是完整 CI 闸。开发者若忘了手跑，ConHost
特有故障仍可能漏网。真正还账的办法，是挑无网络、无私钥、时序确定的 fake
backend 驱动纳入 Windows CI；依赖真模型的探针继续 opt-in。

证据：[`testing.md`](../docs/development/testing.md)、
[`tests/CMakeLists.txt`](../tests/CMakeLists.txt)、
[`ci.yml`](../.github/workflows/ci.yml)。

### 4. 这工具每天用吗？它真坑过你吗？

**先答：** 我不能拿现有材料证明“每天都用”。能证明的是 2026-08-29 有一场真实
项目 dogfooding：用户只发了 `3` 条 query，工具往返却带出 `31` 次 normal 调用，
累计输入约 `728.3k` token，provider 报缓存命中 `208.4k`，约 `29%`。这场使用
一口气记下 `10` 个真实问题，不是拿测试夹具扮日常使用。

最扎手的一回，是首轮主答已经出来，程序才同步起标题请求。用户眼看答案落完，
提示符却又卡了约 `6.3` 秒。那枚标题只吃 `492` 输入 token、吐约 `10` token；
偏生 `cheap` 没配独立小模型，回落到与 normal 同一只 `gpt-5.6-sol`。token 不算
大，关键路径白等六秒多，体验照样坏。后来把它拆成两层：首问建档时先起本地
临时标题，模型精炼异步跑，不再堵住首轮收尾。

这才算 dogfooding 故事：先报现场与代价，再说根因、修法和回归。若面试官追问
连续使用天数、日均会话数、失败率，现有账答不上来，就直说没量。

证据：`LubanCode 真实实测问题记录`（清仓时删档，查 git 历史）、
[`session_title_refiner.cpp`](../src/app/session_title_refiner.cpp)、
[`interactive_session.cpp`](../src/app/interactive_session.cpp)。

## B 组：AI 辅助编码

这组题先认账，再讲控制。别把“个人项目”“独立负责”偷换成“每行代码都由我亲手
敲出”。

### 1. 多少代码是手写的，多少是 AI 生成的？

**先答：** 我给不出可信的行级比例。仓库没给每一行保存“人写、AI 起草、人改写”
这类来源，Git author 也不能回答。当前 `HEAD=90fb612`，主线共有 `1087` 个提交，
都落在同一名人类作者名下；其中 `819` 个提交明确带 Claude 或 Codex 的
`Co-Authored-By`，占 `75.3%`。去掉 merge 后有 `891` 个提交，其中 `739` 个带
AI 联署，占 `82.9%`。

这组数只能证明 AI 深度参与，不能换算成“82.9% 代码由 AI 写”。一枚 AI 联署
提交里可能有人写的设计、AI 起的样板和双方来回改过的测试；没写 trailer 的提交
也未必没有 AI。故我不会报手写行数百分比，更不会说“只有样板用了 AI”。

我把它称作个人项目，意思是没有第二名人类开发者同我分担产品责任。我负责定目标、
拆边界、挑方案、审 diff、跑验收、决定是否合入，并为发布结果担责。AI 参与方案
推演、实现、重构、测试和文档，范围很深。简历里原先的“独立开发”容易叫人误会，
更稳的写法是：

> 个人项目｜主导设计与交付｜AI 辅助开发

面试官若问某一模块是谁写，不凭印象抢功。翻对应提交、设计记录与测试，说清我给了
哪些约束，AI 起了哪些草稿，我又改掉什么、补了什么验证。

### 2. AI 生成的代码怎样验收？出过事吗？

**先答：** 出过事。后台拒权通知那次改动在 `0d3d706` 引入两枚 lambda。它们按
引用捕获 `last_denial_hook_reason`，变量却活在一层 `else` 块里。装配块先退栈，
`sub_loop.Run()` 后调回调，便踩成 `stack-use-after-scope`。MSVC 没立刻炸，Linux
第一回调用就 `SIGSEGV`。引入提交明确带 Claude 联署；Git 不能证明那一行究竟由
谁敲下，故准确说法是“AI 辅助变更引入，人工验收也漏掉”，不能把锅全甩给模型。
修复提交是 `0d44eb8`，同样有 Claude 参与。ASAN 钉出现场后，把变量提到
`RunTask` 函数体，罩住两枚回调整段寿命，再补后台拒权回归。

我如今把 AI patch 当外部贡献验，分五道门：

1. 先对需求、不变量与信任边界；答非所问，测试再多也不收。
2. 逐段看 diff，尤其查所有权、引用寿命、线程、错误路、取消与平台分支。
3. 跑最窄的修前红、修后绿测试；fixture 要复刻故障形状。
4. 再跑 Release CTest 与三平台 CI；终端、进程、网络另上真控制台、裸 socket、
   假 provider 或 opt-in 真服务。
5. 留提交、测试和故障复盘。AI 参与不免责任，发现漏检便反过来抬高验收门槛。

这五道门也不保证零事故。那枚悬垂引用说明：AI 会写出看着顺眼、编译也过、只在
另一平台发作的代码；人工若只看 happy path，一样会放它过关。

证据：提交 `0d3d706`、`0d44eb8`，以及
[`development-challenges.md`](retrospectives/development-challenges.md)、
[`agent_tool.cpp`](../src/tools/agent_tool.cpp)、
[`test_agent_tool.cpp`](../tests/unit/agent/test_agent_tool.cpp)。

## C 组：把设计推到失效

这组题不考背类名。先画出抽象押了什么，再说哪一步先裂。没有量过的成本，
当场认账。

### 1. 模型若直接流式输出可执行状态机，中立层还守得住吗？

**先答：** 现版中立层守得住的，不是“一切模型协议”，而是**消息式交互、宿主
掌握执行权**这一族协议。`Backend::send_stream` 吐出线性 `StreamEvent`，
`MessageAssembler` 把事件攒成一条 assistant `Message`；`AgentLoop` 再从中找
`ToolUseBlock`，逐枚交给本机工具。三家 wire 虽不同，都还服从这副骨架。

若新状态机只是一份声明式程序，模型负责描述，宿主仍能校验节点、掌握权限、调度
工具、记录副作用，那便可扩中立层：新增 `ProgramBlock` 与 `ExecutionEvent`，再接一只
宿主执行器。旧消息路不用推倒。

若 provider 自己推进状态、并发调度、暂停恢复，甚至绕过宿主直接做外部副作用，
第一处会裂在 `StreamEvent -> MessageAssembler`。它只会把线性内容流收成一条消息，
表达不了状态转移、检查点和分支。下一处才是 `AgentLoop`：它押的是“模型回包 ->
有限 tool use -> 宿主执行 -> tool result 回填 -> 下一 step”。硬把状态机拆成伪
`ToolUseBlock`，会丢转移身份、依赖、回滚与并发语义，审计账也会说谎。

那时该另开执行协议，例如 `ModelOutput = Message | ExecutionPlan`，把内容事件与执行
事件分账；权限和副作用仍由宿主把关。若 provider 不肯交回执行权，我不会宣称
“加一只 adapter 就支持”，而会把它列成另一套 runtime。中立层的承载边界，就在
这里。

证据：[`backend.hpp`](../src/api/backend.hpp)、[`types.hpp`](../src/api/types.hpp)、
[`assembler.hpp`](../src/api/assembler.hpp)、[`loop.cpp`](../src/agent/loop.cpp)。

### 2. 顺序执行实测过代价吗？工具等待占多少？

**先答：** 典型任务占比没测过。现版能在 trace 开启时为每枚 `RunOneTool` 链写
`duration_ms`，轮视图也记 `wall_duration_ms` 与 `approval_wait_ms`；可前一项从工具
入口起算，夹着 Hook、确认与真正 execute，并非纯工具 I/O。仓内也没有报表把同一轮
的模型等待、工具链、审批等待和宿主开销对齐。2026-08-29 那场真实 dogfooding
留了 `31` 次模型调用与 token 账，没有逐工具墙钟。故我不能报“工具占 30%”一类
数字。

顺序执行并非全凭感觉。文件改写、确认、Hook、ESC 分账和结果次序都要求先有一条
确定路径；在未证明调用彼此独立前，并发会改语义。可“这份正确性花了多少时间”
仍没量，性能取舍只做了一半。

下一轮测试会固定三类任务：只读检索、读后改写、混合长命令。每轮同时记模型首字节
与收尾、每枚工具起止、审批等待和整轮墙钟。主指标为：

```text
工具链占比 = sum(run_one_tool_duration_ms) / turn_wall_ms
去审批工具链占比 = (sum(run_one_tool_duration_ms) - approval_wait_ms)
                 / (turn_wall_ms - approval_wait_ms)
可并行上界 = sum(独立只读组耗时) - max(该组耗时)
```

还要在 `execute` 前后另打点，才报纯工具等待。各项再报中位数与 P95。只读且无依赖
的调用方可进候选并发组；写文件、共享进程、需要确认或 Hook 改参的调用仍顺序跑。
没有这份账之前，不拿“顺序可审计”冒充成本已经算过。

证据：[`loop.cpp`](../src/agent/loop.cpp)、[`turn_view.hpp`](../src/runtime/turn_view.hpp)、
[`schema.cpp`](../src/trajectory/schema.cpp)。

### 3. 主请求不重试，用户要怎样续？

**先答：** 不重试守住了副作用，现版也保住了可信前缀；可一键续跑体验还没补齐。
每次模型请求前，用户输入与已完成 step 已进 history。上一 step 的 assistant、工具
调用和工具结果也已逐条落 session。网络错、`429`、`5xx` 或解析错来了，本轮明报
`请求失败`，会话不杀，输入提示符回来。用户可输入“继续上一项任务”；进程重启后
也可用 `--continue` 或 `/resume` 找回已成账前缀。工具自己失败时，宿主写一条错误
`ToolResultBlock`，模型可换参数，用新 call id 再走确认与 Hook。

有一道窄缝必须明说：普通 transport 断流后，屏上已经露出的半截正文不会写进
history。只有用户按 ESC，主循环才强制收起 partial assistant，添打断标记，并给
未执行工具补结果。故网络抖在第一 step，原始用户任务还在，用户不必从头重写；
可那段只在屏上见过的半句话，模型续跑时看不见。若半句含了关键判断，用户得补述。

当前错误文案会带 HTTP 状态或清洗后的错误摘要，却没告诉用户“保住了哪一步、下一步
该按什么”。也没有 `/retry-last`。这便是没还完的可用性债。补法分三层：错误卡明列
最近耐久检查点；给一枚“从检查点继续”命令；只有确认零响应字节、零工具副作用，
且服务端有幂等保障时，才做有限退避。其余情况仍由用户点明续跑。

证据：[`loop.cpp`](../src/agent/loop.cpp)、
[`reliability.md`](../docs/architecture/agent-loop/reliability.md)、
[`session_utils.cpp`](../src/tools/session_utils.cpp)。

### 4. 单二进制最终多大？启动多快？

**先答：** 2026-08-30 本机 `0.26.127` Windows x64 Release 快照为
`11,575,296` 字节，即 `11.04 MiB`。程序主体是一只原生 `lubancode.exe`；完整发行
包还带 LICENSE、skills、文档与安装脚本，不能把“单二进制”说成“发行包只有一只
文件”。

同一文件用 Windows `ProcessStartInfo` 起进程、读完输出并等退出。`--version` 连跑
`51` 次：中位 `211.03 ms`，P95 `853.49 ms`，最小 `83.43 ms`；`--help` 连跑
`21` 次：中位 `225.17 ms`，P95 `824.90 ms`，最小 `90.58 ms`。这一批只能说明
CLI 启动、解析参数、输出后退出的墙钟；它没有量交互终端何时可输入，也没有控制
Windows Defender、文件缓存与调度噪声。故我会说“可执行文件 11.04 MiB，短命令
中位约 0.21 秒”，不会说“完整冷启动 0.21 秒”。

这还是一次本机点测，不是持续 benchmark。README 里的旧 `8.2 MiB` 已跟当前产物
不符。下一步应在 Release CI 固定落三项：exe 字节数、`--version` 热启动分布、
新环境交互 ready 时间；超阈值便告警。

证据：[`version.hpp`](../src/app/version.hpp)、[`release.yml`](../.github/workflows/release.yml)、
[`portfolio.md`](portfolio.md)。

## D 组：往算法里钻

这组题最忌顺着面试官的话头乱认。实现若与通用教科书不同，先把本项目的数据结构
画出来。参数没调过，便直说没调过。

### 1. BM25 的 `k1`、`b` 取多少？按代码语料调过吗？

**先答：** 现版 `k1=1.5`，`b=0.75`。这两只数取的是常用档，不是从 LubanCode
语料上扫参得来。仓库里没有 `k1/b` 参数化入口、网格搜索或消融报告。故“为什么
这样取”的准确答案是：先用了稳妥默认值，再靠固定检索集守最终指标；不能说成
“针对代码仓库调优”。

还要纠正一个前提：这里的 BM25 文档不是源码文件，也不是任意 Markdown。每条
`MemoryEntry` 才是一篇短文档。索引只拼 `title + summary + scope.value + keywords +
paths + evidence.path/symbol`，连长 `content` 都不放进去。`doc.len` 统计分词器吐出的
词项次数；中文整词、二元片段、标识符整串与拆词都可能同时入账。它的长度分布更像
结构化记忆卡，不像新闻语料，也不像整份代码文件。

`k1` 管词频饱和。取 `1.5`，同一词在标题、关键词和路径里反复出现仍会加分，可很快
收住。`b=0.75` 给了较强长度归一；对短卡未必合适，长卡可能吃亏。更要紧的是，
系统另有路径 `12` 分、关键词/符号 `8` 分等硬命中，BM25 又乘 `2` 后封顶 `24`
分。固定尺子全绿，未必说明 `k1/b` 取对了，可能是硬分把参数差异盖住了。

当前 IDF 也不是教科书原式，而是：

```text
idf = ln(1 + N / df)
score = weight * idf * tf * (k1 + 1)
        / (tf + k1 * (1 - b + b * dl / avgdl))
```

这样做是防小库 `N=1` 时唯一命中词被压到近零。可真正调参还欠三笔账：先报真实
记忆卡 `doc.len` 的 P50/P95/极值；再扫 `k1={0.8,1.2,1.5,2.0}`、
`b={0,0.25,0.5,0.75,1}`；最后同时看 Recall@1、误命中率、过注入和长短卡偏置。
现有 `100` 条中文卡、`127` 条问句能当尺子，却没有做这轮消融。

证据：[`project_memory.cpp`](../src/memory/project_memory.cpp)、
[`project_memory.hpp`](../src/memory/project_memory.hpp)、
[`test_memory_retrieval_zh.cpp`](../tests/unit/memory/test_memory_retrieval_zh.cpp)。

### 2. LaTeX 分式、基线、嵌套高度和伸缩括号怎样排？

**先答：** 块级排版器的最小件是 `Box{rows, width, baseline}`。它没有另存
`ascent/descent`；两者可直接推出：

```text
ascent  = baseline
descent = rows.size() - baseline - 1
```

分式先递归排出 numerator、denominator 两只盒。宽度取两者最大宽再加两格：
`max(num.width, den.width) + 2`。每一行按终端显示宽度居中。分子各行在上，随后铺一行
`─`，分母各行在下。分式盒的 `baseline = numerator.rows.size()`，正好落在分数线上。

左右若还有普通文本，`HBox` 先取所有子盒最大 ascent 与最大 descent，再逐盒平移：

```text
local_row = output_row - (max_ascent - child.baseline)
```

普通单行盒 baseline 为 `0`，故会同分数线对齐，不会贴着分子顶边走。嵌套分式也不
另开特判。内层先返回自己的 `rows/baseline`；外层把它当分子或分母整盒堆叠，新的
总行数与 baseline 再交给上一层。高度便一层层冒上去。

**括号这里不能顺着题面答“几档”。** 现版没有离散字号档，也不挑最近档。内容高
一行，直接用普通 `(`、`)`。高于一行，圆括号用 `⎛/⎜/⎝`、方括号用
`⎡/⎢/⎣`，花括号用 `⎧/⎪/⎨/⎩`，按 `inner.rows.size()` 拼出任意整数高度；
右侧换对应部件。竖线逐行重复。括号 baseline 原样取 inner baseline，再交给
`HBox` 对齐。这是终端字符拼装，不是字体引擎的离散 glyph size。

现存欠账也要报：终端单元格没有 TeX 字体度量、math axis、kerning 与真正可伸缩
轮廓；花括号中心只按 `height/2` 取一行。它能把常见公式排齐，不等于实现了 TeX。

证据：[`latex_math.cpp`](../src/cli/latex_math.cpp)、
[`test_latex_math.cpp`](../tests/unit/cli/test_latex_math.cpp)。

### 3. compact 的 map/reduce 怎样切？第几层终止？

**先答：** 先算压缩模型可用输入：

```text
input_budget = window_tokens - output_reserve_tokens - protocol_headroom_tokens
```

整份 history 连同压缩指令装得下，就只发一次，不走分层。装不下，最新外层用户轮
留作热区，只拿冷区做 map。上一轮 archive 从冷区首条里剥出，只给 reduce 当参考，
不送回 map，免得摘要再抄摘要。

冷区先按 episode 切。新的外层用户输入开一段，`todo_write` 也开一段；边界只落在
轮界，绝不劈开 tool use/result。episode 装不下 chunk budget，再按用户轮边界切。
相邻小段按顺序贪心合并，合到下一段会超预算便收块。故块数不是固定三块，也不由
配置直接指定；它由冷区 episode、各段 token 估算和压缩模型窗口共同决定。单轮本身
超预算时仍自成一块，不会从工具对中间硬劈；这只 map 请求也没有更细的本地预检，
provider 若拒绝，整趟失败，旧 history 不动。

map 为每块产一份局部摘要。reduce 先把所有局部摘要拼起来；仍超窗，便把相邻两份
两两归并，一轮大约把数量砍半。代码不是无限递归，而是一只循环：

```text
kMaxReducePasses = 4
while over_budget and summaries.size() > 1 and passes < 4:
    pairwise_merge()
```

四轮最多把摘要份数压到原来的约 `1/16`。这里有一处实打实的欠账：四轮之后若仍
超预算，现版**不会本地明确放弃**，而是照样发最终 reduce；只剩一份却仍过大时也
一样。中间 pair merge 请求本身也没逐对做窗口预检。服务端若拒绝，整趟 compact
报错，旧 history 一字不动；可错误来得太晚。这便是递归终止条件有了，超限终态却
没收严。

更稳的收口应是：每层归并前先验 pair 输入；四轮后重算最终输入，仍超窗便本地拒绝，
错误里报块数、估算 token 与上限。若想继续归并，层数应由
`ceil(log2(chunks))` 和总请求预算共同裁定，不能只把 `4` 改大。最终 manifest 与
活动待办守恒只在终稿严验；map 和中间归并较松，也要一道算进失真风险。

证据：[`compact.cpp`](../src/agent/compact.cpp)、[`compact.hpp`](../src/agent/compact.hpp)、
[`test_compact.cpp`](../tests/unit/agent/test_compact.cpp)。

### 4. `atomic<bool>` 用什么 memory order？默认 `seq_cst` 够吗？

**先答：** 先限定到回合取消旗。`TurnRuntime::request_interrupt()` 用
`store(true, memory_order_release)`；`interrupted()` 用
`load(memory_order_acquire)`。多信号合并的 `CancelChain` 也用 acquire/release。
往下走到 `AgentLoop`、HTTP transport 与进程轮询，还有若干裸 `load()`；它们默认
`seq_cst`。

默认 `seq_cst` 当然够。它比 acquire 更强，读到 release store 写出的 `true` 时，
仍能建立同步关系。这里没有靠多只原子排一条全局顺序，故那份全序大多没用；相比
网络、20ms 取消轮询和进程等待，性能代价也不是眼下瓶颈。

再往深处说，若这只原子**只传一位停止信号**，`memory_order_relaxed` 已足以消掉
data race，并让各线程在该原子的 modification order 上读写一致。现版没有靠
cancel=true 发布另一块无锁 payload，故 acquire/release 偏保守。源码注释把问题
只说成“普通 bool 可见性”，不够准：普通 bool 的根病是并发读写形成 data race，
不是偶尔刷新得慢。

若将来约定“先写 `cancel_reason`，再 release-store true；读线程 acquire-load true
后读取 reason”，acquire/release 才有明确价值。若要同时维护 cancel、phase、结果
所有权等多字段不变量，哪怕每只原子都 `seq_cst`，也凑不成一笔事务；该换 mutex 或
单一状态机原子。内存序同样救不了阻塞系统调用，取消能多快生效仍看 polling 与底层
可中断点。

证据：[`turn_runtime.hpp`](../src/runtime/turn_runtime.hpp)、
[`turn_harness.cpp`](../src/agent/turn_harness.cpp)、[`loop.cpp`](../src/agent/loop.cpp)、
[`http_stream_transport.cpp`](../src/api/http_stream_transport.cpp)。

### 5. UTF-8 清洗怎样判 ACP 重解成功？合法歧义怎么办？

**先答：** `IsValidUtf8` 会拒绝坏续字节、截断、过长编码、代理项与越界码点。
`SanitizeUtf8` 先看原字节；本来就是合法 UTF-8，立即原样返回。原字节非法时，
Windows 才整段走 `CP_ACP -> UTF-16 -> UTF-8`，随后再跑一次 `IsValidUtf8`；通过便
认作重解成功，否则把非法字节逐枚换成 U+FFFD。

这只“成功”是**结构成功**，不是**语义成功**。`AcpBytesToUtf8` 调
`MultiByteToWideChar(CP_ACP, 0, ...)`，没有 `MB_ERR_INVALID_CHARS`；只要 Win32
肯转，回到 UTF-8 后通常天然合法。它证明不了原文真是 ACP。外来混合文本稍保守些：
只有 `invalid_bytes > valid_multibyte_sequences` 才试整段 ACP，否则保留合法 UTF-8
片段，只换坏字节。可这仍是启发式。

GBK 字节若碰巧也是合法 UTF-8，第一关就会放行，压根儿不试 ACP。例子很直：
`C2 A1` 按 CP936 解是“隆”，按 UTF-8 解是“¡”；`D0 A1` 按 CP936 是“小”，
按 UTF-8 却是西里尔字母“С”。现版会取后者。这类歧义只看字节无法可靠判断，
测试里的“你好”样本 `C4 E3 BA C3` 恰好是非法 UTF-8，没覆盖这道缝。

真正的解法不是再叠一层猜测，而是把编码契约从来源带下来：PowerShell wrapper 明示
UTF-8，`cmd.exe` 带 console/ACP code page，Hook 候选页用
`MB_ERR_INVALID_CHARS` 严解并记录 `cp936`，HTTP 看 charset，文件读取则只收 UTF-8。
来源未知又有歧义时，保留原始字节摘要与编码标签，允许用户指定，不静默改字。

证据：[`text_encoding.cpp`](../src/platform/text_encoding.cpp)、
[`paths_win.cpp`](../src/platform/paths_win.cpp)、
[`test_tools.cpp`](../tests/unit/tools/test_tools.cpp)、
[`test_utf8_boundary.cpp`](../tests/unit/cli/test_utf8_boundary.cpp)。

## E 组：现场白板

这组不能只报类名。先画状态，再落崩溃点；先圈信任边界，再画 IPC；测试则要穿过
生产控制流，不能只验最底下一只格式化函数。

### 1. 把排队消息改成 durable ack 协议，三态怎样走？

**先答：** 现版已经有稳定 `queue_id`、session queue 快照、失败回队和 attempt
上限。它能扛一部分失败，却还不是 durable ack。`TakeFirstAutoSendable()` 仍会先把
条目移出活队列，再靠外层失败分支 `ReturnToFront()` 补救；queue 快照与目标 history
也不是一笔原子提交。

白板先画这张图：

```text
             claim + durable write        accept + durable receipt
    pending ----------------------> inflight ----------------------> acked
       ^                              |
       | lease 到期，且目标无 receipt   |
       +------------------------------+
```

三态各管一件事：

| 状态 | 含义 | 何时落盘 |
| --- | --- | --- |
| `pending` | 宿主已经收下，尚未交给目标 | 先追加 `queue.enqueued`，再 `fsync` / `FlushFileBuffers`；写稳后才在屏上说“已排队” |
| `inflight` | 某个 dispatcher 已认领，目标尚未留下耐久收据 | 追加 `queue.claimed{delivery_id, owner, lease_until, attempt}` 并写稳，而后才投递 |
| `acked` | 目标已把这条输入纳入自己的耐久账 | 目标写 `queue.accepted{queue_id, delivery_id, message}`；这同一条记录既是 user message，也是 receipt |

`acked` 只表示“目标耐久收下”，不表示“模型答完”，更不表示“工具副作用成功”。这条
边界要早定。若一直等到回合终态才 ack，模型已经跑过命令、进程却在收尾前倒下，恢复
时再把原输入重排一遍，反而会重做副作用。

主会话最好只留一本 append-only journal。`queue.accepted` 事件里直接嵌 user message；
回放这同一行时，一边把消息放进 history，一边把 queue 折成 `acked`。这样没有“消息
已入 history，ack 还没写”的双写裂口。子代理若另有任务账，做不到跨文件原子提交，
便按这条次序走：

```text
先写目标 inbox receipt -> 再写源队列 ack
```

两边都带 `queue_id + delivery_id + payload_hash`。若进程倒在两写之间，恢复器先扫目标
receipt，再替源队列补 ack。次序不能倒。先写源 ack，后写目标 inbox，中间一倒就会
永久丢消息。

白板上要把裂口逐条圈出来：

1. `enqueue` 尚未写稳便崩：宿主没有收下，界面不得先报成功。
2. `enqueue` 已写稳，回显前崩：消息还在；客户端重提时靠稳定 `queue_id` 去重。
3. `claimed` 已写稳，尚未投递便崩：lease 到期后回 `pending`。
4. 已递进目标内存，receipt 尚未写便崩：仍按未送达处理。故目标必须先记收据，再让模型或工具看见。
5. 目标 receipt 已写，源 ack 尚未写便崩：按 `queue_id` 对账，补成 `acked`，不可重投。
6. ack 后，模型请求途中崩：队列不回滚。用户输入已经成账；恢复时报告这轮未完成，让用户续跑。
7. journal 尾行只写了一半：带长度或 checksum，回放只认完整前缀；坏尾截掉，不能猜半条状态。

多 dispatcher 还要一枚 lease。`owner_generation` 防旧进程复活后交迟到结果；
`delivery_id` 防同一 queue item 两次认领混账。`acked` 事件可长期留在 journal，活队列
只展示 `pending/inflight`。这给的是 at-least-once transport 加 idempotent accept，
不是凭空许诺 exactly-once 副作用。

现版 `SessionStore::AppendQueueEvent()` 只是 `ostream.flush()` 后的整表快照。若口径要
扛断电，还须补 OS 级 flush；只扛进程崩溃与扛掉电，不能混叫。

证据：[`queue_model.hpp`](../src/cli/queue_model.hpp)、
[`interactive_session.cpp`](../src/app/interactive_session.cpp)、
[`session_utils.cpp`](../src/tools/session_utils.cpp)、
[`test_queue_model.cpp`](../tests/unit/agent/test_queue_model.cpp)。

### 2. Lua / DLL 插件怎样搬到进程外？文件权限怎样传？

**先答：** 当前边界先说准。Lua 与 DLL 都在 LubanCode 进程里。Lua 有内存、指令预算，
pure 画像会收掉 `io`、`os.execute`、`package.loadlib`；manifest v2 的网络又走受控 HTTP。
这些能减攻击面，却不是 OS 沙箱。DLL 一进 `LoadLibrary`，`DllMain` 已拿到宿主权限；
访问越界也会拖倒主程序。

我要另起 `lubancode-plugin-host`。一件不可信插件占一只 host，不跟别的插件合住。核心
进程不加载 DLL，也不建 Lua state。host 在受限进程里加载插件，崩了只折这一件工具：

```text
AgentLoop / ToolRegistry
          |
          | invoke / cancel / result
          v
   PluginBroker (可信，主进程)
          |
          | 长度前缀帧：JSON 或 CBOR
          v
 lubancode-plugin-host (受限进程)
          |
          +-- Lua VM
          `-- LoadLibrary / dlopen

插件若要文件、网络、Secret：
host -- broker request --> PluginBroker -- policy + user grant --> 代办 I/O
```

现有 process plugin 是“一次调用起一只进程，stdin/stdout 各一份 JSON”。新 host 若要
保 Lua state 与 DLL 静态状态，须用长命协议；可借它的请求/响应形状，不该硬拿
newline 当长期分帧。stdout 混一行日志就会拆坏协议。我的帧头至少有
`magic/version/length/type/request_id`，正文设字节帽；stderr 单走日志管道。起服先做
`hello`，核对协议版本、插件 id、内容哈希与随机 nonce。每次调用带 deadline、取消 id
和 host generation。host 崩溃后，本批调用全收成 `plugin_host_crashed`；有副作用的调用
不自动重放。

文件权限不传一只 `allowed=true`，也不把工作区路径和宿主 token 整包塞给插件。分三步：

1. manifest 声明上限，如 `fs.read:${workspace}/docs/**`、`fs.write:${workspace}/out/**`。
2. 调用时求交集：`声明权限 ∩ 工具策略 ∩ 本场确认 ∩ 当前 worktree`。
3. broker 发短命 capability：绑定 `plugin_hash + invocation_id + op + canonical_root +
   expiry + max_bytes`。host 只拿一枚不透明 `grant_id`。

插件读文件时发：

```json
{
  "type": "broker.fs_read",
  "request_id": 17,
  "grant_id": "g-...",
  "relative_path": "docs/plan.md",
  "offset": 0,
  "limit": 65536
}
```

主进程逐次验 grant，拼规范路径，拦 `..`、绝对路径、设备名与 reparse/symlink 逃逸。
随后由 broker 自己开文件，再把字节或一只受限流句柄送回。不能先校验字符串，再让
插件自己 `open(path)`；两步之间可换 symlink，且恶意 DLL 压根儿能绕过这条 API。大文件
走 opaque handle 分块读，handle 同样绑 invocation。写文件则由 broker 落临时文件、
算 diff、走确认，再原子替换。

这套 capability 只有配上 OS 拒权才算数。Windows 侧用 restricted token 或 AppContainer，
再套 Job Object：不继承宿主句柄，空环境，单独临时目录，默认无工作区与网络权限，
限制进程数、内存、CPU 和墙钟。Linux 侧用 user/mount/network namespace、只读根、
`no_new_privs`、seccomp 与 rlimit。若宿主进程本身还能随手读工作区，broker 协议只是
君子协定，不是沙箱。

Secret 也照同一规矩：插件只声明名字，核心按调用解析，最好代签 HTTP 或传短命凭据，
不把整份环境变量继承过去。每次 broker I/O 都落审计：谁、哪次调用、哪枚 grant、
哪条规范路径、多少字节、结果怎样。插件 host 的确认只能收窄能力，不能自行扩权。

证据：[`plugin_process.cpp`](../src/runtime/plugin_process.cpp)、
[`plugin_lua_host.cpp`](../src/runtime/plugin_lua_host.cpp)、
[`lua_tool.cpp`](../src/tools/lua_tool.cpp)、[`plugin_loader.cpp`](../src/tools/plugin_loader.cpp)、
[`plugin-runtime.md`](../docs/architecture/extensions/plugin-runtime.md)。进程外 host 与文件
broker 是设计稿，现版尚未实现。

### 3. 不碰真终端，怎样证明 `Ctrl+O` 会补画历史工具？

**先答：** 不能只测 `FormatTranscriptItems(items, expanded=true)`。现有测试已经证明
formatter 会铺子工具；旧 bug 坏在上游：Ctrl+O 只翻 atomic、打一行“详细模式”，
没有把历史快照交给 formatter。测试若绕过按键 action，旧实现照样全绿。

我会把 Ctrl+O 分支抽成一只不认 Win32 的 `HandleTranscriptToggle`。它只依赖四个口：

```text
ExpandedState   读写紧凑/详细
SnapshotSource  取当前不可变 transcript snapshot
RenderSink      记 clear / forget-anchor / write / redraw-footer
WidthSource     本测固定返回 80
```

真终端 adapter 仍负责把 `KeyInput::CtrlO` 翻成这枚 action。单测直接调同一 action，用
`FakeRenderSink` 收命令，不造控制台，也不比 ANSI 光标串。

故障形状要这样摆：

1. 初始为紧凑档。
2. 依次送 `agent start -> subtool read_file start -> subtool done -> agent done`。子工具须在
   按 Ctrl+O **之前**已经结束，参数放 `{"path":"old.txt"}`，结果放 `OLD_RESULT`。
3. 先断言紧凑投影没有 `old.txt` 与 `OLD_RESULT`。
4. 调生产同款 `HandleTranscriptToggle(CtrlO)`。
5. 断言 action 把状态翻成 expanded，只取一次快照，并按
   `clear footer -> forget anchors -> write snapshot -> redraw footer` 排序。
6. 断言最终语义帧等于 `FormatTranscriptItems(snapshot, expanded=true)`；历史 `read_file`、
   参数与结果各出现一次，不能只有“详细模式”。
7. 再切回紧凑、再展开，结果须幂等；其间添一枚新工具，下一次展开须同时含新旧两枚，
   顺序不变。

这条测试在旧代码上会红：若 `expand_renderer_` 没接，或 snapshot 没在工具落定时更新，
输出里只剩模式行，`OLD_RESULT` 找不到。它还不验证 ConHost 的滚屏与残影；那层仍归
真终端驱动。这里专钉业务合同：“一次 Ctrl+O action 必须拿当前历史快照重铺”。

落点可放 `tests/unit/cli/test_transcript_toggle.cpp`。现有
[`test_transcript.cpp`](../tests/unit/cli/test_transcript.cpp) 留作纯投影测试；新测试穿过
[`console_input.cpp`](../src/cli/console_input.cpp) 抽出的 action 与
[`tool_display.hpp`](../src/cli/tool_display.hpp) 的快照源。这比真实终端更稳，也比只测
formatter 更接近那次病根；目前仍是待补测试设计，不能说已经进 CTest。

## F 组：动机与取舍

这组不比类图。面试官看你肯不肯认：有些功能只是自己喜欢，有些选择没有做过正式
算账，有些欠账丢人，却实实在在躺在仓里。

### 1. 若从头再来，第一刀砍掉什么？

**先答：我先砍块级 LaTeX 盒式渲染。** 行内公式可以保留普通文本或轻量 Unicode
替换；分式、矩阵、伸缩括号这套二维排版，首版一个字也不写。

理由不复杂。用户打开 coding agent，头一件事是读准仓库，第二件是改对代码，第三件
是跑完验证。公式排得漂不漂亮，挨不到前三排。块级 LaTeX 好玩，也好演示；它让我
写了递归 parser、`Box{rows,width,baseline}`、终端宽度对齐和一摞边界测试。可这些工时
没有替队列补 durable ack，也没有把真终端驱动送进默认 CI。

这刀还说明一件难听的话：我当时有“先做有趣功能”的偏心。LaTeX 有画面，做完立刻
能截图；session 落盘、失败率、许可证与 crash recovery 没那么上镜。若按用户价值排，
顺序该倒过来。

首版我只守四件事：

```text
可靠收输入 -> 读/改/跑核心工具 -> 一条稳定模型协议 -> session 可恢复
```

第二条协议用来验证中立边界。MCP 等核心闭环站稳再接。LSP、记忆、插件与漂亮公式，
件件要拿真实任务证明“没有它真办不成”，才准进主线。三协议最终仍值得留；它们直接
服务“不绑一家”的目标。compact 也不能久砍，长会话迟早撞窗。LaTeX 则可永久留在
可选层，主循环照样成立。

证据：[`latex_math.cpp`](../src/cli/latex_math.cpp)、
[`test_latex_math.cpp`](../tests/unit/cli/test_latex_math.cpp)、
[`portfolio.md`](portfolio.md)。

### 2. 为什么不直接改 OpenCode / Codex？许可证怎样处理？

**先答 build vs buy：** 若目标只是尽快得到一只好用的 coding agent，我应该直接用
Codex 或 OpenCode，不该重造。LubanCode 的合理性主要来自另一个目标：我要亲手练
C++ 长命进程、终端并发、跨平台进程树、C ABI 与 provider-neutral loop。这是学习与
作品集上的 build，不是已经算赢了商业总成本。

为何没有直接 fork？我想换的不只是一枚 UI 按钮。语言、消息模型、工具 ABI、终端
renderer、Win32/POSIX 进程边界都在核心。拿现成项目深 fork，会长期追上游冲突；另写
一只窄 runtime，反倒能把这些实验收在同一技术栈里。可这只能解释选择，不能冒充
正式 buy/build 评审。当时我没写功能缺口、维护成本、上游同步成本与退出条件矩阵。
若在公司里立项，这一步缺了，方案就不该过会。

OpenCode 也要说准。Git 历史里，最早的模型目录提交 `3bb0f79` 只写“借鉴 Codex
model-catalog”；当时仓里没有 OpenCode 或 Models.dev 字样。OpenCode 是后来写架构
文档时拿来作同类方案对照。不能倒过来说“当初比较两家后选了自研”。OpenAI 官方
文档把 Codex CLI 的开源主仓指向 `openai/codex`，这只能证明源码出处公开，不能替我
完成本项目的许可证审计：<https://developers.openai.com/codex/open-source>。

**许可证这笔账，眼下没处理干净。** 仓内有两份来源痕迹：提交说明与
[`model_catalog.hpp`](../src/config/model_catalog.hpp) 文件头。可根目录没有 `LICENSE`，
也没有 `NOTICE` 或第三方归属清单；[`README.md`](../README.md) 却宣称项目采用
Apache-2.0，还链接一枚不存在的文件。`scripts/check_docs.sh` 会当场报断链。故我不能
回答“已按许可证妥善处理”。一句“只是借鉴思路”也不能拿来糊账；先得逐段审查，到底
只借了抽象，还是搬了字段、数据或受版权保护的表达。

补账要走四步：

1. 钉住当时参考的上游 URL、commit 与许可证文本，不能拿今日 HEAD 代替历史来源。
2. 对 `3bb0f79` 的字段、注释、测试与上游逐项比对，分清独立实现、改写与复制。
3. 给 LubanCode 补真实项目许可证；按上游条款保留所需声明、改动说明，并建
   `THIRD_PARTY_NOTICES` 或同类归属账。
4. 条款对不上，或来源说不清，便删掉那段，照公开规范独立重写；审完再发版。

这不是法律意见，只是最低工程纪律。眼下面试最诚实的答法是：“来源写过，许可证
闭环没做完；这是发布阻断项。”

证据：[`schema.md`](../docs/architecture/providers/schema.md)、
[`model_catalog.hpp`](../src/config/model_catalog.hpp)、[`README.md`](../README.md)、
提交 `3bb0f79`。

### 3. 这项目最大的遗憾是什么？

**先答：我太早把它当作品集，太晚把它当日用工具。** 我一度拿“又接了一项功能”、
“又发了一版”当进度。三协议、MCP、LSP、插件、LaTeX、compact、记忆，一路往桌上
摆。桌面看着丰盛，底下几条腿却没先钉牢。

这不是四项技术债凑出来的标准答案。它是同一处毛病：我偏爱能看见的新增，轻看了
枯燥收口。

- 现有材料只能证明 2026-08-29 一场真项目 dogfooding，证明不了“每天都用”。
- coverage 百分比没量；真终端驱动还没进默认 CTest。
- 排队虽有快照、恢复与失败回队，durable receipt 仍缺一笔。
- README 挂着 Apache-2.0 徽章，仓根却没有 `LICENSE`。

最不体面的地方就在这儿：部分功能更像“我能做”，不全是“用户真缺”。块级 LaTeX
比 durable queue 先落地，正是这股偏心留下的脚印。

若重来，我会在“核心工具 + session + 一条协议”可用时停四周。自己拿它做真实任务，
再找三位不参与开发的人试用。路线图只收三类东西：反复撞到的失败、能量出改善的
瓶颈、用户确实办不成的任务。新功能还要写删除条件；没人用，便砍。等 data-loss
边界、许可证、默认 CI 与 usage 账站稳，再谈展示性功能。

我后悔的不是项目做大了。我后悔的是，做大以前，没有先证明哪一寸最该长。

证据：本题库[A 组第 4 题](#4-这工具每天用吗它真坑过你吗)、
[`development-challenges.md`](retrospectives/development-challenges.md)、
[`portfolio.md`](portfolio.md)。

## 🧠 模型、Provider 与 Schema

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 模型信息从哪来 | 内置 provider 快照、本地 `models.json`、端点 `/models` 各管一段 | 数据来源分层 |
| 参考了 OpenCode 什么 | 参考元数据驱动与覆盖思路，没照搬 schema；仓内直接记载借鉴 Codex catalog | 技术诚信、竞品研究 |
| JSON Schema 管什么 | 先分 provider catalog、tool input、structured output 三种 schema | 概念边界 |
| 为什么不全硬编码 | 新模型元数据可更新，wire adapter 仍守协议 | 数据与代码边界 |
| 为什么有 schema 还手写 parser | schema 给编辑器/CI，C++ 才是运行时校验与交叉引用 | 双契约维护 |
| capability 可信吗 | 是声明，不是探测；部分字段已接预算，部分只解析展示 | 能力协商 |
| 未知模型怎么办 | 允许直切，不猜高级能力，下一次请求验证 | 开放世界设计 |
| `/models` 与目录冲突信谁 | 端点列表判可用，目录补元数据 | 动态事实与静态知识 |
| variant 是新模型吗 | 不是；同一 model id 上叠请求参数 | 配置建模 |
| `extra_body` 如何合并 | 协议字段后叠 provider，再叠 variant；顶层浅合并 | 覆盖语义 |
| 输出上限从哪取 | config > provider > model catalog > unset | 优先级与 optional |
| 为什么 `unset` 不给默认数 | 服务端上限未知，擅猜会截短或拒绝请求 | 缺失值语义 |
| think 档位不认识怎么办 | 提示未经验证，不硬拦兼容端新值 | 前向兼容 |
| 新模型何时必须发版 | 只换 ID/元数据不用；新 wire 或新事件形状要改 adapter | 扩展边界 |
| 模型价格怎么算 | 当前没价格账，只记 usage；不能伪报金额 | 可观测性诚信 |
| 为什么模型专属 instruction 单独成段 | 不覆盖人格与环境段；切模型时能整体清掉 | prompt 组合 |

深读：[模型、Provider 与 JSON Schema 深挖](../docs/architecture/providers/schema.md)。

## 🔄 Agent Loop 与终止条件

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| turn、step、attempt 有何不同 | 外层任务、一次模型来回、一次 HTTP/SSE 尝试 | 状态机定义 |
| 主循环何时停 | 无工具的最终回答、取消、显式步数闸或输出预算耗尽 | 终止性 |
| `max_steps_per_turn=0` 是什么 | 无硬步数上限，不是零步 | 配置语义 |
| 如何防无限工具循环 | 可配步数闸、临近上限给收束提示、用户可 ESC | 失控保护 |
| 多工具为何顺序跑 | 守副作用、确认、Hook、转录与结果顺序 | 并发取舍 |
| stop reason 与内容冲突信谁 | 有 tool use 就按内容执行并配结果 | 防御性协议处理 |
| 工具失败是否抛异常 | 化成 `is_error=true` 的结果，交模型纠错 | 错误作为数据 |
| `max_tokens` 后为何还能继续 | 追加宿主标记开新 step，次数有界 | continuation 设计 |
| continuation 是重试吗 | 不是；输入与 step 都变了 | 术语准确 |
| 外来消息何时插入 | 工具收齐、下次请求未发的 step 边界 | 安全点 |
| 为何第一 step 不收 inbox | 刚提交的用户消息该先领起本 turn | 消息次序 |
| 工具表为何每步重建 | `tool_search` 可在中途挂新 schema | 动态能力 |

深读：[Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)。

## 🌐 三协议与流式解析

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 为何不为三家各写一套 Agent | wire 只翻请求与事件，主循环吃中立消息 | 抽象边界 |
| 中立层最小单位是什么 | Message、ContentBlock、StreamEvent | 数据模型 |
| tool call JSON 怎么拼完整 | input delta 累积，结束后再解析 object | 增量组装 |
| SSE 一帧跨多行怎么办 | framing 层按空行断 event，再拼多行 `data:` | 协议细节 |
| UTF-8 从半个字断开怎么办 | gate 暂存不完整尾巴，下一 delta 再拼 | 字节边界 |
| thinking 与正文怎样区分 | 归一成不同 block / delta，UI 与 history 分账 | 推理通道 |
| 三家 tool result 形状不同怎么办 | 中立 ToolResult，adapter 出线时再翻 | 反腐层 |
| Responses item id 有何用 | 维持后续关联与协议要求，不能随意丢 | wire 状态 |
| Anthropic 为何怕相邻 user | 角色交替更严，archive 要并进热区首条 user | 兼容约束 |
| Chat reasoning 要不要回放 | 按 provider 的 `reasoning_replay` 策略 | 供应商差异 |
| 流断了 partial assistant 怎么办 | 不冒充完整回复；ESC 路显式记打断，transport 错明报 | 一致性 |
| 如何加第四种协议 | 实现 request、events、client，再接 backend factory | 开闭原则 |

深读：[Query 数据流与三协议适配](../docs/architecture/query-data-flow.md)。

## 🔧 工具调用与 JSON

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 单工具 wire 上是什么 | provider 原生 tool/function JSON；宿主内统一 ToolUseBlock | 内外数据形状 |
| 多工具结果怎么回 | 顺序执行，结果按调用 id 同批回填 user message | 配对协议 |
| tool call id 谁生成 | 通常 provider 给；宿主按 id 关联 result | 关联键 |
| 参数坏 JSON 怎么办 | assembler / adapter 报解析错，不拿半成品执行 | 输入完整性 |
| JSON Schema 是否统一验原始参数 | 当前尚未全统一；工具 execute 仍自验 | 现存欠账 |
| Hook 改参后为何再验 schema | 防 Hook 绕过工具参数边界 | 变更后验证 |
| Schema 支持多深 | 当前宿主轻量校验只做一层基础类型、required、enum | 不能说满 |
| 未知工具怎么办 | 回错误 ToolResult，不崩主循环 | 开放集合 |
| 工具说明为何也占 context | name、description、schema 都在请求前缀 | 隐性成本 |
| 延迟工具怎么找 | 先注索引，模型调 `tool_search`，下一 step 挂完整 schema | 按需加载 |
| 工具返回很长怎么办 | 源头封顶，再 artifact + 预览 + microcompact | 长结果治理 |
| side-effect 工具为何不判重 | stdout 相同不代表动作相同 | 幂等判断 |

深读：[工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[工具调用流程](../docs/architecture/tool-calling-flow.md)。

## 🔐 权限、安全与注入

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 模型说“这是安全命令”能信吗 | 不能；确认、策略、Hook 与 OS 权限在宿主 | 信任边界 |
| Prompt injection 怎么挡 | 工具输出与外来资料标不可信，权限不交给模型 | 指令层级 |
| `read_file` 能读哪里 | 按工具路径规则与项目边界，不把 schema 当权限 | 文件隔离 |
| 命令为何要确认 | shell 是高副作用能力，参数形状合法不等于安全 | capability security |
| allow 与 deny 冲突谁赢 | deny 优先，固定归并次序 | 策略确定性 |
| 多 Hook 同时表态怎么办 | 同步并发执行，定义顺序归并，权限 `deny > ask > allow` | 并发归并 |
| PostToolUse 能回滚吗 | 不能；副作用已发生，只能报错与留痕 | 事务边界 |
| MCP 工具为何默认确认 | 外部服务器实现不在本仓信任域 | 第三方代码 |
| API key 会不会进 catalog | 不进；只存 env 名或发前替换占位符 | 密钥治理 |
| extra headers 能覆盖认证吗 | 能，空值还能删；高级配置者自负其责 | 逃生口风险 |
| 在线 catalog 安全吗 | HTTPS、大小、schema、revision、原子写；尚无内容签名 | 供应链 |
| 项目配置能否偷偷开记忆 | 不能，只能在全局授权内降档 | 陌生仓库边界 |

深读：[安全模型](../docs/development/security.md)、[Hooks 流程](../docs/architecture/hooks-flow.md)。

## 📚 Context、长文本与 compact

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 为什么不把历史全塞回去 | 窗口、成本、缓存与注意力都会坏 | 预算意识 |
| history、request view、session 有何不同 | 活历史、临时请求视图、完整持久流水 | 状态分账 |
| system prompt 会被 compact 吗 | 不会；compact 有自己的 system，只换 history | prompt 稳定 |
| 保留最近几条 | L3 按完整 turn 与 token；L4 才是首轮加最近三轮 | 算法准确 |
| 程序过滤与 LLM 总结如何分工 | 程序守边界与引用，LLM 做语义归纳 | 混合算法 |
| episode 在哪切 | 新用户输入与 `todo_write` | 任务阶段识别 |
| map/reduce 为什么要多层 | compact 模型自己也有窗口 | 二阶窗口问题 |
| 摘要漏待办怎么办 | manifest 与 active todo 守恒不过便拒收 | 可验证摘要 |
| compact 失败怎么办 | history 不动，L1/L4 继续兜底 | 失败原子性 |
| 单条用户输入超窗怎么办 | 无冷区可切，明确拒绝，让用户落文件或拆分 | 不可能边界 |
| hard trim 有损吗 | 有；只改请求视图，终端告警，session 原文留着 | 降级可见 |
| prompt cache 如何稳 | system 稳定、L1 决策钉住、hard trim sticky、epoch 明记断因 | 缓存工程 |

深读：[Context 压缩算法深挖](../docs/architecture/context/compaction.md)。

## 💾 Session、重试与恢复

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 网络错会自动重试吗 | 当前不会；一 step 一 attempt | 副作用意识 |
| 为什么不重试 `429/5xx` | 流可能已有正文/tool use，工具可能已执行 | exactly-once 困难 |
| 崩溃会丢整场吗 | JSONL 逐条 append+flush，通常只伤尾行 | 持久化模型 |
| session 首行坏怎么办 | meta 无法识别，整场不能 resume | 根记录重要 |
| 中间一行坏怎么办 | 跳过并计数，继续回放完整行 | 部分恢复 |
| tool use 缺 result 怎么办 | 补显式错误结果，修协议形状 | 不变量修补 |
| 孤儿 result 怎么办 | 找不到任何 use 便删除 | 引用完整性 |
| compact marker 如何回放 | 读到事件便替换有效 history，旧消息仍在 all_messages | 事件溯源 |
| marker 写失败怎么办 | 内存已短、磁盘仍长；重启回到压缩前 | 两阶段裂口 |
| ESC 后保存什么 | partial assistant、打断标记、已完成结果与未执行错误结果 | 取消语义 |
| memory job 会重试吗 | pending job 可跨进程再捞，坏 job 进 failed | 耐久队列 |
| MCP 超时后迟到包怎么办 | pending id 已删，迟到响应丢弃 | late response |

深读：[Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)。

## ⚙️ 并发、取消与后台任务

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 哪些东西并发 | 流式输入监听、footer 心跳、Hook handler、子代理等各有边界 | 并发地图 |
| 主工具为何不并发 | 副作用与确认次序优先 | 串并取舍 |
| cancel flag 如何跨线程 | `atomic<bool>`，监听线程写，执行线程读 | 内存可见性 |
| 为何不用普通 bool | 跨线程读写是 data race | C++ 内存模型 |
| 取消命令怎么杀树 | Windows Job Object，POSIX 进程组 | OS 差异 |
| 后台任务跨重启吗 | 不跨；日志可留，内存 task registry 不重建 | 持久性边界 |
| 子代理结果何时进主循环 | 安全点 drain completion notices | 消息交接 |
| 子代理能否污染主 history | 主 history 只收委托与最终结果，中间探索隔离 | 上下文隔离 |
| MCP 进程死了会自启吗 | 当前不会；重启还须重新 initialize 与 tools/list | 生命周期 |
| Hook 并发返回如何稳定 | 收齐后按定义次序归并，不按完成先后 | 确定性 |

深读：[文件读取与命令执行深挖](../docs/architecture/tools/file-commands.md)、[Query 数据流里的子代理路径](../docs/architecture/query-data-flow.md#agent-子代理为何不一样)。

## 🌐 MCP、Skill、Hook 与插件

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| MCP 是不是 HTTP | 当前 stdio 长命子进程，一行一帧 JSON-RPC | 协议实现 |
| MCP 怎样握手 | initialize、initialized notification、tools/list | 生命周期 |
| stdout 能混日志吗 | 不能；一行须是协议 JSON，日志走 stderr | framing 边界 |
| MCP content 都支持吗 | 当前主要拼 text，其他类型有占位 | 不能说满 |
| Skill 如何触发 | 常驻名称摘要，命中后模型调 `skill` 按名加载全文 | 渐进披露 |
| Skill 是代码吗 | 主要是说明与流程，不直接绕过普通工具权限 | 能力边界 |
| 同名 Skill 谁赢 | 按本地覆盖层级决定 | 发现顺序 |
| Hook 与 Skill 差别 | Skill 教模型，Hook 由宿主在事件边界直接执行 | 控制面 |
| Hook async 做了吗 | 当前解析展示并记 skipped，没真异步跑 | 实现现状 |
| 插件到底有什么用 | 让可信本地函数不必包装成 MCP 服务 | 设计动机 |
| 插件如何进工具表 | Lua/C 定义适配成统一 Tool 接口与 Schema | 扩展接口 |
| 插件能真装吗 | Lua 拷文件；Windows C 插件编 DLL 再拷；重启后 `/plugins` 查 | 可交付性 |
| 为什么不用全用 MCP | MCP 换隔离与独立生命周期；插件换少部署与低调用成本 | 取舍能力 |
| 为什么 Lua 与 C 都留 | Lua 轻，C 能接原生库；开发门槛与能力范围不同 | 分层设计 |
| C ABI 为何不用 C++ 类 | 收窄编译器、标准库、对象布局与跨 CRT 边界 | ABI 意识 |
| 谁分配谁释放怎么做 | DLL 返回 content，宿主拷贝后回调插件 free_result | 内存边界 |
| 插件调用前确认就安全吗 | 不；Lua 顶层与 DllMain 在加载时已能执行 | 信任边界 |
| Lua/DLL 崩了怎么办 | 普通 Lua error 可接；死循环、os.exit、DLL 崩溃会拖宿主 | 不能说满 |
| 插件有热更新和包管理吗 | 当前都没有；投放文件后重启 | 产品现状 |
| 插件并发安全吗 | DLL 作者自保；Lua 同 state 加锁串行，不同 state 可并行 | 并发边界 |
| 扩展工具为何还要确认 | 注册成功不等于可信 | 最小信任 |

深读：[工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)、[进程内插件系统深挖](../docs/architecture/extensions/plugin-runtime.md)。

## 📈 性能、缓存与成本

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 最大 context 就一定更好吗 | 不；日志噪声、延迟、价格与注意力都有成本 | 性能观 |
| token 怎么估 | 本地启发式用于预判，provider usage 用于实账 | 估算与测量 |
| 为什么还留字符硬限 | token 估算与窗口资料可能不准 | 双保险 |
| tool schema 占多少 | 名、说明、JSON Schema 全算；工具多时延迟挂载 | 静态开销 |
| prompt cache 何时失效 | system、旧消息、工具定义或裁剪形状变化 | 前缀稳定 |
| 为什么记 cache epoch | 把“为何断缓存”从猜测变成可观察事件 | 可诊断性 |
| artifact 会不会无限长 | 需靠会话仓与清理策略；请求只留预览 | 存储预算 |
| 命令输出为何 2 MiB 封顶 | 防失控进程吃内存与 context | 资源限制 |
| microcompact 为何有迟滞 | 冷区不长 50% 不再跑，防反复烧模型 | 抖动控制 |
| usage 缺失怎么计成本 | 明写未报告，不当零 | 数据质量 |
| cheap 模型失败会怎样 | 后台小活降级，不拖垮 normal turn | 服务分级 |
| 为何不用向量库做 memory | 当前 BM25 更轻、本地、可解释；召回语义较弱 | 取舍能力 |

深读：[上下文、长文本与记忆深挖](../docs/architecture/memory/context.md)。

## 🧪 测试、可维护性与跨平台

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 网络协议怎么测 | parser 与 request builder 先做纯函数测试，client 再测边界 | 可测试设计 |
| 为什么大量 pure function | 输入输出可钉，少靠真实终端与网络 | 确定性 |
| SSE 怎么做失败测试 | 喂碎帧、坏 JSON、半 UTF-8、错 stop reason | 边界覆盖 |
| session 恢复测什么 | 坏行、缺 result、重复 compact、最后事件胜出 | 恢复正确性 |
| schema 改字段如何防漏 | schema、C++ allowlist、parser 与测试同改 | 契约同步 |
| Windows 与 POSIX 最大差别 | 路径编码、Job Object/进程组、终端重画 | 平台抽象 |
| 为什么 C++23 | expected、variant、性能与单二进制；代价是异步和生态更重 | 技术选型 |
| 如何查内存泄漏 | RAII、进程句柄封装、长会话压力与 sanitizer | 资源生命周期 |
| 文档如何防过期 | 行为页连源码入口，变更矩阵与测试一同审 | 文档工程 |
| 如何做回归演示 | 固定 Release 包、假 provider/fixture、成功与失败各一条 | 可复现性 |
| 哪处最难维护 | 协议兼容差异与终端并发，需窄接口和纯函数 | 复杂度识别 |
| 下一步先重构哪里 | 选有证据的欠账，如 schema 单源生成或 capability gate | 优先级判断 |

深读：[架构说明](../docs/architecture/README.md)、[文档规范](../docs/development/documentation.md)。

## 💥 开发难题与故障复盘

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 开发中最难的问题是什么 | 先挑一件跨边界故障，不要说“时间紧、任务重” | 技术深度 |
| 遇过最严重的线上事故吗 | 报影响面、止血、根因、修复与防复发 | 事故担当 |
| 哪个 bug 最难复现 | 讲并发请求永挂；用裸 socket 复刻“连接成、响应不来” | 复现能力 |
| 用户现场复现不了怎么办 | 留版本、平台、输入、日志与时间线，再缩变量 | 取证习惯 |
| 有过误判吗 | Unicode 故障先疑 wire，后来又查到 `path::string()` 窄口 | 诚实与排除法 |
| 怎样证明找到了根因 | 修前测试必红，修后必绿；解释因果，不只报相关性 | 证据强度 |
| 为什么同一问题修三层 | 源头保真、统一边界保命、旧数据入口兜底 | 分层设计 |
| 测试全绿为何用户还能撞 bug | fixture 没复刻故障形状，或只跑了 POSIX 语义 | 测试反思 |
| 跨平台最坑的地方 | 文件柄、ACP/UTF-8、控制台与进程组语义各不相同 | 平台经验 |
| 遇过内存生命周期问题吗 | lambda 引用退栈对象，ASAN 抓到 stack-use-after-scope | C++ 基础 |
| 遇过资源泄漏或退出挂死吗 | 后台请求永挂加无界 join，须给请求与退出各设边界 | 生命周期治理 |
| 怎样设计故障回归 | 不测抽象“超时”；真起装死 socket，不回响应头 | 测试质量 |
| 修完 bug 还要做什么 | 补 fixture、注释、错误文案、排障文档与回归 | 工程闭环 |
| 现在还有什么已知问题 | 排队已有快照与失败回队，尚无 durable receipt，双写裂口仍在 | 边界诚实 |
| 若重做一次先改什么 | 先把 queue 变成 pending/inflight/acked 的耐久协议 | 优先级判断 |

深读：[开发难题与故障复盘](retrospectives/development-challenges.md)。每件事都照“现场 → 误判 → 根因 → 修法 → 验收”拆开。

## 🎓 项目归属与设计取舍

| 追问 | 先答哪句 | 他在验什么 |
| --- | --- | --- |
| 哪些由你主导设计 | 报具体决策、AI 参与、提交与验收，不报空泛“全栈” | 真实贡献 |
| 参考开源是否算抄 | 当前只能证明来源留痕；仓根许可证与第三方归属尚未闭环 | 工程伦理 |
| 为什么不用现成项目 | 若只求生产力应直接用；自建服务于 C++ runtime 实验，商业总成本没证实更低 | build vs buy |
| 若重来先砍什么 | 先砍块级 LaTeX，把工时还给恢复、CI 与真实使用 | 优先级判断 |
| 最大一次事故是什么 | 讲症状、根因、不变量、回归测试 | 故障复盘 |
| 最大遗憾是什么 | 太早按作品集加功能，太晚按日用工具收可靠性与许可 | 反思能力 |
| 若用户量涨百倍怎么办 | CLI 本地瓶颈与云服务瓶颈分开，不乱套分布式 | 场景判断 |
| 哪项指标最该加 | attempt、首字节、cache hit、compact 收益、tool latency | 可观测性 |
| 如何排技术债 | 看事故频率、数据损失风险、共享边界与测试成本 | 工程判断 |
| 为什么项目值得做 | 协议、工具、终端与恢复在一只真实 runtime 里相互制约 | 项目价值 |
| 与 OpenCode/Codex 区别 | 报语言、部署、支持边界与设计取舍，不贬竞品 | 竞品理解 |

项目故事与简历说法见[求职项目手册](portfolio.md)。真实故障怎样讲，见[开发难题与故障复盘](retrospectives/development-challenges.md)。

## ⚠️ 十句不能说满的话

1. 不说“支持所有 OpenAI 兼容端”。兼容端常改字段、路径与 SSE。
2. 不说“接入了 OpenCode/Models.dev”。当前只是同类思路对照，格式不兼容。
3. 不说“JSON Schema 保证安全”。它只管形状的一部分。
4. 不说“模型 capability 全部自动生效”。若干字段仍只解析存储。
5. 不说“模型请求会自动重试”。当前主请求没有。
6. 不说“多工具并行”。主 AgentLoop 顺序执行。
7. 不说“compact 绝不丢信息”。机器只强验 manifest 与活动待办。
8. 不说“session 绝不会丢”。首行坏、落盘失败与磁盘故障仍有边界。
9. 不说“MCP 全协议支持”。当前 transport 与 content 类型都有范围。
10. 不说“跨平台语义完全一致”。Windows 与 POSIX 的进程、终端能力不同。

## 🔗 最后复习路线

时间只剩半小时，按这次序读：

1. [面试深挖导航](deep-dives.md)
2. [开发难题与故障复盘](retrospectives/development-challenges.md)
3. [Agent Loop、重试与恢复深挖](../docs/architecture/agent-loop/reliability.md)
4. [模型、Provider 与 JSON Schema 深挖](../docs/architecture/providers/schema.md)
5. [Context 压缩算法深挖](../docs/architecture/context/compaction.md)
6. [工具协议与扩展运行时深挖](../docs/architecture/extensions/tool-extension.md)
7. [求职项目手册](portfolio.md)

每页挑一条成功路、一条失败路、一项欠账。把源码入口记住。面试官再往下问，便有路可走。
