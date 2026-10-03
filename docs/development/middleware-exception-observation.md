# 中间件：异常出口与 Next 所见

基 #319 `7b6d4d16`，只修 #317 真实 MSVC 红点。旧代码用连续 `current_exception()` 的 `exception_ptr ==` 追认同一异常；[Microsoft 主文档](https://learn.microsoft.com/en-us/cpp/cpp/transporting-exceptions-between-threads?view=msvc-170) 明言每次可取得不同副本，地址比较不等。原 Windows 六案过四案，Terminal/Continuation 四处误落 HandlerThrew；旧原件另封，不复跑旧头遮红。

## 两份事实，各归其位

`failure_source` 只记当前实际失败入口。handler 返回错误仍为 HandlerReturnedError；handler 两处 opaque catch 一律 HandlerThrew，不追认祖源。缺 handler、observer 结算抛错等原 producer 保留；enum 值不重排，cause 仍由真实分支与原 policy 决定。

在 `InvocationRecord` 尾添 `next_exception_source`，只取 None、TerminalThrew、ContinuationThrew。它记本次同步 Next 亲见的抛错：terminal 实际 catch 填 Terminal；Next 实际 catch 仅在尚未见 Terminal 时填 Continuation。局部槽只持 enum，不存 exception_ptr，不查捕获地址、type、what 或错误文案，不设 MSVC 特例。

原生 `NextCall::Call` 仅在 impl 正常返回非 Invalid 后标 consumed；抛错后 handler 仍可再试，本笔照留。每次真实 lambda 入口另设短栈：terminal catch 与这次 Next catch 之间没有外层 handler 换抛。record 保最后一次实际 Next 抛错所见；后来正常返回不抹先前抛过事实，另次抛错则按那次真实来源替换。handler 吞错成功、换抛或自报错，其 failure_source 另按真实出口填。字段不声称最终异常同身份、整体 Abort 祖因或只跑过一次，不越尝试、帧、observer、dispatch 或会话共用。

原 callback、throw/catch、异常类型、kind、code/detail、reduce、短路、policy、事件 payload/schema/次序及缺省 CLI/SDK 行为全留。不包装异常，不新增中间件或 Job 能力。旧 terminal 空链抛错仍逸出，不造 outcome。新事实只在 owned live 值；当前 receipt lease 没有 record 列表，不声称它自动持这份新增事实，也不拿 in-memory 值充持久采用。

## 验收与归属

只改 `hooks/middleware.hpp/.cpp` 与原 `test_middleware_dispatch_cause.cpp`；sink 若确需记录另交根代理定范围。合同先单交，实施另审。原六 CASE、六 marker、预算及旧 cause/kind/error 硬断言不减；四处依赖异常身份的 source 期待改验真实 handler catch 加真实 Next 所见，两组事实同时守住。

在原 exceptions 案补非 std 异常、吞错成功、同 type/what 换抛、离开 catch 后新抛、跨帧不串，以及抛错后真实 Next 再试时 Terminal/Continuation 不串。owned outcome 副本与实际 receipt snapshot 各按所持事实验，真实 native schema 不添字段。复用原 argv/三平台/full/ASan 来源，不新造镜像册。

目录及 CI 门由根代理登记；实施候选交两轮静审后才发布。尚未跑新头原生，不能借不同头绿色。本地不 configure、编译、CTest 或运行原生程序。
