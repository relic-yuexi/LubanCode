# 子 Agent 终态失败诊断

`68e1fcf2` 远端 CI 的两条 Windows 安装消费者均失败：`subagents`
只报 `child operation failed`，`subagent_seed` 只报 `restart seed incomplete`。
恢复场随 seed 依赖跳过。原日志没给 Operation.error 或子场退账细节，
临时主账也未上传，不能凭这两句断定根因。

这笔先补失败原件，不改生产代码。成功场原来检查 Succeeded 与
result_persisted，seed 原来检查 result_persisted；两处在原条件失败时，
调用已有 `DiagnoseChildFailure`，随后照原条件抛错。

诊断只读本场实际 Operation、模型与工具调用计数、已缓存子场报告和
真实 live receipt。没有报告便留真实错误，不拿默认值充成功。
诊断仍走安装后的 SDK/STL 宿主，不读内部 Writer 或另造主账。

原 CASE、步骤、时长、审批、回环、关场与重启验收照旧。失败才多写
诊断；它不能把失败改成通过，也不能替下次 CI 给结论。后续修正须
依据这份源在远端留下的错误，另作源码审查。

本地只核源码和文档。配置、编译、消费与原生测试全部交远端 CI。
