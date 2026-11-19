# CLI 交互入口共用执行 owner

基线 `603a07e4`。单次提问已接共用 ExecutionOwner；这笔接交互主场。
仍由原 SessionStack 持 Backend 与 ToolRuntime，不换 provider wire 或工具栈。

控制器只持一份 HostBorrowed ExecutionOwner。每次取 Agent 都从活 owner
取借用指针，不另存会随重建悬空的指针。成员仍放在原 Agent 槽位，沿原
声明次序退场；宿主资源须活得更久。构造与早退仍走原控制器。

RebuildLoop 先保原历史，再拼原 profile。到原 emplace 那一处，首次创建 owner，
后续在同一只宿主槽内退旧 Agent、重建新 Agent。原 profile 留给原子 Agent 派生代码，
不能让 Construct 清捕获后派生出缺工具策略的孩子。复制失败仍留空主场槽位，
同原 optional emplace 失败；不保旧场、不偷偷重试。

新 owner 沿已有 Construct 创建 Agent。首次另分配 owner 与私有稳定槽，后续
重建不换槽，但多复制一份 profile；本笔不承诺同样的分配成本。

Soul、请求配置、保历史重建、恢复、收件点、UI、RunTurn、审批、后台工具
和控制器收尾沿原路走。命令表保存的借用在成功重建后仍指原地址；空槽期间禁用。
本笔不增加线程、取消协议或公共 SDK 声明，也未完成 CLI 的公开 SDK 装配。

远端全量须跑原交互真构造、EOF、真析构案；三平台 SDK 和 ASan 沿共用 owner
原验收跑借用与退场。保原名册、断言、预算和 CI 接线。命令重建与 provider
wire 的完整端到端证据仍待后续补齐。本地只读写源码和文档，不编译或跑原生。
