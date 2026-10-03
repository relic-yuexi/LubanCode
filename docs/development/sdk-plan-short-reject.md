# SDK 冻结计划：有界短拒重试

基线 `da9f805d820c2c1baccc013248d6fe0dc89c06e9`。本笔只管新场锁内写入子 Agent、MemoryWrite 与 Skills 冻结计划。恢复只读、工具执行、报告、事件账和全局 `AtomicWriteFile` 不改。

本轮远端 Windows 原件已证三处失败：组合构建子 Agent 计划替换回错误码 32；SDK-only MemoryWrite 计划报 `atomic.replace_failed / TransientReject / NotCommitted / Windows32`；子 Agent 分支 `3ed` 的组合构建也报 `sdk.skill.plan_write_failed` 与 Windows32。Skills 新场先核锁内目录归属与旧计划缺件，再冻结同份 bytes，沿同一生产原子写提交，范围收在这条新场写口。这只说明本次原子替换受分享模式拒绝，不指认持句柄者，也不倒推旧 `2f3d8bc7` 唯一起因。原件另册封存，原生断言照留。

真实场锁与计划 owner 仍归现有 opening participant。它在首次写入前照旧核身份、目录、旧计划与绑定。内部共用写入口固定同一逻辑 target、同一冻结 bytes、同一 `ProcessCrashDurability`；每次仍调生产 `AtomicWriteFile`。只在 `failure_kind == TransientReject` 且 `outcome == NotCommitted` 同时成立时再试。首次调用计入最多 51 次，单调时钟守 1 秒截止，每次计划等待至多 20 毫秒；任一门耗尽，立即还最后真实错误。

1 秒限重试调度与等待，不给单次 native fsync/close 承诺硬截止。

平台标作 `Permanent`、临时打开或 close 失败、换名后耐久未确认，以及任何已提交态都不再试。成功回执也须 `CommittedDurable` 才立计划 hash 与系统绑定；不删 target，不回滚已提交文件，不重新规划，不重跑模型或工具。原诊断保底层 code/kind/outcome/message，坏 UTF-8 沿原 replace 渲染。没有公开重试参数或测试故障开关。

重试只包新场三份冻结计划。报告、MemoryRecall 与 Memory CAS 沿各自原合同。SDK 后续 Action 计划另评，不能顺手扩到那处。私有 helper 可由原生夹具注入写调用、单调钟和等候函数，真实生产封装仍只走标准平台写入口；它不进公开头或 ABI。

原 `test_atomic_write.cpp` 十九份静态 CASE 逐字留存，其中三份只在 Windows 编译，原生实跑为 Windows 十九案、POSIX 十六案。另补六案：短拒后真成功；永久拒即停；已提交态不再试；次数耗尽还最后错；截止耗尽不多写；Windows 真句柄不分享删除，先证生产 sharing rejection、释放原句柄再原子提交，POSIX 同册验真实耐久提交。门据实际平台核 Windows 二十五案、POSIX 二十二案与六枚路径标记；真 Windows 路径只在关键断言通过后另留唯一标记。SDK focused 仍二十五份来源；ASan 原三十份 required 来源补入现有 `unit.platform.atomic_write`，实为三十一份，核该来源二十二案、JUnit 和 LastTest。没有新增测试 CPP，六套安装消费者仍二十场，子 Agent 原十二场与四会话隔离照跑。

先提交合同，再交代码二读、推远端三平台 CI。本地只读源码、纯数据、AST、文档与 diff；不配置、不编译、不跑原生。新头失败仍留原件，成功分项不代全量验收。
