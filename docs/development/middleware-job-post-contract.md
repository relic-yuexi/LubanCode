# 中间件：共享 Action 值校验与私有 Job Post 档

基 #321 `35f58c23`。本笔只共享原 Action 值规则，再给真实 Dispatcher 加默认关闭返回档；没有 SDK Jobs、Accepted、Job Operation、持久 PostSettlement 或新 owner factory。合同先单交，实施交两轮静审。本地不 configure、编译、CTest 或运行原生程序。

## 公开内部什么

新增 `hooks/middleware_action_contract.hpp` 纯值头，只依赖 std、JSON、中立 middleware 与 text encoding。迁原 SDK Action 文本、参数、输入、admission、supplement 和错误边界，共用一份规则。`sdk/extensions.cpp` 的 JSON parse、检查顺序、稳定码、人话、Next lease、实际 mainScope 与 callback 归属不改；PreUser、Estimate、Capacity 等旧路不迁。

`MiddlewareDispatcher::Dispatch` 尾添内部 `DispatchReturnContract`，默认 Legacy；显式 JobPostSupplementsV1 只收 PostAction。错挂点在任何 callback/sink 之前明拒，不降回 Legacy。档由可信调用方按值传入，插件 JSON 不能打开它，更不能凭档取得执行或写账许可。

Job Post 只收 null output、非 Denied、ResultSupplement 且 payload 恰为 `{text:string}`。单枚 text 至多 16KiB，无 NUL，须真 UTF-8；一 callback 至多 16 枚、32KiB。本次 dispatch 也最多 16 枚、32KiB。observer 沿 SDK Action 原规则：不得输出、deny 或给 effects。错误 code/message 沿原 Action 256/4096 字节及 UTF-8/NUL 口径。

## 谁持值，怎样入门

校验只借本次实际 HandlerReturn；不存大输出，不先 dump 坏值。Next candidate 在 impl 自有 input 拷贝、Proposal、Continuation 之前回真实 Invalid/原 bad_candidate 码，不消费、不写该提案。旧 Native Next 抛错后仍能再试；#321 每次尝试短栈与最近一次实际抛错记录原样保留，不能假称只执行一次。SDK Next 原先入场消费规矩照旧。

链与 observer 正常返回后，先校整份，再在本次 owned count/bytes 的短锁内一次预留全部 effects，随后才准 Effects、Proposal、Completed。坏 callback 不得先放一部分再拒其余；已先完成的其它 callback 仍沿原 reduce/policy 收口。余额不跨 dispatch/Session；锁不跨 callback、Writer 或 observer join。

这两份帽只管准入 supplement 正文与枚数，不管可信 handler 自己分配或运行时长，不是整个堆 32KiB，也不是 DeferredEffects 元数据总帽。现 DeferredEffects 不改；借来的 sink/Writer 仍须活到实际 Dispatch、observer join 与 Settle 全退。Settle(true) 仍只是内存采用，不充 durable adoption。

## 结局和旧路

在原 DispatchFailureSource 尾添 ReturnContractRejected，仅真实 producer 校准拒绝时填。required、Abort、KeepOriginal 与 cause 继续走原分支；合法 Next 已返后拒绝仍只用实际 downstream receipt，不重跑 handler/Next。candidate Invalid 不凭文案自动造 aggregate Failed。普通 handler 错、取消与异常仍各归原 producer。

原 native receipt 未确认/Writer broken/实际 Settle 抛错保 c1a gap，不能用已知校准拒绝洗成成功、Committed 或回滚。lease 只拷实际最终 cause/source；live record 与 native receipt 分层，不造新 timeline。Legacy 默认、SDK main Action 接受/拒规则、旧 CLI callback/throw/catch/kind/code/reduce/schema/事件次序全部留住。

## 原生验收和范围

新源 `tests/unit/hooks/test_middleware_job_post_contract.cpp` 固定六 CASE，marker `[middleware-job-post-contract-path]`：`valid / payload / budget / observer / native-gap / compatibility`。用真 PublishedRegistry、callbacks、Dispatcher、Captured sink、DeferredEffects 与 V3 Ledger 走合法边界、巨大坏 candidate/return、整次预留、observer 真 join、首次真实 Writer 未确认与 Legacy/SDK 对照；不镜像测试 validator，不 mock outcome 或回执。

生产仅新值头、`sdk/extensions.cpp`、`hooks/middleware.hpp/.cpp`；sink、DeferredEffects、public SDK、Agent/Job owner 与配置路径不改。原 Actions 与 #321 六案/标记继续保门。目录、SDK-only exact header/source、CMake/focused/ASan 完整 argv、三平台原件与纯反例由根代理接并集。未跑新头原生，不借父绿色。
