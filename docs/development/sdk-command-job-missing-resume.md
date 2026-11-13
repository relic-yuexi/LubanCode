# Command Jobs 缺会话预检补正

远端 `a9d3f16e` 的 macOS 安装消费者已跑完四场隔离与 Shutdown。随后独立 SmallToolCaptureLifetime 恢复不存在的会话，收到 sdk.job.plan_invalid，违背原 sdk.session.open_failed 约定。原始失败证据有 69 件，封存报告 SHA-256 为 1201925068fb7e8fa3949204b2ffd94a466d5c03cc51125529463630f51ddf89；本笔已逐件核证。

只改 SessionCommandJobPlan::Prepare 的缺失分类。找不到 workspace，或 session 路径的 symlink_status 明确返回 no_such_file_or_directory / not_found，沿旧 sdk.session.open_failed 拒绝。Session 目录已存在但链接、非目录、不可读，或计划、V3、绑定坏了，仍走原 Job 严格校验。省略 Jobs 不等于略过既有绑定检查；foreign-owned system 仍先报 Job 原码。

不改预检顺序，不改 consumer 原错误码断言，不创建替代 Session。读计划、冻结预算、审批、模型和工具执行均不添新路。

原 private actual-public-source CASE 补真实 Runtime 验收：新 workspace 尚未落账、已有 workspace 缺 session、同名非目录、已有会话坏计划。每次核精确错误码、零模型与工具、数据树和冻结材料字节不动。真实新场 Close 后再恢复，确认旧 owner 放锁、后端退出。Jobs 关闭与显式正预算各验一遍；原七案名册和原命令预算不动。

本地只核源码、纯数据门与文档。编译和原生执行交远端 CI，本笔不推送、不建 PR。
