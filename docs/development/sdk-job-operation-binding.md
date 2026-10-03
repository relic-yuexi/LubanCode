# SDK Job：真实 Operation 绑定与宿主收件边界

这是后续实现合同，不是交付回执。当前 Index 小笔见 [普通 Operation 与 Job 结果边界](sdk-job-result-boundary.md)；中立执行见 [J2b](job-owned-adoption.md)。SDK 后台能力仍默认关闭，J1 仍拒缺完整能力的交单。合同中的内部名称暂定；不添公开 API，不另起模型循环、ActionRun 或执行栈。本地不配置、编译、CTest 或运行原生程序。

## 真实归属，不借主轮

拟由 private `SessionJobs` 持有 live registry、逐 Job 取消源、审批 lease、完成快照与冻结绑定。它随当前 Session 保活，不能挂 CLI 全局名册；协调器、工具与回调都须有真实拥有者。实例、epoch、SID/run、project/cwd 一并校准；同串 Job ID 或 Session ID 不代表同一宿主，也不是权限票。

`JobOperationBinding` 须连接一份真实 Job Operation 接纳事实和实际 Job ID、业务 Action、turn、attempt；保存注册 Pending/Registered、采用事件及它们的原生引用。Job Operation 只有真实接纳落稳后才成立，不能生成字符串便填进 `ToolInvocationIdentity`。执行五键从这份绑定与真实 Started 派生；审批尚未获准时，只有声明范围，没有执行身份。

父 operation 单列因果。现场来源取当前已受理主 Operation、实际 TurnBridge 声明，以及同份 verified V3 里的 assistant message、source Pending、source admission、prepared Pending、Registered 和 adoption 五键引用。恢复逐枚核 SID/run/id/hash/seq、原参数与有效参数摘要、业务 Action 与实际顺序，不能拿调用方字段互证。旧 `operation.dispatched` 不记 turn；缺这份持久绑定时，不能猜最后活动 operation 来补父归属。

业务 turn 继续沿实际声明所在 turn，不造第二个模型 turn。普通主 Operation 仍一 turn 一 op；Job 另走 typed registry/绑定读面，不塞进旧主轮 map、模型输入队列或 `sdk-results` 主轮收尾。不能调用 `SubmitInput` 来登记 Job，也不能调用主轮 `Complete` 替 Job 写 Memory、子 Agent 或主轮 final。普通结果索引排真实 owned Job 业务 Action；以后 Job 索引只收绑定中的实际业务 Action/attempt，不按同 turn 捞全场结果。

源码落点：`src/runtime/session_service.cpp:619` 接纳后入模型队列，`739` 写主轮 final；`src/sdk/operation_ledger.cpp:108` 保主轮 turn 唯一；`src/sdk/core.cpp:746` 校主轮缓存归属，`778` 建结果索引；`src/trajectory/v3/reader.hpp:508` 读真实采用事实，`515` 判业务 Action。本笔不规定新的持久 binding schema，实施前须另钉 typed 接纳/绑定事实及严格读面，不能把 live registry 当恢复证据。

## Action/Post：新版本收真实结局

旧 b `OwnedJobCapability::post -> WriteReceipt` 和 strict `HookCompleted` 接口保持。新 capability 明确版本和完整能力；缺位继续拒，不能回退 inline。候选 `PostSettlement` 分 `ActualCompleted / NoHandlersSkipped / KnownRequiredAbort / Unknown`，带 `state`、真实 `native_receipts`、`error_code` 和 owned `result_adoptions`。前两份 raw/terminal 引用作为只读输入保留；采用件另带真实来源与持久回执，不替换原件。这些状态不能从普通 `is_error` 或错误文案猜来。

Post 输入取本票真实 Started、Terminal、已持久 raw、最终参数与 binding，不借结束父轮的 active operation、TurnWiring、Trace hub 或 `OperationScope`。`ActualCompleted` 校真实 Post dispatch/invocation 配对；`NoHandlersSkipped` 必须对应实际空冻结清单和真实 skip/宿主结算事实，不伪造一次 handler；`KnownRequiredAbort` 保已确认失败回执及已知错误，不能洗成 Unknown。命令 terminal/raw 仍记原执行事实，Post 结局另存，不能补第二枚业务 terminal。

补料沿真实清单、definition、dispatch 归属生成，只添不抹 raw。只有实际持久/采用完成才记 applied；返回一段 text 不等于已采用。补料限额沿新 Action 合同，不提高模型 wire 帽。真实 receipt 未确认优先 Unknown，保阶段首回执与 gap，挡重复结算、重派及本票后续自动模型消费；普通 required/Abort 则留已知 Job 失败。不追认已结束父轮，也不借父取消旗去中止另一主轮；跨票治理另定合同。历史 reader/Observed 与新 capability 同笔校准，旧 b 形状不偷换。

现有 SDK `RunPostAction` 返回结果或已知错误，尚不交原生完成回执；V3 sink 的 requested 也没带 terminal/raw inputRef。`SessionExtensions` 先读全局主轮 `OperationScope`，不能在后台换这份字段。落点：`src/sdk/action_dispatch.cpp:120`、`src/runtime/middleware_v3_sink.cpp:71`、`src/sdk/extensions.cpp:461`、`src/tools/tool_job_coordinator.cpp:503`、`src/trajectory/v3/reader.cpp:1648`。新适配须从真实 dispatch 收回执，不补假 HookCompleted 迎合旧接口。

## 宿主泵、审批与退场

Session 统一发布 owned 状态快照。`ReadJobOperation` 候选只读快照；Wait 等宿主通知，不直接包 Coordinator Get/Wait。后两者会 Pump，可能派工或触 Post。完成通知须唤醒当前 Session 宿主，不等下一主轮，也不借下一轮身份。

writer 尚未有覆盖全场写侧的共享 serial。首批真实 SDK 适配仅在同一宿主安全边界 Pump：批次/请求边界，或主 worker 已退出后的 Close drain。不得从查询线程、完成 worker 或任意回调进 Pump。现 `tool_results_mutex` 只护部分结果事务，不足以宣称任意后台 Post 与主请求写侧并行安全。

审批复用现 pending/Future/逐票 lease，新增独立 Job owner 活域。不能拿 `RegisterScoped` 绑定当前主轮：下一轮会换 owner，普通 CancelAll 会清整表。Job 票绑定实际 Job Operation、声明 Action、Job/epoch/cwd；借取消旗只在 Wait 栈，取消/超时/Close 与迟答同锁判输赢。b scope gate 在 writer serial 内运行，不能在那里等审批或调用 publisher；宿主先锁外取得逐票授权，gate 只复核本票事实和失效。

默认关闭的内部 owner 仍须能退场：先停止新票/新派工，唤醒等票，请求本票取消，等真实进程、worker 与捕获对象退出，串行结算，再撤 callbacks/桥，最后封账和释放资源。附件析构不能代替 join；回调销毁在生命周期/状态锁外。真实未知事实保 gap，不借清资源成功补可信终态。恢复只交 `PassiveHold` 与 owned 历史快照，不注册活授权、不入队、不补消息或重跑；缺绑定留明确缺口，不把旧 b Job 冒充新 SDK Operation。

落点：`src/sdk/approval.cpp:170/190/244`、`src/tools/tool_job_coordinator.cpp:2269/2293`、`src/runtime/trajectory_turn_bridge.hpp:46/268`、`src/runtime/session_service.cpp:402/413/959`、`src/sdk/core.cpp:1403/1443`。公开 Close 是否取消并等待本场 Jobs、取消父 Operation 是否连带已接单 Job、Job 的以后同意授权限哪一域，仍待用户定；只挡公开启用，不挡当前 Index 边界小笔。Detached、跨 Close 保活、Runner 和远端 Full 不在这里决定。

## 最小三笔与原生验收

1. **c1 typed PostSettlement。** 先交新 cap 与真实结算读面，以中立实际 Job scope 验参考适配；旧 b 接口和 J1 缺能力拒绝保持。SDK 真实五键调用等 c2 绑定齐后再接，缺 Job Operation 不捏字段。先验真实 completed、零 handler skipped、required Abort 三路，再验 supplements 正常采用/超帽、真实 Writer 故障 Unknown、错 owner/错 Post 锚/重复结算共六路；不得用 throw 假装账故障，也不得为零 handler 造调用。
2. **c2 typed Job Operation。** private SessionJobs/live binding、真实接纳与 V3 锚、普通/Job 索引隔开和 PassiveHold；仍不开放 Accepted。验真实父子因果、缺绑定与坏源拒、同 turn 主 op 不串、迟归结果不进父缓存、换主轮后 Job owner 不变、Close/恢复只读且零 Pump。无真实 binding 的历史保持未绑定。
3. **c3 独立票域与 host pump。** 先在默认 off 内部装齐权限、Post 和 owner，再渐进接 J1：共同 Prepare、暂存/采用、父真实句柄链确认、业务派工各留原事实，不能双写业务 Started/Finished。验主轮/Job 票互不撤、取消/迟答、真实完成唤醒、同项目二场加异项目二场、Close 真退场、receipt Unknown 后零再派/再模型。公开值配置、冻结计划/锁下恢复和安装消费者另接。

每笔合同在代码之前；新来源按实际并集登记 focused/ASan，三平台远端验完整 argv、非零 CASE/断言、真实 marker、JUnit 和 LastTest。现有绿不替新 source；失败先封原件，再修新头。这里不宣称三笔已实现或已验收。
