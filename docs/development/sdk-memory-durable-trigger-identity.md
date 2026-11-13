# Memory 持久触发身份

2026-10-09。第 2 步自动 Memory 前置。基线为 #341 实际合入 `16d02a22`，与已验源 `db592938` 同树。本合同先提交，再接实现。

公开什么：修正 MemoryTurnLedger 评估事实中的触发轮号。SDK 不添人工注册轮号入口。V2 payload.turn_id、V3 信封 turnId 与 payload.turnId 都须指实际触发轮，不能借当前新轮或最近 user 消息猜号。事件名、用量缺席语义、收货门和 CLI 行为照旧。

谁拥有：SuspendTurn 冻结旧轮到 suspended_turn_id_，再清 state_.turn_id，保住回合间写回执无轮号。私有评估方法显式收 trigger_turn_id：同步 FinishTurn 传当前轮，匹配 SettleSuspendedTurn 和新轮冲账传旧悬账轮。不暂填回已清空 state_，也不让 Outcome 自报号覆盖已冻归属。

怎样关场：匹配结果只评估一次，重复收货不追加。新轮冲旧悬账，先追加旧轮 aborted，再开新轮；旧结果迟到不补账、不挂新轮。换代弃账不写新场，错号收货不编评估。save / forget / accept 的回合间写回执仍无旧 turnId，jobId 和已确认写事实照旧。

验收走真实主桥 BeginTurn / RecordInput、worker、Backend、ProjectMemory、SettleTurnMemory 与 Journal。新增七案核成功、失败、新轮冲账、回合间回执、弃账、重复及错号收货、恢复投影。V3 核实际输入与评估同号、信封及载荷同号，再走真实同 ID continuation；V2 由实际旧盘和 RecoverWorkspace 恢复 writer，核输入、评估及封卷重读。不用退役环境开关冒充 V2，不手填评估或 Outcome。

旧盘装配 friend 仅在测试 source 定义，接住真实恢复器已持 owner、锁和材料。内部头只留声明，SDK 不添入口或故障函数。Windows/Linux/macOS 全 CLI、SDK Lua ON/OFF、九套移位消费及 ASan 须有同源原件，旧名单不得删减。两道 ASan selector、required 登记、七案七标记须接新 source。禁止本地 CI、构建、原生或 HTTP 验收。

此片只管评估身份。旁路 late preparation 的 parentTurnId、可撤销借用、writer 回执分类和公开自动 Memory owner 另交；不凭评估归号宣称这些也齐了。

验收补记，2026-10-09：候选 `4d224179` 的 Windows 全 CLI 有 801 来源，800 通过；`unit.runtime.child_history_adoption` 在默认 180 秒处超时。最后阶段为 scope-reuse 的真实 Run 入口，不能凭这个入口断言内部死锁。该候选持久身份七案七标记实际通过；全套失败原件另存，整片未验收。

同次原件中，SDK focused 跑的是同一份 `test_child_history_adoption.cpp`，八案完整通过，用时 270.773 秒，既有预算 300 秒。后续只把 Windows CLI 这份实际多场 Journal / child 链来源的 CTest 预算对齐 300 秒；其他来源和平台照旧。命令、八案、断言、真实 writer、事件和耐久条件不得删减，不以跳过或缩小来源换绿灯。修复后重新收三平台同源全量原件，不能挪用旧候选绿灯。

验收补记，2026-10-10：候选 `0148bc57` 的 Windows Test 已过，两份 child-history JUnit 原件均无失败，CLI 来源用时 92.4965 秒。提取器仍将 CLI 预算固定为 180 秒，拒收实际 300 秒注册，报 `Full child-history registration changed its timeout or disabled the source`；该提取步骤失败，整片仍未验收。

修复先交这条补记，再让提取器依明确平台核预算：Windows CLI 只认 300 秒，POSIX CLI 只认既有 180 秒，SDK 两平台都只认 300 秒。不可改成接受任意预算，也不可只信注册表自报值。纯提取证明保留全部旧案，补跨平台借错预算的拒收；真实八案、断言、命令、阶段配对、成功标记及坏原件留存检查均保留。禁止本地执行这些 CI 门或测试，修后换新源重新收三平台远端原件。
