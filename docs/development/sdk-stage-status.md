# SDK 阶段进度与验收边界

核查日期：2026-10-05。已合基线为功能分支 `756bf02a`；[#325](https://github.com/relic-yuexi/LubanCode/pull/325) 的已验源 `2c376020` 与合入头同树。

目标：新宿主只调用公开 SDK，便能运行完整会话，不必复制内部运行栈。CLI 逐项迁入，原功能保住。现已能嵌入；CLI 主入口和 one-shot 仍用内部装配，尚未全部对齐。

## 八阶段现状

| 阶段 | 已合范围 | 接下来补什么 |
| --- | --- | --- |
| 收口当前批 | Worker #245、Runner #246、SDK #325 已合功能分支；旧子 PR 已收口 | #332 窄修复已推，审查与本源 CI 全过再合 |
| SDK 完整化 | 五件内置工具；显式 Skills；项目 Memory Recall/Save 和片段 CAS；前台深度一子 Agent；主 Action；strict standalone Lua；Package 清单分析 | 其余 CLI 工具、自动 Memory、Skills 管理、Package 挂载、后台与嵌套子任务、公开 Job、CLI 主入口迁移 |
| 四口 SPI | Session 真接 EventSink；召回片段真接 Memory Blob provider；公开 PolicyProvider 原语 | 完整 JournalStore/BlobStore，替换结果与恢复读写，接真实 Session 授权；本阶段不引数据库 |
| 依赖瘦身 | updater、Release 查询、渠道、Gateway 留宿主；Package 只带中立 parser | 可关闭 Lua 的编译画像已在私有候选通过，仍待合入；继续检查依赖闭包 |
| 身份与治理 | 身份值、授权 action、可撤销 PolicyProvider 和订阅合同 | 新托管会话 ownership、执行前重查、恢复归属、查询与审批隔离；旧账迁入另批 |
| 公开服务 | AppServer 内部网页、WebSocket；本地可信 Worker IPC | 正式 Worker 协议、HTTP/SSE、持久 outbox/ACK 和游标；gRPC 按需 |
| 分布式与存储 | 本地 Worker；独立实验 Runner；两端部署合同 | 网络登记、心跳、鉴权、存储与队列适配；池化和 Sandbox 池各守前置门 |
| 扩展与编排 | 可信 C++ 窄中间件；前台子 Agent 与主 Action | 动态库、热卸载、沙箱、工作流与多 Agent 的公开装配 |

上述范围只算已经合入功能分支。接口存在不等于真实执行已接管，私有候选通过也不等于交付。GPU 占用和网络连通交用户处理；同项目可开多场，不能把 Session 当成独占工作目录。

## 已合基线

[#325 原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357534) 和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357428) 均过。六套 SDK 各有 28 场安装移位消费、48 册 focused、414 条原生 CASE；全量 Linux 744、Windows 746、macOS 747；ASan 实跑 147 册，54 册重点来源通过。

859 件材料逐大小与 SHA256 复核，封账摘要为 `e82c9e15e01e0150287bb03ebb1c10f2326a6038a323fd3f5e450b7c98822278`。全量结论取实际日志与保留原件；并未上传每册完整 JUnit/LastTest。LSan 关闭，浏览器和 TSan 按路径跳过。SDK 插桩不能替 Worker/Runner 插桩。

## 当前实施批：#332

[#332](https://github.com/relic-yuexi/LubanCode/pull/332) 源 `0066ac3e` 仍为 Draft，尚未合入。它补[真实 Job 来源绑定](sdk-job-operation-binding-v1.md)与 [V3 日志回执](v3-journal-witness.md)，并同步 main `3b973ffe`。Job 历史只读 PassiveHold；首次未确认追加和首次 checked Close 持值，不靠重复 Close 改口。这批仍属内部前置，尚未开放后台 Job 或完整 JournalStore。

本源[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915212)和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915234)已触发。scoped PowerShell 包装器三枚 cmdlet 改用模块限定名，原管道、格式、错误流和退出码保住；原第六案末补 legacy/scoped 对象表格逐字对照。尚待本源验收。

前源 `52fa7d6d` 的[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651084)整轮失败；[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651072)通过。它留下这些实际结果：

- 三平台全量 Linux 748、Windows 750、macOS 751 与 ASan 149/56 均过。新 Job/V3 两册各六案保实际 argv、非零断言与原始日志。
- 五套 SDK focused 通过；Windows SDK-only 50 册中 49 过、一册失败。六套安装移位消费各 28 项通过。
- 失败册 `sdk.focused.run_command_execution_limits` 六案五过、一案失败。`powershell-exact` 实际登记 `RUN_SERIAL=true`，仍等到 15054ms 超时。同次 shell-entry、wrapper-ready 确认；user-block-entry 未确认，probe 起步和结束记录均缺。原帽仍为 15000ms，未加时限、预热或重跑求绿。
- 监督器五案通过。健康拍夹具只由手动拍驱动，消除背景拍先改状态、稍后入通知时断言抢跑；生产监督器未改。
- 失败原件已封 568 件，摘要 `96ce13da2b5e4d8131638ec2dc9372f7c3a90064ac1f6ea6b2a96462773fec60`。Windows 全量另保完整实际登记、JUnit 和 LastTest；ASan 保 raw File API、149 册实跑与 56 册重点门。

入口观测只收窄待查区间，尚不能断唯一起因。全量较后通过不能盖掉这次失败。修复后验新源，审查和远端 CI 全过才合功能分支。

## 下一笔候选

私有源 `37219c98` 的[整轮 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37245678268) 已通过，仍未合入功能分支。

| 模块 | 合同与已验范围 |
| --- | --- |
| [Package 盘点](sdk-package-inventory.md) | 显式根、有界目录与文件清单；不挂载、不执行、不替宿主授信 |
| Lua ON/OFF | 默认 ON；OFF 真去编译依赖；三平台跨画像恢复，在读取所选 Lua 脚本、创建 VM 和调用模型前拒绝启用的 Lua 来源 |
| [主场 Todo](sdk-todo-write.md) | 每场自持清单，整表替换；恢复保历史、重新起空表；CLI 提醒与展示留宿主 |
| [Agentic RAG 参考例](sdk-agentic-rag-example.md) | 仅公开 SDK 和标准库；宿主注入检索工具，真实模型工具回环；不增核心检索 API 或向量库 |

ON 六套各 54 册 focused、452 条原生 CASE、33 项安装消费；OFF 三套各 52 册、437 条 CASE、30 项消费。三平台跨画像各五条真实命令；九套独立 RAG 示例从四份复制源码单独构建和执行。全量 Linux 756、Windows 758、macOS 759；ASan 实跑 153 册，60 册重点来源通过，另核 18 份附件和实际 PCH。

876 件材料已封，摘要 `d1ec767ac5539b1b0c58fe6081f44db582fa29ac99f91a9bf89c03d109ab0264`。本地组合 `640c0f71` 只另加监督器夹具修复，尚未送它自己那轮 CI；不得借 `37219c98` 绿灯验收。

操作日志退场、ResultPolicy 开场锁与 Managed ownership 另留私有前置；真实 Managed 授权尚未接管。[后台 Job 合同](sdk-background-jobs.md)先定取消、许可和 Close 次序，尚未公开执行。

## 后续装配规矩

每项先写短合同：公开什么，谁持资源，取消怎样传，关场怎样等借用退出，恢复依据哪份事实。只交实际接通的一段，不先铺一排空接口。

工具、模型、存储、事件和策略各留窄 seam。宿主显式配置能力，Session 冻结本场配置；关闭等在途调用真退出，未知回执保未知。未来网络、数据库和插件实现接这些 seam，不能让核心依赖具体 UI 或基础设施。

默认只出 preview。Full 须 Node 许可和每场参数同时开启。子 Agent 默认关闭，宿主显式给预算，许可只管当前子场。

保持一张实施 PR，收完再开下一笔；功能分支到 main 继续走 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)。用户已授权自行收口，仍须逐源审查和远端三平台 CI。源码、Git、纯数据和文档可本地检查；配置、编译、CTest 与原生执行全交远端。
