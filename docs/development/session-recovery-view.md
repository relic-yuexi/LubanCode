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

每卷、每行、每实体、目录项数与合计字节都设帽。具体默认值在源码合同先定，
超帽明报，不能截短、跳坏行或只取前几项后自称完整。
实体判型与 bounded regular 读口仍守现 reparse/FIFO 门，不宣称 OS 沙箱已交付。

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

这些只是候选接点，不代表源码已经动了。公开 core 头与整场注册不在首笔。
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
