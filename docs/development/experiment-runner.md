# 独立实验 Runner：首批本机托管

`luban-runner` 单独托管长实验。Worker 只发 `job.start/get/cancel` 请求，不创建或销毁 Runner。关闭 Worker 不关闭已交给 Runner 的实验。

这批先交独立程序和本机接口。正式 `luban-worker` 的 `job.*` 接线另在 Worker 分支实现；下面的验收用真实独立进程充当 Node/Worker 调用端，不代表那条产品接线已经完成。

## 部署与寿命边界

构建时打开 `LUBANCODE_BUILD_EXPERIMENT_RUNNER`，构建 `luban-runner`，安装 `ExperimentRunner` 组件。程序不需要 GPU、模型配置或网络入口。Windows 采用与本次构建一致的 MSVC 运行时；同时启用 SDK 的动态 CRT 构建仍须具备相应运行时。

Runner 必须单独启动，并放在 Worker/Node 的终止域之外。例如先在独立终端或独立监督单元启动 Runner，再启动 Node。不要让 Worker 派生 Runner，也不要把两者放进同一个会被整体停止的服务单元。首批不自动安装服务，不自动从父 Job/cgroup 逃脱。

```text
luban-runner serve --state-root /absolute/private/runner-state
luban-runner info --state-root /absolute/private/runner-state
```

`serve` 不读 stdin，也不随控制客户端断开退出。启动输出包含持久 `runner_id` 和本次 `boot_id`。Node 应把 `runner_id` 保存到自身绑定中；后续请求必须带原 ID。状态目录丢失后，新 Runner 换 ID，旧客户端会收到 `runner.identity_mismatch`，不能自动认领新目录并重开实验。

| 动作 | 本批合同 |
| --- | --- |
| 关闭调用客户端或 Worker | 在独立部署条件下，Runner 与实验继续 |
| 杀掉普通 Node 进程及其 Worker 子树 | 在独立部署条件下，Runner 与实验继续 |
| 任意 systemd/launchd/Windows Service 重启 | 尚未保证；须另核实际 cgroup、Job、服务关闭与登录会话范围 |
| 关闭 Runner | 停止它管理的活动任务；有退出证据才落终态 |
| Runner 崩溃或机器重启 | 无可靠终态的旧任务标 `indeterminate`，不承诺透明续跑 |

Windows 每项实验由 Runner 持有独立 Job Object，实验在加入 Job、启动身份落盘后才恢复主线程。取消终态同时检查主进程退出和该 Job 的活动进程数归零。

任务以入口进程的寿命为准。入口结束后，Runner 会清理本任务范围内的后代；需要长期托管的程序应保持入口运行，不靠入口退出后的后台子进程续命。

Linux/macOS 每项实验有独立进程组。Runner 留住未回收的直接子进程，先清理进程组，再回收组长，避免 PID/组号复用后误杀。这里的终态证据是直接子进程退出，加组信号投递或确认组内已无可运行成员，不冒称逐个观察所有后代退出。macOS 若对只剩僵尸进程的组回权限错误，Runner 另查完整组成员；只要仍有活成员或查不清，就保持未知，不把权限错误一概忽略。主动脱离进程组的后代不在本批控制范围。程序不构成文件系统或同一系统用户之间的安全沙箱。

## 本机请求

本机 IPC 使用私有状态目录里的有界请求/响应文件。目录须只让本机管理用户及系统管理员访问；新目录由 Runner 建立严格权限，已有宽权限目录会被拒绝。凭据 cookie 不进入输出。每次请求另校 Runner 身份、启动 nonce、请求 ID 和截止时间。

```text
luban-runner request --state-root /absolute/private/runner-state --runner-id <已绑定ID>
```

向 stdin 写一行 JSON，程序回一行 JSON。`runner_id` 由命令行绑定，正文示例：

```json
{"method":"job.start","session_id":"session-a","client_job_id":"experiment-001","spec":{"argv":["/absolute/python","experiment.py"],"cwd":"/existing/project","env_refs":["EXPERIMENT_TOKEN"]}}
```

`argv[0]` 和 `cwd` 须为绝对路径。没有隐式 shell 展开；要用 shell，应明确传可执行文件及参数。`argv` 会作为启动规格持久保存，凭据请用 `env_refs` 引用 Runner 启动环境，别把明文凭据写在命令参数里。Runner 保存环境名，不保存解析后的值；子进程只得到基础系统环境与明确引用的环境项。

Runner 不查询 GPU、不分配设备、不记录显存，也不自动注入 `CUDA_VISIBLE_DEVICES`。设备选择由用户脚本或显式环境引用决定。`--max-running` 只限制本 Runner 同时托管的进程任务数，默认 16；同项目目录下可同时运行多场 Session，不设项目长锁。

同一 `(session_id, client_job_id)` 与相同规格摘要返回同一 `job_id/job_nonce`。异载荷返回 `runner.idempotency_conflict`。受理键保留在持久任务账中，不因响应超时或客户端退出丢掉；默认最多保存 1024 份任务，可通过 `--max-records` 配置。任务账另有 32 MiB 上限，并为终态留空间。达到上限拒绝新任务，不忘掉旧键后重新执行。

```json
{"method":"job.get","session_id":"session-a","job_id":"<任务ID>","job_nonce":"<任务nonce>"}
{"method":"job.cancel","session_id":"session-a","job_id":"<任务ID>","job_nonce":"<任务nonce>"}
```

查询和取消都核 Session 与 nonce。`job.cancel` 先保存请求，再向本次 Runner 真正持有的进程对象发停止动作；刚受理取消时可能仍显示 `running`，应继续查询到终态。任何旧 PID 都不会被重开、收养或当作取消目标。

状态包括 `starting/running/succeeded/failed/cancelled/indeterminate`。启动意图先落盘，创建的进程先暂停，启动身份落盘后才放行用户代码。这个窗口丢了 Runner，没有退出证据就保持 `indeterminate`；相同幂等键不会重跑，取消也回 `runner.job_indeterminate`。操作者核对输出和外部进程后，须用明确的新操作键启动恢复工作。

任务账与启动 endpoint 采用同一份有界写入规则。只在原子写返回 `TransientReject` 且 `NotCommitted` 时重试；每次计划等待至多 20ms，重试预算 1 秒，总尝试至多 51 次。平台标作 `Permanent`，或已换名但耐久未确认时，立刻报 `runner.store_failed`，不删正式文件，不伪造回滚。这份预算限制重试等待与后续尝试，不承诺线程唤醒或操作系统单次 I/O 必在 1 秒内返回。首次短拒与最终失败另写本机 stderr 诊断，保留固定文件名、原子写错误码、提交阶段、次数和底层错误；不写账本正文、凭据或环境值。

## 日志与出网

stdout/stderr 直接写入节点 `jobs/<job_id>/stdout.log` 与 `stderr.log`，不经 Worker 管道。任务账保存规格、摘要、启动身份、状态和退出结果；目录损坏或缺账时拒绝当成空目录继续接单。

校验和能查出账文件损坏，不能识别整份有效旧备份的回滚。首批没有外部单调账锚点；不要拿旧备份直接恢复接单。须先核对仍在运行的实验和外部结果，再建立新身份并明确重绑，不能自动重放旧启动请求。

状态接口只回任务身份、状态、退出码、固定错误码、日志不透明 ID 与字节数。不回启动参数、环境值、完整日志或滚动 tail。`job.logs` 目前明确返回 `runner.full_log_sync_disabled`。Node 出网仍须经过已定 preview/full 投影；开启工具结果 full 不等于允许读取任意日志文件。本批日志留存依赖节点磁盘容量和管理员保留策略，没有无限日志或自动磁盘配额承诺。

## 远端验收

`runner.process.lifecycle` 使用安装、搬迁后的真 `luban-runner`，运行 `tests/runner/test_runner_process.py`。固定 11 项进程检查，零测试、漏项或跳过均失败。

Windows 将 Node/Worker 夹具放进带 `KILL_ON_JOB_CLOSE` 的 Job；POSIX 将夹具放进独立进程组。Runner 由另一父域先行启动。检查真实杀 Worker/Node 后实验继续、另一客户端重连、同 cwd 多 Session、取消组内子进程、丢失回执后同键不重跑、错误 Session/nonce 拒绝、日志与环境引用边界。成功及启动失败的终态都须经重启复验。Runner 崩溃另造有效的未收尾账快照，把持久 PID 换成仍活哨兵，验证恢复不收养、不误杀；`starting` 窄窗用持久快照重放，不冒称完成了所有崩溃指令点的穷举。

Windows 另在原有崩溃与身份检查中，分别持住不共享删除权限的真实 jobs 与 endpoint 句柄。先观察原子替换确实受拒，再释放句柄，核新启动、持久 `indeterminate` 和同键不重跑；另一路持 endpoint 到预算耗尽，核固定失败码、有限次数、旧文件字节未动，释放后仍能启动。失败时保留夹具任务账、endpoint 与服务日志，不复制含凭据的身份文件。顶层仍报原有 11 场，Windows 子场另报实跑标记。

这份验收不代表任意服务管理器重启、跨注销、断电后续跑或 GPU 调度已支持。真实进程验证只在远端 CI 执行。
