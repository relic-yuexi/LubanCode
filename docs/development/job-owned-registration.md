# Job owned 暂存注册：J2a

合同在先，实现随后。固定基线为 J1 `ee577795527c7ef91e3bf7e4ee6b2898e024a46b`，私有 Draft #311。只添内部真实暂存注册；J1 仍拒缺完整 scope/Post 的后台请求，SDK 默认关闭。没有 Adopt/worker/公开 Accepted，不接 Session Close 产品规则、Runner 或 Detached。本地不 configure、编译、CTest 或执行原生。

## 入口与真实声明

候选 `RegisterPreparedJob` 收最终 Prepare owned 值、原 provider 调用号、assistant messageRef、父声明 action/turn/step、原入参、宿主 project/cwd 与执行策略。provider ID/声明锚必填；toolName/args 本身不能证明来源。父 operation 只存 causal 材料，不塞进 Job invocation。最终有效参数另存 owned 值及真实摘要，不能改写原 assistant 声明或冒称原声明已经采用了改参。

新登记域须显式配置同会话 writer 共享串行锁。锁顺序固定为 writer serial → jobs；真实读取、审批或宿主回调都不能握 jobs_mutex。握共享锁，读实际已校验账，逐项核同 SID/run、assistant 角色、精确 messageRef/provider ID/原工具与原入参、真实 turn/step/action 和接纳事实；歧义/缺件/坏账拒绝。锁下再次核实际 writer/session 与关闭状态，再注册。不得用宿主传入名/参数比对自己造的 snapshot，也不按工具同名猜调用。

共享锁不仅保护新函数。新登记域不与旧未遵守这把锁的 Legacy Start/early/恢复派工混用；新域这些入口明确拒，旧默认实例原路不变。新暂存表与旧可派 FIFO 分开。首笔没有 SDK read-budget 接线，不声称所有恢复读面都已迁入；后续 J3 须沿宿主实际预算，而非另添全局硬帽。

内部 API 定为 `Options::prepared_registration` 可选可信宿主 context（同 writer 的 `shared_ptr<recursive_mutex>`、project/cwd）；配置即启 registration-only 域。`PreparedOwner()` 回实际 SID/run、协调器号、epoch、project/cwd；这些值只供匹配。`RegisterPreparedJob(PreparedJobRequest)` 收原声明与最终值，回 `Rejected / Registered / Unconfirmed`、owned facts 和真实两枚写回执。`GetPreparedJob(owner, job_id)` 只读本暂存表；没有 Adopt 接口。DTO 只依赖 tools/trajectory 中立值，不 include AgentLoop 或复制 TurnWiring。

读面须核账尾 seq/hash 等于实际 writer 的 `next_seq/last_line_hash`；逐枚声明与接纳事件须同 SID/run。owner/context 不发权限许可。writer 已关或 Shutdown 已开始就永久拒新票；旧入口 guard 放在 Reap、clock、Gate 和宿主回调之前。

`V3Writer::closed()` 只读实际私有关闭状态，沿现有 getter 同步，不改 Close。首次 Register 看见 writer 已关就零 Pending，撤整个登记域；即使换请求也不重开。

## 登记事实、容量与拒绝

仍由同一 ToolJobCoordinator 发真 job/action 并落原 Pending/Registered；provider ID 与原声明锚来自实际匹配。返回 owned 注册事实：实际 writer SID/run、Job/action/attempt、本协调器归属、登记 epoch、cwd/project、最终 input 和已确认事件引用。SID/run 取实际 writer，不能从父材料拼出 SDK operation 五键；JobId/epoch 字符串不是授权凭证。

回执直接保 V3 `Committed / Rejected / IoFailed` 三态、事件 ID/seq/hash 与原错误。实际部分写入或 IO 未确认报 Unconfirmed，不退 inline、不补假成功、不重登同票；纯前门拒绝零登记。注册已落稳但仍待采用，明确不是可执行或已获准派工。故障注入沿现有 V3Writer 真口，不新加公开测试开关。

首次真实写失败可能仍回原 `Rejected`，同时 writer 已 broken；不得光按 enum 洗成普通拒绝。保原回执，再按实际破损与已落事实分 Unconfirmed；前置已破 writer 不虚记新 IO 回执。

暂存记录占实际有限容量；与既有待队列同用明确上限核余量，计入未确认保留件，不能零 queue 便无限收。首次准入需预留稳定 owner/返回事实空间，拒绝或异常不遗留无界幽灵。重复原声明/登记票、跨实例/场/run/cwd/epoch 拒绝，不换掉旧票。首笔无 Adopt API，不能写「重复采用已验」。

所有旧 Start/early、Pump/Get/Wait/Grant/Cancel/Shutdown 路都不能把暂存票当 queued 或业务 execution；不调用老两参 Gate、clock、executor 或插件来处理新票。若提供内部只读查询，只回本登记表 owned 投影。Shutdown 永久关新准入、撤活票 owner，零 worker 可派；已落历史事实不洗成已取消业务，恢复首笔不复活票。注册回执未知继续留 gap，不自动重试或改记普通工具拒绝。

新 Registered 明记 `preparedOnly=true`，仍用既有 schema 允许的 job_handle，不造 mode。新 PlanRecovery 给 `prepared_hold`；Legacy/Hold Adopt 都不把它放进旧 jobs/queue，不补接单链。旧无标记行为不动。只保证本版本 reader；旧宿主未必识别，公开 J3 启用前须钉冻结计划和协议兼容，J2a 不供外部部署。

## 所有权与下一笔

暂存表只持 owned 值与内部归属，不留 std::function、TurnBridge、父取消旗、registry/tool/Session 裸指针。同一 Session writer 保活覆盖方法；外部 capture 退场在锁外。只靠 completion_ready 或 business terminal 不能退资源；J2b 真采用、派工、进程/capture finished/join 与锁外完成泵另交。

新域 Shutdown 先 RequestShutdown 关准入，再等真实 writer serial，让在途 Register/读面退完才清 writer；锁内二次查 closing 的登记不能再写。等线程时不握 serial；清 Gate/executor 等 capture 前先释放 serial。旧默认 Shutdown 顺序不动。

J2b 才接逐票采用与 worker，只收真实业务 scope、自有取消旗及 #310 的 owned CommandExecutionLimits。#310 本轮还有真实编译失败待修，本笔不先吸错误头。J2c 再接 Job 专用 Action：实际 Started/Terminal 归 Coordinator，Post 取业务配对与 raw，锁外串行派；owner/Post 不齐就沿 J1 拒。普通 Abort 保已知失败，真实 Adopt/Settle 未确认先 StopIndeterminate，不双写主轮证据。

## 六场远端验收

实际 Writer 写 assistant 声明、共同 Prepare 给最终值，再走注册，重读本账反校事件/原声明/有效 input；不能镜像传参自己验自己。六场覆盖真注册零派工、真改参材料与来源、容量/重复票、缺件/错场拒绝、真实 receipt 三态和部分写失败、关闭/旧入口隔离。精确核零 worker/quota/executor，暂存票没有业务 Started/Terminal；旧 Job16/startup6/Hold6/J1六场全部保持。

关闭案另验真 writer.Close 后首次注册零 Pending；成功暂存关场后，换 default/Legacy 协调器重读 Plan/Adopt，仍零 append/executor。不能拿 registration-only 实例自拒顶这条恢复验收。

测试与 CI 接线由另一代理独写；本代理写合同、Header/production/owner。新增来源按实际 union 推导 focused/必需 ASan/安装消费者；三平台新 CI、完整 argv、非零断言、JUnit/LastTest 与六 marker 交叉核，不挪父笔通过。当前只定内部实现边界，不报 SDK 后台 Job 已交付。

## e934 关闭路径修订

本头远端 CI `37110146170` 的 macOS combined 原六案四过两败；`clock==0` 共三次读到 1。原日志 job `111166354780`，SHA256 `be56afe104ab0edad5cbed0ef8d121443386fc592a238131ff4f132b6d56abe1`。

实际 Shutdown 仍调用旧 `PumpLocked`，它先跑 `CheckDeadlinesLocked`，即使旧 jobs 为空也调用宿主 clock。capacity 在 Shutdown 后沿用计数；close 在同次 Shutdown 后查两回，三处红与这条生产链相符。构造只收 clock source，没有调用它。

新 registration-only Shutdown 跳过旧业务 Pump；serial 排空、writer 退场、锁外 capture 销毁和关闭回执照原序。旧默认域仍跑原 Pump。六案、六 marker、`clock/executor/Gate/thread==0`、所有原断言与时间帽均保，不把计数改成 1。旧 e934 首红原件另封；修订源须取全新远端 CI，本地仍不跑原生。
