# SDK Job 前置：命令执行限额

基线为私有 SDK 组合 `373df8d894239dcbbd3a69a6ca17af3bde4a14d4`（#308）。本笔只补中立工具执行口，不开放后台 Job，也不把事后截短结果当成捕获限额。

## 公开什么

暂不添公开 SDK API。内部 `ToolExecutionContext` 增添可选 owned `CommandExecutionLimits`：`timeout_ms` 与 `max_output_bytes` 均用无符号整数。缺省没有画像，CLI、Hook 和原工具调用沿旧行为。

启用画像须显式填两项：时限 `1..86400000` 毫秒，捕获帽 `1..2097152` 字节。本笔只收紧原进程捕获帽。零值、超界值在起进程前报 `process.invalid_execution_limits`，不把零当无限。

命令仍须过原参数、cwd、隔离与 shell 检查。实际时限取模型显式值或原命令默认值，再与宿主帽取小。实际捕获帽直接传给既有 `RunProcess` / `RunShellCommand`；四条前台分支都接这份值，不改平台进程实现，也不在结果返回后假做限额。

Windows 首轮运行验收发现：旧前台 PowerShell wrapper 把整段输出收进 `$oco`，命令退出后才格式化。带画像的 PowerShell/pwsh 改用流式管道，逐枚识别 ErrorRecord，再经 `Out-String -Stream` 输出纯文本；不留整段捕获数组。退出码仍先取最后一枚 native `$LASTEXITCODE`，没有 native 才按 ErrorRecord 判失败；显式 `exit N` 原样保留。没有画像仍用旧 wrapper，CLI、Hook 不换路径。帽管实际 shell 输出，不能管用户命令自己分配的内存。

这份画像只许前台受控执行。`run_in_background=true` 或带 `max_runtime_ms` 字段（连 null/0 也算）均在起进程前报 `process.execution_mode_rejected`，免得绕进全局 CLI 后台名册。公开 Job 后续由原协调器接单，再用本前台入口执行；本笔不新造后台名册。

## 谁拥有

画像归当次上下文，工具只在同步 `execute` 内借读，不存指针，不改共享 `RunCommandTool` 字段。取消仍优先取当次旗；旧 `SetCancel` 留作兜底。业务身份、权限、审批和 Session owner 都沿既有合同。

启用画像时，实际进程结果附 owned 标量 `timeout_ms`、`max_output_bytes`。帽管平台捕获字节；平台编码转换、UTF-8 清洗和工具解释文案不算捕获原文，不能拿最终正文长度反推捕获量。旧调用结果和文案不增字段。

超限仍报 `process.output_limit`，超时报 `process.timeout`，取消仍报 `cancelled_during_run`。超过帽便收整棵进程树、等捕获退出后才返回；恰到帽仍可成功。不把半截结果报成功。画像内文案写实际帽，不劝模型绕进 CLI 后台入口。

## 怎么关场

本笔不改 Session Close。原同步进程入口返回前已收树并退捕获；当次画像随栈退场。后续 Session-owned Job 必须持有独立取消旗与画像，Close 仍须等真实 worker、进程和捕获退场，不存父轮借用。

## 验收

独立私有、纯标准库 probe 只进测试目标，测试关闭时不出现，不落 SDK 依赖闭包或安装包。三平台同源原生册走实际 RunCommand 两种 shell：Windows cmd/PowerShell，POSIX sh/bash。测无效画像与背景绕路零进程；恰到帽成功、帽加一强制终止；宿主时限真生效及模型只能收紧；取消等真实退出；四笔同工具真实并发调用，两笔同 cwd、另两笔各取 cwd，限额不串；缺省画像沿旧路径。这里只验内部调用上下文，公开 SDK Session 后台装配与隔离另笔。

超额 probe 输出后仍停留 60 秒；宿主帽 15 秒。PowerShell 必须先报输出超限，不能靠等退出或放宽时限过验收。Windows 同笔补验流式 wrapper 的正常输出、cmdlet ErrorRecord、显式 exit 与 native 非零码优先；六场原册、原断言和预算均留。

新来源须登记真实 `unit.tools` 与 `sdk.focused` 两套 CTest；focused 35、必需 ASan 40、安装消费仍 25。保留原来源、完整 argv、非零原生断言、JUnit、资源锁及预算。真 CI 原件核源头、checkout/merge 同树；不借 #308 或其它叶笔绿。只在远端编译和跑原生，本地只读、文档与纯数据门。

## 本轮调用诊断

`c0682677` 的 Windows 全量原件中，同源 SDK 册有两场失败：PowerShell 恰到帽的调用报错，超时场未见真实 started 文件。前一份 focused 与后一份 CLI 同源册均过，不能代这份全量红账，也不能据此断定负载、启动或捕获哪一处出了错。原输出没有留下报错调用的完整结果，也没有标出缺 started 那笔的 shell 与 tag。

本笔只给原生册添逐调用资料。调用前后以独立 `[command-limits-invocation]` 前缀，记真实 shell、tag、完整入参、当次上下文画像、结果 `is_error/outcome/error_code/details/content` 和耗时；started/done 文件分别记存在状态、读取状态与原字节。并发调用逐行写出，资料归本次调用，不存工具或进程借用。诊断读取失败也留说明，不替原断言作判断。

六场原册、六条成功标记、所有断言、时限、捕获帽、会合闸和失败清场均不改。PowerShell 实现、probe、注册名册与 CI 门也不改。诊断不是修复，更不算本头原生已过；另推鲜头后，只认远端实际调用与失败原件。本地只做结构核对、文档与 diff 检查，不起编译器、CTest、probe 或项目 PowerShell。

## POSIX 进程阶段诊断

`4aba35e1` 的 macOS 全量原件中，`sh-cancel` 调用用了 60141 毫秒才返回。返回时取消旗已立，started 和 done 原件都在；probe 走到了自然结束。原 10 秒取消退场断言失败。同头另两次原册通过，不能抵这份红账。现有资料还分不清阻塞落在 exec 握手、杀组回收还是读线程退场，先补阶段资料，不猜根因。

新增内部 header-only `ProcessDiagnosticBuffer`。固定 128 槽，每槽只放 POD 阶段、真实 steady 时刻、pid/pgid、系统调用返回码、立即保存的 errno 和数值附项。原子索引领槽，写完才 release 发布；读者只在 acquire 看见发布后读值。溢出另立标记，缺阶段不得补猜成功。不添宿主回调、环境开关、动态日志、全局日志锁或新的 source target。

默认指针为空，不多查 getpgid、不读诊断时钟、不碰诊断原子。测试在实际 `ExecuteRecorded` 调用线程建 `ScopedProcessDiagnostics`，`RunProcess` 入口冻结这份借用，读线程按值取同一指针。只在父进程和真实读线程记 exec 握手前后、进程组查询、首次取消、killpg 原返回码、wait/reap 首次及终结、reader 退出和 join 前后。fork 子分支保持原字节，不添任何操作。每次原生返回后先存 rc/errno，记录后还原 errno；不改原判断、等待、杀组或回收次序。

缓冲归测试调用；取消场由调用方持有，活过 `RunProcess` 返回、读线程真实 join 和失败清场。原取消 store 前后各记真实发布时刻，不读取未发布槽。实际调用退场或抛出后才把已发布记录写入原逐调用资料；不截 join，也不留下后台。Windows 进程实现不变，只留下测试调用与旗发布资料，不冒称已有 Windows OS 阶段。

原六场、断言、预算、成功标记、probe、PowerShell、注册与验收门全留。此笔只添诊断，持续内存耗尽不许诺日志完整；真实进程结局仍由原实现返回。源码改完先核结构和 diff，再交远端鲜头验收；本地不跑原生。

## Windows 进程阶段诊断

`4aba35e1` 的 Windows 全量原件另有一场红：`powershell-exact` 用了 15029 毫秒，报 `process.timeout`，started/done 均缺。cmd 两笔已过，仍不能据此定 PowerShell 启动哪一处出了错。本笔顺着同一 opt-in 缓冲补 Windows 真实阶段，前段“Windows 只有调用资料”随此笔收窄，不拿 POSIX 阶段代 Windows。

只改前台 `RunProcess(wstring)`：冻结缓冲给 caller/reader，记实际 CreateProcess、建 Job、配置与绑定、ResumeThread、首次 Wait 及终结、首次取消/超时/超额、CloseJob 或 TerminateProcess、原 5 秒等待、读线程退出和真实 join。此入口靠 `CloseHandle(job)` 的 KILL_ON_JOB_CLOSE 收树，不冒称调用了 TerminateJobObject。每枚 API 返回后立即存返回码和 GetLastError；记录包住诊断时钟，再还原 LastError 与 errno。默认空指针不多查 GetProcessId、不读诊断时钟或原子。

不改 API 短路、原失败出口、CloseJob/fallback 条件、ResumeThread 原返回码判断或任何预算。只记首个 Wait/read 与终结，不让轮询冲满 128 槽。读线程仍按值借同一缓冲，等真实 join 才读资料；原六场、断言、帽、probe、PowerShell、注册与门逐字核对。输出注明平台和错误域，不添加子进程输出标记。本地仍只查文本、纯数据和文档。

## Windows scoped wrapper 的模块限定调用

`52fa7d6d` 的 Windows SDK-only 原件再次超时：`powershell-exact` 用了 15054ms，宿主帽仍为 15000ms，实际 CTest 登记已有 `RUN_SERIAL=true`。同一 PID 的 shell-entry、wrapper-ready 已确认，user-block-entry 文件为空，probe 的 started/done 均缺；创建进程、挂 Job 和恢复线程成功。这把排查范围收进包装准备与用户块入口之间。空观测不能独自证明某条指令未执行，现有材料也未指认某只 cmdlet、模块或系统检查为唯一起因。

这笔只限定有执行画像时 wrapper 自带的三只命令：`Microsoft.PowerShell.Core\ForEach-Object`、`Microsoft.PowerShell.Utility\Out-String -Stream`、`Microsoft.PowerShell.Utility\Write-Output`。保留同一管道、ErrorRecord 识别、对象格式化与逐行输出，不改用户正文。显式 `exit N`、末次 native `$LASTEXITCODE` 优先、cmdlet-only 错误码仍沿原约定。普通前台、后台、Hook、cmd、POSIX 与进程执行器均不改。

[Windows PowerShell 5.1 命令优先级文档](https://github.com/MicrosoftDocs/PowerShell-Docs/blob/main/reference/5.1/Microsoft.PowerShell.Core/About/about_Command_Precedence.md) 明列模块限定调用；[5.1 Out-String 文档](https://github.com/MicrosoftDocs/PowerShell-Docs/blob/main/reference/5.1/Microsoft.PowerShell.Utility/Out-String.md) 说明 `-Stream` 沿对象格式器逐行返回。[PowerShell 7.4 CommandDiscovery 实现](https://github.com/PowerShell/PowerShell/blob/v7.4.0/src/System.Management.Automation/engine/CommandDiscovery.cs) 将限定名送指定模块加载，非限定名才遍查可用模块导出的命令。这份公开实现说明机制，不代替 5.1 本轮现场栈。指定模块本身仍可加载，超时是否收住须远端新头验。

不预先 Import、不另跑空命令、不改自动加载偏好、执行策略、环境或时限。六 CASE、旧调用与旧断言保留；在第六 CASE 末加两对象表格式验收，让同一正文实际经过旧前台与 scoped 两路，逐字核输出，再核表头、两行值和无 CLIXML。原恰到帽、超额即收树、取消、四场并发、错误与退出码验收继续执行。格式对照在这些调用之后，不给首笔边界案预热。新脚本只交远端 CI，本地只核文本、纯数据与源码守卫。
