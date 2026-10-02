# Memory CAS 能力合同

[开发手册](README.md) · [Memory 召回](sdk-memory-recall.md) · [Memory 保存](sdk-memory-save.md)

先交合同，再写实现。基线固定 `41280cb13ea1610da04400a67316da17ce38bab8`。
沿存储 SPI 合同候选 #273，本笔只迁 Memory 召回片段 CAS。
尚无原生验收，不开放公开存储注册，不称整场已能换存储。

## 真接线与归属

SDK 和 CLI 都走现有 `runtime::MemoryLedgerBridge`。
三处片段 Store、strict 写后校读、SDK 报告读取和同 ID 恢复校读一起迁。
片段大于 512 bytes 时，先确认 CAS，再写引用与实际 Context/Fact。
小片段仍按现规则 inline；完整 V3 采用与 operation/turn/report 归属仍由领域核。

`MemoryCapabilityFactory` 为内部 owned 工厂，每场拿独立能力句柄。
值只含 workspace/session、SHA、完整 bytes、媒体信息、请求帽和耐久档。
Provider 不交本机路径，不自报“已采用”；默认 File 的 root 留在适配器内。
逻辑 ref 保现 `artifacts/sha256/<桶>/<SHA>`，不让 Provider 物理路径进账或模型。
读口同时校声明、请求帽、实际完整 bytes/hash；不信 verified 布尔。

工厂由 SessionManager 持有。拿到真实独占场锁后，先开本场能力，
再交 `V3OpeningContext`；opening participant 只拿这份能力，不调用外部工厂。
`SessionMemory::Open` 先绑定能力，再核 saved reports；恢复读也落在真实持锁窗内。
不在 `SessionService` 准备完成后另挂回调，不加全局 setter 或共享可变 root。

能力沿本场执行 owner 活，Close 先停实际借用、关 Writer，再拒新写。
失败开场也收能力；scope 已关，旧 borrowed handle 不得写入。
持值报告与必要只读查询保原 API 规则，不让能力析构更改执行结论。
本笔只沿本地场独占锁，不宣称跨机 fence 或整树沙箱已经交付。

## 提交与耐久回执

写回执分 `not_committed / committed / indeterminate`，另列实际确认耐久档。
成功对象也要核 scope、SHA、bytes 与请求身份，不能只看 `has_value()`。
请求 Buffered、ProcessCrash、PowerLoss，做不到须明拒或报耐久未确认，不能暗降。
`trajectory::Durability` 与平台 `WriteDurability` 定义不同，不按同名字面硬映射。

默认 File 用同目录私有临时件，检查真实 write/flush/close，再不可覆盖地发布。
已有 SHA 实体须有界核原内容；坏件拒绝，字节不改，不覆盖成“命中”。
同 SHA 竞发也先核已有实体，不凭 `exists()` 复用。
换名前失败给 not_committed；换名后目录确认失败保已可见字节，
回 committed 加耐久未确认或 indeterminate，不把它说成从未落盘。
已确认文件、已有桶目录与新建目录的耐久保证分别报告；不冒称多文件断电事务。
读取先开 regular file 再有界读，短件、超帽、坏 SHA、FIFO/reparse 都拒绝。

CAS 请求未确认，strict 召回不写新的 Context/Fact/ModelRequest 引用。
Blob 已落、后续账失败可留孤件；不声称回滚，不盲目重放未知提交。
旧 File wrapper 若继续 best-effort 返回 BlobRef，须保兼容边界，
不得拿它作本笔 typed 口强耐久证明；未迁消费者如实列入余项。

## 本笔文件边界

中性值与工厂在 `src/trajectory/cas_store.hpp/.cpp`，默认 File 复用 BlobStore 底层。
SessionManager、locked opening 与运行时场 owner 管能力，不另复制 SessionExecution。
MemoryLedgerBridge 与 `src/sdk/memory.hpp/.cpp` 接真写、校读、报告与恢复。
新源码只归 engine；runtime/SDK 引用它，不反向链接宿主。
不新增公开 `core.hpp` 存储字段。若公开 SDK 装配还需窄接线，先协调并行写权。

## 远端验收门

1. 默认 File：同 SHA 真复用、预置坏件拒绝、不可覆盖发布、短件/超帽/坏 SHA/非普通件。
2. 分阶段失败：临时件检查失败拒发布；成功 close 后的发布前/后边界注入各留真状态，新 Memory 引用仍为零。
3. CLI accounting 与 SDK Submit 真召回大于 512 bytes 片段，核模型实际正文与 V3 refs。
4. 内部 fake 不向默认 artifacts 路径写件；真 Memory participant、桥、报告与持锁恢复仍走能力。
5. 同项目两场与不同项目两场各持能力；同串 op/ref 不串结果，取消/Close/失败开场收写权。
6. Installed consumer 只用公开 SDK，跑默认 File Submit→报告→Close→同 ID Resume。
7. 三平台 focused、完整 native 与宿主 parity；ASan 必选新来源，登记与原件须非零实跑。

Fake 验收走真实 SessionManager opening，检查场锁已持有；不直接调 ValidateReport 冒充恢复。
公开安装消费仍只证明默认 File 路，不能宣称外部 fake 注册已交付。
不删原 Memory/Blob 断言，具体新 CASE 数在实现完成后登记，不预报原生通过。
本地只读源码、写源码与跑文档/纯数据门，不 configure、编译、CTest 或执行原生探针。

## 当前实现与证据边界

实现册为 `tests/integration/sdk/test_lubancore_memory_cas.cpp`，固定 10 CASE。
本基底新增这一册后，SDK focused 登记 22 册、源码计数 201 CASE；
ASan required 集合新增 CAS，合计 27 来源。这些只是静态登记，远端实跑尚未验。
若后续私有合流增册，取实际来源并集重算，不能把旧头原件算入新头。

Fake 组件册经真实 SessionService 受理、分派，开真 Turn，采用完整 Memory Context，
落本场报告。旧 CLI `ProjectMemory::BuildTurnContextForDispatch` 也通过同一能力，
给真实打开但未执行的 child 冻结片段，再按 Cancelled 真封子账。
同 ID 恢复在开写前核这些实际材料，fake Read 计数须增长，默认 CAS 路径仍无实体。
这册没有调用公开 SDK Runtime 的完整执行与 Complete，不冒充 complete result 验收；
完整 public Session 与 installed consumer 另用默认 File 跑 Submit、模型请求、终态与恢复。
公开 Core 没加 fake 注册字段；只为内部工厂与将来公开 SPI 留真接线。

File 的 close/publish 故障接点在真实 native close 之后，只模拟边界拒绝或确认缺口；
不冒称操作系统 fclose 真的失败。发布后实体留存，typed 回执保已提交与未确认。
坏件、目录、超帽读取都走真实文件；FIFO 与目录 alias 只在 Unix 场验，
Windows 不凭缺少链接权限偷记已验。旧 BlobStore wrapper 仍保 best-effort 兼容承诺。

## 余项

Named tool metadata/result、recorder 超限正文、环境/图片、完整 Context 其余落点与导出 reader 未迁。
owned metadata、operations、Skills/Memory plans/reports、项目 topic/catalog 仍沿现本地材料。
Journal lease/fence/stable snapshot、数据库、对象存储与整场替换注册后置。
本笔没有迁完 Blob 口，也没有开放 SDK child API 或扩大后台生命周期承诺。

## Windows 原生路径补笔

源 `3631fa2470faf4d089aa2136e81cf850cefc1001` 远端 Windows SDK 册出现
`cas.tmp_open_failed`：CAS 10 册中 1 册失败，旧 Memory 召回 14 册中 2 册同错。
原日志留在 `memory-cas-3631fa24`；当前没有 errno 或实际路径长度原件，
不能断称长路径便是唯一根因，也不能拿别个平台绿账冲掉这轮失败。

这笔只补默认 File 原生路径口。沿现 `platform::FileIoPath`，普通短路径保持原样；
延展路径只供 native I/O，workspace、containment 与账上逻辑 ref 保原形式。
临时后缀拼完再规范，读取也过同一原生路径口。目录创建、判型、清理和发布
核实际路径；不放宽 regular/reparse、完整大小/SHA、不可覆盖与 typed 耐久回执。

现 10 CASE 内补 Windows 子例：实际目标跨延展阈值；另核目标长度小于
`MAX_PATH - 12`，实际临时名加 PID/counter 后跨过同一阈值。
测试真 Store、读回、同 SHA 复用，故障接点只记录实际临时名，不能假称 flush/close 失败。
册数不涨，原断言不减。先交合同，再改代码；本地不跑原生，fresh CI 尚未验。
