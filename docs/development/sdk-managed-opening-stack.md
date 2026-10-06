# Managed 新场的内部存储开场链

基线 `80bb443c`。这笔只把现成 Managed 准入交到真实运行时账本。
不新增公共 SDK Managed 入口，不交业务授权、操作来源或模型执行。

新内部入口消费 Finish 产出的 move-only ManagedSessionDirectory 和创建审计值。
SessionService → SessionRuntime → TrajectorySessionLedger → SessionManager
逐层 move 同一份目录、真 SessionLock 与首发布回执。最后仍由现
LaunchManagedSession 验原 durable publication、sidecar、workspace 与创建来源，
再走共用 V3 装配；不能重拿锁、重新 Publish 或先开 Local 场再补身份。
可复制 Options 只放配置，不放 bundle 或可重复消费 bundle 的回调。

这条入口须显式给 workspaces root 与冻结 WorkspaceIdentity，不读 HOME、
不按临时 cwd 另猜身份。resume 标志、来源场或不相容开场参数明确拒绝，
不退回 Local。每层保明确 ManagedStorageOnly 模式，开场失败也不能变成
Local 启动或换目录的跳板。

本片只准实际开场、读取原生 ownership publication 与 Close。Submit、Pop、
InitializeExecution、业务域命令、cwd 换场、clear/resume 和模型轮桥均拒绝。
不为将来 provenance 或 Policy 安一只空授权壳。后续解除相应业务口时，
仍须使用已有 SessionExecution/Agent 栈，并补真实授权与持久来源。

只读 publication 引用来自实际 ActiveSession，保存原首回执，不由文件存在
重新推断。成功关闭沿原 V3 Close，writer 与附属写者退完才放唯一锁。
开场失败沿真实 RAII 退候选，marker 和非空账保留；Unknown 不被读回洗白。
只读值不带 Writer、锁或权限，不把它称为授权句柄。

Local 原构造、恢复、业务方法、CASE、断言和时限照旧。验收嵌原 Managed
reservation 册：真 Reserve/Publish/Finish 经 Service 下传，核首行 metadata、
held-lock 排他、同项目两场隔离、Close 后可重新取锁、失败残留与业务口拒绝。
不增加原生注册，不拿模型桩成功冒充授权。

本地只查文本、文档与纯数据；编译、CTest、原生和 HTTP 只留远端 CI。
