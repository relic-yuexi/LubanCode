# 中间件：实际 Hook 回执保存前置

本笔只交 c1a 原生回执读面，基 #314 `12da504d`，其生产基线为 #313 `7e15b8`。不交完整 PostSettlement、Job Operation、SDK 后台装配或 J1 Accepted。旧 b `post -> WriteReceipt`、strict HookCompleted、Observed 与协调器保持；默认 sink、CLI 和 Middleware reduce 保原路。本地不配置、编译、CTest 或运行原生程序。

## 公开内部什么

在 `NestedHookDispatchSession` 增 checked Open，交回真实 Requested 首回执；checked Skip 可带真实 inputRef。旧 Open/Skip 接口仍可调用，旧事件不添字段、不增写盘。`Open` 当前丢 Requested 回执，见 `src/trajectory/v3/hooks.cpp:264`；不得从最后 Completed 或错误文案倒填首错。

在 `V3MiddlewareEventSink` 增显式 capture 工厂与逐 dispatch lease。工厂先核本场 writer、实际 session/run/turn/step/action、真实 terminal/raw 五键引用及原件顺序；须在宿主单写串行区内调用。首真实 Requested/Skipped meta 绑定 dispatch 和 registry revision，此后不换身份。缺句柄、坏原件、错 owner 或已封 writer 在写账前拒。输入只引用已确认 raw/terminal，不再执行命令或补业务终态。

每次真提交立即缓存阶段、实际 dispatch/invocation/hook 身份、完整原生 WriteReceipt 和当次 writer.broken。Requested、Skipped、Started、Completed、Failed、Cancelled、OutputProposed、ContinuationConsumed、EffectsApplied/Rejected 全按原调用点收。收集只存 refs 和 receipt，不复制大 output、任意 candidate 或命令原文。checked Requested/Skipped 携 terminal/raw inputRef；其它事件仍按原 schema。

内部正整数 receipt 帽限定 1..4096，缺省 256；入册前查容量。超帽留明确 capture gap，不伪造 native receipt。首次真实 Writer 故障可能给 Rejected 并置 broken，原 status/code/id/seq/hash 照留；捕获路后续不再调用 writer 追问，也不补 IoFailed 或成功行。默认未开启收集时仍走原 NoteError/继续投影路径。

capture 不改变 Middleware 的调用/合并/异常策略。native 未确认或录面超帽，表示报告不完整；不能据此追认整次 Post，也不能把真实已知 handler 结果抹成未知命令执行。c1b 宿主结算须优先认这份 gap、停止本票后续采用/重派；本笔不声称即时中断已经在跑的可信 callback。

## 谁拥有，怎样退场

sink 借 writer，宿主保证单写并在真实 Dispatcher 返回、observer 都 join 后先退 sink，再退 writer。lease 只持共享 owned 记录状态，不含 writer、Session、回调或取消旗；快照可在 sink/writer 退出后查，不会写账、Pump 或保活执行资源。

一份 capture 只容一场 dispatch。宿主 Finish 实际 DispatchOutcome 后才封录面；校 dispatch/revision，不接受另场 outcome。封录面、重复绑定、换场或 lease 关闭后拒新阶段；已经入场的真实提交仍可能随后落账，首回执照缓存。Close 不等于 Dispatcher 排空，宿主仍先等 Dispatch/observer 退场，再销 sink/writer；不持记录锁跨 Writer callback。Finish 只说明收集退场，不代表 PostSettlement、真实补料持久采用或 Job 成功。快照分别给实际 outcome 和 capture gap；普通失败不靠 is_error 或文案猜状态。输入引用规范化为已经核准的五键，原调用方多余字段不进入快照。

## 下一笔才接 typed PostSettlement

候选顶层 `ActualCompleted / NoHandlersSkipped / KnownFailed / Unknown`。KnownFailed 另带 typed cause；required/Abort、取消、宿主补料拒绝不混成一类。optional KeepOriginal 或 observer 失败仍可能整体 Completed。零 handler 走真实 no_handlers/no_matched_handlers Skipped，不造一次 Started/Completed；缺 middleware 不是空清单。

补料持久/选择/采用、版本化 cap、Observed/reader 和真实 SDK Job Operation 留 c1b/c2。现 `action_dispatch.cpp:143` 的 in-memory Settle(true) 不当 Job durable adoption；`extensions.cpp:461/475` 仍绑定主 OperationScope，本笔不换它或拼父 operation 五键。

## 原生验收

新源 `tests/unit/runtime/test_middleware_native_receipts.cpp` 固定六 CASE，marker 前缀 `[middleware-native-receipts-path]`：

1. `completed`：真实 FrozenRegistry、Next 与 observer，逐枚 native receipt 对本账 id/seq/hash/owner；旧默认与 capture 禁用路对照事件 payload/顺序，自动身份/时间/hash只按实际引用关系归一。
2. `skipped`：真实空清单、实际不匹配两路，Skipped 有真实输入锚，handler 零次；缺原件/能力拒，不伪造调用。
3. `failure-policy`：required Abort 与 optional KeepOriginal/observer，保真实 DispatchOutcome、失败原件与实际下游次数。
4. `effects`：真实 proposed/continuation/applied/rejected 回执，补料只证 Hook 效果事实，不冒充 Job durable adoption；超帽录面留 gap。
5. `writer-fault`：真实 V3Writer 注入口，首 Rejected/broken 缓存原件，后续零补调用；完整原生状态不伪装同枚 IoFailed，不重跑 handler。
6. `owner-lifetime`：错场、错 turn/action/input refs、重复绑定/Finish 拒且零 append；sink 及 writer 退后 owned 快照仍可读，关闭/晚回调不留借用。

三平台远端核完整 argv、六实际 marker、非零 CASE/断言、JUnit/LastTest 与 ASan 精确来源。新头未跑原生前，只报静态候选。目录与 CI/CMake 登记由根代理收并集；合同先提交，生产二审后再送远端。
