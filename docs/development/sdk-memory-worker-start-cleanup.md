# Memory 抽取线程启动失败合同

2026-10-09。第 2 步自动 Memory 前置。基线 `52b71aa8`：#339 已验、已合，实际合并与验收源 `5c463e9b` 同树。本合同先提交，再改实现。

## 公开什么

补 CLI TurnMemoryExtractor 真正创建 worker 时失败那条路。线程没起，不调用 Backend，不落旁路账，不把失败记成 in_flight_dropped。

公开面沿用现有 SDK。本片交宿主错误码 `worker_start_failed`；内部抽取错误枚举末尾添码，旧值与公开 SDK 签名保住。故障注入口归原生测试，不加公开线程工厂或宿主调度接口。公开自动 Memory 入口另有 owner 合同。

保留 Start / Busy / Ready / TakeFinished 调用方式。Start 的 true 表示任务已接纳；创建失败留下已完成失败槽，由原收账路取走。false 仍只表示无效输入或单飞拒绝。接纳不等于 worker 已起。失败槽带原 turn_id、session_generation、model、task_type，零请求、无申报 usage、无候选。错误码单列，不能借 transport_failed 或 route_miss。

## 谁拥有

前台先冻结失败身份与共享槽，再进入真实 std::thread 创建边界。创建成功才交出 worker 句柄；创建失败销毁闭包中独占 Backend，留下确定终态。不得先移动身份，再从空 Inputs 拼回。每个失败槽只收一次；收过即空闲，可再启动。

只接线程创建边界抛出的标准及未知异常，包括构造 std::thread 内部分配失败。冻结身份或失败槽时分配失败仍向调用方传播；后台执行异常另立一片。本片不许宣称 Start 全面 noexcept，也不以一个 catch-all 吞掉所有错误。短文案不得拼 what() 原文，更不得捎上提示或凭据。

## 怎样关场

创建失败没有线程可 join 或 detach。RequestCancel、析构、TakeFinished 都须安全。成功路径沿旧取消、单飞和五秒退出窗。原忽略取消后延迟返回案仍须过；不得改成无界等待。

失败结果交原 SettleTurnMemory，结清匹配悬账，记 failed 与 worker_start_failed，不入候选队列、不补造 usage。实际 Journal 在创建失败前后逐行相同；收账只追加真实失败评估。

后台异常总出口、旁路借用撤销、完整持久身份及用量来源另有欠账。现有 SuspendTurn 清掉 state_.turn_id，迟到 assessed 仍读此字段；值槽保 ID 与内部对档不能冒称持久身份已齐。V3 用量投影与晚归裸 ledger 借用也尚未补齐。公开自动 Memory owner 上线前须各自验清，旧材料不回写。

## 必需验收

注入点放在内部真实创建边界，仅测试构建提供；不加生产 thread_factory、环境变量或公开 SDK 故障接口。原生五案证明标准失败、未知失败、身份不丢、Backend 销毁且从未调用、完成槽一次收取、失败后再启动、待收单飞、取消和析构无 joinable 泄漏。

新增 `unit.app.memory_extractor_start`，五案分别留下 standard、nonstandard、restart、gates、settlement 标记。两道原 ASan selector 与首轮 required 名单都添这册；执行门核五案及五条实际标记，不能只增登记。与 #339 原件比较，全量名单只能增这条，ASan 来源只能增这册，CASE 只增五案。最终数量取远端原件，不拿预期冒充实跑。

故障口只在 lubancode_app 的 BUILD_TESTING 构建中定义，线程本地、作用域恢复。SDK 不装配 app，普通宿主构建无故障定义。测试口不进入公开 include，SDK 安装材料门也拒带这份私有头。

真实 CLI 悬账收口须走 ProjectMemory、TrajectorySessionLedger 与 SettleTurnMemory，不只测手拼 Outcome。保住原抽取、候审、队列、取消、晚归与换代案。Windows/Linux/macOS 全 CLI、SDK Lua ON/OFF、移位安装消费及 ASan 同源远端原件齐了才交付。禁止本地 CI、configure、build、原生或 HTTP 验收。
