# 标题精炼起飞失败收场

2026-10-10。第 2 步自动 Memory 前置。#343 已同源验收、合入 bf465cb7，与验收源 f5cfdbd0 同树。八份待迁基线已核实际合并树；本合同先提交，再接实现。

公开什么：SDK 不添线程工厂。内部 SessionTitleRefiner 接住前台旁路绑定与线程创建异常，给一次失败 Outcome，保住 model 与 generation，不泄露异常内容。本地标题、单飞、取消、七秒有界关场和迟到标题代数门照旧。

谁拥有：前台先备失败槽与稳定错误串。绑定失败立即释放未用 Backend，不起线程；线程创建失败由闭包销毁独占 Backend。Start 已接任务便返回 true，Busy 和 Ready 同为真；TakeFinished 收一次，复位后可再起。空 Backend、空 model、上一笔未收仍返回 false，不消费调用方 Backend。

怎样关场：未进 RefineSessionTitle 就不记 cheap 调用、不编用量。Outcome 的 refinement_invoked 仅指进入精炼函数，不能冒称物理 Backend 发送数；真实传输失败仍保留旧计数和报告缺席。CLI 收货经同一私有记账函数；标题采用仍走实际 SessionTitleAccount，失败不换本地标题、不追加生成标题。title.requested 保留接受本次尝试含义，不拿它作物理发送证明；prepared/sent 才走真实旁路回调。

私有 TLS 故障口只在测试构建定义，不装入 SDK。七案核标准与未知创建失败、单飞与真实重启、无效入口、TLS 隔离、标准与未知绑定失败、实际标题采用和一次收货。证明须含真实 Backend 调用/析构数、真实 ModelUsageLedger、真实场次与标题账。Windows/Linux/macOS 全 CLI、SDK ON/OFF、九套消费及 ASan 原件齐后才验收；旧来源不能删。禁止本地 CI。

此片不补后台函数体异常总出口，不宣称捕获对象析构已结束，也不准据此公开自动 Memory。后台异常、公开回调重入与 sent 后物理交接另交。

前驱修复已把 V3 writer 加入 TSan 十三条插桩路径；本片沿用这条检查，不用旧 CI 草稿覆盖。前驱十五条注册中，十三份 Workflow 有案，一份已退役为空，另有 owner 十六案；空来源不计有效测试。
