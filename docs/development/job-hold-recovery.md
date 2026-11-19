# 显式 Hold Recovery：中立小笔候选

本笔先定合同，随后实现。起点是启动事务 `e944c56b`；它的独立远端 CI 尚在跑。本笔只添中立恢复策略，供下一批 SDK Jobs 使用。不开放 SDK 后台入口，不决定用户尚未答复的 SDK Close 规则。

## 发布基线与组合门

实现 `b2833406` 已在私有线收好。发布前先用普通 merge 接 SDK 组合 `373df8d894239dcbbd3a69a6ca17af3bde4a14d4`，叶 PR 改以 `codex/sdk-completion-integration` 为 base；共享 feature/main 不动。此处先提交合同，再合代码。

本笔五份生产改动、新六册与旧 16/启动 6 册断言保持原文。组合只收上游已落实现、夹具和证据门；冲突取真实并集，不拣回旧 API，也不删旧来源或完成标记。发布门拟为 SDK focused 35 来源、ASan mandatory 40 来源、installed consumer 25 模式，合后须核实际名册、source 归属、完整 argv 与原 Resource Lock。

这些数字只定新头验收范围。上游局部绿灯、旧独立头静核或运行结果都不算 Hold 新头通过。实际受测 merge parents/tree、九份 FileAPI、三平台原件和 ASan 原件另记；本地不 configure、编译或跑原生。

## 现有路径

### 现有 Producer 的声明缺口

`StartJobCommon` 现向真实 `ToolActionSession::Admit` 传 `provider=nullopt`，只写 `toolName`。`JobStartRequest` 也未带原 provider call-id。完整 assistant 调用块虽在账里，共用 Fold 仍无法按空号取出那枚声明的工具名与参数；不能按名称猜一枚，也不能补假参数。保留重试 Pending 的已有 ID，并不补齐这种从未落号的旧账。

本笔 Hold 只采用来源材料完整的真实账。现有 producer 这类缺件明确报 `job.recovery.invalid_source`，不发布恢复记录，不执行、不补账；它不等于 SDK 后台能力已交付。后续显式 owned producer 须持可信 provider ID 并在新入口落稳；Legacy producer 尚未迁移，本笔不改它。

原六册内另添真实 `StartJob → Wait/Shutdown → Close → Read/Continue → PlanHold/Adopt` 负验。assistant 声明沿实际 Writer 完整落账，原工具真执行一次；关场后重读，确认声明材料缺失，恢复明确拒绝、executor 为零、原卷逐字不动。不得手补恢复字段绕过严门。此节先提交，再落这段回归；`7ebd3f33` 及此前旧轮各留各的原件，新头独立验收。

### 首轮失败与共享折叠修补

源 `023e635c` 的鲜 CI `37102791017` 已真跑新册。Linux SDK-only 原件里六案四过两败；两个合法业务 attempt 2 窗都在 Adopt 报 `job.recovery.invalid_source`。原卷已过 Reader 与 Continue，不是坏卷，也不能放宽严格准入。

`BeginNextAttempt` 只写本 action、attempt 和 reason。共享 `FoldToolActions` 原路却在每枚 Pending 缺 provider 字段时冲掉首次声明号，后续声明配对失去工具名。本次只收这条读取缺口：实际带 `provider_tool_call_id` 或旧别名 `providerToolCallId` 才更新；后续缺键继承已存声明号。首次缺号保持旧缺省，不新猜调用。writer、旧恢复重派、Hold 准入和持久事实都不改。

原六案增核真实 Fold/Plan 工具名、声明参数和首次 provider 号，并用真实 Writer 验旧别名跨 attempt 2 保留。原断言、预算和六条完成标记不减。旧 `023e635c` 失败原件独立封存；修头须另取三平台与 ASan 原件，不重跑旧轮，不拿源码静核抵验收。

`ToolJobCoordinator::PlanRecovery`（`src/tools/tool_job_coordinator.cpp:1575`）从真 V3 卷折 Job、Action、接单消息和结果。未 dispatch 的 `job_handle` 可成为 `requeue`；有 dispatch 而缺确认终态成为 `unknown_hold`。接单 attempt 1 完成，不等于业务 attempt 已执行。

`AdoptRecovery`（同文件 1704）会补接单/观测、压旧 Job 入队，最后调用 `TryDispatchLocked`。只把 executor 暂时设空，挡不住恢复中间窗，也会改变旧失败含义。本笔不走这条旁门。

`AsyncToolRuntime::RestoreFromLedger`（`src/runtime/async_tool_runtime.cpp:540`）读卷，调用上述 Plan/Adopt，再调用结果投递规划器。规划器（`src/runtime/result_delivery_planner.cpp:242`）只恢复 `native_deferred` 的投递欠账；它不重跑业务，却可补投旧消息。显式 Hold 路必须同时跳过这次旧账投递恢复，不能称“只读”却又自动补消息。Legacy 路保持原调用。

## 最窄接口

内部增加 `JobRecoveryPolicy { Legacy, Hold }`，默认 `Legacy`。`PlanRecovery(verified_ledger, policy = Legacy)` 产出 owned 计划，计划持策略值；`AdoptRecovery(plan)` 消费同一份计划。策略随 `AsyncToolRuntimeOptions` 传入原 Restore 入口。没有 executor/global registry 开关，没有另一套恢复器。

Hold 采用旧记录时只建本 coordinator 的 owned 投影。保真 job/action/turn/step/attempt、dispatch 与终态事件、结果引用、取消请求和投递缺口。不得新写 Registered、Started、Observed、ToolMessage、ContextInput 或模型请求。不得补旧投递，不读模型参数伪造来源。旧 job 编号仍占本场命名空间；新 Submit 必须发新键。

每份恢复记录添独立 `recovery_held` 和 typed `recovery_knowledge`。它们是内部投影，不能新增 V3 业务状态 `held`，也不能把历史未知写成 Cancelled/Failed。建议知识值如下：

| 知识值 | 真材料与公开内部投影 |
|---|---|
| KnownNotDispatched | 真注册/queued/awaiting，dispatch=0；保原状态，明确业务未执行，不冒完成 |
| ExecutionUnconfirmed | 已 dispatch，缺确认业务终态；投影 unknown，保实际执行 attempt/来源与缺口，不写合成终态 |
| TerminalDeliveryGap | 真业务终态或原文已落，接单/Observed/投递缺口仍在；分别保原事件与结果，不能填成完整确认 |
| TerminalConfirmed | 原终态和投递事实齐；读原结果，不重新派发，不重新捕获 |
| UnsupportedMode | 原模式不适用本批；留真实 mode 和缺口，不转成 job_handle 或自动执行 |

知识值必须从本次真卷推出。保 Fold 原 state，分别核真实业务 attempt 的 Started/terminal；不能只看 `dispatched_count==0`，不能把 attempt 1 接单 done 当业务已结束。业务 attempt>1 的 failed/cancelled/unknown 都须参与知识分类，不能沿旧 `observed_missing` 仅认 done 的判据漏掉。terminal execution、observed state 和 admission completeness 分开存；不能凭字符串 `succeeded` 猜全部链已确认。TerminalConfirmed 仅指这套真实业务/观测/接单链齐，不声称全卷或 provider 已认证。Hold 记录不占 running/queued quota，不进入 dispatch queue，不持 worker，不借上一进程捕获或 executor。

成功执行但业务结果落仓失败，Observed 仍可报 succeeded；这份报告须留 TerminalDeliveryGap。确认成功须有同一实际业务 persisted 事件和 observed resultRef。失败、取消原路不强求正文结果仓。当前 admission 齐否沿共用 Fold 的 current-chain 投影，不能从它宣称全历史采用已独立验证。

## Get / Wait / Cancel / Close

`GetJob` 沿原授权门取 owned 快照，明确上述 held/knowledge/来源。无键与拒绝沿旧路。`WaitJobs` 保原“等业务终态”口径：已知未执行的 held 注册项到点返回 timed_out，状态仍未完成；unknown/真实终态按旧 terminal 判据返回，同时带知识与缺口。satisfied 从来不等于执行成功。新 Job 仍按原时长和完成泵运行。

`src/tools/job_tools.cpp` 复用这份值，只在恢复字段有值时给 `job_get` / `job_wait` 添 `recovery`：policy、knowledge、原 state、turn/step、dispatch 次数、真实业务 attempt/Started/terminal 与 admission 齐否。Legacy JSON 不添字段。字段不许可重跑，也不把知识枚举推成业务成功。六册须真调两枚工具，核这份 JSON 投影；不能另写同逻辑镜像冒作验收。

`GrantApproval` 对 held 旧项明确拒绝 `job.recovery.held`，不改 state、不压队。`CompleteAdmission`（同文件 1310–1328）也是旧欠链补账入口：held 未齐项明确拒；已齐项只返回原回执，不调用 WriteAdmissionMessage。`CancelJob` 首笔也明确拒绝 held 非终态项，不新写过去执行结局或取消意愿；真实已确认终态可沿 `already_terminal` 只读回答。下一公开 SDK 若要允许写取消意愿，应另有合同，不能把这批只读 Hold 偷换成那个产品入口。

Pump、TryDispatch、取消和关场各处都检查 `recovery_held`；不能只在 Adopt 末尾漏调用。新 Submit 与新 Job 批次不受阻。关场沿现 owned worker 真 join/完成泵/capture 锁外清理，旧 held 项没有 live worker，不触发 shutdown 合成 Cancelled、补投递或失败收账。生命周期清理成功可关闭；这个成功只说明本次实际 owner 已退，不补齐旧 Job 持久确认。历史缺口仍留在 retained 投影里，不能从 Shutdown bool 推出旧业务已确认。现关闭后 Get 的 `coordinator.closed` 保留；未来 SDK 若需闭场查询，应由 SDK 缓存 owned 值，不能回用已关闭 writer。

## 采用与所有权

策略必须在任何旧记录可能入队之前固定。它不覆盖持久 JobExecutionPolicy.resume_policy，不偷改那个字段旧含义。Hold 的分支仍用共用 Fold/Action 对齐，不复制事件 parser。采用应先验证整份计划/身份，再公布 held 记录；重复 Adopt 同键不得降级现新 Job 或重置已派发 owner。已有 live/new Job 不许被 Hold 计划覆盖。

Async Restore 在原 session 独占 owner 下执行。新增策略只约束恢复记录，不是 session 广域“禁 dispatch”。Hold 下本次恢复不调用旧 ResultDeliveryPlanner Restore；新 completion notice、实时 planner、主轮正常接单保持原路。`native_deferred` 本身不在首批 SDK Jobs 能力里，本笔不宣称它已可安全恢复采用。

失败不补假确认。坏卷/Action 来源缺失照现读取边界报错；不得 silently requeue。不得让 held 旧 item 被 completion mailbox 或 `DebugSubmitEnvelope` 当成本次 worker result。审批迟票、旧 provider call-id 与旧 job ID 不能解除 Hold。没有 Resume/Retry 旧 Job 入口。

## 远端验收候选

固定新来源册，实际用原 V3Writer、ToolActionSession、coordinator 和 AsyncToolRuntime；源码计数不能抵原生执行。每个成功检查有唯一 marker，focused/ASan 校完整 argv、JUnit、LastTest、非零断言。

新来源固定 `tests/unit/tools/test_tool_job_hold_recovery.cpp` 六册。独立实现起点原定 SDK focused 28、ASan mandatory 33；发布改接上述组合后，按 35/40/25 实际并集收证。原 16 册与启动事务 6 册保留。六条完成标记分别钉住注册/接单缺口、新旧隔离、派发未知、四种业务终态、Async 传播、Legacy/身份门。它们只在断言走完后输出，本地文本计数不当原生通过凭据。

1. 真 registered 未 dispatch 窗：Hold Get 为 KnownNotDispatched；Pump、Wait、Grant、Cancel、Shutdown 后旧 executor=0、旧卷逐字不动；Wait 到点，不能假终态。
2. 真 queued/awaiting 两窗：Hold 不压队、不占 quota；新 Submit 成功且实际 executor=1，只归新 action；旧迟票不启动。
3. 真 dispatched/Started 无 terminal 窗：Hold 为 ExecutionUnconfirmed；零旧 executor/模型，保真 attempt 与 dispatch 来源，不合成 Cancelled/Failed；Close 仅退本次 owner。
4. 真 terminal 齐全与 terminal delivery gap：done/failed/cancelled/unknown 实际业务各分支、仅接单 attempt 1 done 都须覆盖；旧结果/来源不改，gap 不自动补接单、Observed 或消息。调用 CompleteAdmission 的旧欠链仍不写账；两者均不重跑，不把接单 attempt 当业务成功。
5. Async Hold 真传播：恢复旧 registered 和旧 native_deferred 投递欠账时零旧调用/模型/消息新增；同 runtime 接一个新 Job，沿原工具执行与实时通知成功。
6. Legacy 默认回归：真未 dispatch registered 仍按原重排并执行一次，unknown 仍沿原观察路径，已终态不重跑；保旧 16 册和启动事务 6 册、原预算、quota、factory/Close 验收。

不做公开 SDK Job API、跨场身份/恢复授权、重试、数据库、后台子 Agent 或 Detached。根与第二审已读过合同；在独立 managed tree 先提交本合同再实现。所有原生只在远端三平台与 ASan 跑；本地仅静读、纯数据和文档检查。
