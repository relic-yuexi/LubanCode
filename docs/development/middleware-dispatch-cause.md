# 中间件：真实派发原因

这笔接在 #315 `c5c012c2` 后，只补 c1b 所需内部 producer 事实。合同先提交，代码另审。本地不配置、编译、CTest 或运行原生程序。完整 PostSettlement、SDK Job Operation、补料持久采用、权限与后台装配均留后笔。

## 公开内部什么

在 `hooks/middleware.hpp` 尾添 `DispatchCause` 与 `DispatchFailureSource` 值字段；不改公开 SDK ABI。原因只由真实 `RunFrame -> FrameResult -> DispatchOutcome` 分支填：required 失败终止、配置 Abort、KeepOriginal 无可用下游、帧入口真实取消、显式 Denied、链深超限。成功用 None。失败来源另分缺 handler、返回错误、handler 抛错、真实 terminal 抛错、真实 continuation 抛错及 observer 返回后结算抛错。

`InvocationRecord` 保自身失败来源；整体 cause 跟随旧合并路。optional KeepOriginal 与 observer 失败不自动变成整体失败。框架私有 `NextCall::last()` 保存实际下游来源，外层透传或 KeepOriginal 沿这份值传递；插件返回值不能另填 producer cause。显式 Denied 与下游失败同时出现时，仍照旧 reduce 分支取结果，不顺手改优先次序。

真实取消只认 `middleware.cpp` 帧入口检查。`HandlerError.code`、文案、Dispatch 收尾取消旗均不作来源证据。handler 伪造 `hook.dispatch.cancelled`，仍按 handler 失败与原 policy 收口。真正 terminal/next 抛错仅记当次 exception 身份、原样重抛；旧 handler catch 只有接住同一异常才认来源。handler 自报错、另抛错不追认宿主异常。空链 terminal 异常仍直接逸出，不造 DispatchOutcome；未返回的 sink/setup/observer 线程异常也不补成功或失败报告。

旧 kind、error code/detail、拒绝字段、reduce、短路、异常、事件 payload/schema/序号与 CLI/SDK 表现均保留。新字段放原 aggregate 字段后，旧位置初始化照旧。失败来源不宣称命令副作用、持久状态或权限。

## 谁拥有，怎样退场

所有新来源字段只持 enum 值，不持 writer、callback、取消旗或 Session。为辨异常而暂存的 exception_ptr 仅活在同步 Dispatch 栈，observer 不共用链异常槽；不出报告、不留到下一场。

c1a lease `Finish(actual outcome)` 可把整体 cause/source 拷进 owned snapshot。它只收本场真实返回结果，沿原 dispatch/revision 与 native receipt 校验；原 gap 优先保留。snapshot 退 sink/writer 后仍可查。cause 属实际 live producer 事实，不进旧事件，不充 durable PostSettlement，也不拿快照自证 SDK Job 五键。缺省未 capture 路不添写盘。

## 原生验收

新源 `tests/unit/hooks/test_middleware_dispatch_cause.cpp` 固定六 CASE，无 skip。marker 前缀 `[middleware-dispatch-cause-path]`，依次为 `policies / cancellation / denied / propagation / exceptions / snapshot`。

用真实 FrozenRegistry、callbacks、取消旗与 Dispatcher 核 required/optional/configured Abort、KeepOriginal 无下游、真实取消与伪造同码、前后 Denied、下游透传、terminal/continuation/observer 失败。断言同时核旧 kind/code、下游与 terminal 次数、记录与事件顺序。最后一案用真实 V3 Writer、业务 terminal/raw、capture lease 与 actual outcome；native receipts 对本卷 id/seq/hash，sink/writer 退后读 owned 来源，旧默认与 capture 路的事件形状对照。不 mock DispatchOutcome 或 runtime 事件。

三平台远端核完整源 argv、六 marker、六实际 CASE 与正断言、JUnit/LastTest、ASan 精确来源。CI/CMake 与目录登记由根代理收并集。尚未跑本头原生，只报静态候选。
