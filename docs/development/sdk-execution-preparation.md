# SDK 执行准备的共用私有阶段

基线 `d3fc1c42`。这笔只拆原 Local Initialize 的真实装配；Local 立即改走
这些阶段。Managed 存储入口不变，不启动它的 worker，不添公共方法或激活票。

原参数校验、InitCleanupScope、initialization_backend、SourceScope、workspace
裁决留在 Initialize 原位。关着的 Memory/Write/Skills 等仍经原 Prepare 得到
真实关闭态模块，不能拿空指针或伪成功报告代替 Run/Complete 的依赖。

私有阶段同步套接：

1. WithPreparedExecution 按原顺序准备模块、工具表、Backend/MCP 和唯一
   SessionResources 候选；实际局部持有这些值，只借视图给下一阶段。
2. WithPreparedLaunch 组装原 SessionLaunchRequest 与真实 opening participant，
   Source 回调一直活到 Service 开场及执行绑定完成。
3. OpenLocalPreparedStorage 调原 SessionService 构造，保错误分类和首错次序。
4. WithPreparedBinding 读取实际 Session 身份，冻结结果策略，绑模块/工具，
   准备实际 AgentProfile 与恢复历史。
5. AdoptLocalExecution 调原 Service::InitializeExecution、LoadOperations，最后
   发布模块借用和快照。它消费第一阶段那份资源，不另造 Agent 或资源图。

视图只含指向上述实际局部的引用，不可复制或移动；消费回调是同步模板调用，
不装 std::function、不分配回调盒，也不把视图藏进 Service 或 profile。
registry_factory 仍由原 BuildSessionResources 同步消费；其 moved-from 源
仍在原准备栈内退休。模块/participant 的准备与采用顺序一字不改。

退出次序须能逐段还原：绑定局部先退，再退 launch/participant 源，再退
资源准备局部与 workspace；最后 SourceScope 清原 options 捕获，Backend
锚随后退，TLS guard 压轴。失败与异常走同一逆序，不提前清借用。

以后 Managed 文本入口可在已验证 workspace 与真实准入之后消费同一准备阶段，
替换实际 Service/执行采用口；本片不提供这项能力，也不让 storage 直接 Start。

完整原 Initialize 按移动、参数别名及同步调用机械反剥核字节；原 CPP 业务
语句、原生 CASE、断言、时限、公共 SDK 与构建名册保留。本地只查源码、
文档及纯数据，编译、CTest、原生与 HTTP 留给远端 CI。
