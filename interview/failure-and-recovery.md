# 失败之后怎么办：断网、半截回复、落盘与崩溃恢复

核对日期：2026-09-11。按当前 `AgentLoop` 与默认 V3 写账路径讲。已用假后端、真实 V3 写账桥和临时文件系统故障跑过审计，见第 9 节；没有做真实断网、断电或磁盘写满实验。旧版“不重试”“坏行跳过就能续”的说法，不能直接套到现版。

[面试目录](README.md) · [问题库](question-bank.md) · [请求与 wire 数据流](../docs/architecture/query-data-flow.md)

## 1. 面试先答这段

> 我会先问失败落在哪一步。请求还没发，先看输入和请求准备账；请求发了没收齐，就看错误分型，决定能不能重试；回复收齐却写不进去，就停在工具执行前。工具若已改过文件，再丢结果，不能说“没执行”，更不能整轮重跑。重启后只从账里恢复能证实的状态，拿不准就记 unknown，再查现场。
>
> 当前网络类错误会有限重试，总共最多 6 次尝试。每次重新收 assistant，成功后才交给后续工具循环。前一步工具结果仍作输入，不重跑前一步工具。这能守住本地执行边界，却不能保证服务端只生成一次、只扣一次费。

这题别只答“加 try/catch，再重试”。面试官要听四件事：**失败前做到了哪儿、哪份状态可信、下一步做什么、还有什么不能保证。**

## 2. 你举的例子：前序发出去了，assistant 没拿到

这里把“前序”理解为本次请求携带的 system、历史消息、用户输入和已完成工具结果。一次用户任务可以连发多次模型请求；一次逻辑请求又可尝试多次。别把请求重发说成整条 turn 重跑。

假设正在改配置，流程走到这里：

```text
用户：把配置改好，再检查一下
  → 第一步模型返回 write_file 调用
  → 宿主执行，文件已改，工具结果已提交
  → 第二步把前序消息和工具结果发给模型
  → 网络断了，没拿到完整 assistant
```

当前第二步这样处理：

1. 后端返回错误，恢复环按错误类别判定。`Network` 可以重试。
2. 等一段带抖动的退避时间。等待期间可以取消。
3. 保留同一份 `Request`，清空这次 assembler、文本闸和临时图片列表，重新发模型请求。
4. 收齐成功回复，才往下提交 assistant、执行新工具。前一步 `write_file` 不在恢复环内，不会让恢复环再跑一次。
5. 连首发最多尝试 6 次。仍失败就报错，保留先前已接纳状态，交上层收尾。以后继续任务，要从这个状态重新请求，不能声称接回原先那条 TCP/SSE 流。

若失败发生在第一步，还没有工具结果，处理一样：重发模型请求；不会凭空补一条成功 assistant。

**没拿到回复，不等于服务端没处理。** 请求可能没到，也可能已经生成，只是回程断了。本地没有成功回包，不能推出“没扣费”。`model.request.sent` 也只是本地记录，不是服务端收据。

证据：[恢复环](../src/api/model_request_recovery.cpp) 的 `RunRequestWithRecovery`；[主循环](../src/agent/loop.cpp) 的 `run_one_attempt`、`OnOutputCompleted` 和后续工具循环。

## 3. 哪些错重试，哪些错立即停

| 失败 | 当前处理 |
| --- | --- |
| `Network`：连接、DNS、复位、超时等网络类错误 | 可重试；实际类别以后端返回值为准 |
| HTTP `408 / 429 / 500 / 502 / 503 / 504` | 可重试，不是全部 `5xx` 都重试 |
| HTTP 200 中夹 `upstream_error` 等白名单 API 错误 | 折成 API 错，再按白名单重试 |
| HTTP `400 / 401 / 403`，确定性 API 错误 | 不重试；先修请求、凭据或权限 |
| `Parse` | 不重试；重发不是协议修复器 |
| `Cancelled` | 不重试；退避中取消也不再发下一次 |
| 本地请求准备账写失败 | 不发本次模型请求，不当网络错重试 |

API 白名单为 `upstream_error`、`server_error`、`overloaded_error`、`overloaded`、`model_overloaded`、`internal_error`、`internal_server_error`。

五次重试分别等待 **1–2 秒、4–8 秒、15–30 秒、30–60 秒、60–120 秒**。这些只是等待区间，不含请求耗时。`Retry-After` 目前没有透过 Backend 接口，不能说已经遵守服务端建议。恢复函数自身也没有总截止时间参数；若谈总墙钟限制，须另查调用方如何传取消信号。

证据：[策略与常量](../src/api/model_request_recovery.hpp)、[分型和退避实现](../src/api/model_request_recovery.cpp)。

## 4. 已经出了半屏字，怎么还敢重试

先分四处状态：

| 状态放在哪儿 | 能说明什么 | 不能说明什么 |
| --- | --- | --- |
| 终端显示 | 用户看见过片段 | 不代表完整回复已提交 |
| 临时 assembler | 本次尝试收到这些内容 | 不代表下一次请求会携带它 |
| V3 流事件 | 有些片段已分批记账，可供追查 | 不等于正式 assistant 已接纳进模型上下文 |
| 正式消息与上下文接纳事件 | 这份消息已成账，接纳链已推进 | 不保证此前失败尝试没有服务端费用 |

普通断网重试会重建 assembler，失败那次半截正文不与成功正文拼接。半截 tool JSON 也不会交给工具执行。**“不进有效 history”不等于“磁盘上完全没有痕迹”**：V3 会分批记录流片段。

也别把 assembler 清空说成终端必定擦掉旧字。本次已复现：按主路未接 `on_request_attempt` 的方式接入事件适配器，失败片段与成功正文进入同一个文本 item。终端 sink 又逐段追加，没有收到撤回事件。有效 history 正确，不代表用户看到的正文正确。见 FA-05。

ESC 另走取消分支。内存侧会收起半截回复、添打断标记，为未执行调用补错误结果；V3 已起流时尝试定稿为 `interrupted` assistant，尚未起流则只记取消事件。取消写账仍会失败，不能把“走了取消分支”说成“一定保存成功”。

证据：[主循环](../src/agent/loop.cpp) 的取消分支；[V3 接线](../src/runtime/trajectory_session.cpp) 的 `V3StreamDelta`、`V3FlushStreamBatch`、`V3OutputCancelled`。

## 5. 没有落盘，要按哪一步没落来问

| 失败点 | 当前行为与可信边界 | 继续时该怎么想 |
| --- | --- | --- |
| 用户消息写入或接纳失败 | V3 桥记诊断；I/O 错会把 writer 标坏，后续 prepared 不能正常提交 | 内存有输入不等于重启后能找回；先确认账中是否真有这条消息 |
| `model.request.prepared` 写失败 | 返回空请求 ID，主循环明确停在请求边界，不发本次模型请求 | 先处理存储故障，不靠重发 HTTP 补本地账 |
| `model.request.sent` 写失败 | 当前回调返回 `void`，只记错误；主循环随后仍可进入 `send_stream` | 这是现存缺口，不能说所有发送前记录都有硬闸 |
| 流片段写失败 | 记录错误，不当场拦流；writer 若已坏，后续定稿也会受阻 | 屏上片段可能多于磁盘片段 |
| 完整 assistant 定稿或接纳失败 | `OnOutputCompleted` 返回 false；主循环报“模型输出未落账，不执行工具” | 不把完整回包当成已提交结果；账可能已有部分行，需检查 |
| 工具已执行，结果仓保存失败 | 保留执行终态，另记 `tool.result.persist_failed`；结果仓打不开则记诊断 | 外部改动不会随保存失败撤销，先核对文件或远端状态 |
| 结果已选用，tool 消息尚未写入就崩溃 | 恢复投影可识别 `selected_no_message` | 应补交已有结果，不能重新执行；识别状态不等于全套自动补交已实现 |
| 进程在一行中间退出 | 留下截断尾行；V3 `Continue` 明报拒开，不偷偷裁尾 | 留原账，先诊断；不能照旧版说“跳过坏行继续写” |
| 工具已 started，终态还没记就崩溃 | 恢复折叠为 `unknown`，不是成功，也不是未执行 | 查现场；只有具备业务幂等或明确核验后，才谈重做 |

这里还有一道裂口：`V3ToolResultsCommitted` 返回 `void`，存储错误没有统一以失败回执交回主循环。若只是结果仓坏、主账仍可写，不能据此承诺“任何结果落盘失败都阻止下一次模型请求”。这是应补故障注入验证、再补闸门的地方。

证据：[V3 桥](../src/runtime/trajectory_session.cpp) 的 `V3RequestPrepared`、`V3RequestSent`、`V3OutputCompleted`、`V3ToolResultsCommitted`；[writer](../src/trajectory/v3/writer.cpp)；[恢复投影](../src/trajectory/v3/reader.cpp) 的 `FoldToolActions`、`ProjectResume`。

## 6. 追问：写盘成功，到底保证了多少

**问：用了 `flush`，断电就不丢了？**

答：不能这么说。`ProcessCrash` 走 `fflush`；`PowerLoss` 再调用 Windows `FlushFileBuffers` 或 POSIX `fsync`。当前 V3 用户输入、prepared、sent 和流片段多处使用 `ProcessCrash`。两档不能混讲，更没有做过真实断电实验来兜底。

**问：函数叫 Complete，里面几行账就是原子事务？**

答：不是。`CompleteStreamResponse` 依次写 completed 事件、assistant 消息、上下文接纳事件。锁能防同进程写者交错，不能让三行同时耐久。第二行、第三行失败时，前面已经写成的行不会自动消失。得看完整提交链，不能只查一个 completed 标签。

**问：追加返回失败，说明磁盘没写进去？**

答：也不能。可能只写半行，也可能整行写完却刷盘报错。writer 会标坏，停止后续提交，不推进那笔成功状态。下一步要验磁盘，不能假定“什么都没发生”，再原样补写。

证据：[journal](../src/trajectory/journal.cpp) 的 `FlushFileDurable`、`AppendLine`；[V3 writer](../src/trajectory/v3/writer.cpp) 的 `AppendLineLocked`、`CompleteStreamResponse`。

## 7. 面试官再往下追

**问：重试为什么不重复执行工具？**

答：恢复环只包模型发送和收流。完整 assistant 通过输出提交闸门后，才进入工具循环。后一步请求重发时，前一步结果已经在输入里，不会回头走工具循环。这个保证限于本地这条路径；服务端托管工具或外部副作用还得另查合同。

**问：同一输入再问一次，输出一定一样？**

答：不一定。能说沿用同一请求，不能说答案一样。`history_commit_hash` 还只是角色与内容尺寸等摘要，不是全文哈希，也不是服务端幂等键。同长异文可能同摘要，不能拿它证明请求字节完全相同。

**问：工具报失败，磁盘没空间，网络断了，为什么不能都重试？**

答：错的对象不同。模型请求可按白名单重发；工具错误可作为 `is_error` 结果交模型改参；磁盘故障要先修存储；工具执行结果不明则先查外部状态。统一套重试循环，会把“结果没保存”错当“事情没做”。

**问：resume 能从原来那个指令位置接着跑？**

答：恢复的是账中消息、有效上下文和执行状态，不是 C++ 栈、TCP 连接或服务端生成现场。`ProjectResume` 识别未完工作；只有 started 没有终态时标 unknown。不能把这份投影说成任何中断都能自动完成。

**问：你最想补哪几条测试？**

答：先钉发送前与执行前边界，再查双写裂口。prepared 写失败，backend 调用次数应为零；assistant 接纳失败，工具执行次数应为零；工具副作用已发生、结果保存失败，不许重跑。最后逐行切断 completed／message／admitted，检验内存与重启状态如何分叉。

## 8. 已有证据与待验证项

2026-09-11 初次审计时，下表六册已重建并运行：48 项测试、828 条断言通过。它们验证各自覆盖的合同，不能推出整条失败链已经正确；新增跨层探针仍找出了下节问题。

| 证据入口 | 已有测试内容 |
| --- | --- |
| [请求恢复测试](../tests/unit/api/test_model_request_recovery.cpp) | 白名单、最多 6 次、五档退避、确定性错误不重试、等待中取消 |
| [Agent 恢复测试](../tests/unit/agent/test_agent_recovery.cpp) | 首字节前断网、半截正文、半截工具 JSON、已有工具结果后断网；校验 history 与工具执行次数 |
| [V3 流接线测试](../tests/unit/trajectory_v3/test_v3_stream_wiring.cpp) | 片段分批落账、唯一正式回复、中断回复、未起流即取消 |
| [V3 writer 测试](../tests/unit/trajectory_v3/test_v3_writer.cpp) | 引用必须先落稳、正常续卷、截断尾行与坏链拒开 |
| [工具状态测试](../tests/unit/trajectory_v3/test_v3_tool_action.cpp) | 执行成功而结果保存失败，保留 done 并另记 persist_failed |
| [恢复投影测试](../tests/unit/trajectory_v3/test_v3_reader_resume.cpp) | 已完成工具与未完工具分开、未提交压缩不冒充主链、selected_no_message 折叠态 |

还要补真实现场证据：代理中途断流、磁盘写满、刷盘失败、进程强杀，以及不同界面重试时如何处理半屏字。单测能钉住控制流，不能代替这些实测。面试时把边界说到这里，比一句“我们有完善容错”更经得住追问。

## 9. 本次故障审计：五处已复现问题

这节是当前缺陷，不是修复完成清单。生产代码未改。优先级按影响排：P1 先处理状态丢失、语义变样和写账闸门；P2 补齐尝试终态与显示撤回。

### 怎么查，证据放哪儿

探针直接跑 `Agent::Run`，接真实 `TrajectorySessionLedger`、`TrajectoryTurnBridge` 和 V3 writer。模型用本地假后端；退避不真等。工具只计数，并在本次新建临时目录内制造路径冲突。显示测到事件适配器，终端消费路径另查源码；没有做真人界面录屏。

初版探针共 6 项测试、43 条断言，记录 7 组观察。入库版补强到 65 条断言，修复目标另有显式运行模式。**探针通过，表示复现了所列行为，其中包含 bug；不表示故障验收通过。** 修复时，应把对应观察断言改成目标不变量。

- [跨层故障探针](../tests/integration/runtime/test_failure_boundary_audit.cpp)
- [复跑命令、故障编号与修复目标](../tests/integration/runtime/README.md)

Git 只保存测试源码、复现办法与缺陷说明。原始日志、七份会话账和 artifact 留在本地 `.local-audits/failure-boundaries/2026-09-11/`，不随仓库分发。旧记录只说明当时源码行为；审查当前版本请重跑探针。测试默认清理临时账，只有显式设 `LUBANCODE_FAILURE_AUDIT_KEEP=1` 才保留并输出位置。

### FA-01 · P1：结果仓打不开，仍继续发模型；恢复也漏列缺失结果

**触发：** 工具已经执行；结果目录位置变成普通文件，`ResultStore::Open` 失败。

**实测：** `result_store_unavailable` 观察中，工具执行 **1 次**，模型调用 **2 次**。第二次请求带 **1 份真实工具结果**，磁盘却有 **0 条 tool 消息**。`run_ok=true`；`VerifyV3File`、回放与 resume 投影均返回成功，`open_actions=0`。

**根因有两段：** `AgentLoop` 先把结果推入内存 history，再调返回 `void` 的保存回调。`V3ToolResultsCommitted` 打不开仓，只记诊断并 `continue`，没有把失败传回 loop。恢复侧 `FoldToolActions` 仍把该操作折成 `done`；`ProjectResume` 直接跳过 done。只有“已 selected、没 message”才另列未完工作；“执行已 done、结果还没保存”没接住。

**后果：** 当前进程拿真实结果往下走，重启却无法从主账还原同一份输入；而且恢复清单没有提醒这笔缺口。工具副作用确实发生过，重跑整轮会有第二次执行风险。

**修法：** 保存回调返回明确回执，在下一次模型发送前核对结果已提交。恢复状态要分别认“执行结束”“结果持久化”“最终消息接纳”，给 done-without-result 留恢复工作。补账可复用已保存内容，不能把保存失败改写成执行失败，更不能默认重跑工具。

源码定位：`loop.cpp:2260`；`trajectory_session.cpp:1666`、`:1728`；`reader.cpp:591`、`:1165`。实测的是 loop、桥和恢复投影；没有模拟重启后真实 provider 如何拒绝或解释缺结果请求。

### FA-02 · P1：工具失败标记在 V3 回放时丢失

**触发：** 工具正常返回一份 `is_error=true` 结果，再从 V3 主账投影模型上下文。

**实测：** `tool_error_flag` 中，当场发出的请求有 **1 份错误结果**；读盘后错误结果数变成 **0**。账本校验仍通过。

**根因：** V3 最终 tool 消息写入正文与调用身份，没有保存这份回喂结果的 `is_error`。`ProjectHistoryFromReplay` 新建 `ToolResultBlock`，填调用号和正文，错误标记留在默认 `false`。执行终态与 selected 事件虽另有状态，当前转换没有把最终结果语义带回来。

**后果：** 文件没坏，字也还在，但工具反馈从“这是错误结果”变成普通结果。依赖错误标记的 wire 会收到不同内容；不能拿 JSONL 可解析、hash 连续来证明恢复无损。

**修法：** 明确存储并还原最终回喂语义。以 Hook 处理后实际交给模型的结果为准，别只从原始执行终态猜。补 round-trip 断言：同一结果写盘再读回，`is_error`、正文和调用配对都不变。

源码定位：`V3ToolResultsCommitted`、`ToolActionSession::AppendToolMessage`、`trajectory_session.cpp:3695`。

### FA-03 · P1：sent 记录写失败，本次请求仍会发送

**触发：** 用 writer 故障注入口，让 prepared 写成功，紧接着 sent 写失败。

**实测：** `sent_gate` 中，writer 已 `broken=true`，backend 仍调用 **1 次**。之后输出记不住，本轮失败。对照组让 prepared 写失败，backend 调用 **0 次**。

**根因：** `V3RequestSent` 返回 `void`，写失败只进诊断。loop 调完回调，直接进入 `backend_.send_stream`，没有检查这笔提交结果。

**后果：** 本地已经知道账写不动，仍发出可能计费的模型请求；回包又无法正常提交。prepared 闸门有效，不等于 sent 也有闸。

**修法：** 发送前最后一道本地提交必须返回成功或失败；失败就停止本次发送。还要分清“准备发送”“本地交给 transport”“服务端确认”三个事实，不能把提前落下的 sent 当远端收据。

源码定位：`loop.cpp:1579`、`:1584`；`trajectory_session.cpp:1240`。

### FA-04 · P2：重试成功，前一次失败响应没有终态

**触发：** 第一次流出 5000 字节后返回 Network 错；第二次完整返回 `SUCCESS`。

**实测：** `retry_terminal` 中，模型调用 **2 次**，主账有 **2 条 response.started、1 条 response.completed、0 条 response.failed**。校验返回成功。

**根因：** 各次尝试都在恢复环内创建新的轨迹请求；`OnOutputFailed` 却放在整个恢复环失败退出后。中途失败但后来成功，前一请求没有收到失败收口。重试相位回调不等于 V3 终态落账。

**后果：** 事后只看轨迹，无法从这份账明确判定前一次响应为何结束；完成统计与悬空请求判断也会失真。当前校验通过没有证明尝试生命周期闭合。

**修法：** 每次失败尝试都先落自己的终态，再开始下一次；另记逻辑请求与 attempt 关联。最终失败不能再重复记第二笔同请求终态。增加“失败后成功”“多次失败后取消”“用尽次数”三种闭合断言。

源码定位：`loop.cpp` 的 `run_one_attempt`、`:1774`、`:1842`；`V3OutputFailed`。

### FA-05 · P2：有效 history 没拼错，显示事件却把两次正文拼在一起

**触发：** 与 FA-04 同一故障，按主会话方式接 `TurnEventAdapter`，不安装主路目前没有的重试撤回回调。

**实测：** 有效 history 只有 **7 字节 `SUCCESS`**；显示事件累计 **5007 字节**，包含前一次失败片段，而且全在 **1 个文本 item** 内。

**根因：** 恢复环重建 assembler 和 UTF-8 闸，却不通知主路文本 item 撤回或分段。适配器继续向当前 item 发 delta；终端 `TerminalTurnSink` 又把每段交给 `body_tracker->OnDelta` 追加。子代理另有重试回调，不代表主界面已经接好。

**后果：** 模型下一轮所见正文与用户所见正文不一致。失败尝试里若先给了半段结论，用户可能把它和后一次回复当作同一份答案。这里已复现到事件层；未声称做过终端截图验收。

**修法：** 给显示流增加明确 attempt 边界或撤回事件，失败文本标为作废，成功文本另起 item；同步处理转录与正文重绘。验收同时比较有效 history、显示事件和最终 transcript，不能只断言 history 正确。

源码定位：`TurnEventAdapter::OnTextDelta`（`turn_event_adapter.hpp:112`）、`TerminalTurnSink::RenderEvent`（`terminal_turn_sink.cpp:61`）、`turn_runner.cpp` 的主路 wiring。

### 三个对照：别把所有失败都算 bug

| 对照 | 实测 | 结论 |
| --- | --- | --- |
| prepared 写失败 | backend 调用 0 次 | 请求准备闸门有效 |
| 完整输出提交失败 | 工具执行 0 次 | 该注入点下，工具执行前闸门有效 |
| 结果仓已打开，第二份 metadata 保存失败 | 2 次工具执行；`persist_failed=1`；当场请求和回放都含 2 份结果 | 正文通过主账保住了；不能把这次失败说成“结果全丢” |

第三组仍留下另一层问题：失败那份结果没有 selected 事件，原始结果仓链不完整。但“溯源链不完整”和“模型上下文丢了结果”是两件事。前者应另定降级合同，后者本次只在仓打不开那条路径复现。

### 复跑与下一轮验收

在仓库根目录运行：

```powershell
cmake --build --preset release --target lubancode_tests -j 2
ctest --test-dir build/release -C Release -R '^integration.runtime.failure_boundary_audit$' --output-on-failure
```

默认检查当前表现，通过只说明复现一致。测试已另写修复目标断言；设 `LUBANCODE_FAILURE_AUDIT_EXPECT_FIXED=1` 再跑，会把已知差距报成失败。完整命令与边界见[测试说明](../tests/integration/runtime/README.md)。修复后将相应目标断言转为常规回归，不继续要求旧 bug 出现。

FA-01 先检查仓打不开时停止下一次发送、保存缺口进入恢复清单；若改用可靠补存继续执行，须连同恢复与实发对账一起调整验收。FA-02 检查错误标记往返不变；FA-03 检查 backend 调用为零；FA-04 按 requestId 检查每次尝试恰有一笔终态；FA-05 暂以事件录音器不拼入失效正文为目标，若修法采用 item 撤回协议，须扩展显示投影测试，不能只清 history。

下一轮还需专门注入：completed 写成而 assistant 未写、assistant 写成而接纳未写、工具 started 已写而进程被强杀，以及 artifact 文件未刷盘却已记持久引用。当前只沿源码确认这些窗口存在，没有把它们算作本次已复现 bug。
