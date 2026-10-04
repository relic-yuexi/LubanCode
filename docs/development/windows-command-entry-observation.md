# Windows 命令同调用入口观测

这笔只补内部诊断。公开 SDK、命令 JSON、进程预算与默认包装不变。复用 `platform::ScopedProcessDiagnostics` 所持本次 `ProcessDiagnosticBuffer`；默认配置为空，不另建 TLS，不设全局环境变量，不添命令调用。

## 原件与尚未坐实处

基线 `ec3d19e285ab2ecf6b41c662fdffc042aed2ef58`，Windows full job `111512481279`。sole collector 原日志 SHA256 为 `cdc0a515e8ee238624fb3556b52a24e5d7e4567de18f974682cc29dd61939cca`。原第 18596 行 `powershell-exact` 等 15126ms，返回 `timed_out/process.timeout`；第 18602 行 `powershell-host-timeout` 等 8015ms，同码。两处原帽分别 15000ms、8000ms，Create/Assign/Resume 均成功，首次管道读在杀树后报 0 bytes / Windows 109，started/done 均 absent。

两处真实 Job accounting 查询均成功，各有 total=2、active=2、terminated=0。计数不识别那两只进程，不能拿它当用户块或 Probe 已入场。原件未证明解析错误、执行策略错误、模块装配阻塞或唯一时限波动。诊断不放帽、不加 warm-up、不改原断言，也不重跑旧头求绿。

## 内部入口与所有者

`ProcessDiagnosticBuffer` 尾持可选 owned 配置：固定 ASCII tag、三条绝对 UTF-8 文件路径。配置只能在调用开始前放入一次；已记录 POD 或已配置就拒再换。已有 128 槽、POD 记录、原发布次序与 scope 保存/还原规则不变。

原限额夹具拥有独占目录、配置与三份标记文件。builder 在真正调用栈读取同份 buffer，再复制并校路径/tag。配置不由用户 JSON 打开。新口只用于这里实际链接同份 platform 的私有 `RunCommandTool` 路径；可执行文件内 scope 不承诺穿透 SDK DLL 内另一份 TLS。

Windows PowerShell 两份前台 builder 仅在本次配置有效时插三条观测：第一句 `shell-entry`；原编码与状态初始化结束后、原管道前 `wrapper-ready`；原 `& { user }` 第一条语句 `user-block-entry`。只用 .NET 文件静态调用，写固定 tag、阶段与真实 shell PID，不走 stdout/stderr，不添管道段，不提前解析命令或加载模块。观测异常在观测语句内收住；原 ErrorRecord、LASTEXITCODE 与显式 exit 判据不改。

这是可信固定夹具诊断，不能宣称任意用户脚本内存或所有 PowerShell 隐含状态完全透明。无配置时原两份 builder 正文、编码字节、参数、空环境与 `RunProcess` 路径逐字保留。后台 builder、CMD、POSIX、Probe、Windows 杀树和 reader join 都不迁。

## 读取与退场

每阶段最多一份短 ASCII 记录，单份读帽 256 bytes。fixture 只在实际调用退回或抛出后读文件，记原 bytes、读错与完整性；校 tag、阶段、整数 PID 与完整换行。读错、缺件、半行或不符只报观测未确认，不反推那阶段没执行，也不改 `Tool::Result`。

文件写入只是诊断，不是 durable receipt，不承诺目录刷盘。配置与 buffer 活到原 caller/reader 退完；取消与四场失败展开仍先放原退场闸、等待 future，再删独占目录。observer 不增线程，不借主会话身份，不复活权限。

## 原册验收

只改 `process_diagnostics.hpp`、`run_command.cpp` 与原 `test_run_command_execution_limits.cpp`。原六 CASE、六 path marker、每次执行、Started/done/实际结果和退场断言都留。exact 15s、host 8s、model 5s、cancel 20s 与原输出帽不变。纯源码守卫剥掉新增诊断后核原函数/六案逐字；新远端仍留完整 argv、JUnit、LastTest 与原 POD stages。

更早 Probe main/参数/cwd 入口尚无新证据。这笔不添环境传参或 Probe 早期 Unicode 改动。即便 wrapper-ready 已确认而 user-block-entry 未确认，也只收窄待查区间，不认唯一根因。

## Windows 原册调度

`be378d08` 本轮 job `111539649641` 中，同一 SDK 限额册先在 focused 阶段通过，全量阶段又与 MemoryRecall、MemorySave、Subagents 同起。后一次 `powershell-exact` 等 15013ms，三入口未确认；`powershell-host-timeout` 等 8018ms，三入口均确认且 PID 同为 10180，probe 起步标记仍缺。两条 CTest 当时已有彼此互斥锁，不能靠再添同锁排除其它册负载；现证据也未锁定唯一根因。

只给 Windows 的 `unit.tools.run_command_execution_limits` 和 `sdk.focused.run_command_execution_limits` 加 `RUN_SERIAL=TRUE`，让它们各自独占 CTest 调度。CLI-only、SDK-only 与 combined 均覆盖。原资源锁保留，册内四场并发仍实跑；六 CASE、完整参数、结果断言、各层预算与全量 `--parallel 4` 原样。此改动只排除已见外部并发，不能称产品进程实现已修复。实际 Windows 登记须带布尔 `RUN_SERIAL=true`；Linux/macOS 不加这条要求。

Windows 全量 Test 在实际执行前，按同一选择参数保存整份登记；成功、失败都上传登记、原 JUnit 和原 LastTest，单列 `full-test-originals-windows-msvc`。原四处局部取证门照留，不再添解析器。未过滤全量之外的手动单册运行不冒称全量原件；原件与新调度须由远端实际执行核验。
