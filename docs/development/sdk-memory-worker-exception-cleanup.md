# Memory 后台异常收场合同

2026-10-09。第 2 步自动 Memory 前置。基线为 #340 实际合入 `9da20137`，与已验源 `e3d725fc` 同树。Windows/Linux/macOS、SDK Lua ON/OFF、九套安装消费与 ASan 原件已齐。本合同先提交，再动实现。

## 公开什么

只补 TurnMemoryExtractor 已起 worker 的异常总出口。Start 仍表接纳，Busy 仍含在途与待收，Ready / TakeFinished 仍收一次。已有 transport、deadline、解析、取消结果沿旧分类，不改成后台异常。

内部枚举末尾添 WorkerExecutionFailed，对外稳定短码 worker_execution_failed。标准与未知异常都产失败槽；不透传 what()、提示、正文或凭据。公开 SDK 不添线程工厂、宿主调度器或故障环境变量。

线程未起仍走 worker_start_failed；线程起了却未进同步抽取，不补造模型调用。真实抽取已进场，分角色调用沿 extraction_invoked 计数，不拿 usage_reported 当开关。

## 谁拥有

前台先冻结失败身份、短文案与所需结果槽，再交独占 Backend 和值材料给 worker。异常 handler 不再拼字符串、复制身份或解析正文。所有 worker 语句，包括准备 Outcome、旁路桥、提示采样、结果解析及发布，都须落在受控边界内。

成功槽与失败槽只有一位发布者。选定槽后才立 done；主线程只看 done，不碰尚在写的值。实现须静态核 move / swap 不抛的前提，不能把 mutex 锁失败、析构抛错或全进程耗尽内存也写成保证。

若 accounting 已写回，只取现成值；不能在 catch 中把 reported 改假、把已有用量清零，或补一条猜出的 committed。旁路既有落盘材料不撤销、不自动重试。SampleModel 内部局部值尚未归还就遇 recorder 异常时，CLI 目前拿不到那份用量；本片只能保住已归还的账，持久来源与未知提交另交，不能宣称这处已齐。

后台闭包不碰 TurnMemoryExtractor 本体。成功与失败都在 worker 尾部退场，TakeFinished join 后再交结果，Backend 真销毁。旧 raw ledger 旁路借用未撤销，本片不能凭捕异常宣称寿命安全。

## 怎样关场

RequestCancel 仍只拉旗。析构保留五秒等待窗，忽略取消的旧 60000 ms 案照旧须在规定退出窗内返回；不能借异常修补改成无界 join，也不能把 detach 写成线程已停止。

执行失败交实际 SettleTurnMemory，结清匹配悬账，记稳定失败码，不把部分解析候选入队。新场仍拒旧场候选与评估；换代门前只记录真调用。悬账持久 turn 身份另交，不能靠手拼 Outcome 掩住旧缺号。

## 必需验收

内部故障口只在测试构建提供。前台冻结测试回调供 worker 持值调用，不让 worker 读前台 thread_local；每案恢复故障口，隔离另一只 owner。注入实际执行边界，不能手拼 Outcome 当异常总出口证据。

须核标准异常、未知异常、同步抽取之前、真实采样用量归还之后、结果发布之前、失败后恢复、取消与有界退出。至少一案先让真实 Backend 产 usage，再在真实归还之后抛错，核调用与已有账仍在。另核真实 Backend 抛错仍交 SampleModel 既有 transport 分类，不冒充 worker_execution_failed。

真实 ProjectMemory、TrajectorySessionLedger、MemoryTurnLedger 与 SettleTurnMemory 须接完整收货路，失败不入候选。五案启动失败与旧 Memory 全册保住。新增 source / CASE 与实际标记进两道 ASan selector、required 登记和执行门；最终数量以远端原件为准。

Windows/Linux/macOS 全 CLI、SDK Lua ON/OFF、移位安装消费与 ASan 同源原件齐了才交付。禁止本地 CI、configure、build、原生及 HTTP 验收。本片仍须独立同源验收，不借用 #340 绿灯。
