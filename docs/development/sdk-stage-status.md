# SDK 阶段进度与验收边界

核查日期：2026-10-05。已合基线：功能分支 `756bf02a`，通过 [#325](https://github.com/relic-yuexi/LubanCode/pull/325) 收入源 `2c376020`；两头代码树相同。

目标仍是：新宿主只调用公开 SDK，便能运行完整会话，不必复制内部运行栈；CLI 逐项迁入，原功能不丢。首批已能嵌入，尚未达到 CLI 全部能力对齐。CLI 交互与 one-shot 仍走内部装配，共用中立实现不等于共用公开入口。

## 已交范围与未完事项

| 阶段 | 已交范围 | 后续工作 |
| --- | --- | --- |
| SDK 完整化 | 五件内置工具；显式 Skills；项目 Memory Recall/Save 与片段 CAS；前台深度一子 Agent；主 Action；strict standalone Lua；Package 根清单分析 | 其余 CLI 工具、用户层与自动 Memory、Skills 管理、后台与嵌套子任务、完整 Package 挂载、公开 Job，以及 CLI 主入口迁移 |
| 四口 SPI | Session 真接 EventSink；召回片段真接 Memory Blob provider；公开 PolicyProvider 原语 | 完整 JournalStore/BlobStore、结果与恢复路径替换、真实 Session 授权接线。此阶段不引数据库 |
| 依赖瘦身 | updater、Release 查询、渠道与 Gateway 留宿主；Package 仅两份中立 parser | Lua 仍强编译依赖，缺关闭 Lua 的最小构建画像；运行时默认关闭不算编译闭包已拆 |
| 身份与治理 | 身份、作用域、撤权与订阅原语 | ExecutionContext 接真实 Runtime/Session，模型、工具、查询与 Memory 提交前执行策略；旧账迁入另批 |
| 公开服务 | SDK 暂态 bounded pull；本地 Worker 私有 stdio；内部 AppServer | 公开 HTTP/SSE、持久 outbox、ACK、游标与断线恢复；gRPC 按需后接 |
| 分布式与存储适配 | 本地 Worker 只调用公开 SDK；独立 Runner | Node/Control 网络登记、心跳、鉴权与两端持久收件链，再接数据库、对象存储和队列 |
| 扩展与编排 | 建场冻结 C++ 扩展；五个公开挂点；关闭收尽实例 | 动态库、运行中替换与卸载、沙箱、Lua Hook 装配，以及公开工作流与多 Agent 编排 |

后台 Job 尚未公开。`run_command` 既删除后台 schema，也拒绝实际后台请求。内部准入、绑定与中间件回执只算前置，不能当作公开装配验收。

## 已合基线证据

[#325 原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357534) 与 [文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357428) 已通过。六套 SDK（SDK-only/组合 × 三平台）各有 28 场安装移位消费、48 册 focused、414 条真实原生用例。全量 Linux 744、Windows 746、macOS 747 全过；ASan 实跑 147 册，54 册必需来源均核真实 argv 与非零断言。

859 件材料逐大小与 SHA256 复核。终封证明摘要为 `e82c9e15e01e0150287bb03ebb1c10f2326a6038a323fd3f5e450b7c98822278`。完整全量各源 JUnit/LastTest 并非全部上传；全量结论取实际日志与保留子集。浏览器与 TSan 按路径跳过；LSan 关闭。共享 SDK 的 ASan 不能替 Worker/Runner 插桩证据。

这些证据只说明已合基线。新模块与最终 main 组合各跑自己的 CI，不借旧支线绿。本地只查源码、Git、Python 数据门与文档，不配置、编译或执行原生。

## 当前实施批

先补 [Job 实际绑定](sdk-job-operation-binding-v1.md) 与 [V3 Journal 回执](v3-journal-witness.md)。前者核真实 Service、主 Operation/turn 锚与已 adopted Job，历史只读 PassiveHold；后者保实际追加凭证和首次 Close 结果。两项仍属内部前置，不开放后台执行或完整 JournalStore。

两条私有支线收成同一批，只开一张实施 PR。新增来源须进 SDK-only、组合、三平台全量与 ASan；验实际 argv、非零断言、各六条路径、JUnit 与完整 LastTest。源未通过前不勾已验收。功能分支到 main 继续走 Draft #234；当前与 main 有冲突，最终组合须另解冲突、验 CI、取得合入确认。

此前远端源 `ec3d19e2`，源中已同步 main `3b973ffe`；共享功能分支尚未收入。[本源原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37228276882) 六套 SDK 各过 50 册 focused、426 条原生用例与 28 项迁位消费；ASan 实跑 149 册，56 必需来源过。新增两册各六案在六套 SDK 与 ASan 均过。

整源仍未验收。全量 Linux 748、macOS 751 过；Windows 实跑 750 册，749 过、一册失败。失败来源为 `sdk.focused.run_command_execution_limits`，原六案四过两败：当场 `powershell-exact` 等满 15000ms，`powershell-host-timeout` 等满 8000ms，都返回 `process.timeout`，未读到 started/done。CreateProcess、AssignJob、Resume 均成功；当场 ToolResult、进程 POD 与 Job accounting 原件保留。更后同源通过不能盖掉这笔失败，也尚不能断唯一起因。早先编译失败、取消轮另封，不借旧绿收账。

后续源 `be378d08` 已补[同次 shell、包装与用户块入口观测](windows-command-entry-observation.md)，内部默认关闭；原时限、输出帽、六案和硬断言不改。全量 CI 另留 SDK 与 CLI 两份实际注册、JUnit 子集及原始 LastTest 段，失败也保存，不能拿较早的 focused 命令代替失败全量命令。新头须重验，不沿用 `ec3d19e2` 绿项。

`be378d08` 本轮 ASan 已通过。149 册实际 JUnit/LastTest 均逐册复核：1742 条原生 CASE、65803 条断言，每册只有一份非零通过汇总。56 册另有上传的实际登记；其余 93 册尚无上传 show-only 登记，不能把执行原件说成完整登记原件。Windows 全量仍待本轮核验，整源尚未收口。下面私有组合另算源码与 CI，不能借这笔结果勾成通过。

## 私有候选，尚未合入

以下候选只排开发与验收次序，不替已合基线添能力。维持一张实施 PR；当前 #332 收口后，再按模块交下一笔。

| 候选 | 交付范围 | 当前验收边界 |
| --- | --- | --- |
| [Package 盘点](https://github.com/relic-yuexi/LubanCode/blob/42eca2c3bb894a289aab6305961158e397235904/docs/development/sdk-package-inventory.md)与 Lua ON/OFF，源 `42eca2c3` | Package 显式根盘点；SDK 可关闭 Lua 编译依赖；两份移位安装包验跨画像恢复 | [远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37231298587) 已出现 Windows 全量命令限额册失败，整源未验收。ON 六套、OFF 三套 SDK 及三平台跨画像恢复原件已过。macOS 第四枚坏 UTF-8 文件名由原生文件系统拒造，只证明原生拒绝；其余三枚由实际 SDK 拒绝 |
| 操作日志退场，源 `473c609a` | 首追加、首次 checked Close 与未知结果持值；关场先退所有 writer owner，再核关闭凭证 | [远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37228560157) 整轮失败；Windows 全量命令限额册的 PowerShell exact 同形起步超时，首错原件另封。只迁操作日志，不称完整 JournalStore 已能替换 |
| ResultPolicy 开场锁试点，源 `5ae20f22` | 新场在实际开场锁内发布；恢复先读同份捕获字节；持有原生耐久凭证 | [远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37232350552) 在跑；真缺文件的旧场仍走兼容口。未知耐久结果不得靠读回文件改成成功 |
| 主场 `todo_write`，源 `f2386e75` | 显式准入，每场自持清单；整表替换；恢复保历史、重新起空表 | 源码与 ON/OFF 验收门已封，尚未送本源原生 CI；CLI 展示、提醒、压缩与子场清单仍归宿主 |
| Agentic RAG 参考例 | 宿主经公开 Tool 注入检索器，实际模型工具回环取证、补查、引用来源 | 按架构单近期目标开发；不增核心检索 API、向量库或依赖。尚无整源原生验收 |

下一批私有组合已收 Package 盘点、Lua ON/OFF、Todo 与 RAG 参考例，并带上 #332 同次命令观测及全量原件门。公开 SDK 与消费验收清单为 Lua ON 54 册 / 33 项、OFF 52 册 / 30 项；ASan 原两条选择器实际执行 153 册，其中 60 册另过专门门。私有 ASan 编译画像按本源清单求闭包，保 18 份支持、私有实现与消费附件。这些是待送 CI 的组合要求，尚无本组合原生通过证据，也未合进共享功能分支。
