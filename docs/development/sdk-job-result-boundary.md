# SDK 普通 Operation 与 owned Job 结果边界

基线为私有 J2b `7e15b8ede0be6af06cbd8953115bab23fce5a5f3`。本笔只收普通 Operation 结果索引，不开放 SDK 后台 Job，不另造模型 turn、操作终态或业务回执。

## 公开什么

不添公开 API。现有 `ListToolResults`、`ReadToolResult` 继续查本场普通 Operation。其 turn 内出现真实 owned Job 业务 Action 时，普通索引不收该 Action 的 persisted 或 selected；父 inline Action 与父接单句柄仍照旧收录。

判别复用真实 verified V3 账与共用 `FoldToolActions`：prepared-only 登记和版本化 `tool.job.adopted` 标出的业务 Action 不欠原 provider 第二回复。只取这份实账分类，不按工具名、provider call-id、attempt 数字或调用方提供的 owned 表猜测。没有 owned 标记的 Legacy Job 与普通工具仍走原索引。

普通 selected 的本场来源、同 Action/turn、终态、attempt、元数据与 artifact 校验不放宽；原跨场 source 拒绝门保持。被排除的业务行仍在原 V3 账与结果仓，本笔不删、不改、不迁原件，也不把它冒充另一笔公开 SDK Operation。

## 谁拥有，怎么关场

索引仍只接 verified、静止的 ledger，返回 owned 值，不保 writer、协调器、worker 或原结果正文借用。父 Operation 的完成缓存保持原规则；迟归业务结果不能混入恢复时重建的父索引。新的 typed Job 索引、注册映射、后台审批、Post 与公共 Close 合同另笔。

## 远端验收

保留现有 result projection 六场、原断言与预算。在原册追加真实中立组件路径：Writer 写 assistant 声明与采用，协调器真写 prepared Pending/Registered，真 Adopt，再写父接单 terminal/persisted/selected/tool-message/admission；真实 `run_command` 完成业务 attempt 1、terminal、raw、Post 与 Observed。业务派发前缓存父索引，迟归后同 turn 重建，再核两份只含父结果；业务五键和真实正文另从原账与结果仓查验。Legacy 无 owned 标记的实际结果继续入索引。

这条组件验收走生产 SDK 索引函数，不能冒称公开 SDK 已接后台装配。原册已有真实 SDK Session、完成缓存、Close、resume 与投影断言全部保留。六场来源与 focused/ASan 门不增减；若需额外测试接缝或门接线，先报根代理分工。本地只核静态、文档、纯数据与 diff；编译、CTest、命令进程均只在远端 CI 跑。
