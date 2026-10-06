# Managed operation provenance：内部持久事实

本笔从 `80bb443c` 起步，沿已冻结准备稿 `e2d705d5` 落码。先有合同，后有实现。Managed 开场依赖另一笔真实 move-only Service → Runtime → Ledger → Manager 接线；准入能力不放 copied Options。

新口只供受信内部宿主写存储事实。它不认证主体，不查 Policy，不开 Agent、模型、工具、普通 Pop 或公开 Managed API。新 Managed 场只记文本输入；旧 Local schema1/2、artifact1、无键、图片、多跳恢复、错误码与字节照旧。

操作账采用 schema3，输入原件采用 schema2。冻结完整原主体四字段、owner 四字段、bindingVersion、RequestModel 能力、typed intent、真实 operation/input/execution/run ID 与首次 admissionPolicyRevision。创建者不能充每次发起人。缺来源的旧账不能用当前 actor 补齐。当前 policy revision 不进稳定 intentHash；同场同 key、同主体与 intent 回原 receipt；换主体、binding、能力或正文拒绝，不漏原 ID。新口仍须由以后公开边界先现查 Submit。

受理共用 Service 原发号、去重表、队列与 commit_mutex。输入原件 create-new，保真正 native 详细回执；accepted 走真正 JournalWriter Detailed Append(PowerLoss)。两份均确认后才发布内存与回执。已提交却内存发布失败，保 native committed 与 publication gap 两层事实，封后续受理。进入写调用却没有回执才记 Unknown；不读回洗绿，不重用 ID，不删未知残留。首失败与首次 Close 回执不升级。

schema3 首片只准 Accepted → RejectedBeforeDispatch。独立 `operation.rejected` 行只引用原 provenanceHash，记 rejected/cancelled 与稳定原因；不记 dispatched、turn、usage 或 sdk-result。明确内部裁决口须核真实队首 witness，追加确认后才移队。Close 以真实 cancelled 终态收场；写入未知则保待定材料，不伪造终态。普通 Pop、RecordTurnFinal、InitializeExecution 仍拒 ManagedStorageOnly。

专用 ManagedOperationMaterials 捕获真实 operations 与 accepted 所引输入原件。它在 Service commit_mutex 与真实开场 SessionLock 寿命内读取，按准确 roster、owned 根和有界 regular-file reader守门。严格消费者只吃 owned bytes，核 schema、类型、主体、owner/binding/run、原件 SHA/长度、typed intent/provenance、唯一键/ID与合法阶段。单份输入8MiB、合计128MiB、4096件；操作账16MiB/65536行/256KiB每行；总份384MiB。帽指一份读取材料，不是整个进程驻留上限。

只读材料是纯值，不抓 Writer、Service、Policy 或 public Session。它可越 Close 存活。通用 RecoveryKeyKind::OperationInput、受锁 Managed 同 ID恢复、SDK result/turn绑定、Policy复查、View与supervisor另笔接；本笔不改旧 RecoveryCaptureRequest 的选择或默认帽，不借 Local Seed 祖先链恢复 Managed。

Close 先停受理，再给排队操作落未派发取消事实，保首次详细操作账 Close，随后按旧顺序关闭主 V3 和退真实锁。两边都收错；未知写不阻资源退场，也不自动认领残留。

验收补在原 CASE内，旧断言、marker、预算和 CASE数保住。真走 Reserved → Publish → Finish → Managed Service → 输入原件/accepted → owned严格读回 → 明确拒绝/Close。核重复 revision、换主体冲突、scope漂移、缺字段、互换、重哈希篡改、重复行、错误阶段、实际 native失败、已提交发布失败、零 dispatch/turn/sdk-result和真锁退场。只提交源码；原生与三平台运行交远端 CI，不在本地编译。
