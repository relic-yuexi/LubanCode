# 终态面板夹具：重复渲染不借活钟

基线 `22bba238`。本笔只收 CLI 原面板夹具，不接新 SDK 能力。

渠道分支 `9a252c6a` 的 Linux 原生全量中，`unit.app.agent_task_transcript` 十四案有一案失败。`test_agent_task_transcript.cpp:231` 比较两次紧凑画面；原件只差运行中耗时 `3224.1s` 与 `3224.2s`。原日志 SHA256 `4bf872c74ba9704810c3f710cefebd98d56554b1dd9d17a0a351de41f7e84db4`，另册保留。

这场夹具不启动 Agent，也没有活调用；它直写两枚已完成工具结果，却沿默认 Running 快照绘制整场元数据。运行中画面取当前单调钟，跨 0.1 秒本该变化，不能用全串相等验不改账。

原端到端场改用显式 Done 快照，起止点取固定值，仍由原 ledger 注册与 presenter 渲染。原十四案和全部断言照留，尤其 `again == compact`，不删时间文本，不改 renderer、时钟、缓存或真实运行态。其它原流尾 Running 卡验收照跑，不添等待、重试或公开故障开关。

该源点亮远端三平台全量 CI，核原 `unit.app.agent_task_transcript` 来源实际运行。合并前复核原断言、原 case 数与新场快照；文档与静态检查不代原生。本地不 configure、编译或 CTest。
