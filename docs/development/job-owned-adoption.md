# Owned Job 接管与执行：J2b

合同先落，代码随后。本笔从 J2a #312 分出私有分支，吸收 #310 的真实命令限额实现；各轮仍凭自己的三平台 CI 收账。本地不配置、编译、CTest 或执行原生程序。公开 SDK 的后台门仍关；本笔只接内部命令文本 Job，不接 Detached、Runner 或远端 Full。

## 两份 Action，两个事实

父 Action 记实际接单句柄，Job Action 记实际业务。Job 业务从原 Pending 的 attempt 1 开始。不先写假 done，再开 attempt 2。原 `preparedOnly=true` 的 Pending/Registered 不回写，不补第二枚 Registered。

添独立无 status 的 `tool.job.adopted`，布局固定为 `parent_admission_job_business_v1`。写入真实 Job ID、业务 Action/attempt 1、父 Action、SID/run/turn/step、原 provider/assistant 来源、原 Pending/Registered/父声明采用引用、original/effective 入参摘要、ToolIdentity/Policy、owner/epoch/project/cwd。来源由同份已验账反校，不能拿传参互相证明。布局事实不是权限票，也不是 Dispatched。

同笔更新枚举、单行 schema、共用跨行校准、Fold、协议欠账与恢复计划。Job 的 provider ID 只指因果来源，不能把父调用映到 Job，不能欠模型第二份原调用回复。未采用的 prepared 窗也不能抢父 provider 映射；实际共用 Action 投影须让 history、resume、compact 与协议账沿同一规则处理。业务结果仍归真实 Job Action。

## 接管先留票，父句柄齐后再派

`AdoptPreparedJob` 仅接本协调器活票及完整 owned 执行能力，核实际源账与 writer 尾 seq/hash，落 PowerLoss 采用事实，回原 native receipt。成功先留 `parent_delivery_pending`；尚无父句柄链，零 worker。重复采用、跨场/实例/cwd/epoch、撤销/坏账、缺 scope/Post/限额均拒；部分写入或破 writer 留 Unconfirmed，不自动重试、不退 inline。

`ConfirmParentAdmission` 只查实账。父真实终态、formal persisted/selected、tool message 与 ContextInputApplied 须同 Action/attempt/provider，顺序成立，真实句柄正文指同 Job 与采用事实。a 的 sourceAdmissionEventRef 只是 assistant 声明采用，绝不是句柄采用。调用方 refs 不能自证；这里不写父结果、不补采用链，也不等下一次模型请求。父链缺件或未知就留票，零派工。

父链齐仍须复核关场、活 epoch、冻结 input、权限能力与取消。宿主回调在 jobs_mutex 外调用；回来沿 writer serial → jobs 锁序再查，不将一次预检查当永久许可。关闭时这两口永久拒。

## 真拥有，真执行

每票持有执行体、实际业务 scope 校验、Post 收件能力、自有取消源、正数 `CommandExecutionLimits`。只接 `run_command` 命令文本；冻结的 effective input 实际交执行机，宿主限额覆盖本票真实进程捕获与超时。旧两参 Gate、bool allowed、裸 Tool/Registry/Session 指针均不能补 owned 能力。可信宿主须给捕获拥有者；中立层不 include SDK/AgentLoop/TurnWiring。

业务上下文取实际 SID/run/turn/step、Job Action/attempt、owner/project/cwd。父 operation 只存因果资料。未有公开 SDK Job Operation 就不造 operation_id，不借父 active bridge、OperationScope 或父取消旗。scope/Post 缺任一项零派工。SDK c 的真实五键和 Action 适配另接，J1 的拒绝门不动。

复用现有 stable WorkerLaunch、mailbox、资源键与全局/场/工具配额，不另起线程栈。采用前预分配稳定拥有者；暂存到队列容量只计一次。真 Dispatched 和 attempt 1 Started 先落稳才起执行。线程 starter 在发布前抛错、发布 joinable 后抛错各按真实窗口记账；已发布线程继续归本票，收它真实完成，不伪称零执行。

worker 只执行和投完成信封，不碰 writer。独立取消旗、正数限额和 owned 捕获随本票保活。StopIndeterminate 保未知，不按 `!is_error` 洗成成功。Post 在串行收件方、jobs 锁外取真实业务配对和 raw；不借下一轮模型状态。Post 未确认优先保 gap。

真实 Started 落稳后，一律调用 owned `RunCommand.execute`，传本票取消源和实际限额，收平台原有取消或预算正文。这个窗口不承诺零 spawn；不手造 command raw，也不补假限额资料。

执行体抛异常或完成信封受损，保真正 unknown 终态和 gap，不拿预分配诊断充 command raw，不进入普通 Post 或写成功 Observed。已取得的真实结果随收件拥有者保留，也不能据此补可信终态。

完成泵缓存每阶段首次 native receipt：终态、原文持久化、Observed 分开。原执行成功或失败文本均可留 raw；preview 单独有界。缺 raw/观测不改业务结局。已落 terminal 不再写第二枚；任一未确认就冻结收账，不重写、重派或降为普通失败。首批不声称 rich payload 支持。

## 收场与恢复

RequestShutdown 先关准入、撤 epoch、请求本票取消。等线程时不握 writer serial/jobs；真实进程和捕获退出后 join，再串行收件。writer 在在途方法和 worker 完整退出前保活；捕获在锁外释放，最后发布 close complete。完成、finished、持久收账各记各事；close 清资源成功不补未知持久事实。迟取消保真实成功。

新布局历史始终被动 Hold，连默认 Legacy 也不派、不补父消息、不补 Observed。仅暂存、已采用待父链、已排队、派发未知、终态/原文/观测缺口、完整确认分别保真；真实 dispatch/terminal 不藏回 prepared_hold。未知布局或采用来源不符拒绝。无新事实的旧 CLI/Hook/Legacy 路保持原行为。本版内部使用；公开启用前另钉宿主协议兼容。

## 远端验收

用真实 Writer、原声明与共同 Prepare 登记，再真采用、真写父句柄链、真执行 `run_command`，重读校账。六册覆盖：采用留票零执行与来源/权限拒绝；真父链前后和所有缺件；真实成功/失败文本与 Post；取消/限额/同项目两场及异项目隔离；真线程发布前后抛错和关场串行 drain；真实 native 写回执及每阶段 gap、Hold/Legacy 零恢复重跑。保旧 Job16、启动6、Hold6、登记6、J1六场与命令限额6；不放宽预算或结果断言。

真实新来源才添 focused/ASan 门，按源并集算注册数，完整 argv、唯一 marker、非零断言、JUnit/LastTest 互核。CI 失败保原件，再修源码、推新头；不 rerun 旧头抹失败。
