# CLI 单次提问共用执行 owner

基线 `8be71c94`。这笔先统一实际 Agent 创建与退场，不冒称 CLI 已经改走
公开 SDK。SDK 的文本后端值还不能替代 CLI 的 provider wire、媒体与推理请求。

`ExecutionOwner` 新接内部 `HostBorrowedExecutionResources`。它明确借用宿主
现有 Backend 与 ToolRegistry，只持自己的 Agent；两份宿主资源必须活得更久。
这类执行不是子 Agent，也不是完整自持的 SessionResources。
`session_resources()` 仍返回空值，不捏造资源图。

AskOnce 继续持实际 SpinnerBackend 与 ToolRuntime，只把直接构造 Agent 换为
这条共用创建口。profile 捕获在创建成功或失败时沿原 Construct 清空；
Agent 先销毁，宿主工具、Spinner 与真实后端随后退场。同步调用未退出时，
宿主不能先销毁这些资源。这笔不增加线程、取消协议或后台借用。

原 AskOnce 在栈上 move profile。这条共用 Construct 会复制 profile，并首次
分配宿主稳定槽，故多一次分配，复制也可能失败。两条路不承诺同样的分配成本；
失败时沿已有 owner 合同先清捕获，再退宿主资源。

模型配置、prompt、Memory、Package、权限、RunTurn 与 ScopedTurnBindings
沿原路走。SessionService 受理、派发、异步工具、关账和 harness 导出也沿原路走。
主交互入口、公开 SDK 的 CLI 后端适配与完整 CLI parity 仍待下一笔。

保留原 ExecutionOwner 七 CASE、旧断言、恢复语义和预算。原册另核真实宿主
借用：同一 Backend 与 ToolRegistry 收到实际工具回环；Agent 捕获销毁后
原工具与后端仍能用；profile 复制失败先退捕获，不销毁宿主资源。
这些验收只管共用 owner，不冒称调用了完整 AskOnce 端到端路径。

远端 CI 须在本源跑三平台 SDK-only、组合、全量、ASan，并编译实际 AskOnce
接线。后续完整 CLI 迁移须另补真实命令与 provider wire 的端到端证据。
本地只改源码、查文档与纯数据，不 configure、编译、跑 CTest、原生或 HTTP。
