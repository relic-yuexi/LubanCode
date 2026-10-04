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

两条私有支线已收成 Draft [#332](https://github.com/relic-yuexi/LubanCode/pull/332)，源 `fb1bef4b`。源中已同步 main `3b973ffe`；共享功能分支尚未收入。新增来源进 SDK-only、组合、三平台全量与 ASan；验实际 argv、非零断言、各六条路径、JUnit 与完整 LastTest。旧源 Job 夹具漏直接声明头，四条 Linux/macOS 腿在编译处失败，余腿取消。本源补一行包含，保六案正文；[新原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37225193222) 正在重验，不能借 #325 或旧轮结果勾完本批。

下一笔 [Package 显式根有界盘点](sdk-package-inventory.md) 在私有分支接线。只盘宿主明给的单根，五道帽封顶；返回持值文件账、目录、摘要、七类入口形状和根清单分析。源树需静止，遇链接或可见变动整次失败。它不解析组件正文、不解跨包引用，也不挂载或执行。新八案与安装迁位消费者另算来源；源码审查、纯数据门或旧清单测试都不能代三平台原生验收。

当前只留 #234 总 PR 与 #332 实施 PR。Package 先在私有支线开发，不添平行实施 PR。功能分支到 main 继续留 Draft；最终组合须验 CI，再取得合入确认。
