# SDK Memory 片段 CAS SPI

先交合同，再写实现。基线 `3310de72740f25fb5abae823ef38b4d99eb28b76`。
本笔只接项目 recall 大于512 bytes 的片段。公开宿主可换这份 CAS，
不称整个 Blob、Journal、named 工具结果或项目 Memory 已能换后端。

## 公开值与真接点

`memory_blobs.hpp` 只含 STL 与 `api.hpp`，名称置 `memory_blobs::v1`。
`Scope` 持 workspace/session；`Reference` 持 scope/SHA/完整 bytes/media type。
`WriteRequest` 持 owned bytes 与请求耐久档；`WriteReceipt` 分
NotCommitted/Committed/Indeterminate，另列实际确认档与 Error。
`Store` 只给同步 Write/有帽 Read；`Provider::Open(Scope)` 返回本场独占 Store。
`SessionOptions` 尾添可空独占 `memory_blob_provider`。空仍走原 File；
选 provider 不自动开 recall、save 或用户 Memory。

SDK 薄适配器填现 `SessionLaunchRequest.memory_capability_factory`。
SessionManager 持真实场锁后 OpenLockedMemory，实际 MemoryCapabilityLease
管写权，MemoryLedgerBridge/SessionMemory 共用原算法。宿主不填 V3 身份，
不拿 Writer、目录、平台柄或原生故障开关。请求、返回值均 owned。

## 确认与采用

核心算 SHA/bytes，核 provider 回执 scope/reference/state/确认档。
未知 enum、错误 identity、确认不足不能造成功。发布后抛错保未知，不重试。
实际 bridge 要 ProcessCrash 确认，再 Read 核完整 bytes/hash；通过才写新的
Context/Fact。逻辑引用仍 `artifacts/sha256/<桶>/<SHA>`，不入物理路径。
512 bytes 以内仍 inline。blob 已写而账失败可留孤件，不声称回滚。

公开 Operation/报告沿原状态机；context unhealthy 可以归 Indeterminate，
不能统一改成普通 Failed。旧报告和已采用历史保留。
同ID恢复在锁内用本次真实 scope 校读已存片段；坏件/缺件/错读拒，
不向 File 或别的 provider fallback，不重 Store 历史片段。
返回长度帽与完整 SHA 校验不等于宿主 allocator 硬堆帽。

## owner、回调与关场

每场 SDK 独占 provider；scope Store adapter 持其 owner，factory/Store/Read
及对象析构用实际短栈 TLS。Open 仍在 Runtime 锁内，阻塞 Runtime/Session
lifecycle、WaitResult 必须在取锁前拒重入，不能只借 worker 标志。
同场 CAS 回调不可重入自身能力；同步合作返回，不加线程、timeout 或 detach。

原 MemoryCapability mutex 串行借用与 CloseWrites。关场先停止执行并 join，
checked service Close 后 seal 写权，再退 modules/provider。失败开场也退
实际能力与捕获。scope 的最后借用退后才销 Store；Store 析构后才销 provider。
Store 回执已经交完该笔耐久；析构只退资源，不补确认，不新增 Close ACK。
持只读值报告不续写权。旧 SDK query/Close 规则不变。

## 边界与验收

生产仅新公开头/SDK adapter、core.hpp 尾项与 core.cpp 装配/TLS门。
CAS/lease/bridge/Reader 算法不复制。root 接 CMake/CI/catalog/README。
Recall/Save/Skills/Lua 计划、operations、sdk-results、named raw/formal/index、
recorder offload 和项目 topic/catalog 仍 File，禁止全场替换注册或后台承诺。

新 public native 固定六案目标：实际采用；同ID恢复；回执/错读/未知；
四场隔离；held借用关场；factory/失败开场退场与重入。
六 `[sdk-memory-blob-path]` 路标 actual/resume/receipt/isolation/drain/opening。
安装 consumer 只用已安装公开头/STL，注册宿主 Store 后真 Submit→报告→
Close→同ID恢复，核原引用、模型正文和不重写。默认旧 CAS/Recall/Save
与 CLI/parity 原册保全，远端三平台、实际完整argv、非零册与 ASan收件。
本地只静读/纯数据/AST/docs，禁止 configure/build/CTest/原生；不推送。
公开后台 Close/父取消/许可域仍按旧待答，不在这笔替用户定。
