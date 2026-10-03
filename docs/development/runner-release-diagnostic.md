# Runner Release 拒绝诊断与失败原件

这笔从私有组合 `22bba238` 分出。它只补诊断，不修启动，也不重跑掩盖旧红。

## 已见事实

#296 源 `4bc27a42`、CI `37085035667` 的 Mac Runner-only 十一场中，一场 `runner_crash_indeterminate` 失败。夹具尚未杀服务，已等不到起跑标记。落盘记录含 PID `13925`、`state=failed`、`runner.launch_failed`，原启动回应、作业 stdout/stderr 与 marker 未留。

代码先 `Create`、记 PID、保存，再 `Release`。这份阶段与外码把调查收至 Release 的 gate 写入拒绝。原件未记 write 返回值和 errno，不能判 EPIPE、工作目录、资源压力或旧红唯一根因。

## 生产边界

只在 POSIX `Process::Release` 的真实 `write != 1` 分支记固定字段：`stage=release`、`syscall=write`、原 count 和 errno。errno 在任何 `close` 前捕获；非负短写记 errno 为零，不冒领旧 errno。gate 仍照原次序关闭，外码仍为 `runner.launch_failed`。

不记 argv、环境、身份或认证值。不改 Create、状态机、PID、重放、取消、恢复、原启动时限；不加启动重试或公开故障参数。诊断也不把未知作业当成可恢复作业。

## CI 失败原件

只收这份进程夹具自行创建的材料。`job.start` 原回应 stdout/stderr 和退出码在 JSON 解析前缓存；不收请求正文或 `identity.json`。外部 Node/Worker 夹具也把自己的原回应存进其临时根。

失败场在 `Runner.close` 前另留状态快照，保失败当时 jobs/endpoint、服务输出、回应、夹具作业 stdout/stderr 和 marker。close 后仍保原有收场快照；临时目录清理失败仍用 owned 缓存。两阶段分名存，不拿收场后的状态代替失败时状态。成功场只丢缓存，不上传输出。

只读明确列出的本场文件。单件最多 256 KiB，单次快照最多 2 MiB、最多 64 件；超过时明确记截取、原件实际大小和已存前缀 SHA。缺件、非 regular、读错分别记清。POSIX 打开时不阻塞 FIFO、不跟随最终链接；Windows 从实际已打开文件判 regular。文件柄与缓存都归本场，不扫描他场目录。夹具 stdout 中的 `RUNNER_TEST_SECRET` 是本场生成的 nonce，不是 Runner 认证值；这些原件只证远程 CI 夹具，不改产品 preview/full 同步合同。

原十一场名称、时限、断言逐条保留。原响应的 JSON 与退出码校验照旧；先存字节再显示，编码错误不能盖过原错。

## 验收

复用内部 `Process` 拥有者，组合 CLI+Runner 在原测试二进制加一册、一案。POSIX 真 Create 暂停子进程、Cancel 本场 group、Poll 确认退出并 reap，再真实 Release，核 gate write 拒绝、诊断原 count/errno、外码不变；这只证明已知故障入口，不证明旧 Mac 同因。Windows 同案核真实正常 Release，随后收本场进程。没有假 Error 或公开 fault flag。

这册只在 CLI 与 Runner 都启用时编入、链接已有 Runner client；Runner-only 仍只构建原两目标，安装范围、十一场不变。SDK-only、SDK focused 与 ASan 不加此册，不冒称插桩覆盖。新增独立三平台原件门核注册来源、一案实际非零断言、原输出与平台标记。

纯 Python 另验回应原字节、失败前状态与收场状态分离、缺件/截取/IO 错误、身份不收、编码与清场错误不吞。原生只交远程 CI；合入前看本头真实 checkout、来源与原件，不借旧绿。
