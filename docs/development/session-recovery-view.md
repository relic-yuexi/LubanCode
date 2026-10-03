# 本卷同 ID 恢复读面合同

[开发手册](README.md) · [Memory CAS](memory-cas-capability.md) · [存储 SPI 候选 #273](https://github.com/relic-yuexi/LubanCode/pull/273)

先交合同，再定源码范围。基线固定 `eb94b3531921c4e880ed293048cfc50f1befc637`。
本笔只有文档。内部 `SessionRecoveryView` 尚未实现，不开放公开 Provider 注册，
不称 JournalStore、MetadataStore 或整场存储已经换完。默认文件账继续跑原路。

## 先收哪一段

CLI 和 SDK 共用本卷主 V3 的同 ID 续接读面：拿真实场锁，冻结本卷字节，
再从这份字节 Verify、Fold、恢复 Writer。Memory 恢复所采用的 plan、operations、
sdk-result、recall-report、完整目录清单和缺件状态也归同份只读视图。
不把锁前历史、锁后新计划和第二次读到的账拼成一份采用事实。

视图只读。本笔保留原生发布、Journal Append、Finish、Close 和 SessionLock。
OperationsFile 的 accepted/dispatched/final 属真实追加流，不能改叫 metadata publish。
输入原件是 `operations-inputs/op.json` 原子发布；本笔不迁输入发布或排队恢复。
Memory plan/report 与 SDK result 各有现成发布口，也不在这笔换写口。

## 值、归属与退场

内部 factory 归运行宿主，实际 Capture 归已持独占锁的 SessionManager。
视图强拥有完整原字节与清单，只用逻辑 workspace/source-session/stream/key 寻址；
默认 File 适配器保存物理根。Provider 不交任意物理路径、裸 V3Ledger 指针或 verified 布尔。
源场与目标场分别标明；子场 SID 只在本父场内有意义，不能借同串 SID 跨父认领。

SnapshotToken 只标 owned 视图身份，不是认证、权限票、跨机 fence 或多文件原子事务。
核心核真实 schema、canonical/hash/seq、源场归属与 V3 采用链；metadata 自哈希不能顶替它。
File 参考在锁下捕获，继续开写前还须核持锁文件归属与这份前缀的真实尾游标。
不凭 token 自认 verified，也不拿 fake 的有效前缀冒充任意 File 可续写。

开场参加者只借本次 invocation 的只读上下文，持值材料由场 owner 收着。
开场失败收借用，不能留悬空指针。退场沿现执行 owner 次序，停止实际借用、
关 Writer、释放锁；纯 owned 值销毁不改执行结论，也不释放别场能力。
本笔不扩后台任务寿命，不把恢复视图当 live 查询缓存或新写能力。

## 读取裁决

SDK 每卷、每行、每实体、目录项数与合计字节都设帽；CLI 新总帽可 unset。具体默认值见下段，
超帽明报，不能截短、跳坏行或只取前几项后自称完整。
metadata 判型与 bounded regular 读口守现 reparse/FIFO 门；主卷保 follow 后核 opened regular，不宣称 OS 沙箱已交付。

读取分四态：Absent、Value、Corrupt、IOError。Value 只含未经领域采用的完整字节，
核心随后验证。缺件与空件分开；坏 JSON、坏归属或坏链不能吞成空表。
完整清单保缺目录、缺实体和临时残留事实，枚举帽计入每项，不跳 tmp 绕帽。
Memory 仍沿原规则区分合法未完成与完整结果缺报告；不能把合法空场一概拒掉。

Memory 恢复采用严格的 owned operations parser；旧 CLI 宽容 reader 默认不改。
报告 operation/turn、Context/Fact、真实请求与选中材料仍由原领域核逐项校准。
相同 op 串只认本场，不凭信封重 hash 冒领别场结果。

## 真正接点

- `src/trajectory/session_recovery_view.hpp/.cpp`：内部 owned 值、逻辑键与只读 File 参考。
- `src/trajectory/session_manager.cpp`：同 ID 开场 Acquire 后统一 Capture，不再以有无 opening participant 为条件。锁外初读仅作预检；最终 Fold、归属和 outcome 从锁内视图重建。
- `src/trajectory/v3/reader.hpp/.cpp`：同份 bytes 进入 VerifyV3Lines、ReadVerifiedV3Lines。新增 ProjectResume 的 owned 本卷入口；FoldV3ResumeChain 不能调用旧 path wrapper 再读本卷，timeline/model_context/FoldToolActions 与起始 current 都取这份 own。
- `src/trajectory/v3/writer.hpp/.cpp`：内部 owned-prefix Continue 消费上述字节，避免 VerifyV3File 与 ReadRawLines 各读一份。原 JournalWriter 开追加句柄、实际 Append/Close 全留。
- `src/trajectory/opening.hpp` 与 `src/sdk/memory.hpp/.cpp`：锁内只读视图交给实际 Memory 开场校准；plan/ops/result/report 和完整清单不再在这个窗内逐路径重探。
- `src/runtime/session_service.hpp/.cpp`：提供严格 owned-bytes operation parser 给 Memory 恢复。原追加、输入发布、队列处理不改。

这些只是候选接点，不代表源码已经动了。公开 core 头只添下段预算值，整场注册不在首笔。
本卷只读适配器也不能证明 append 已可替换。

## 留在原路的材料

FoldV3ResumeChain 的祖先卷 resolver 和 ResumeAsNewV3 导入新场尚未迁。
本卷同份字节不等于全祖先链快照；要迁这两路，另列每卷捕获与真实持锁接点。
SDK 子场历史采纳仍读 child 卷和产物；Skills、Save、扩展计划、named tool result、
项目 Memory 提交账、排队输入 inputRef 原件及一般 live 结果查询也保原路。
只迁 Memory 恢复采用材料，不能说全部 SDK 恢复只走 SPI。

## 后续源码验收

默认 File 真跑 CLI 与公开 SDK 同 ID Resume，核模型实际历史和 Memory 采用链；
无 opening participant 的 CLI 也须采用锁内本卷视图。
内部 fake 在真实持锁 Capture 窗被实际消费者使用，不在准备结束后才挂，
不伪造成功 V3、Provider verified 或模型回执。fake 只证只读接线，不证替换 Appender。
坏、短、超帽、截断、错 scope/stream、坏 operations、清单或缺件不一致都明拒。
输入格式合法却冒领采用链也须拒；原发布与 Append/Finish/Close 继续跑三平台。
新增册数、精确来源和边界反例在源码笔冻结，文档不预报 native 通过。

后笔 MetadataStore 再独立定 CreateNew/ComparePublish 的
not_committed/committed/indeterminate 与实际耐久；写后 throw 或 flush 失败不能洗成布尔拒绝。
TypedAppend/Finish/Close 另定请求键、期望尾与未知冻结，不把这笔只读合同算成完成。
全程远端 CI。本地只查源码、写文档、跑纯数据门，不 configure、编译、CTest 或原生探针。

## 源码首笔：读取预算候选

下段等二审，再动源码。沿“CLI 不丢功能”先收口：CLI 保留旧总量规则，SDK 给有限默认，
宿主能按场调整。恢复采用同份字节和读取预算分开；这不开放公共 SPI，也不换写面。
本卷旧 reader 没有 byte/line 总帽，不能径直把新 SDK 默认当 CLI 总帽。

公开参数只添值类型与 SessionOptions 一项。候选形状如下；没有 Provider、物理路径或回调：

```cpp
struct RecoveryStreamReadLimits {
    std::size_t max_bytes = 0;
    std::size_t max_lines = 0;
    std::size_t max_line_bytes = 0;
};
struct RecoveryReadLimits {
    RecoveryStreamReadLimits journal{128u * 1024u * 1024u, 262144u, 8u * 1024u * 1024u};
    RecoveryStreamReadLimits operations{16u * 1024u * 1024u, 65536u, 256u * 1024u};
    std::size_t result_total_bytes = 128u * 1024u * 1024u;
    std::size_t view_total_bytes = 384u * 1024u * 1024u;
    std::size_t result_directory_entries = 4096u;
    std::size_t view_directory_entries = 8192u;
    std::size_t directory_name_bytes = 1024u;
    std::size_t directory_name_total_bytes = 8u * 1024u * 1024u;
};
// SessionOptions:
RecoveryReadLimits recovery_read_limits{};
```

这组值只管本次恢复读入，不改保存的 Memory 计划或执行许可。相同 ID 再开场能提高有限预算；
不用重写历史或冻结原预算。每场拷贝值，四场各改各的。新场也先验参数，不等恢复才报坏值。
全字段须为正；零明报 `sdk.recovery.invalid_limits`，不用零或 MAX 冒充“不限”。
宿主能降低或提高新增预算；原 Memory 单件帽仍守原值，不能借新参数扩大 Recall 选材或报告容量。

| 读入材料 | SDK 默认 |
| --- | --- |
| 主 V3 | 128 MiB、262144 行、每行 8 MiB |
| operations | 16 MiB、65536 行、每行 256 KiB |
| sdk-result 正式原件 | 每件保原 64 MiB，新增合计 128 MiB |
| SDK result 目录 | 4096 项，tmp 计项，不采用 |
| 两 metadata 目录合计 | 8192 项，含 report 原有 4096 项帽 |
| 名称 | 每个 1024 B，合计 8 MiB；记完整 UTF-8 原名 |
| 视图原字节与名称 | 合计 384 MiB，不称 RSS 或硬时间截止 |

Memory 既有八项材料帽不改：catalog 4 MiB、topic 16 KiB、topic 合计 24 MiB、context 128 KiB、
evidence 每件 16 MiB、evidence 合计 64 MiB、entries 1024、目录项 4096。
另保 plan 4096 B、report 每件 512 KiB、report 目录 4096 项、report 合计 64 MiB。
主卷预算与这些单件容量分别校验，先遇哪条拒绝线就明报哪条；不能返回前缀后自称完整。

内部 `RecoveryCaptureRequest` 强拥有 `std::optional<RecoveryReadLimits>` 与 Memory key 选择。
CLI 不设新增预算时用 nullopt；它仍取完整本卷、同字节 Verify/Fold/Continue。
SDK 总交一份已验的有限值。Memory key 选择只由冻结模块
`SessionMemory::RequiresRecoveryMetadata()` 纯 bool 定，不猜 participant 成员，不从环境自动开启。
原 report 单件/总额等帽无论内部新增预算是否 unset 都保留。

内部 trajectory/runtime 保存中性 owned 预算值，SDK 在边界逐字段转换。
内部层不反向包含 `lubancore/core.hpp`，也不持公开 SessionOptions 或 SDK 模块指针。

旧 CLI 主卷实际沿 `session_switch.cpp` 的 exists/ifstream、reader 的 ReadRawLines、
Writer 的 VerifyV3File/ReadRawLines 读取；这些入口没有 opened-regular/no-follow 门。
主账 symlink 会随旧路径读取，这是源码事实，尚未本地起原生探针。
首笔 CLI 和 SDK 主卷都保原 follow 路径语义，跟随后核已打开对象确为 regular；
不借读面重构新增主卷 no-follow 拒绝，也不称行为零变化：opened-regular 是新核。
现有 SDK Memory ReadOwned 已拒 symlink、核 canonical 与 opened regular，本笔保这条旧门。

Capture 从实际打开的主卷读字节并记内部 native identity：POSIX 用 fstat 的 dev/ino，
Windows 用真实 HANDLE 的 volume serial/file index。opaque File anchor 归场 owner，
持住 Capture 那只原生读柄直到 Continue 完成同对象复核，不能读完关闭后仅留一串 inode。
纯 owned 视图不持 FD；anchor 与预算、只读值分开收。File 参考从该句柄记下身份，
Provider/fake 不填写 native 身份或 verified 标志。
fake 读面从真实锁窗参考捕获复制 owned 值，再替换 metadata 或逻辑键；不能造“原生柄已验”成功。
POSIX Capture 选 `O_RDONLY|O_CLOEXEC|O_NONBLOCK`，不设 O_NOFOLLOW；先 fstat regular，再读取。
Windows Capture 用实际 OPEN_EXISTING 读柄和共享读/写/删除，跟随后核 GetFileType、目录属性和真实文件身份。
Continue 必须打开已有件：POSIX 选 `O_RDWR|O_APPEND|O_CLOEXEC|O_NONBLOCK`（无 O_CREAT），
Windows 选 `_wsopen_s` 的 `_O_RDWR|_O_APPEND|_O_BINARY|_O_NOINHERIT`、`_SH_DENYNO`（无 `_O_CREAT`），
再 fdopen 成读写 append 流。POSIX 读取前核 fstat regular；非普通设备不读。
Windows 经 `_get_osfhandle` 核 `GetFileType` 与 `GetFileInformationByHandle`，不能拿 CRT 常为 0 的 st_ino 认身份。
fdopen 的 a+b 起始读位置不跨平台假定：核 prefix 前显式 seek 到 0，核完再 seek 到 END。
同柄身份须等 Capture anchor，再逐块核完整 prefix、实际大小、EOF，最后定位尾部交原 AppendLine。
同名换成另一对象，即使字节相同也明拒。不用 r+b 丢掉原 append 语义，也不用另开 rb 冒充同柄核验。
缺件不能偷建，失败不追加；复核柄 Close 失败留实际诊断，不盖原拒开，不冒充 native durable 成功。
这些只核实际续写对象，不把 anchor/token 当认证，不称合作场锁消除了外部换树竞态。

调用流固定：SDK 参数验型、freeze 值→内部 launch/runtime/session-manager 透传→
same-ID 真实 Acquire→Capture→核心验 scope、清单、bytes→同份 Verify/owned ProjectResume/Fold→
Memory adopted metadata 核→native open-existing 同柄 prefix/EOF 核→原 Writer 开写。
无 opening participant 的 CLI 也走锁内本卷 Capture。祖先、AsNew、queued input 与其它旁路仍留上文原路。

未设预算不塞 `size_t::max()`：现 bounded reader 的 `cap - used + 1` 遇 MAX 会溢出。
新读口以 4096 B 分块；有限路径先核 `used <= cap`，按 `cap - used` 取块，满帽后只探 1 B 判 EOF。
unset 路径收至 EOF，每次 append 先核容器和 size_t 余量。计行、计项、计总额也先减再比，
没有 MAX+1，也不把无法分配洗成空件或 Absent。诊断保 Corrupt、IOError、limit；不记认证成功。

对应源码只添公开值形状和 SDK 装配接线；实际 factory/view、严格 operations parser 与
owned Continue 都归内部。新增两来源固定 8+4 共 12 册，核 finite 精确边界/+1、
CLI unset 真长卷不误拒、SDK 调高预算再开场、MAX 附近算术不回空件、真实同柄拒偷建，
以及报告半完场、四场隔离。consumer 保基线 20；初稿基线 focused 24→26、ASan required 29→31。
合入 Windows result-store 来源守门后，实际 focused 27、ASan required 32；consumer 仍为 20。
最终从本树登记与实际来源核数，不凭旧头绿灯算验收。

SDK 同 ID V3 锁前预检也沿有限主账 owned 读取与 Verify，不先走旧整卷无界读取。
这份预检只核格式、资格与原 tail fence；它不能充当恢复采用事实。
真正采用仍在 Acquire 后重做 Capture，祖先与 AsNew 保原路。
预算按每份 snapshot 计，不称全生命周期累计 IO、RSS 或耗时上限。
超帽先拒，opening participant 尚未调用，Continue 尚未打开，原账字节保留；
宿主调高预算再 Resume，应能从真实旧账恢复。CLI nullopt 仍留原预检读取策略。
主账保原 V3 Reader 跳过空物理行语义，计行帽仍计实际物理行；
operations 严格读面继续拒空行与缺末换行，不沿宽容 live reader 跳坏行。

选中 Memory 时，本笔 operations.jsonl 也核 opened regular/no-follow。旧 SDK
ValidateOperationLedger/ReadOperationFacts 沿 exists/is_regular_file/ifstream 跟随路径；
这条 owned operations 门新增拒绝 symlink，不能称所有 metadata 行为不变。
plan/report 沿原 strict 门；CLI 未选 Memory 不探这组 operations metadata。
主账 fake 必须保 File reference 全文和 snapshot token；核心再核 token 等 SHA(main bytes)，
改 main 即在 domain adoption 前拒，不借后续 Continue 拒绝遮先行伪事实。
Capture 取得 anchor 后，metadata/factory/核心核验失败都先 checked Close，再接原错误。
