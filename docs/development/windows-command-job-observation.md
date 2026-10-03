# Windows command timeout Job observation

本笔只补内部 opt-in 诊断。公开 SDK、CLI 命令行为和限额不变。基线固定 `9f67c1cfb0f6fc0407b91e8c663fd5f8f408ddb2`，独立分支 `codex/windows-command-job-observation`；旧 `codex/sdk-job-current-integration` 与 ignored 原件保留。

## 实际缺件

#322 `5e7e0a02` 的 Windows full 真实 `powershell-exact` 超时：Create / Assign / Resume 成功，15 秒落在根进程等待内；关 Job 后才见零字节 EOF，reader join 很快。started / done absent 不证明探针从没创建。现有材料没有该 Job 的进程统计，不能据此认唯一根因。

原账：CI `37137672506`，job `111245509007`；`failure-final-proof.json` SHA256 `95aa32daca5b473b2da4e48090fd6074631b9136009c195fab48f5331c329ba5`。原红保留，不 rerun，不借父头绿账验本笔。

## 单次观察

仅前台 Windows `RunProcess` 已记录真实 `TimeoutObserved` 后、关现有 Job 前，且借入 `ProcessDiagnosticBuffer` 非空、Job 仍非空，调用一次 `QueryInformationJobObject(..., JobObjectBasicAccountingInformation, ...)`。取消和输出超帽不触发该 query。没有 Job 的旧 fallback 不添虚构统计。

只留原 query rc / GetLastError、TotalProcesses、ActiveProcesses、TotalTerminatedProcesses、TotalUserTime、TotalKernelTime。CPU 保原 100 ns 数值，进程计数不冒充探针身份或业务任务数。成功 API 的 LastError 可能陈旧；失败以 rc 为准。query 失败时统计值不可采用，JSON 明列 `query_failed`、`values=null`。

借用只跨当前同步调用。记录沿现有 128 槽固定 POD；尾添统计 POD 和两枚 stage，不改既有枚举值。单槽写完再 release 发布，序列化只读 acquire 已发布槽。槽满、未发布或缺阶段保未知，不能倒推成功。

新增 query 保存进入时 errno / GetLastError，离开时恢复。额外观察不得改原判断、错误分支、句柄权属、等待、杀树、reader join、输出或环境。默认 buffer=null 路不添 query、诊断时钟或原子操作。不增线程、callback、堆分配、PID 枚举、句柄、重试或 child 输出。

## 测试资料与边界

原返回 JSON 的 native record 只给真实 query After 添有界统计字段；序列化仍在真实 RunProcess / reader join 完成后，沿既有测试诊断异常护栏。它不发第二次 query，不新增 Status / 文件读取，不更改工具结果。

原六 TEST_CASE、所有 CHECK / REQUIRE、15s / 8s 等预算、输入命令、PS wrapper、probe、markers 与清场次序逐字保留。反剥新增诊断后，三个源码须恢复基线原字节。真实原生只跑远端三平台；本地仅源码 / 数据 / 文档检查。本笔不称修好超时或证明某个底因。

实现限定：`src/platform/process_diagnostics.hpp`、`src/platform/process_win.cpp`、`tests/unit/tools/test_run_command_execution_limits.cpp`。不加公共 API、source target 或链接依赖；CMake / CI / 目录由根代理另审。
