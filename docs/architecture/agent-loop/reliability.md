# Agent Loop、重试与恢复深挖

_面向技术面试与源码走查：讲清一轮代理怎样推进，失败后哪本账还能信，又为何不能见错就重试。_

---

[面试深挖导航](../../../interview/deep-dives.md) · [Query 数据流](../query-data-flow.md) · [会话与上下文](../../features/sessions/README.md) · [工具调用流程](../tool-calling-flow.md)

## 📋 先把结论说准

LubanCode 的主循环是一只同步状态机。一条外层用户消息叫 `turn`。turn 里每发一次模型请求，叫一枚 `step`。模型若只回正文，turn 收口；若回工具调用，宿主顺序执行，攒齐结果，再开下一枚 step。

当前主模型请求没有自动 HTTP/SSE 重试。三家 client 都只发一次 `cpr::Post`。网络错、HTTP 非 2xx、流解析错、API 错，照实返回。`429`、`5xx` 也不会在 client 里退避重发。

这并非“完全不恢复”。系统把恢复拆成四路：

| 路 | 解决什么 | 是否重发原请求 |
| --- | --- | --- |
| turn 内续跑 | `max_tokens` 后没有可交付正文 | 否。追加宿主标记，开新 step |
| 工具级纠错 | 工具找不到、拒绝、执行失败 | 否。把错误结果回给模型 |
| 会话级续命 | 异常、ESC、进程重启 | 否。保存已成账消息，稍后继续 |
| 数据修补 | JSONL 坏尾、工具配对残缺 | 否。跳坏行、补错误结果 |

面试时可用一句话收住：

> retry 是把同一 attempt 再发；continuation 是拿已成账状态开下一 step；recovery 是保住可信前缀，再从边界往下走。三者不能混叫。

## 🔄 主循环怎样转

### 三层循环

外面是会话泵，中间是 turn，里面是 step。

```mermaid
sequenceDiagram
    accTitle: Agent Loop Request Lifecycle
    accDescr: A user turn advances through repeated model steps, sequential tool execution, result refill, and a final assistant response, with persistence after every turn outcome.

    participant user as User
    participant session as InteractiveSession
    participant loop as AgentLoop
    participant model as Provider
    participant tools as ToolRegistry
    participant store as SessionStore

    user->>session: Submit outer message
    session->>loop: Run user message
    loop->>loop: Append durable history

    loop loop_step
        loop->>loop: Build request view
        loop->>model: Send one stream
        model-->>loop: Text and tool calls
        alt Tool calls exist
            loop->>tools: Execute in order
            tools-->>loop: Batch tool results
            loop->>loop: Append paired results
        else No tool calls
            loop-->>session: Return outcome
        end
    end

    session->>store: Append and flush new messages
    session-->>user: Return to prompt
```

一枚 step 走这条路：

1. 在安全边界收外来消息。第一步不收，免得抢在刚提交的用户消息前头。
2. 建 `api::Request`。`model` 与主 `system_prompt_` 在此装入。
3. 若步数快见底，往末条消息添一次收束提醒。
4. 估 `system + tools + history + 输出预留`。到压力线，先请上层 compact。
5. 从 `request_history_` 派生结构压缩视图。
6. 若仍超字符上限，走 sticky hard trim。
7. 重建工具定义。`tool_search` 可能在上一 step 新挂工具。
8. 做最终硬限检查。仍装不下便拒绝，不发请求。
9. 发一枚流式请求，assembler 收正文、thinking、工具块、usage 与 stop reason。
10. 没有工具便收口。有工具便顺序执行，结果合成一条 user 消息，再转下一 step。

### 为什么工具定义每步重建

工具表不一定静止。模型可先调 `tool_search`，把一件延迟工具挂进 registry。下一 step 若还沿用旧 schema，模型知道名字，却没有正式入参定义。

因此每步重建 `request.tools`。代价是一点 JSON 组装。所得好处很实在：工具挂载状态与请求一致。

### 为什么多个工具顺序执行

一条 assistant 消息可带多枚 `ToolUseBlock`。现版按出现次序逐枚跑，不并发。

顺序执行守住四件事：

- 前一枚工具改过文件，后一枚能看见新状态
- 确认框、Hook 与终端转录顺序固定
- 用户 ESC 时，当前调用与后续未调用容易分账
- 工具结果按原调用顺序批量回填，便于审计

代价也明摆着：互不相干的只读工具不能并发吃满带宽。将来若并发，先得补能力标注、依赖分析、稳定排序、取消传播与副作用隔离。

## ⚙️ 一枚工具怎样过关

`AgentLoop::RunOneTool` 不是裸调一个函数。它逐关过门：

```text
按名查 registry
-> 检查当前角色与 deferred filter
-> 发 PreToolUse Hook
-> 若 Hook 改参，统一跑 JSON Schema 复检
-> 确认或 PermissionRequest
-> execute(input)
-> 清洗 UTF-8
-> 发 PostToolUse Hook
-> 产 ToolResultBlock
```

工具失败不是主循环异常。找不到工具、参数坏、用户拒绝、Hook 阻断、命令退出非零，都化成 `is_error=true` 的工具结果。模型在下一 step 看见它，可改参数、换工具，也可向用户说明。

这叫模型级纠错，不叫宿主重试。宿主没有暗中再跑一次命令。

### stop reason 与内容打架怎么办

有些兼容端会把 `stop_reason` 报成 `end_turn`，正文里却已有 tool use。主循环信内容，不信那枚矛盾标签。只要 assembler 组出了工具调用，便照常执行并补结果。

这条防线很要紧。若只看 stop reason，assistant 留下一枚 tool use，却没有紧跟的 tool result。下一次请求常会被协议端拒掉。

## ⚠️ 失败怎样分

### 失败矩阵

| 失败点 | 当下动作 | history 怎样 | session 怎样 | 会不会自动重试 |
| --- | --- | --- | --- | --- |
| 图片预处理失败 | 本轮不进模型 | 不加用户消息 | 无新增 | 不会 |
| `UserPromptSubmit` Hook 阻断 | 正常拦下本轮 | 不进 loop | 无新增 | 不会 |
| 请求前硬超限 | 返回错误 | 用户消息已入账 | turn 收尾后落盘 | 不会 |
| DNS、连接、TLS 错 | 返回 transport error | 用户消息保留 | turn 收尾后落盘 | 不会 |
| HTTP `429` / `5xx` | 返回 HTTP error | 用户消息保留 | turn 收尾后落盘 | 不会 |
| SSE 中途坏 | 返回 stream/API error | 已显示片段不冒充完整回复 | 已成账消息落盘 | 不会 |
| 工具执行失败 | 回错误 ToolResult | 调用与结果成对 | 两者一起落盘 | 不会 |
| 用户 ESC | 记 partial assistant 与打断标记 | 保住半截与配对 | 正常落盘 | 不会 |
| step 用尽 | 返回 budget outcome | 已成账工作全留 | 正常落盘 | 不会 |
| compact 失败 | 保持旧 history | 一字不换 | 不写 compact event | 不会立即重试 |
| session 尾行坏 | 跳过坏行 | 回放完整前缀 | 原文件不改 | 不会 |

### 请求错误为何不盲重试

一次流式请求不是纯读查询。它可能已经吐过正文，也可能吐出工具调用。工具又可能改文件、发网络请求、启动进程。此时原样重发，风险有三宗：

- 正文重复，history 难以判哪份才真
- tool call id 与调用内容换了一套，配对变乱
- 有副作用的工具跑两遍，文件、进程或外部系统吃双份动作

可安全重试的窗口很窄：确认请求尚未出任何响应字节，尚未执行工具，服务端又支持幂等键。现版没有把这三项证据串成一只 retry controller，故而选择明报失败。

### 将来若加 retry，边界该怎样画

| 场景 | 建议 | 理由 |
| --- | --- | --- |
| 建连前失败，无响应字节 | 可有限重试 | 尚无可见输出与副作用 |
| 收到 `429`，服务端给 `Retry-After` | 可按限额等待 | 仍须确认没有流内容 |
| `5xx` 且未收任何字节 | 可退避重试 | 最多若干次，加 jitter |
| 已收 partial text | 不原样重试 | 可能重复正文 |
| 已收 tool use | 不原样重试 | 新 attempt 可能换 id 或参数 |
| 工具已执行 | 绝不自动重跑 | 副作用未必幂等 |
| compact 子请求失败 | 留旧历史，稍后再试 | 数据仍在，不必冒险替换 |

真正落地还要带 attempt id、首字节标记、重试预算、可取消 backoff、服务端幂等键、日志与指标。只写一个 `for` 循环，不叫可靠性。

## 🔄 continuation 不是 retry

模型可能把输出预算全花在 thinking 上，`finish_reason=max_tokens`，最终却没有正文或工具调用。主循环此时可做受控续跑。

默认由 `profile.length_continuations` 给次数。尚有额度时，宿主往 history 添一条用户标记，请模型停下空转，立刻调用工具，或给一份不超过五行的检查点。然后开新 step。

它与 retry 有四处不同：

| 项 | retry | length continuation |
| --- | --- | --- |
| 输入 | 原请求大致不变 | history 多了一条宿主指令 |
| attempt | 重发同一逻辑请求 | 新 step、新请求 |
| 已有 usage | 常归同一重试组 | 前 step usage 已成账 |
| 目的 | 克服传输或限流失败 | 逼模型产出可交付内容 |

额度烧完仍空，turn 以 `length_empty_output` / output budget exhausted 收场。系统不无限催。

Stop Hook 要求再补一轮终稿，也不是 retry。它只准多开一轮，免得 Hook 与模型互相催，转成死循环。

## 🚫 ESC 怎样收残局

ESC 走 `RunOutcome.cancelled`，不是 exception。主循环先关掉 assembler 尚未闭合的块，再把现场补成可回放形状。

### 流式回答中 ESC

已收到的 assistant 片段会留在 history。随后追加：

```text
[用户按 ESC 打断了这条回答]
```

若片段里已有完整 tool use，却还没执行，宿主给它补一枚错误结果：

```text
用户按 ESC 打断,该工具未执行
```

这样下一轮仍守住 tool use / tool result 成对。

### 多工具中 ESC

当前正在跑的工具先收尾，真实结果照常记。排在后面的调用不再起跑，各补一枚“未执行”错误结果。

这套做法没有假装强杀所有工具。对文件写入或外部命令，半路砍线程反会留下更坏状态。取消只在宿主掌得住的边界生效。

### UTF-8 半个字怎么办

SSE delta 可能恰好从一个多字节字符腰上断开。显示层先把不完整尾巴扣住；下一段到来再拼。流结束仍缺字节，才用替换字符收口。

这不是业务 retry，却是字节流恢复。它防止终端、JSON 与 session 吃进非法 UTF-8。

## 💾 会话怎样续命

### 写盘策略

用户消息一进 `AgentLoop::Run`，便先入 history。turn 无论成功、报错还是 ESC，`RunUserTurn` 最后都调 `PersistNewMessages`。

`SessionStore` 逐条 append，逐条 flush。崩溃若发生在一行中间，前头完整 JSONL 行仍在。代价是写盘次数多；换来恢复边界清楚。

`ProcessLine` 另有会话级 `try/catch`。slash 命令、回合收尾、记忆抽取若抛 `std::exception`，它打印错误，尽力落盘，再把控制权交回输入循环。只有启动边界的异常才该掀掉进程。

### 重启回放

首行 meta 必须能解析。它坏了，session 无从识别，整场不能 resume。首行以后则宽容许多：

1. 空行略过。
2. 坏 JSON、坏事件、未知事件类型记入 `skipped_lines`，继续往后读。
3. 普通消息按序追加。
4. `compact` / `compact_v2` 事件当场应用，重建当时生效的 history。
5. `title` 与 `cwd` 取最后一条，合乎 append-only 语义。
6. 全部读完，再跑 `RepairToolPairs`。

### 工具配对怎样修

恢复器分三遍：

1. 收齐所有 tool use id。
2. 删除找不到任何 use 的孤儿 tool result；若消息因此空了，连空消息删掉。
3. 扫每条 assistant。某枚 use 后没有对应 result，便在下一条 user 消息开头补一枚错误结果；若下一条不是 user，插一条新的 user 消息。

补丁正文固定是：

```text
[会话恢复:结果缺失]
```

这不是伪造成功。它明说结果丢了，同时修好协议形状。模型下一轮可据此重查状态，却不会把那枚工具当作已经成功。

## 📦 compact、memory 与 artifact 的恢复边界

### compact 两阶段落账

compact 先在内存里验收并替换 history，后往 session 追加 `compact_v2` marker。marker 写失败时，会出现一处诚实的裂口：

- 当前进程里的模型已看短 history
- 磁盘没有这枚 compact event
- 下次 resume 会按旧 JSONL 重建较长 history

旧消息还在，数据没有丢。可缓存与 token 占用会退回压缩前。文档与终端必须明报写事件失败，不能装作持久化已成。

### memory job 是耐久队列

项目记忆写入另有 `pending/*.json`。worker 成功才删 job。进程退了，pending 仍在，下次启动可再捞。坏 job 不在原地死转，会挪进 `failed/` 并写错误。

这是跨进程重试机制，却只管 memory job，不管主模型请求。两条链不可混说。

### artifact 如何防坏数据

长工具结果进 artifact 后，索引带内容指纹。读取时会校验 blob。坏 blob 隔离，不拿来喂模型。session 与 Markdown 导出不靠 artifact 才能成立；仓坏了，会丢“追回大原文”这项能力，不会抹掉会话主账。

## 🌐 MCP 与后台任务怎么恢复

### MCP timeout

每枚 JSON-RPC 请求都有 id 与 pending entry。等待时每约 `100 ms` 看一眼 transport 是否还活。超时或进程死掉，pending id 删除并返回错误。迟到响应找不到 id，直接丢。

当前不会自动重启 MCP 进程，也不会重做 `initialize -> notifications/initialized -> tools/list`。重连若要做，必须重新握手、刷新工具表，并处理旧 id 世代；不能只把子进程拉起来。

### 后台命令

后台任务台账活在进程内。日志文件可留，任务对象不会在新 LubanCode 进程里自动重建。Windows 侧用 Job Object 收进程树，宿主退出时收尾较强；POSIX 异常退出后的语义弱些。

故而“后台任务可查询”只指同一宿主进程。不能说成 durable job queue。

## 🔍 不变量与 corner case

| 不变量 | 如何守 | 守不住时怎样办 |
| --- | --- | --- |
| 主 history 从真实用户输入起 | `Run` 开头先追加 user | 输入预处理失败则不进 loop |
| tool use 后有 result | 正常执行批量回填；ESC 与 resume 补错误结果 | 删除无 use 的孤儿 result |
| compact 失败不改 history | 先生成、解析、验收，后 `ReplaceHistory` | 留旧 history，交 hard trim 兜底 |
| 单次工具不暗中重跑 | 工具错化成结果 | 让模型决定下一步 |
| 一枚 step 只发一次 transport attempt | client 单次 `cpr::Post` | 返回明确错误 |
| 有损裁剪要看得见 | pressure callback 告警并开 cache epoch | session 原文仍留 |
| 异常不轻易杀会话 | turn 与 session 两道 catch | 尽力落盘，回输入提示符 |

还要防几处边角：

- `max_steps_per_turn <= 0` 代表无硬步数闸，不代表死循环不会发生。最终仍靠模型 end turn、用户取消或外层终止。
- 配了正数步数时，剩三步会收到一次固定收束提醒。预算本来少于三步，第一步便提醒。
- step limit 是预算耗尽 outcome，不当异常抛。部分工作照留。
- 一条巨型用户输入可能连 hard trim 都救不了。程序在发请求前拒绝。
- 工具执行已经成功，PostToolUse Hook 再失败，也不能回滚先前副作用。只能报错与留痕。
- session 追加失败后，内存仍可继续；再崩一次，未落盘段便真会丢。故而写盘告警不能吞。

## 🎓 面试追问答法

### “为什么没有自动重试？”

先说边界：模型流可能已产正文与 tool call，工具又有副作用。盲重试会重复输出或重复执行。现版选择单 attempt、明报失败、保住 history 与 session。若将来加，只在零响应字节、零副作用且有幂等保障时做有限退避。

### “失败后用户要从头来吗？”

不必。用户输入、已完成 assistant 块、工具结果会按实际情况留账。ESC 还会补打断标记与未执行结果。turn 收尾逐条 append+flush。重启后跳坏尾、应用 compact marker、修 tool pair，再续聊。

### “工具失败会不会自动再跑？”

不会。宿主把错误作为 `ToolResultBlock` 回给模型。模型可改参再调，但那是一枚新调用，有新 id，也能重新走确认与 Hook。

### “有哪些真正的重试？”

主模型 transport 没有。memory pending job 可跨进程再捞。L2 microcompact 失败后会冷却，冷区增长 50% 才再开一趟。`max_tokens` 可受控续跑，但它是 continuation。

## 🔗 源码与测试

| 题目 | 源码入口 | 关键测试 |
| --- | --- | --- |
| step 主循环 | `src/agent/loop.cpp`、`loop.hpp` | `tests/test_loop.cpp` |
| turn 异常与 UI 收口 | `src/app/turn_runner.cpp` | `tests/test_loop.cpp` 与交互回归测试 |
| 会话泵与落盘 | `src/app/interactive_session.cpp` | `tests/test_session_store.cpp` |
| JSONL 回放与配对修补 | `src/agent/session_store.cpp` | `tests/test_session_store.cpp` |
| 三协议单 attempt | `src/api/chat/client.cpp`、`responses/client.cpp`、`anthropic/client.cpp` | 各 provider client/request 测试 |
| MCP pending 与 timeout | `src/mcp/client.cpp`、`transport.cpp` | `tests/test_mcp_client.cpp` |
| memory durable jobs | `src/memory/project_memory.cpp` | `tests/test_project_memory.cpp` |
| hard trim | `src/agent/context.cpp` | `tests/test_context.cpp` |

再往下追 context 细节，读[上下文压缩算法深挖](../context/compaction.md)。文件、命令与进程树另见[文件读取与命令执行深挖](../tools/file-commands.md)。
