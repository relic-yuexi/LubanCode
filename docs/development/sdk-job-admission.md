# SDK Job：共用 Prepare 与后台准入前置

本笔合同在先，代码随后。基线为 Hold 私有头 `023e635c`，它已普通合入 #308 的 `373df8d8`；来源还须各取 fresh remote CI。本地不 configure、编译、CTest 或跑原生；共享 feature/main 不动。

## 范围

先收最小中立切片：复用现有 Prepare，产出 owned 最终参数与注册元数据；新增显式要求 owned admission 的 batch 前置拒绝路线。完整 Job scope、后台 Post、宿主串行完成泵尚未具备，首笔只交明确拒绝出口与真实 loop 验证，不开放 SDK Jobs、不让 SDK 默认装半套能力。旧 CLI seam、inline 与全部旧验收保持原路。

这笔不决定尚待确认的 SDK Close 产品规则，不做公开配置/查询、Detached、Runner、SPI 或进程捕获预算。Session-owned Jobs 完整交付仍有后续门。

## 实际边界

`src/agent/loop.cpp` 中 ToolCallFrame 借 call/wiring/filter/trace/proxy 与父轮取消旗。Prepare 同时跑门、审批和拒绝收尾；MarkExecutionStarted、ExecuteApprovedTool、CompleteToolCall 仍留原主轮生命周期。新 owned 输出只复制最终 input、实际工具名/调用号、注册来源与效果类，不含 callback、工具/账/Session 裸指针或父取消旗；它不是授权凭证，也不证明 Job 已注册。

缺完整 Job scope 的 batch 新路在消费 Prepare 回执之前便拒绝。共同 Prepare 值入口也须在任何 deferred/Action/Post 回调之前识别能力并拒绝，不能先采用 Pre 效果再报缺位。共同准入不另写 reducer。原工具解析、曝光/turn/模式/作用域门、proxy schema、PreToolUse 改参复验、规则/floor、PermissionRequest 与审批继续调用同一份 Prepare。新 owned 输出遇需要后续异步 adopted/settle 或 Post 的 Action 能力，先拒绝；不能把主场 Action adapter 搬进后台、不能提早记 applied。原 inline Action 不受影响。

目前 batch gate 在 Prepare 前接单；缺来源、队满或落账失败会返回 nullopt。旧注释说 fallback inline，实际第二遍仍按原 adjudication 跳过 non-inline，不能拿注释当真执行证据。新增显式路线用 typed 回执表达 `Rejected / Accepted / Unconfirmed`，这笔只实现缺能力时前置拒绝；尚无完整 owned Job scope/Post 接点时，前置产出 Rejected，留下明确配对结果，零旧 TakeJobOrder、零业务 Started、零 worker、零 inline 真执行。旧默认实际路径不变，旧注释欠账另记。首笔不提供 Accepted 实装，也不伪造 Unconfirmed；共同 Prepare 返回或真实 receipt 检查已经给出 StopIndeterminate 时，原样保未知控制态，不洗成普通拒绝。

真正接入 Accepted 须另笔核 scope 与 ledger：ToolJobCoordinator 自己写接单 attempt 1、业务派发 attempt 和终态。不能再调主轮 MarkStarted/FinishTrace/Complete 去写第二份业务链；父原声明与 operation 只留因果材料，不能搭进 Job 业务五键。拒绝或未知状态须沿真实 receipt 返回，不能凭工具错误字符串猜持久阶段。

## 所有权与退场

共同 Prepare 的引用只活到本次同步调用返回。返回值不留 std::function、不延寿 TurnWiring，不查已结束父轮 active identity。后续 worker 只能消费 owned 参数、真实 Job scope 与本身取消旗，工具所借 registry 须由会话 owner 保活至真 thread/capture 退场。

SessionApprovals、宿主插件/publisher 与模型调用不能塞进 coordinator jobs_mutex 下的两参 gate。后笔应将真实派发/完成 receipt 冻结为 owned 通知，再由单一宿主通道锁外 drain；receipt 未确认先停后续派工/模型。普通 hook Abort 留已知失败，不能升 Unknown。

原 ScopedTurnBindings 先退 async bridge，再退 wiring；本笔不变。原 SessionService::Close 先 ShutdownExecution，后 CloseRuntime；Async Shutdown 真 join coordinator，再撤 writer/bridge、锁外销 callbacks/captures。完整 Job 装配须沿这条寿命链，不能拿附件析构当异步退场证明。

## 下一笔与前置

1. 本笔：owned Prepare 输出与后台准入前置拒绝，原 inline/CLI 兼容。
2. 后笔：实际 Job scope、逐票 owner、采用落稳后派发、锁外串行完成泵与 Job-specific Action adapter；方能接受新交单。
3. 最后接公开 Session 值配置/查询、冻结计划与锁下恢复。默认关闭、首批只选 SDK 自建 run_command，Resume 用显式 Hold，不重派旧 Job。

Hold 与 thread startup 合同不能替新交单验收。#309 首轮真实恢复失败暴露 Fold 材料缺口：后续 Pending 没带 provider 调用号，不能将首次 Admit 已有调用号抹空。先修共用 Fold、保严格 source 守门，再把修头普通并入本笔；旧失败单独留件，不借新组合冒称 Hold 已验收。

进程限额另由 [#310](https://github.com/relic-yuexi/LubanCode/pull/310) 交付，已发布内部 owned 当次画像与六场测试源码，远端验收另收。它把真实进程 timeout/output cap 接进 ToolExecutionContext，默认空画像沿旧 CLI；仍不是公开 Jobs。本笔基线尚未含这笔时，RunCommand 仍用固定捕获帽，coordinator max_output_bytes 只截事后结果。后续装配须纳实际限额原语，不能凭事后正文截断声称守住捕获帽。

## 验收

新原生册必须走真实 Prepare 与 AgentLoop，核最终参数由共同改参/权限门产出、Deny/Ask/取消与注册元数据不串；新严格路线在缺完整 owner/Post 时明确拒绝，旧 gate 与工具执行计数均为零，下一模型只见拒绝。原 RunOneTool/CLI 协议与旧 Job16、startup6、Hold6断言全留；不拿镜像 reducer 或值类型测试报完整 SDK Jobs。

新增 source 依实际名册接三平台 focused 与必需 ASan，完整 argv/非零册数/JUnit/LastTest交叉校；本笔实际六案、六路径，focused 由35增至36，必需 ASan 由40增至41，安装消费者仍25。公开安装消费者数不因内部原语增加。本地只跑静态、纯数据和 docs。
