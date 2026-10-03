# SDK EventSink v1：独占订阅队列

本笔基线 `edcf8cb8f822fed95d5d4045e00ed64d97cf0a34`，属于私有待验组合。
只接真实 SDK `Subscribe → Emit/Push → Next → Close`。不接后台 Job、Journal、Runtime factory 或 CLI EventSink。
首合同先提交，原生验收只跑远端；静读与文档通过不算原生通过。

## 公开值与装配

`include/lubancore/events.hpp` 承接原 `ApprovalMode`、`ApprovalDecision`、`Approval` 与 `Event`，字段及次序不改；`core.hpp` 继续导入原名称。
`events::v1::EventQueue` 提供 `Push(Event)`、`Next(milliseconds)`、`Close()`，均返回原 SDK `Result`。
`events::v1::EventSink::CreateQueue(capacity)` 返回本次独占 queue。`SessionOptions` 尾添可空 `unique_ptr<EventSink>`。
可信宿主在 SDK 外构造独占 sink，移交本场。每次 `Subscribe` 建独立 queue，不共享跨场实例，不添加投递线程。
这口只投递 owned 临时通知，不返回 durable ACK、cursor、执行许可或 Job Operation。

## 默认行为

默认 queue 搬原 deque 算法：容量、16 MiB text/payload/approval-input 总帽、FIFO、超帽关流清空、超时返回 nullopt。
原 Event 字段、转换、投递顺序、`sdk.events.overflow`、`sdk.events.closed`、容量和负超时错误均保留。
`Session::Impl::Emit` 仍锁内取 weak targets，锁外逐队列 Push。旧 runtime EventSink、CLI、Agent、账与结果读取不迁。
façade 不另存一份 Event deque。provider 只许 bounded 短时 Push；Next 才能按传入 timeout 等待。
宿主实现须守容量、字节帽、FIFO 与合作退场；本笔不认证恶意宿主，不把宿主自身分配说成固定堆帽。

## 生命周期与关闭回执

perSession delivery owner 持独占 sink，计真实 factory 与未退 queue。factory 在 Session/API 锁外运行，返回后再核 closing；迟返 queue 真 Close、销毁，不发布。
queue control block 持独占 queue，计 Push/Next；调用 provider 不握 façade/API 锁。Close 先封新入口，调用 provider Close 唤醒 Next，再等已入场 Push/Next 真返回，锁外销 queue。
Close 与已入场 Push/Next 可以并行，provider 须支持这条退场合同。抛异常或返回错误不证明借用已退；核心照样等实际退出，不 detach、不加关闭超时、不销活对象。
尾添 `EventStream::CloseChecked() -> Result<void>`。一位 close owner 调 provider，别的调用等同一份实际结果；重复 Close 不再调 provider。
原 `void Close()` 与析构沿 checked 路线退场，丢弃返回值但保原回执，不宣称 checked 成功。
delivery error 与 checked-close error 分栏。默认 overflow 只坏本订阅，默认 Close 仍成功，不把旧 overflow 抬成 Session Close 失败。
owner 汇总第一 close error，弱 target 已消失也不丢错。queue 与其 capture 真销毁后才退 owner 计数。Session 关场等 factory/queue 退净，再销 sink、冻结关闭回执；Runtime 原 first-close-error 继续保存。
Session 原停止执行、worker join、service checked Close、锁外退资源、最后退 streams 次序保住。新 queue 不借 Session::Impl、service 或 Writer。

## 错误与重入

factory null/抛错/返回失败让本次 Subscribe 失败。Push/Next 失败封本订阅，保第一 delivery error；不重投、不重跑、不改主 Operation 结局或 V3 Unknown。
新 provider 错域为 `sdk.events.provider_failed`、`sdk.events.provider_invalid`；保实际宿主错误详情，不能凭详情判业务结局。
Close 失败单列 `sdk.events.provider_close_failed`，Session/Runtime 返回实际 first close error。调用返回失败仍不能越过活借用。
实际 factory/Push/Next/Close/析构的短栈 scope 标记 provider 回调。递归 Subscribe/stream Next/CloseChecked 拒 `sdk.events.reentrant`；阻塞 Session/Runtime lifecycle、WaitResult 拒原 `sdk.lifecycle.reentrant`。
非阻塞已发布查询可重入。scope 不借 Writer，不留在事件中，不授予权限。可信宿主不得从回调销 owning Runtime/Session；SDK 不替违规代码造退出成功。

## 真路径验收

一源 `tests/integration/sdk/test_lubancore_event_sink.cpp`，固定六 CASE。
`[sdk-event-sink-path]` 六 marker：`default`、`actual`、`overflow`、`error`、`drain`、`isolation`，各在硬断言后输出。
实际 Runtime/OpenSession/Submit/审批/完成事件验证默认值与顺序、宿主队列、独立 overflow、factory/Push/Next/Close 错误、弱票退出首错、关闭幂等。
真 held provider 测 Close 等退场、迟返 factory 回收；失败先开闸再 join。factory/Push/Next/Close 真实重入逐口拒绝。
同项目两场、异项目两场核事件 SID/operation/queue 归属、独占宿主、四场退净。原 SDK native sources 保全，不用镜像队列函数替公开路径。
交叉 CMake、focused/ASan/full argv、SDK-only closure、目录登记由 root 接；本地只纯检查，不 configure/build/CTest。
