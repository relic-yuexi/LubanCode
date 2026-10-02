# SDK 存储、事件与策略 SPI 合同

状态：接口前合同，不含可注册 SPI、默认适配器新实现或运行时接线。基线为 SDK 写入源 `220c25f5`，包含整合源 `3244002e` 和项目提交门 `bcd70a5b`。整合批已独立收远端证据，写入与提交门仍在验；本合同不替任何原生验收。按用户八步，SDK 分模块继续，四口只出本地参考实现，不引数据库。

目标：JournalStore、BlobStore、EventSink、PolicyProvider 四口，本地文件账作默认实现。公开头只用标准库和已安装 LubanCore 头，不露内部 JSON 库、Writer 或平台句柄。

## 不能只换 append

当前 `SessionManager` 管独占场锁、manifest、主账、子账和 lifecycle。SDK 还写输入幂等材料、结果、扩展/Skills/Memory 计划与报告；恢复会逐项核实际 V3 采用事实。只替 `JournalWriter::AppendLine`，其余材料仍直接摸本地目录，不能称宿主已能换存储。

JournalStore 须交一束本场存储能力：开场/恢复时独占 owner、读取稳定前缀、追加原字节，以及 owned metadata/receipt 的 create-new 与 compare-and-publish。逻辑寻址只认 workspace/session/stream/key，物理路径留默认适配器内；路径不是授权。manifest/hostBindings 和恢复校验仍归领域核，Provider 不替它猜身份或修坏账。

首笔实现不把本束硬塞成跨文件事务。V3 三步 system/context 采用、正式结果/operation.final 仍按现规则提交；断在中途照实重放或留未知态。

## JournalStore

- Runtime 持 factory；每场持独占 SessionStore/lease。主执行结束、后台实际借用退出、Writer 关柄后才释 lease。单场仍只有一位执行 owner。
- 底层追加只收核心已生成的 canonical 原字节、期望尾游标和请求键；seq、消息 ID、hash chain 与 V3 状态机仍由共用 Writer 发号和验证。每次 append 和 owned metadata 发布都须在同一原子门核 owner generation/fence 与预期版本。失去 owner 后，旧借用不能续写；持有 factory 不等于持有场租约。默认文件适配器须给出等效独占与失主拒写证据，不能声称现有锁文件天然支持跨机 fence。
- 读取返回有界页和稳定前缀游标。游标带 stream、序号和尾摘要；同一恢复视图不得悄悄混进更新后的 manifest/hostBindings。不信 Provider 自报的 verified 标记。
- 回执至少分 `not_committed`、`committed`、`indeterminate`，列实际耐久档。同请求键须核原字节、预期游标与归属；同键异字节拒绝。未知 append 冻结内存链，只有恢复核过同请求事实与真实尾游标才准前移，不能凭 Provider 一句 committed 推进 Writer。只有能证明未提交才允许重试；未知态停本场执行，不盲目重放。
- 默认 File adapter 沿现场锁、JournalWriter 和平台原子替换。提出 Buffered/ProcessCrash/PowerLoss 请求后，做不到须明确拒绝，不能暗降。文件与目录刷盘、目录新建和已有路径的保证分别说明，不冒称断电多文件事务。

## BlobStore

- 引用保内容 SHA、完整字节数和媒体信息，逻辑归属带本场身份；返回存储引用，不要求外部实现返回本机路径。
- Store 实际核字节、请求耐久；同 hash 复用前仍核已有实体。坏实体不覆盖、不当命中。Read 有请求字节帽，核完整 bytes/hash 后才给领域核。
- blob 先落稳，主账后指它；主账失败可留孤立 blob，不能倒称 blob 回滚。清理与跨场去重后置。
- CAS 默认适配器仍用 `artifacts/sha256`。Named artifact 另持不可变映射：本场逻辑 artifact ID → 内容 SHA、完整 bytes、媒体信息。相同逻辑名不同内容拒绝；只在有界读取并核全实体后复用。先 blob 落稳，再 create-new 发布映射，最后写 `tool.result.persisted`。任何未知阶段都不冒充命中。
- 结果 ledger 和 preview 清单保留现有逻辑 `artifacts/<resultId>...` 引用；适配器负责解析，不让临时实体路径进模型。`ResultStore`、SDK reader、Memory 完整 context/片段、图片、环境快照与恢复读面须一起迁。只有 recorder 的 Store 接口换了，不能勾 Blob 口完成。

## EventSink

- 复用 SDK 当前事件值与内部序列合同，单场次序不重排。当前 `EventStream` 已有有界 pull、overflow 与 Close；先以这条路作参考，不再自造一条无限 callback 队列。宿主持 sink，Session 持有明确租约，不借可能先亡的 UI 对象。能力租约和持久 fact 引用分别表述。
- 临时 delta/观察事件与持久事实分开：普通 sink 收到事件，不证明 Journal 已提交；sink 错误不改真实执行结果。持久事件先账后通知。
- 慢 sink 不无限卡执行 owner，也不无界攒内存。先交有界本地参考实现和明确满队列行为；公开服务的 outbox/ACK/游标另按第六步交付，不把普通 callback 冒充可靠上行。
- Control 默认 Preview 与双层 Full 仍由公开投影口执法。存储、sink 和 transport 不靠拦截器自行抹掉这条门。

## PolicyProvider

- 沿身份原语候选 #261 的 ExecutionContext/动作合同核接点，先审现实现再决定迁哪个公开头；不另造第二份 actor/tenant/project/session。
- Provider 给决策值和版本，不替 CLI 审批 UI。确定拒绝在副作用前生效；失败默认拒绝。Session 固定归属，实际执行时核有效授权和撤权，而非只看建场快照。
- 本地可信 SDK、未来 Managed factory 与旧账迁入分合同。单靠路径或 ApprovalMode 不称租户 ACL；未接 Managed 的原语不能勾第五步完成。

## 拆笔与验收

先审这四口的共有身份、耐久与未知态；再逐口出接口、默认 File/reference adapter 和一致性册。每口只有代码和真实接线都齐，才准宿主注册实现。默认配置仍跑现共用 SessionExecution，不能另复制执行栈。

每口至少验证：两个宿主争同场 owner、同项目两场独立、同请求重复与冲突、取消/Close 与借用、追加或换名之后失败、坏/短/超帽读取、默认文件账同 ID 恢复。再用 fake provider 跑公开安装消费、完整工具结果、Skills/Memory 计划/报告和模型实际输入；真实目录直读漏口必须显式暴露。全程三平台远端 CI，本地只写源码、查文档和跑纯数据门。

第一口可以先抽 Blob 能力，但不能借一个 `BlobStore` 虚表宣称数据库可插、整场存储已换完。Journal 的 owned metadata/read snapshot 合同先定，再动持久入口。

## 本轮真实读写面

迁移清单按归属收口。会话材料须经过同一场租约与稳定恢复视图；项目 Memory 另受项目 owner/fence 和本场实际请求归属约束。同项目双场仍可并存。项目工作树读写由执行 world 的工具负责。只保留 `data_root` 当默认 File adapter 配置，不能要求任意 Provider 交本机目录。

| 材料 | 当前代码入口 | SPI 归属 |
| --- | --- | --- |
| 场独占 owner、manifest、主/子 JSONL、生命周期账 | `src/trajectory/session_manager.cpp`、`src/runtime/trajectory_session.cpp` | Journal 场束、lease、稳定视图 |
| 输入幂等、排队、`operations.jsonl`、`sdk-results` | `src/runtime/session_service.cpp`、`src/sdk/core.cpp` | Journal owned metadata 和事实链 |
| 扩展、Skills、结果投影计划 | `src/sdk/core.cpp`、`src/sdk/skills.cpp`、`src/sdk/results.cpp` | Journal create-new 计划；冻结 binding 仍归领域核 |
| Recall/Save 计划和逐 operation 报告 | `src/sdk/memory.cpp`、`src/sdk/memory_write.cpp` | Journal owned metadata；两请求 SHA 与采用校准不下放 |
| CAS context、Memory 片段、图片和环境材料 | `src/trajectory/blob_store.cpp`、recorder/Memory 桥 | Blob bounded write/read；先 blob 后引用 |
| Named tool metadata 和各输出通道 | `src/trajectory/v3/result_store.cpp`、`src/sdk/results.cpp` | Blob 与不可变 named 映射；保当前逻辑引用 |
| 正式项目 Memory topic、catalog/index、提交 intent/result/snapshot | `src/memory/project_commit.cpp` | 项目存储适配口，沿原项目锁与分阶段提交；不偷当 Session journal 事务 |

项目提交门跨场共享。接 Provider 时须另外证明同项目两场争锁、request key 与不可变 snapshot 归属；只迁会话目录不能声称正式 Memory 已换存储。旧本地账的迁入与回滚仍另批。

## 分批交付门

1. 先收本合同，给四口共有逻辑身份、请求键、耐久和未知回执定形；这笔文档不发布虚表 ABI。
2. Blob 先迁 CAS 与 named 映射，默认 File 与 fake 实现同跑。若先迁 CAS，须明列 named result/owned metadata 仍未迁，禁止整场替换注册。
3. Journal 先接独占 lease，贯穿 owned metadata 发布与稳定恢复视图，再迁 Writer append。lease 未接的过渡批只能内部迁适配器，不能开放整场替换注册。每批只交真实接点，不复制另一套 SessionExecution。
4. EventSink 沿有界事件流迁；PolicyProvider 沿身份原语迁，先收 fail-closed 与实际执行撤权册。身份原语接上不代表 Managed 场工厂已交付。

策略验收还须覆盖等待项目锁后、首笔 topic 副作用前这道窗。授权已撤就拒写；开场快照不能当后续有效票。

安装消费须只用公开 SDK，接 fake Provider，跑 Submit→完整结果、Skills、Recall、Save 计划/报告→Close→同 ID 恢复。除显式项目工具 FS 与默认 File adapter，producer/session 材料不得绕口直接读盘。CI 要从实际依赖图和数据路径举证，不能只搜有无类名。默认文件账继续跑三平台 CLI/AppServer/Worker parity 与完整原生册；新增 seam 验收也须非零实跑。
