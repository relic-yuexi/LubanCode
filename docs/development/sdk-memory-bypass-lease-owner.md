# 旁路短借 owner 合同

2026-10-10。第 2 步自动 Memory 前置。#342 已验、已合于 77b8817f，与验收源 1b912d47 同树。本合同先提交，再接实现。

公开什么：SDK 不添线程工厂或裸 recorder。CLI Memory、Title 从前台绑定真实旁路桥，冻结触发轮号，再把自持 Proxy 交给 worker。接过场却已退借须明确拒绝；空 recorder 不能被误认成准许无账发送。V3 Memory prepared 仍挂旧触发轮 parentTurnId，Title 仍挂首问轮；V2 保住原事件与材料。

谁拥有：每只 Proxy 持共享 State 和独占桥 Slot；场次只记 weak Slot。Bind 与 Retire 共用短门，过期 Slot 在下次 Bind 清理。Bind 门内只造真实桥，不调模型、不起线程、不重入 owner。准备、sent、usage、完成、失败、取消各借一次；Backend 在门外跑。真实 writer 和 Observer 回调沿既有可信宿主约束，不能重入同一 owner。

装配次序：宿主控制线程串行调用 ledger 的绑定、换场与 Observer 更换。worker 只拿已冻 Proxy，不再调用 ledger 工厂。十六案中的 bind-retire 并发案直接走共享 owner 与真实桥工厂，证明短门阻止退借穿过正在绑定的桥。SDK Session 接入时须在自身调度口保住这条装配次序，再交关闭期间绑定的实际证明；不能把 owner 原语当作整只 ledger 的并发入口。

怎样关场：Retire 等正在用桥那次短借退出，再销毁所有仍活真实桥、清空借用；旧 Proxy 晚归只摸自持 State，不碰旧 writer、books 或 observer。Retire 幂等，旧 owner 不复开。move、析构、真实 Close、Clear、Resume 及 Observer 更换都退旧借；新场、同 ID continuation、Observer 同地址复用均须重新绑定。

Clear 的忙碌、Managed 等前置拒绝和新侧准备先走。只有管理器真正进入 CancelActiveTurn 才退旧借；早拒不可误伤原场。成功切换造新 owner；毁旧场后失败不准盲目复开。第 2 步失败尚未取消，旧桥抵真实 broken writer 并留诊断；第 4 步失败已经退借，晚代理不再触桥。one_shot 拒绝属于恢复源预检，当前 Clear 没有这条门。

Memory 前台绑定捕获标准与未知异常，保住 worker_execution_failed 终态、model/task/generation/turn 和一次收货；Backend 未调用便释放。已归还用量照实保留，抽取 Run 入口计数仍按旧语义，不能改称物理 Backend 调用次数。诊断由共享容器持有，Append、原有 128 条条件追加及 Snapshot 共锁；不替其他入口添新限额，也不许拿新容器遮住 I/O 失败。

晚归成功仍可带真实用量。SampleModel 不按 OnOutputCompleted 返回值改写已完成结果；旁路撤销门挡旧场追加，CLI generation 门另挡旧场采用。sent 已确认至物理调用之间仍有交接窗，detach 也不等于 Backend 已停。

验收备十六案：冻结触发身份、发送前关闭、已发送晚归、move、析构、Observer 同地址更换、同 ID continuation、绑定异常、真实 Clear、恢复早拒、Clear 第 1/2/4 步失败、绑定与退借并发、真实诊断与快照并发、clear.busy 和 Managed 早拒。Memory 与 Title 均走实际入口；V2 用真实旧盘与 RecoverWorkspace，普通 V3 用真实 Open。Managed 用实际 Reserve / PublishOwnership / Finish / LaunchManagedSession，核归属原件、活锁与真实 owner，再由共用测试 friend 组装内部账本。Local Clear 拒绝后旧 worker 照跑，Journal 与归属侧记不得变场；这只证存储和借用，没交公开 Managed API 或 Policy 授权。诊断案分 V3 主轮/子 Agent/旁路和 V2 Workflow node 两组，不套用 V3 缺失的旧 Workflow 工厂。既有 WorkflowNodeSessions 独立 V3 节点也须改收共享诊断容器、读加锁快照，不能保留裸 vector 调用去凑类型。旧 Workflow 来源须全跑；这处调用适配不冒称十六案已专门证明独立 V3 节点的故障并发。

十六案十六标记须在 Windows/Linux/macOS 同源全 CLI 原件里出现；SDK Lua ON/OFF、九套移位消费、ASan 旧名单不得删减。实际 TSan 须跑旧 Workflow 来源和新增 source，并核主桥、子桥、Workflow、旁路、账本、Memory、Title、证明 source、管理器、Managed 预留和归属，以及独立 Workflow 节点场十二份编译命令确有插桩。跳过不算通过。禁止本地 CI、构建、原生或 HTTP 验收。

未齐部分照实留账：本片全部案例尚待原生证明；sent 后物理交接、Title 前台绑定与线程总出口、公开 Backend 回调及捕获对象析构重入还没补齐。主轮/子 Agent/Workflow 其他 raw 借用和 V3 Workflow 工厂另交。此片不凭 Journal 撤销门宣称公开自动 Memory owner 已完成。
