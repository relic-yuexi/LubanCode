# AgentHealthHookBus 寿命册：先钉同批，再验退场

基 #313 `7e15b8ed`。Linux full 实际五场中一场失败，`hits == 2` 得到 1；首红原件另封。源码有一条可达交错：两次独立 Publish 之间，派发线程先取走仅含 first 的 batch。second 留在队列；停止后只跑完手头 batch，不回头拿新批。这份代码证据说明夹具尚未钉住所称“批内剩余”，不冒称日志已证明唯一时序。

## 改什么

只改 `tests/unit/runtime/test_agent_thread_lifetime.cpp` 第一场。先发 seed，回调进准备门闩；等它确实卡住，再排 first 和 second。两条都入队后放 seed，等 first 进第二道门闩，再按原路析构。此时两条必在同一批，收场仍须派完两条。

原五 CASE、hits == 2、析构后 token 未过期、真实释放后的 token 过期，以及五秒入门/五秒退场预算均保留。生产批次、停止、丢弃、detach 与一秒析构截止不动。不添 sleep 猜调度，不放大等待，不少派一条，也不重跑旧 CI。

## 谁拥有，如何清场

准备门、业务门和 hits 由共享值状态持有；真实回调按值持这份状态，不借栈。失败退场先打开两门，仍由总线自身收线、等待或保共享线程状态。既不能把 raw 栈借用留给 detach，也不能用清场成功顶替原寿命断言。

## 验收

先静核可达交错与两次门闩实际先后，再核第二到第五场逐字保全。文档、纯数据和 diff 在本地查；原生仅推远端。三平台 full 与 ASan 必须实际跑本来源五场，核完整 argv、正数断言、真实通过和 LastTest/JUnit。该夹具不在 SDK focused 原生来源里；不冒称公开 SDK 或 owned Job 新能力。基头绿或别场绿不能替本源验收。

full 收证只读同一次运行。`extract_agent_health_full.py --build-dir build --platform nt|posix` 读取 `agent-health-full-registration.json`、已有 `result-store-full-results.xml` 和 `Testing/Temporary/LastTest.log`，不再起原生进程。登记须唯一指向 `unit.runtime.agent_thread_lifetime`，命令须用完整绝对路径和原 `--source-file=*test_agent_thread_lifetime.cpp`，不准添筛选、跳过或换同名异场程序。实际命令逐项对登记；五场须全过，断言须为正且全过，同名 JUnit 也须唯一通过。成功 JUnit 不能顶替失败原生册。

收证目录为 `test-evidence/agent-health-full`。先存本源登记、所有同名原生段和同名 JUnit，再校验；坏 XML 无法切出本源时另存原字节，并注明未能分源。`context.json` 留阶段、输入哈希和失败缘由；`manifest.json` 列实际留件及哈希。缺件、坏编码、重复或超时均留失败账后非零退出。ASan 复用同份登记与原生校验函数，不另写一套五场解释规则。
