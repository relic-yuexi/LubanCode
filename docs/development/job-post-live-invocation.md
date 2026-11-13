# 真实 Job Post 调用借用

首笔只收中立 Coordinator 当前 Post 调用。基线 `bf48042d6f780fd18cb7f3a2c8446779ab46f8b7`；旧 `codex/sdk-job-admission` 本地与远端引用保留。公开 Jobs、SDK Job Operation、持久 binding、SessionJobs 装配均不在这笔。公开 Close、父取消和 Job 许可域仍待答，不替用户定政策。

实现前吸收前置夹具 API 单词修正 `445ad858`（本树提交 `fcccdabf`）；原 c1b 远端编译红另留。此举没有改本笔生产、册数或断言，不拿父支状态证明本笔原生通过。

## 实际 producer 与借用

`SettleOwned` 已持真实 OwnedRecord、当前实例/epoch、业务 attempt1、actual completion、Started/terminal/persisted 回执和实际 capability。它在调用本 record Post 时签内部 `OwnedJobPostInvocation`。该 handle 不从 JSON、ledger、同 run 旧锚、父 Operation 或 caller nonce 构造；只签当前具体 record，不能凭 `current_owned_callback == impl` 为另一份 Job 发许可。

实际 live lease 由 producer 内部独占。交给回调的 opaque handle 只持私有状态弱引用。宿主可保存副本作检查；副本不延长许可，不延 writer、record、module 或 cancel storage 寿命，不作延后执行的许可。当前同步 callback 退出、抛错或退场，producer 先撤 handle，再校原 Post receipt/完成结算。其它线程、其它实例、其它 callback、失效 handle、恢复的历史材料都拒绝。

显式 `OwnedJobCapability::live_post` 只替换这份 capability 的 Post 调用口，返回真实原生 WriteReceipt。`post` 与 `live_post` 恰选一项。默认 `post` 的入口、gate 次数、clock、命令 worker、权限与原回执规则保留。

`CheckOwnedPostInvocation` 返回 owned 值：真实完成件、冻结 Prepared facts、actual adoption 回执和本次 owner phase。先核本实例/epoch、当前具体 invocation/record/完成件、当前同步线程，再核 writer 仍 open/unbroken、实际 SID/run/attempt。异线程在拿 serial 锁之前拒绝。值里没有 writer、cancel 裸指针、SDK Operation 或 main OperationScope。它只证本次真实收件，不签持久 SDK 授权，也不新增业务许可；原 scope gate 不变。中立 ledger capture factory 仍只校原生材料，不能单独签 live 权。

## Open、Draining、Retired

- Open 接实际本 record 完成件。新票与派工仍沿原入口、父句柄确认和 gate。
- RequestShutdown 先关新准入，转 Draining。只收已拥有、已经派发并真实归来的 record；不因 `closing` 一刀拒掉正常 Shutdown Post，也不借 Draining 开新派工。
- callback/observer/真实结算退完，才撤 writer 并转 Retired。Retired 不签、不借、不写；旧 handle 不复活。真实 writer gap 保未知，生命周期退出不等于持久成功。

本笔不启 Middleware observer、第二套执行器或新线程。callback 必须同步收完自己所拥有的调用；producer 借用活过 callback，返回后先撤。原 join、serial→jobs 锁序、锁外销 capability/capture 不改；join 不持 writer/jobs 锁。

`SnapshotOwnedJob(owner, job_id) const` 沿 serial→jobs 锁序，只在宿主安全边界拷实际表内值，不 Pump、Reap、调用 clock、补账或等命令。mutable settlement 字段受 writer serial 保护，不能只握 jobs 锁偷读；此口不供正被 callback 主线程 join 的异线程 observer，也不冒称公开 SDK 任意线程缓存/Wait。Get/Wait 原义不改。关闭后可查已留 owned 值。PassiveHold 仅还原历史知识，不能取得 live invocation，不能重派、重跑 Post 或重建 SDK Operation。

## 范围与验收

生产只改 `src/tools/tool_job_coordinator.hpp/.cpp`。添真实 Coordinator 回归来源与精确 CMake/focused/ASan/SDK-only 表和 CI 路径门；不改旧 source 册、预算、marker、Legacy 语义或 SDK 主 Operation 索引。

新册核六路：默认旧 Post；真实 owned producer/attempt1/五键；当前 callback 假票与跨 record/跨场拒绝；Shutdown 真 Draining、Retired 失效；真 writer gap 与首尝试撤销；Close/Continue 历史 PassiveHold 无 live 权、查询零 Pump。各案由真实声明、注册、adoption、父句柄链确认与实际 command 完成件起，不手填 owned map 或假成功回执。

本地只做文本、纯数据、AST、文档检查。三平台原生与精确非零证据门交远端；源码登记不当原生验收。

同一 Coordinator 再注册时，验“本次不增 worker/执行”，不把前场累计数误认零。真 worker 已退、writer 预先关闭那路，原 Pump 不收完成件；零时等待仍未满足，须由真实宿主 Shutdown drain 收未知缺口，再纯读快照。正常与 Post 写错两路仍走原 20 秒 Wait；关闭、缺口、计数、原生回执、幂等和六枚末标断言保留。这里只修夹具合同，不改执行与公开 Close 政策。
