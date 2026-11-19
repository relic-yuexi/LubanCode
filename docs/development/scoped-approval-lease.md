# 可取消审批与逐票 lease

[开发手册](README.md) · [前台调用上下文](child-foreground-context.md) · [SDK 子 Agent 合同](sdk-subagents.md)

这页定 E2 首笔合同。候选代码已写，原生验收尚待远端。基线为 E1 `24622d5f`。
首笔只补中立等待能力、逐票收票和 SDK 宿主参考实现。
子轮 hook、真实声明身份、规则 floor 与执行前取消检查留到第二笔。
公开 SubAgent SDK、后台异步审批、跨进程票据不在本笔。

## 内部接口

中立类型放 `src/runtime/scoped_approval.hpp`，不依赖 CLI、SDK 或 tools。
既有 `InteractionFuture` 保留；新能力单独声明，不把普通 future 强转后兜底。
以下形状供实现与二审，不导出到公开 SDK 头。

```cpp
class ScopedApprovalFuture : public InteractionFuture {
public:
    using InteractionFuture::WaitApproval;
    virtual std::optional<ApprovalResponse>
    WaitApproval(const std::atomic<bool>* cancel) = 0;
};
class ApprovalLease { // move-only；析构逐票 Retire
public:
    static ApprovalLease Create(std::shared_ptr<ScopedApprovalFuture>,
                                std::function<void()> retire);
    explicit operator bool() const noexcept;
    std::shared_ptr<ScopedApprovalFuture> Future() const noexcept;
    void Retire() noexcept;
};
```

`Create` 遇 null future 或空撤票柄，返回无效 lease。
`Retire` 幂等；撤票柄不得抛异常，不得清整场，也不得在内部锁下销毁用户 capture。
搬移只交所有权；被搬空或已退休 lease 不再撤票。`Future` 可延活等待状态，不能延活 Session。
带旗 Wait 只在调用栈上借旗；返回前结束借用。pending、lease、future 都不保存这根借针。
新路径无有效 lease、缺 owner 或不支持局部撤票时，拒询，不退回无界等待。

## 宿主与归属

从 `src/sdk/core.cpp` 抽出一份私有 `SessionApprovals`，放 `src/sdk/approval.hpp/.cpp`。
原普通审批与新 scoped 票共用这份 pending 表、request ID 路由和 allowed 集合。
`Session::Impl` 委托登记、回答、逐票退休、整场取消及查询；不复制第二套 SDK pending 栈。
内部口收为 `Register`、`RegisterScoped`、`Resolve`、`Retire(id)`、`CancelAll` 和 `Pending`。
`RegisterScoped(owner, approval, timeout)` 返回有效 lease 或明确错误；登记后才锁外发布事件。
发布抛错，由栈上 lease 立即撤本票；发布期间 Close 也不能留下可复活票。

scoped owner 全部拷值：父 Session/已受理 operation、真实 child session/run/turn、
已声明 action、effective cwd。父 operation 只标因果与票据宿主，不属于子 Session。
本笔不造 child operation，不填写 attempt，不拼子 `ToolInvocationIdentity`。
owner 只准可信宿主构造；SDK 宿主须核父场与当前已受理 operation，缺字段拒绝。
第二笔再从真实子桥核声明及 cwd；本笔不宣称结构检查证明了子账归属。

新 scoped 票只收 `Accept`、`Decline`、`Cancel`。
`AcceptForSession` 明确拒绝，返回失败并保持原票可答，不写父 allowed。
用户已选后续 grant 只限当前子会话，不传父场或兄弟。孩子 SID 可能同串，不能只按 SID 存 grant；
后续须先核宿主 owner、真实 child SID/run 与 effective cwd/floor，再写子场权限账。
普通 SDK 审批仍按原规矩处理四态与会话允许；CLI 同步/routed/floored 和后台路径照旧。

## 期限、取消与退场

scoped deadline 在登记时用 `steady_clock` 钉住；迟开始 Wait 不能续期。
回答、过期、逐票撤销、Close 共用宿主锁裁胜负；终态一旦确定，迟答即 stale。
带旗 Wait 在原 cv 等待中查旗与 deadline，计划检查间隔不超过 20 ms；不承诺调度硬时限。
取消或超时只退休本票。父 Close 与整轮结束仍可按原规矩收整场。
不另起等答线程，不 detach，不让 publisher 或撤票柄持强 Session 回环。

锁序固定为宿主 pending mutex → future mutex。Wait 放开 future 锁，再进宿主撤票口。
publisher、撤票回调和最后一份用户 capture 都在锁外退场。
普通审批保留原超时行为；只有新 scoped 票使用登记期限。
第一笔不改 AgentLoop、不挂子 hook；第二笔须在真正 `ToolExecutionStarted` 前再次认取消旗。

## 远端验收

用真实宿主 pending 实现验四态兼容、scoped 会话允许拒绝、登记期限、迟答与 Close。
两票同挂，只撤一票，另一票仍可答；publisher 抛错、lease 搬移/析构都不漏票。
取消与回答同锁交错，只有一方赢；重复退休、future 晚毁、capture 析构重入不得死锁。
缺 owner/null future/缺撤票柄不开票；pending 不留借旗，也不握 Session 强引用。
既有 SDK 普通审批、取消、关闭、恢复迟答及 CLI 同步审批验收照跑。
新增来源精确登记，非零、无 skip；三平台和必需 ASan 只认本头鲜证。
本地只跑静态、纯数据与文档检查，不 configure、编译、CTest 或执行原生夹具。

现有依据：`src/runtime/interaction.hpp`、`src/sdk/core.cpp` 的 `ApprovalFuture`/pending，
以及 `src/agent/loop.cpp` 审批先于执行的次序。首笔通过也不代表子轮异步审批已经接通。
