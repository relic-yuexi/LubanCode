# 公共 SDK Command Jobs 首批接线合同

实施基线：`6900159bdc8731da19a7e33fab8436f800919d4d`。本笔沿 [SDK 后台 Job 装配合同](sdk-background-jobs.md)，交 Session 自持的 Command Jobs。下面列出实现须守的接点；远端新源验收前，接口仍属候选。已封 deadline 只在调用前扣剩余预算并收窄相对 process timeout；启动、kill、捕获、join 仍沿平台原语，不声称硬实时整场截止。

## 1. 公开面与模型请求

拟加 `include/lubancore/jobs.hpp`，只依赖 API、STL；`SessionOptions::command_jobs` 为 `optional<jobs::v1::CommandOptions>`，新场缺省 off。配置冻结并经 opening participant 以版本／摘要绑定 V3 初始及后续 system，恢复沿锁内原件复验：省略继承旧计划，显式值须匹配；旧账无计划保持 off，不能靠这次 resume 添后台执行权。

必填正值：`registration_timeout_ms`、`command_timeout_ms`、`max_output_bytes`、`max_running`、`max_registered_jobs`。时间沿现有命令上限且 command 帽不大于登记预算；转换溢出先拒。`max_registered_jobs` 初批 1..64，计整场已登记项（含历史、已终态和保留缺口），不是只数排队项；这对应现 `RegisterPreparedJob` 用 `prepared.size()` 计容量。并发帽不保证并行：未知外部副作用保留协调器资源互斥。同项目多场共用项目文件，不宣称文件隔离或 GPU 调度。

必须同时显式选择 `builtin_tools={...,"run_command"}`。仅开启配置不改变普通命令。模型只用已声明的 `run_command`，SDK schema 增加：

```json
{"command":"...","execution_mode":"session_job","job_budget_ms":30000}
```

`execution_mode` 只收 `foreground`／`session_job`；缺省 `foreground`。`job_budget_ms` 只准用于 session_job，缺省取宿主登记预算；给出则须正整数且不大于宿主帽。模型已有 `timeout_ms` 继续只收窄实际命令帽。off 时收到这两个后台字段、未知 mode、超帽、空值或错类型，明确拒绝，零命令调用；绝不 inline 兜底。现 `run_in_background`／`max_runtime_ms` 仍属 CLI Detached，SDK 不换义、不放行。

首批提交沿既有 `Session::Submit` →真实 Backend tool call；不添可凭 caller 提供的 parent_operation_id 造来源的裸 `StartJob` API。拟加 Session 查询：`ListJobs(optional parent_operation_id)`、`ReadJob(JobIdentity)`、`WaitJob(JobIdentity, timeout)`、`CancelJob(JobIdentity)`、`ReadJobPreview(JobIdentity, max_bytes)`。身份持 session/run、Job ID、真实 Job Operation ID、parent Operation、turn、action、attempt；列表只出本 Session 值。`JobView` 分开 execution state、cancel_requested、owner_available、recovery knowledge、原回执缺口；“已接单”不写成后台成功。

## 2. 一位 owner、一条真实准入链

拟加 `sdk/command_jobs.hpp/.cpp::SessionCommandJobs`：SessionExecution 的资源附件持它，内部持冻结计划、`JobOperations`、父→Job 索引、Job 审批域和有界查询缓存。复用 `SessionService`，并经现 `SessionRuntime::AttachAsyncToolRuntime` / `ShutdownAsyncTools` 管唯一真实 coordinator。Owned 模式接新的 typed admission 回调；Legacy `TakeJobOrder`、旧 planner 和 native-deferred 路不接新票。附件不会靠析构才停线程：SDK Close 显式退 Bind 和审批后，调用原 Shutdown 链，附件最后释放。

每张票单独建 `shared_ptr<RunCommandTool>`、冻结选项／有效参数、校准权限值和受限 Post 状态。worker 只借其自有 capability 与 coordinator 取消旗，活到真实 join；不从 registry 抽裸指针，不借 backend、父 ToolContext、TLS、临时 hub／bridge／wiring，不强捕获 public Session 或整个 Impl。capability 对 coordinator 只留 weak 回指；Open/Draining/Retired 借用撤净后回调销毁，不能出现 coordinator→cap→Session→coordinator 环。

唯一入口拟为 `SessionCommandJobs::AdmitDeclaredCommand`。顺序固定：

1. AgentLoop 完整 assistant 和原父 declaration/Pending 已真实落账后，冻结 `V3DeclaredCallOrigin` 与真实 main Operation binding；只在此同步栈借 bridge。不能拿公开字符串、预留消息号或伪调用当来源。
2. SDK 纯 normalizer 先将省略／空 cwd 补为冻结 Session cwd。初批 Job 要求最终 cwd 等于这份 owner cwd；其它目录明确拒（前台原规则照旧）。保留原声明 input；对准备副本调用真正 `PrepareOwnedToolInput`，权限看规范化后的命令，登记同时保存原／有效摘要。新的 mode／budget 值亦入原件；执行帽另走 context，不覆盖模型原参数。registry 的实际 source_instance/version/cwd 必须补齐并核对，不能在登记时现编另一枚工具身份。
3. `RegisterPreparedJob → AdoptPreparedJob → JobOperations::Bind`。Bind 必须 Bound 且原生回执确认；Rejected 零派发，Unconfirmed／publication gap 封本场新副作用，保存首回执、先取消意图、不重登记。
4. 拟加 `TrajectoryTurnBridge::CommitOwnedJobAdmission`，用 bridge 已持的原父 action 写接单 Start/Finish、实际 handle artifact、Persisted/Selected、tool message 与 AdmitMessages，返回真实五引用。它须更新原 call book，只完成一次；不在 bridge 之外重开相同父 action，不把后台业务 action 交主轮 FinishTrace。
5. 再 `ConfirmParentAdmission`，其核真 artifact 后才可 Dispatch。接单块标记已持久化，沿现 `job_admission` 配对路径入模型内存，防批尾再落第二套结果。Accepted 只在上述链成立时返回；已确认接单但随后到期，仍如实是已受理后取消。重复来源返回原 owned 票／原缺口，绝不再次执行。

对应实际缺口在 `agent/loop.cpp` 的 `owned_admission` 分支：现在写死 `MissingOwnedJobAdmission()`；`PrepareOwnedToolInput` 也先拒带 Action/Post 的 wiring。须新增 typed 完整接点，保留默认缺能力拒绝，不能只给 Async tools 白名单添 run_command。

## 3. 审批、Post 与完成泵

拟加 `SessionApprovals::RegisterJobScoped/JobAllowed/CloseJobScope`。准入审批尚无 Job ID时，以真实 session/run/main Operation/turn/父 declaration/provider-call 组成候选域；采用后单向绑定实际 Job 票。此值不代替真实来源核对。ApproveSession 只记此 Job 域；不读父 `AllowedTools()`，不放行兄弟 Job 或别场。发布审批／等待都在 coordinator 锁外；派发 `scope_gate` 只核已持审批值、实际 input/identity/policy、撤销与父取消旗，绝不回调 UI 或等待未来。晚答、参数漂移、已退候选域均拒；关场先唤醒审批。

首批启 Job 且存在显式 PreAction/PostAction 声明时，在 Session preflight 明拒，早于扩展 factory 和新场写入。原因有两处实码：`PrepareOwnedToolInput` 尚不接受异步 Action/Post；`sdk/extensions.cpp` 的 Action adapter 强核 `SessionExtensions::SetOperationScope` 的当前 main Operation，不能借给后台。其它 hook 不因此取得 Job 身份；child／workflow／Managed 未接域明确拒，只有真实 SDK main declaration 可交单。

Job Post 使用固定、真实声明的内置 handler `sdk.command_job.verify.v1`，不调用当前 main Action dispatcher。入口走 `OwnedJobCapability::live_post`；先由 `CheckOwnedPostInvocation` 核当前真实 record/thread/epoch/phase，再交 handler 核 `JobOperations::Snapshot` 的真实绑定和 Started/Terminal/Persisted 引用。执行 `HookDispatchSession::Dispatch → BeginInvocation → 实际校验 → CompleteInvocation`，返回原生 receipt；校验或写失败保原错误，不能造空成功 Post、伪 Completed，亦不能将 capture complete 称 durable success。outer `SettleOwned` 仍用 `CheckOwnedJobPost` 核来源，再写 Observed。新 handler 没有用户 callback／observer，不能截短未来插件合同来冒称兼容。

完成泵复用 Session 单只 host worker：在 batch boundary、父 `Complete` 前后与空闲 `Impl::Pump` 中调用 `SessionCommandJobs::PumpAndPublish → coordinator.PumpOwnedJobs`。有未完 Job 时空闲 CV 最迟按短拍醒来（沿现 10ms 轮询尺度），无 Job 则等输入／关闭信号。command worker 只投完成信封，不写 V3。每次泵先冻结真 native 状态／结果索引，再锁短缓存并唤醒 Wait，最后锁外发状态事件；慢事件订阅不拖住 writer。

`WaitJob` 只等 owned 缓存，不能调用 `GetOwnedJob`（会 Pump）或从外部线程重入 writer。父 Backend 或合作工具仍阻塞时，完成结算可推迟，Wait 可超时；回 Session worker 后继续收场，不装成独立完成调度器。与主桥共用 `v3_tool_results_mutex()`，前缀校验／采用／确认这些短事务连续持租约，锁不跨模型、审批或用户回调。首批不另起同时写主账的泵线程；保现主 Operation 结果索引排除 Job business action 的规则。

## 4. 取消、关场、历史与 preview

须补 coordinator 内部 `RequestOwnedCancellation(owner, job_id)` 非写盘入口：校 owner 后只锁短表、立取消旗、保首 reason 并唤醒；不先检查 writer、不等 scope gate。现 `CancelOwnedJob` 会先看 writer 可用，不能单靠它兜账坏时停进程。宿主随后在唯一完成泵补真实 CancelRequested／取消终态；写不成保 Unknown／gap，不借取消洗成成功。

`Session::Cancel` 和父 `Complete` 维护同一父停止 latch。取消活父，以及父最终 Failed／预算耗尽／Indeterminate，先封新后代，再向其未完 Job 立旗；Confirm 前后都复核，覆盖登记与取消竞态。成功父不取消 Job，父已结束后显式 CancelJob。只取消所指 Session/父域；迟到取消不改已知成功、失败或未知。

Close 次序：封 Submit／Job准入 →立全部 Job 取消旗、醒审批与 Wait →退实际 Bind／publisher 借用并 `JobOperations::Close` →主 worker 停轮 →原 coordinator `Shutdown` 真 join、完成 Post/Observed或保首 gap →冻结最终查询值 →关 writer/Service →释放 capability/附件。Runtime 与最后 public handle 共用此序。不能 detach 或以 timeout 冒充 join。writer 坏仍收真线程；不同 SDK callback 的阻塞 Close/Wait 沿现重入门拒。

恢复锁内复用 `ReadJobOperations`、`ReadOwnedJobAdoptions` 与 `PlanRecovery(Hold)` 做值快照；不向新 prepared coordinator 调 `AdoptRecovery`（当前明确 return 0），不重建旧 clock/取消源/线程或续 dispatch。已有完成件可立即读；历史非终态的 Wait 返回 `sdk.job.owner_unavailable`，Cancel 也不能伪称已停远端进程。新开票只由新 live owner 受理，旧票永不复活。

拟加 `IndexCommandJobResult`，按真实 Job binding→business action/attempt→terminal/persisted/Post/Observed 核关系，再复用有界 artifact 读件逻辑。`sdk/results.cpp::IndexToolResults` 当前特意排除这些 action，不许直接拿 main Operation ID 套进去。`ReadJobPreview` 初批只读本场已核值，max_bytes 至多 4096，报 result identity、capture_complete、preview_truncated 和 artifact gap；无命令／无 raw 明报 unavailable，不编空成功结果。事件只发身份与状态。初批无 Full Job API；以后 Full 必须复用 Node 许可＋每场参数及 `ResultProjector`，不能从 main 结果接口漏出。Close 后缓存不留 writer；same-ID 恢复重核原件，坏引用、越场、链接／缺件按既有严格规则拒。

## 5. 一批交付所需源码与远端验收

| 必须落到的入口 | 最小改动 |
|---|---|
| `core.hpp`、拟增 `jobs.hpp`；`sdk/core.cpp::Initialize/Run/Pump/Complete/Cancel/Close` | 显式计划、唯一 Job owner、缓存 API、父停止与真实关场；不是第二套 Session 栈。 |
| `sdk/adapters.cpp::LocalTool`、`agent/async_tool_seam.hpp`、`agent/loop.cpp`、`runtime/async_tool_runtime.*` | schema mode、直接 execute 拒后台绕行、typed owned admission、普通前台不变。 |
| `runtime/trajectory_turn_bridge.*` | 原父 action 的唯一接单持久化及五引用；既有 inline 轨迹原样。 |
| 拟增 `sdk/command_jobs.*`／`command_jobs_opening.*`；`sdk/approval.*` | 冻结计划、独立 capability、Job审批域、真实内置 Post、preview／Hold；复用 JobOperations。 |
| `tools/tool_job_coordinator.*` | 非写盘逐Job取消 latch，旧 Legacy／0预算默认原义；现deadline及startup清理保留。 |

安装消费者拟增 `examples/sdk-consumer/command_jobs.cpp`，只含安装公头、STL 与现受控命令 probe；移位后消费。真实 native／三平台 SDK-only、组合、Lua ON/OFF 与 ASan 核以下路径，册数按最终实际 CASE 定，不先凭合同报通过：

- off／非法mode／CLI Detached字段／Action组合拒绝，普通前台仍真跑；有效 mode 只接原声明，模型下一请求只见一份真实接单 handle。
- 父成功后不再 Submit，Job 仍完成、preview 可读；父 Backend 卡住时 Wait 超时，释放 Backend 后只结算一次。
- 真占槽与排队过期、模型收窄／宿主帽、late worker零command、工厂发布前后抛错；原六 deadline 案仍跑。
- 同项目双场、异项目双场；兄弟Job审批互不放行，旧票／错 Session／参数漂移拒，模型／权限／取消不串。
- 活父取消、父失败／步数耗尽／Unknown、单Job取消、Close/最后句柄/Runtime关场；核真实 started/done、进程退出、线程join、配额与捕获析构，保首原生 gap。
- 真实 Bind-before-Confirm；用 native writer 故障核 Register／Bind／父交付／Post 不确定后零重派，账坏取消先停真进程。
- Close 后与 same-ID Hold 读回真实结果；旧非终态零新进程，缺／伪／冲突／错序绑定拒；preview截断与损坏附件分别报，不退成任意同文本匹配。

此整片只交 Session-owned Command Jobs。Detached、递归子Agent、后台 SDK Action、跨节点 owner 交接另留后笔。实现与 CI 验收沿同一份合同，不降格为私有模拟通路。本地只查文本与纯数据；configure、编译、CTest、原生及 HTTP 全交远端 CI。
