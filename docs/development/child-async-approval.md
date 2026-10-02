# 前台子轮异步审批接线

[开发手册](README.md) · [逐票 lease](scoped-approval-lease.md) · [前台调用上下文](child-foreground-context.md) · [SDK 子 Agent 合同](sdk-subagents.md)

状态：E2 第二笔实现前合同，基于首笔 `66609733`。首笔 CI 尚在跑。
本笔接真实前台子轮、逐票等待及当前子会话允许；仍不开放公共 SubAgent SDK。
后台异步询问、持久 grant、跨进程票据和 Managed 治理另做。

## 接口与事实来源

接口留在内部；以下名字供二审，实现时可收窄，不冻结公开 API。

```cpp
// agent::TurnWiring：显式新能力，返回栈上所有者。
std::function<runtime::ApprovalLease(const runtime::ApprovalRequest&)>
    on_tool_confirm_scoped;
// tools::AgentSubagentHooks：只有前台调用栈接这根口。
std::function<runtime::ApprovalLease(const ChildApprovalRequest&)>
    on_child_tool_confirm_scoped;
```

`ChildApprovalRequest` 拷值保存请求、父因果材料、真实子声明、effective cwd 与 floor。
它不带取消指针、future、工具或 writer 借用。SDK 私有适配器继续用 `SessionApprovals` 原表。
宿主根 Session/已受理 operation 来自实际 Pump；父 operation 只管票据宿主与因果关联。
宿主 run、直接父 run、子 SID/run 分开取值，不能拿父 operation 拼子执行五键。
子 SID/run 取本桥 writer；turn/action/message 取 `V3DeclaredCallOrigin`，并核当前 turn。
审批在真实 `ToolExecutionStarted` 前发生，故不造 attempt，不调执行身份口冒充已起跑。

直接父归属须由开子账时已落稳的父引用提供 owned 快照，不能仅信模型 call ID、task ID 或 TLS。
当前 `SpawnSubagentV3` 有 provider 号回退与根账声明查询；严格新口不接受这两种降级。
本笔先接 main 真实 foreground。嵌套直接父未证，新口拒询，不能借根场字段凑齐。旧 CLI 路照常。
正向嵌套装配另笔接；本笔不改 nested 开账，只验缺证与错证拒绝。
桥仅补必要只读快照；取值时短持现有局部账锁，登记、发布和 Wait 都在账锁外。

## 规则、路由与子会话允许

新 scoped 口优先于旧 async/sync；缺 owner、真桥、有效 lease 或局部取消能力，明确拒绝。
一旦选中新口，失败不回落到普通 future 或同步确认。没接新口，原 CLI 同步、routed、floored 路不变。
规则预裁定、角色工具面、deny_commands 与 ModePolicy 仍先行；父 Yolo 不能绕过子 floor。
新能力另需内部 child-scoped 纯裁定口，入参带 owned scope、pre/class/name/input；有效 floor 取 scope。
宿主调同一 `EvaluatePermission`，只合配置规则、ModePolicy 与 floor，不递父 `AllowedTools` 或临时 grant。
旧三态不能分辨父 Allow 来源，新路径不借旧 eval/floored；缺子裁定口便拒。旧 CLI 无新能力仍照原接法。
新请求携 task 页归属，宿主锁外发布并按 owned request ID 回答；不把 blocking Wait 塞进旧 presenter。

用户已定 `AcceptForSession` 只限当前子会话。新票回答时在原 pending 锁内裁胜负，再写独立子 grant。
grant 键至少含真实宿主 owner、直接父场 SID/run、子 SID/run、effective cwd/floor。
SID 与 run 都可跨父场同串，不限同秒；票 turn/action 不入 grant 键，才能管本孩子后续轮。
不能只按子 SID/run，也不能写父 `allowed` 或后台权限快照。
grant 只免本子会话后续同工具询问；每次仍跑原规则与有效 floor，不越过硬拒绝或工具准入。
grant 不继承给兄弟、后代或恢复 run。单票答完先退 lease，grant 留到本孩子最终退场。
子调用退出、Close/本次取消才收本 scope；迟答不能复活。
参考表也给全程配置 Allow、从未出票的孩子留 closed 标记；闭场后新登记同 scope 一样拒。
先核真实 scope 与宿主归属。标记分配失败只封本 host 后续 child admission，普通审批不动；撤场或换新受理 owner 才复位。
普通 SDK `AcceptForSession`、普通超时与允许账继续照旧。

## 借用、取消与退场

`RunSubagentTask` 在 CancelChain 就绪后装局部 scoped 回调；它只在当前 foreground 调用中借桥与宿主。
shared Hooks、后台 lambda、冻结 env 和 pending 都不保存借旗或这份局部回调。
循环栈上持 `ApprovalLease`，调用显式 `ScopedApprovalFuture::WaitApproval(frame.cancel)`。
取消、过期与用户回答走首笔同一张表；不清整场，不另开等答线程，不 detach。
Wait 返回后再查本次旗；真正 MarkExecutionStarted 前再查一次。取消时不发 started、不执行工具。
这只约定检查点，不承诺阻塞 mutex、宿主 publisher 或外部 I/O 能即时打断。

每票成功或拒绝先撤本票；退子调用时先清 scoped TurnWiring，再撤子 grant，最后退取消链/子桥。
回调只短借宿主；未来状态和票据不强持 Session。publisher、撤票与 capture 退场仍在所有锁外。
本笔复用共用执行 owner；终态 Finish/registry 例外修复沿独立小笔，不在这里复制收尾栈。
孩子工具执行 invocation 仍缺席；缺真实 owned operation 的 `memory_save` 继续拒绝。

## 远端验收

用真 AgentTool、真子 V3 桥和宿主原 pending 实现验询问、回答、声明与 started 次序，不只测假 future。
同串模型 call ID、同串子 SID/run 两父并挂：只答一票，另一票仍挂；核真实父归属与 cwd 不串。
当前孩子 session grant 免第二次询问；父场、兄弟、后代仍问，floor/硬拒绝仍拦，退出后旧 grant 失效。
取消等答、Accept 与取消交错、过期、Close、publisher 抛错、缺能力/坏归属均无工具副作用、无残票。
再验 nested 缺证/错证拒绝、同实例两场取消、旧 CLI sync/routed/floored、后台不问及普通 SDK 四态。
新增来源精确登记并核非零、无 skip；三平台鲜 CI 与必需 ASan 均认本头，首笔绿不能代验本笔。
本地只做静态、纯数据和文档检查；不 configure、编译、CTest 或运行原生夹具。

落点：`src/agent/loop.hpp/.cpp`、`src/tools/agent_tool.hpp/.cpp`、
`src/runtime/trajectory_subagent_bridge.hpp/.cpp` 与 `src/sdk/approval.hpp/.cpp`。
实际 SDK 子 Agent 工具装配、安装消费者入口与严格 Close 仍另有前置门。
