# Memory 批量提交失败诊断补约

2026-10-10。#344 源 4497cfc7 的 Windows 全 CLI 803 项中，unit.memory.project_memory 一项失败。批量六笔有一笔报 memory.commit.indeterminate、atomic.replace_failed、Windows 错误码 5。标题七案通过。失败原件已归档，不能把这轮算成三平台交付。

公开什么：不添公开 API，不改 Memory 写入、回执、重试或准入语义。只给原有 native 批量测试补失败材料，原断言照留，47 案不减，不将未确认提交改记成功。

谁拥有：测试拿本场 ResolveProjectIdentity 返回的 workspace_dir。仅从本次完成回执取 operation_id；先限 256 字节，只准 ASCII 字母、数字、横线、下划线，再拼本场 lifecycle/result.json。读口沿已有 128 KiB regular-file 上限，输出沿原 32 笔和每行 16 KiB 上限。只取状态、阶段和结果字段，不打印记忆正文；raw_not_verified 仍保留，不把调试观察当持久提交凭据。

怎样关场：先领取原完成回执，再读已落地结果。读错、无文件、坏 JSON 都只补观察，不能替原断言放行。成功笔不添 I/O。没有新线程、owner、后台 job，也不补自动重试。

怎么验：源基线、失败日志和三平台全量名单先归档。合同先提交，再接这份测试诊断。新源须重跑远端 Windows/Linux/macOS 全量、SDK 与安装消费；同一真实测试来源和断言保留。若失败重现，以原 result.json 阶段定位；若全绿，仍只证明这次运行通过，不能宣称已证明或修掉并发读句柄竞争。禁止本地 CI、构建、原生及 HTTP 验收。

后续边界：Status 调 LoadCatalog，其 ifstream 读口未带 FILE_SHARE_DELETE，后台原子替换 catalog 可能撞读句柄。现有失败回执没有精确目标和打开句柄证据，这只算审查线索。要改生产读口，另写共享快照读合同，用真实句柄、真实替换和关场证据验；不得靠放宽断言或盲目重跑掩掉。
