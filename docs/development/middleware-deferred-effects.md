# 中间件：共用延后效果缓冲

基 #317 `1f9296e0`。这笔只把 `src/sdk/action_dispatch.cpp` 匿名 `DeferredEffects` 原封搬进内部 header-only `src/runtime/middleware_deferred_effects.hpp`。类型放 `lubancode::runtime::middleware_detail`，SDK 原地用同名别名。合同先提交，代码另审；不增公开 SDK 接口、Job 能力、执行栈、重试、预算或回执。

## 所有权与退场

`Pending` 持实际 invocation meta、类型、准许值、原因与 JSON 副本。队列与 mutex 都归缓冲对象；下游 sink 仍借裸指针，不延长 Session、writer 或 Dispatcher 寿命。非效果事件即时转发；`OnOutputProposed` 先转发，再按原条件缓存 input.rewrite。`OnEffectSettled` 锁住队列入项，顺序沿真实回调到达次序。原未覆写的接口仍走基类缺省，不能顺手添转发。

Pre 成功返回 settle lambda，以 shared_ptr 持住缓冲对象；不捕获短命 outcome 或 DispatchScope。Pre 拒绝、失败或坏参数沿原路立即 Settle。Post 用栈对象，实际 Dispatch 返回且 observer 全部 join 后才 Settle，见 `action_dispatch.cpp:89/126`、`hooks/middleware.cpp:1416`。观察者启动或回调抛错时，原 jthread 退场纪律照留，不把异常收成成功。

`Settle` 沿原宿主串行调用，队列遍历不另加 mutex；它不能与 Dispatch/observer 入队并跑。sink 至少活到实际 Dispatch、observer join 与 Settle 全部退出。Pre lambda 销毁只退缓冲值，不自动补 Settle；主 Prepare 的 `ActionReceipt` 仍在未准入退场时调用原失败结算，见 `agent/loop.cpp:574`。本笔不把 shared_ptr 说成下游借用保护。

## 保住哪些行为

原 struct 全部字段、构造器、方法体、异常、锁范围与效果顺序逐字保留。Denied 只采用原 admission.decision，input.rewrite 仍校最终参数，result.supplement 来源仍从实际 outcome record 补入。null sink 的早退、转发抛错、遍历中途抛错、清队列时点均不变；两处 SDK caller 除 include/type 引用外逐字保留。

SDK Post 当前先拷原工具结果、拼合补料，再调用 in-memory Settle(true)。这不等于补料已经持久化、选择或接纳，也不等于 typed PostSettlement。主业务 terminal/raw、真实五键、owner 与权限合同均不迁动。新 header 只直接依赖 std、JSON 和 neutral middleware 合同，不引 SDK、Agent、CLI 或 writer。

## 验收

文本门从固定基头抽原 struct，逐字比新 header 内 class；再从两版 SDK 文件剥去原 class、新 include 与别名，逐字比剩余 caller。存 class/blob SHA 和基头，不能拿结构相似代替保行为证据。复用原 SDK Action 十案、四条内部 marker、安装 Action 与旧中间件/CLI 回归，不添镜像 CASE。

根代理只给 SDK-only 闭包登记这个精确内部头，保邻近 header、逆 host include、公开头暴露与 testing-OFF 拒绝；三平台/ASan 仍验实际原来源。源码与门均待二审和 fresh CI。本地只查文本、纯 Python、AST 与文档，不配置、编译、CTest 或运行原生程序。
