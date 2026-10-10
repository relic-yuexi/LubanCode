# SDK Backend 回调与最终析构边界







2026-10-10。#346 已同源验收、实际合入 `784fd543`，与验收源 `695ea229` 同树。合同 `25c8beb8` 先于初始实现，修订合同 `e21ee5d1` 先于 `de6072a6`，私有守卫构造、析构归 SDK 模块；交接合同 `e900fe9e` 先于 `695ea229`，测试局部回调显式退掉。两轮失败原件照留，原断言与故障注入不减。十九案与七枚 owner 标记在三平台与 ASan 齐。控制块分配失败、观察口异常均先退捕获、后退 Backend；TSan 本片跳过，不计通过。公开自动 Memory owner 仍待后续。







公开什么：不添公开线程工厂，不改 Backend::Generate 签名。SDK 调用公开 Backend 时自行升 CallbackScope；宿主回调中的阻塞 Close、Runtime::Shutdown 与其他现有生命周期拒绝口，须先报 sdk.lifecycle.reentrant，再碰锁。非阻塞查询及 Cancel 沿现有合同。







谁拥有：Session 接过 unique Backend 后，以 SDK 自持共享控制块管理。控制块内藏唯一 Backend，最后归还先升回调守卫，再析构 Backend；所有 adapter 和后台值借用都分这枚控制块。先分配空控制块，成功后才从 SessionOptions 交接 Backend。分配失败，Backend 仍在原处；先清工具与扩展捕获，再走原关场析构。不提前释放裸指针，不漏删、不重删。观察口只在调用方接稳共享锚后执行，抛异常也先清捕获、后销毁 Backend。未通过前置准入、还在 SessionOptions 中的 Backend 继续走原 InitCleanupScope / CleanupScope。







怎样关场：Generate 的守卫只罩 SDK 调用宿主那段，异常也恢复原 TLS 值；嵌套守卫不能误清外层。最后一枚共享借用归还时，析构中的阻塞重入同样先拒，不能在已经持有 Close 锁时再等自己。Close 不据此宣称取消已经停止 Backend，不销毁仍有借用对象，不把宿主私下另持裸指针算作 SDK 归属。







证明备七案：后台线程回调、两个会话互关、关场后最后借用析构、真实 Session 收场析构、标准及未知异常、嵌套异常与 TLS 恢复、控制块分配失败及观察口异常回滚。最后一案用私有 allocator 在 allocate_shared 实际分配入口抛 bad_alloc；检查捕获先退、Backend 后退且只退一次，不能拿分配前假故障冒充控制块分配失败。每案都走实际共享 SDK adapter。测试观察口只在测试构建里记下 OpenSession 实际接管那枚控制块，再由 weak 值取得借用；不另造替身 owner，不动公开头。互关用五秒有界门，不能靠无限等候掩盖失败。生成与析构都连真实 Runtime、Session、Close、Shutdown、查询及 Cancel。旧十二案和五枚线程开场标记照留，新七案七枚标记须实际执行。普通安装 SDK 不留故障口；三平台全 CLI、SDK ON/OFF、九套消费和 ASan 同源原件齐后才验收。禁止本地 CI。







这片只补公开 Backend 边界。自动 Memory 配置、原触发轮收账、候选保存、授权、恢复、后台任务 owner 及完整 CLI 迁移仍各有合同；不得凭回调守卫宣称自动 Memory 已公开。






八份输入已核实际前驱合并树：七份旧文件逐字相符，新私有头尚不存在。沿用前驱十三条 TSan 插桩路径，不覆盖其 CI；十五条注册含一份退役空 Workflow，不拿空来源充有效测试。
