# Windows 超时进程映像短合同

2026-10-10。#345 源 023d4caa 远端 Windows SDK focused 登记 73 个来源，72 个通过；run_command_execution_limits 六案过四案、错两案。powershell-exact 在 15019ms 退场，powershell-host-timeout 在 8031ms 退场，均报 process.timeout。实际创建、挂 Job 与恢复线程成功，三处 shell 入口未确认，探针 started 缺失。Job accounting 各报两条活动进程；原件没有映像名，根因未定。

公开什么：不添公开 API。只扩展既有私有诊断缓冲。在 Windows 前台超时路、CloseJob 之前，取一次 JobObjectBasicProcessIdList，最多读十六个成员 PID。逐项只读进程映像末段，保存最多六十四枚 UTF-16 code unit；完整路径、命令行、环境和内存均不取。原枚举尾部追加，旧数字不挪。映像查询失败保留原生错误，截断明确标注；不把空值算确认。

谁拥有：实际 RunProcess caller 借用测试缓冲，固定 POD 槽由单写者发布。空缓冲跳过全部查询，不添时钟或原子。查询仅开 PROCESS_QUERY_LIMITED_INFORMATION 句柄，使用固定栈数组，不调用外部程序、不枚举全机、不重试。每只观察句柄当场关闭；还原 LastError 与 errno，不改业务返回。进程可在两次查询之间退出，名单只是一次现场观察，不能冒称完整进程树或等待链。

怎样关场：先沿原路记 accounting，再观察成员，继而执行原 CloseJob/fallback、原终止等待和真实 reader join。不保活成员、不改变 Job 权限、不延后业务截止、不另加成功出口。系统查询本身不许诺硬实时；这是 opt-in 诊断成本，不纳入默认入口。PID 与映像不证明 PowerShell 卡在哪条指令，仍须结合入口、CPU、后续实际原件追因。

怎么验：保留六个 CASE、全部时限、输出帽、probe、Started 断言与旧注册守卫。测试日志在调用返回后读发布槽，输出名单查询返回码、成员 PID、UTF-16 原数、截断与映像查询结果。三平台同源 SDK focused、SDK ON/OFF、安装消费、全 CLI、ASan 和 TSan 仍须实跑；不能用新增资料盖旧失败，不能把新源结果借给 023d4caa。禁止本地 CI、configure、构建、原生与 HTTP 验收。

本合同只补现场材料，尚未修定根因；Title 和八步路线均未收口。023d4caa 失败原件与此前 d9c71ee0 ASan 断言失败另存，均保留。
