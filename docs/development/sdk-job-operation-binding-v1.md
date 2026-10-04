# 内部 Job Operation：实际绑定与只读 Hold

本笔从 `2c37602047b7387cd4280d88a948d7b8a5e02625` 起，分支 `codex/sdk-job-operation-binding`。先提交本合同，再写实现。只交私有绑定与严格历史读面，不开放 SDK Jobs、Accepted、Detached、审批、模型派工或自动恢复。公开 Close、父取消与 Job 会话同意域仍待用户定；child-only 授权和显式 child 预算不授 Job 权。

## 实际入口与归属

`src/sdk/job_operations.hpp/.cpp` 提供内部 owned 值表。实际登记入口只借这次 `SessionService`、`ToolJobCoordinator` 和 Job ID；Job ID 只选本 Coordinator 当前真实 record。调用方不能传 SID/run/op/action/attempt DTO，不能从历史卷恢复现场许可。值表退出不留 Service、Writer、Coordinator、工具、取消旗或回调裸借用。

Coordinator 新增中立同步核口 `WithOwnedJobBindingSource(actual_writer, job_id, consume)`。沿真实 `writer_serial → jobs_mutex` 核 Writer 同指针、当前实例/epoch、Open、已 adopted 且 `parent_delivery_pending`、未 dispatched/Started/terminal、无 gap。交 owned Prepared facts 与首次 adoption 回执。释放 jobs 锁后才进内部 consumer；serial 持到 binding append 返回。既有 callback guard 挡递归关闭，不跑 scope/Post/Gate/clock/Pump，不发线程。这个核口不签执行许可。

SDK producer 从同份 Service 取实际 Writer，读真实账前缀并核 tail seq/hash。父来源须有实际 `BeginMainOperationTurn` 所产 `sdk.operation.turn.bound`；调用真实 `CheckMainOperationTurnBindings` 核 operations.jsonl accepted/dispatched/规范输入摘要与后来 final。由业务 turn 找唯一父锚，不猜 active/最近 operation。父 op 只记因果，Job Operation 单列；主 op 仍一 turn 一 op。

本笔不把 `BeginMainOperationTurn` 接入默认 core Pump/Run，不动旧 SDK/CLI Pop、NewTurnId、Complete 或默认账面字节。原生 fixture 调实际 Service producer，不手造主锚或填 caller 身份。

## 冻结事实与首回执

新增 statusless `sdk.job.operation.bound`，布局 `session_owned_job_operation_v1`，version 1。真实信封必带本业务 turn/step/action，不造另一模型 turn。payload 恰含：`layout`、`version`、`jobId`、`tool_call_id`、`attempt`、`parentOperationRef`、`assistantMessageRef`、`sourcePendingEventRef`、`sourceAdmissionEventRef`、`preparedPendingEventRef`、`registeredEventRef`、`adoptionEventRef`、`originalInputSha256`、`effectiveInputSha256`。attempt 只收 1；引用只收规范五键对象 SID/run/id/seq/hash，原/有效参数分列，不把改参当错原声明。

所有引用从实际已验证行生成。parentOperationRef 指真实主锚；其它六引用逐枚对真实 registration/adoption 材料。绑定同 SID/run/turn/step/业务 Action，必须晚于 adoption、早于本业务 Dispatched/Started。父锚在真实声明之前。ToolIdentity、策略、命令限额与 cwd 从真实 adoption 读，不复制另一套校验器。

Job Operation ID 只在真实 append Committed 后从该枚 event ID 派生 `jobop-<eventId>`。历史读面沿同一规则派生，不把父 op ID、provider call ID 或登记号当 Job Operation。它不授执行、审批或 Post 权。

每次绑定只 append 一枚，档位 PowerLoss。值表先占稳定记录与容量，再进真实 append；首次 native receipt 原样缓存。阶段区分 `RejectedBeforeWrite`、`Unconfirmed`、`CommittedPublicationGap`、`Bound`；native Rejected 若 writer broken 仍属未确认。实际已写后的拒绝不能洗成零写。首 append/发布 gap 保留，同票不重登、不读回升级、不 Confirm、不派工。缺原生回执不造 IoFailed 或 OS 故障。

发布回调只是内部测试/宿主同步发布口，在 writer/jobs 锁外调用。异常保首 Committed 与 publication gap；不能重写绑定迎回执。值表容量计入未确认记录。纯 Snapshot 不读文件、不 Pump、不调用 clock 或回调。内部 Close 停新绑定、等本次在途 Bind/发布退完、保 owned 快照与首错；不替用户决定进程取消。同步发布内递归 Close 明确拒，不能自等。

## 严格读面与恢复

V3 envelope/schema3/实际 `ReadV3Ledger` 与 Python shape validator 同笔登记；不改 Writer。Reader 先调用现有真实 registration/adoption 与主锚校验，再核本事实引用、摘要、顺序和唯一 Job/业务 Action/operation ID。坏 owner、坏 hash、错 turn、错引用或重复绑定须明确拒，不能最后一枚赢。

SDK strict 历史入口还须调用 `CheckMainOperationTurnBindings`，核同份 File operations 来源；普通 V3 Reader 只核本卷材料，不假称它读过 SDK 操作台账。恢复只交 `PassiveHold` owned 值，无现场租约、Writer 或 Coordinator，零 append、零派工、零 Post、零修账。未绑定旧 Job 不补 operation；没有新事实时保持 NotApplicable。合法尚无父 final 的绑定前缀可读，不合成主 final 或 Job terminal。

整场仍由 File V3/operations/result store 持账。本笔不添数据库、Journal factory、第二条运行栈或后台线程。

## 来源与验收

生产：`src/sdk/job_operations.hpp/.cpp`、Coordinator 两件、V3 envelope/schema3/reader、`scripts/validate_trajectory_v3.py`。新册 `tests/integration/sdk/test_lubancore_job_operations.cpp` 固定六 CASE；末标 `[sdk-job-operations-path]` 各一次：`source`、`gap`、`history`、`relation`、`isolation`、`lifetime`。根代理管 CMake/CI/目录门，本笔不改这些交叉文件。

六路用真实 Service 输入接纳/Pop/主锚、真实声明与 Coordinator Register/Adopt：正向绑定与原/有效参数；首次 Writer 注入未确认和真实发布 gap；checked Close/Continue 的 PassiveHold 与旧无绑定场；真重 hash 坏关系先 Verify 再 Reader 精准拒；同项目二场加异项目二场；真实在途发布退场与纯查询零 Pump。绑定前后 executor/Gate/clock/thread 为零；旧来源、册数、预算、断言与默认路径保留。

本地只做文本、Python、AST 与文档检查。新原生来源交三平台远端，取完整 argv、非零六 CASE/断言、marker、JUnit 与 LastTest。父支绿不替新源验收。
