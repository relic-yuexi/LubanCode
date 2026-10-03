# SDK Job 前置：命令执行限额

基线为私有 SDK 组合 `373df8d894239dcbbd3a69a6ca17af3bde4a14d4`（#308）。本笔只补中立工具执行口，不开放后台 Job，也不把事后截短结果当成捕获限额。

## 公开什么

暂不添公开 SDK API。内部 `ToolExecutionContext` 增添可选 owned `CommandExecutionLimits`：`timeout_ms` 与 `max_output_bytes` 均用无符号整数。缺省没有画像，CLI、Hook 和原工具调用沿旧行为。

启用画像须显式填两项：时限 `1..86400000` 毫秒，捕获帽 `1..2097152` 字节。本笔只收紧原进程捕获帽。零值、超界值在起进程前报 `process.invalid_execution_limits`，不把零当无限。

命令仍须过原参数、cwd、隔离与 shell 检查。实际时限取模型显式值或原命令默认值，再与宿主帽取小。实际捕获帽直接传给既有 `RunProcess` / `RunShellCommand`；四条前台分支都接这份值，不改平台进程实现，也不在结果返回后假做限额。

这份画像只许前台受控执行。`run_in_background=true` 或带 `max_runtime_ms` 字段（连 null/0 也算）均在起进程前报 `process.execution_mode_rejected`，免得绕进全局 CLI 后台名册。公开 Job 后续由原协调器接单，再用本前台入口执行；本笔不新造后台名册。

## 谁拥有

画像归当次上下文，工具只在同步 `execute` 内借读，不存指针，不改共享 `RunCommandTool` 字段。取消仍优先取当次旗；旧 `SetCancel` 留作兜底。业务身份、权限、审批和 Session owner 都沿既有合同。

启用画像时，实际进程结果附 owned 标量 `timeout_ms`、`max_output_bytes`、`captured_output_bytes`。最后一项记捕获原始字节数，先记再清洗 UTF-8，不声称含工具解释文案或清洗后投影。旧调用结果和文案不增字段。

超限仍报 `process.output_limit`，超时报 `process.timeout`，取消仍报 `cancelled_during_run`。到帽便收整棵进程树、等捕获退出后才返回；不把半截结果报成功。画像内文案写实际帽，不劝模型绕进 CLI 后台入口。

## 怎么关场

本笔不改 Session Close。原同步进程入口返回前已收树并退捕获；当次画像随栈退场。后续 Session-owned Job 必须持有独立取消旗与画像，Close 仍须等真实 worker、进程和捕获退场，不存父轮借用。

## 验收

独立私有、纯标准库 probe 只进测试目标，测试关闭时不出现，不落 SDK 依赖闭包或安装包。三平台同源原生册走实际 RunCommand 两种 shell：Windows cmd/PowerShell，POSIX sh/bash。测无效画像与背景绕路零进程；恰到帽成功、帽加一强制终止；宿主时限真生效及模型只能收紧；取消等真实退出；同工具、同项目二场及异项目二场限额不串；缺省画像沿旧路径。

新来源须登记真实 `unit.tools` 与 `sdk.focused` 两套 CTest；focused 35、必需 ASan 40、安装消费仍 25。保留原来源、完整 argv、非零原生断言、JUnit、资源锁及预算。真 CI 原件核源头、checkout/merge 同树；不借 #308 或其它叶笔绿。只在远端编译和跑原生，本地只读、文档与纯数据门。
