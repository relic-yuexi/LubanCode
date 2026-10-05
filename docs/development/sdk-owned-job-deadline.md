# Owned Job 登记截止与排队预算

首笔只改内部 Owned command/coordinator、严格事实读取及原生验收。基线 Web `33d55b90` 保留；Legacy/CLI、公开 SDK Job 准入、CMake/CI 不动。旧 `deadline_ms=0` 仍沿内部不限登记时长语义，现有宿主命令帽不变；正预算才启用本合同。后续公开 SDK 须要求正预算，不能拿这里的兼容零值当默认授权。

真实登记使用 `steady_clock`，不调用旧 `Options::clock_ms`，也不加生产假钟口。正预算转换和 time_point 加法均先查溢出；无法表示须在 Registered 写入前拒绝，不能回绕或变无限。截止在真正 Registered 追加边界取时并固定，登记写盘耗时也计入。若 Pending 已落、后续无法形成完整登记，则保留真实缺口和已有回执，不擦掉半笔事实。时钟 epoch 只由当前登记 owner 持有，不写成可跨进程复用的执行凭证。

Adopt 接走同一截止，不重开预算。排队与 scope_gate 复核均扣时间；每次派发前再查。已到期的已采用任务沿现有 `before_started` Cancel/Observed 收场：零 Started、零 command、零进程、无业务 raw/Post。实际调用前取 `min(登记剩余毫秒, 模型合法收窄值, 宿主 command 帽)`；不足一毫秒按已到期处理，绝不把 0 交进“不限时”路径。模型坏参数仍由真实命令入口拒绝，不借预算代码改写参数或伪造命令回执。

Started 是已落账的执行意图，线程工厂之后仍可能迟迟不进入 worker。若到真正 command 调用边界已过期，则保留这份 Started，明确记录 command 未调用；不调用 RunCommand 来制造一份“取消结果”，不倒删意图、不编 raw 或成功 Post。完成泵写真实 `ToolExecutionCancelled`（phase=`before_command`、reason=`registration_deadline_elapsed`）及 `ToolJobObserved(cancelled)`，后者带 `commandNotInvoked={version:1,reason:"registration_deadline_elapsed"}`。这是版本化的未调用证据，不是父交付完成凭证。

schema 核标记闭合形状与版本；严格 reader 另核 Owned 采用、正预算、既有 Started/dispatch、真实父交付引用、对应 Cancel 及无 raw/Post。旧记录缺标记仍走原规则；未知版本或矛盾事实拒绝。新 reader 读旧账不变；旧 reader 遇新跳过事实会因无 raw/Post 拒绝，不能装成兼容执行。`ReadJobOperations` 同过这条关系门；恢复 Hold 可显示已确认取消与 command 未调用，绝不重建单调截止、续跑或补造交付。

实际线程与资源仍归协调器。Deadline 不绕过原线程发布／抛错清理；Shutdown 真 join 后才落完成事实、释放配额、退 callback，再交还 writer。已发布线程即使工厂随后抛错，也等它真实退出。查询、取消、Close 重入门不放宽。

新册 `tests/integration/sdk/test_lubancore_owned_job_deadline.cpp` 固定六案：`registered-queue`、`authorization`、`runtime-budget`、`late-worker`、`startup-close`、`strict-recovery`。沿真实 SessionService/Operation、Prepare、coordinator、Bind、命令 Probe 和已有线程工厂验，覆盖登记起算／排队到期、鉴权耗时、实际运行画像、工厂晚入零调用、线程发布前后抛错／取消／关场／配额，以及实际 `ReadJobOperations` 和坏标记拒绝。

每案完成后印 `[owned-job-deadline-path] <path>`，并输出 `[owned-job-deadline-fact]` owned JSON：`path`、`quiescent`、`global_running`、目标 Job 的 `started_intent`、`command_not_invoked`、`command_calls` 与 `effective_timeout_ms`（未调用为 null）。原始命令、Started/done 文件、native receipt、完整 argv、JUnit/LastTest 交叉核；三平台与 ASan 由 root 接远端。无本地配置、编译、HTTP 或原生执行。
