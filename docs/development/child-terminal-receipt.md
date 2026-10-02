# 子代理终态回执合同

本笔只收内部终态证据。基线 `94b98a5e`。不开放 SDK 子代理入口，不接异步审批，不改关场协议。

## 公开给内部调用方

`TrajectorySubagentBridge::Finish` 返回值持有完整回执：真实执行结果、子账格式与 session/run 身份、原生 append 回执、写者 broken 状态、终态五键引用，以及独立 Close 结果。五键为 session/run/event ID/seq/hash，仅真实 PowerLoss append 确认提交才有值。

append 分 `committed`、`rejected_before_commit`、`durability_unconfirmed`。枚举不能单独证明耐久；V3 初次 I/O 错仍可能带原生 `Rejected`，须同时保错误与 broken 状态。Close 分 `closed`、`close_failed`、`not_attempted`。已提交终态遇 Close 错，五键仍留着，完整交接仍不成立。

V2 写前提交钩子只回原生 `IoFailed`，不碰 JournalWriter，不能凭这枚回执推断句柄 broken。终态仍未确认、没有五键、禁止再 append；健康句柄须照实关掉，Close 可独立成功。原生册同时核摘要与缓存，不冒称操作系统自然写盘失败。

## 谁持有，怎样收场

子桥持原生 recorder/writer 与轮桥。第一次 Finish 固定执行结果，只尝试一次 append，再调用真实 Close，缓存整份回执；重复调用返回同值，不重跑模型或工具，不重 append，不重试 Close。V2 Close 须消费原有 JournalWriter::Close 真返回值，不能只凭文件摘要宣称关柄成功。

子桥持一份 shared registry owner。registry 用 mutex 存、查回执，查口返回值副本；换场换 owner，旧 child 不得写进新场表。完成 Hook 只捕这张表，不再借父轮桥。它只护这张表，不承诺父 writer、派工或 SDK 关场已经并发安全。旧 `trajectory_spawn` 仍借 ledger/hub/父轮桥，子轮 callback/错误 sink 也沿现有借用合同，不能凭 registry 存活延长父场。

`RunSubagentTask`、启动拒绝清理及父完成 Hooks 真消费回执。选了轨迹却 append/Close 失败，须明报 `trajectory.child_terminal_persistence_failed`；真实执行成功仍记在回执，不算 durable completed handoff。部分正文只作诊断，不冒称父模型已采用。失败不等于回滚，不许为补账重放工具。孩子执行过却终态交接不完整，返回既有 `StopIndeterminate`，共用 loop 拦同批后续工具与后续模型请求。后台启动拒绝且未执行时，明确拒写并且 Close 稳可普通拒绝；未知 append 或 Close 错仍阻断父轮。孩子自身未知副作用也原样向父轮传，不借普通 error 洗掉。未接轨迹旧 CLI 保原行为，不造 committed 回执。

## 本笔边界与验收

共用 `DriveTurn` 和 Stop 环聚合未知副作用旗。初轮、续投、Stop 续轮一见未知就停，保真实步数与错误，不冒充取消。未知 Stop 续轮保真实末轮，不沿初轮成功值收账。未知发生在工具执行之后，本轮输入已到模型；已领取批次提交，未领队列原地保留。常规错误与取消仍走旧退批规则。原 CLI/Workflow 共用这份错误投影，不能当普通成功续跑。CLI Stop 未知态须返回非零、发 Failed 事件，并让轨迹同路失败收口。

子执行结果沿共用 `ClassifyTurnEnd` 与真实 TaskOutcome 收口。取消压过普通交账，预算耗尽、空结论都不得记成功；原生终态落稳只证明账已收齐，不证明任务完成。

父 V3 `subagent.linked` 仍只证明开场来源。此笔 registry 没有父账终态观察，也没有恢复采用链；终态回执不能替代 result-store → selected → tool message → context admission → 父请求。

开场 `childCheckpointRef` 使用 `sessionId/runId/seq/lineHash` 四字段，须指到真子卷那段前缀。它没有 event ID，不能冒充终态五键；原生册另核真实终态行五键，保开场与收场两份证据。

新增原生册须验真 V3 spawn/link 与五键、成功/失败整份缓存、真实拒绝与 I/O broken、提交后真实 Close 边界注入失败、前台执行与后台启动拒绝消费、shared registry 寿命及并发值。真父子回合钉 parent/child/task/同批后续工具计数；初轮/续投/Stop 各验未知态停止且不重复送输入；真前台取消与步数耗尽另查执行结果。启动拒绝验已提交且 Close 错、真关闭写者明确拒写、干净落稳三条路。注入钩子只在真实 Close 释放句柄之后报错，不冒称操作系统自然失败。默认空。

新册固定七案，进入三平台 SDK focused 与 ASan 来源登记、原件/JUnit及精确非零计数门。实际前台取消、预算、成功、Close 失败、孩子未知五条路各在断言后打印完成标记，两门须在完整原生日志见到各一枚，不能只凭总数盖过漏跑子例。CLI 轮借用册精确两案，含真 Stop 未知收尾；旧 CLI 派工与寿命册保留。本地只查源码、文档与纯数据，原生交远端 CI。

`554e40ad` 鲜全量 macOS 编译发现三份既有 V3 册还传 bool、取字符串 hash。
本轮将 default smoke、write wiring、verify/doctor tree 三处消费者迁到真实 typed 回执，
同时核成功执行、Committed、Closed、durable 与原子卷终态五键；旧完整验卷和父子边断言保留。
不添 bool 兼容 shim，不改生产与门。SDK focused 通过不能抵全量编译，仍须本头鲜 CI 收齐。
