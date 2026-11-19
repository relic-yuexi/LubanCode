# 模型每次发送前的内部执行门

基线 `37ba5743`。这笔只补 AgentLoop 真正发送前的强制点，不开放 Managed
会话，也不另造身份表。后续宿主须让闭包持已冻结的原发起者、操作与资源，
每次现查许可；不能换成当前读请求操作者，也不能缓存上次允许决定。

`AgentWiring::on_model_send_gate` 接收本次真实 Request 和 ModelRequestAttempt，
返回 `std::expected<void, std::string>`。空门保 LocalTrusted 原行为。门在
`run_one_attempt` 内每次调用，首发、恢复重试及后续模型步都经过；位置在
prepared 落稳之后、`commit_sent` 和 sent 事实之前。外层 pre-request hook
不替这道门。prepared、恢复 Started 和其他先前事实照留，不称拒绝前零 I/O。

门拒绝固定报 `model.send.denied`，抛异常固定报 `model.send.gate_exception`。
返回字符串和异常正文不进日志、模型或用户消息。两码都不重试。只捕获门自身
调用异常，不把旁边分配、轨迹记录、预算钩子的异常兜成授权失败。
门返回后再查真实取消旗；取消沿原取消出口收场，本次不再送后端。

首发被拦，归还尚未提交的预算名额。重试被拦，先前已发名额与 sent 事实
保留，不退账。当前 prepared 沿原失败或取消口收场；不伪造响应或 usage。
恢复 Started 计尝试入场，并不证明后端已经发送。

宿主仍按原 AgentWiring/ScopedTurnBindings 生命周期装配和退捕获。门同步调用，
宿主不能在回调中销毁执行 owner、并发改 wiring 或重入同一 Agent；将来 Policy
接线须用既有回调屏障，且不持 SDK 大锁调宿主代码。这笔不接 Policy 本身。

验收嵌入原恢复测试 CASE，旧 CASE、断言和预算保留。真实 Agent::Run、后端、
边界记录器与 TurnBudgetAccount 对账：首发拒绝及异常零发送、重试前撤权保首趟
已发事实、取消压住后续发送、空门和允许门保持原请求/重试/计费，机密失败正文
不外泄。只直接调门函数不算验收。

范围只含这条 AgentLoop。SampleModel、摘要、Compact、独立恢复发送以及
后端内部网络重试尚未接这道门；新 Managed 宿主不能据此开放这些旁路。
不声称发送和远端撤权原子化，也不回收已经交给后端的请求。

本地只查源码、文档和纯数据。编译与原生验收留给本源远端 CI。
