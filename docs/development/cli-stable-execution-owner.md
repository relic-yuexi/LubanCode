# CLI 主场的稳定借用地址

交互命令表会按值保存 Agent 指针。原 optional emplace 在同一槽内重建，
地址不变。共用 owner 也须守住这条约定，不能每次重建换一块堆内存。

HostBorrowed ExecutionOwner 持原位 optional Agent；Root 与 Child 继续持原
unique_ptr Agent 和资源图。两种存放都沿同一 Construct、恢复与退场口。
宿主 owner 本身不能移动或在借用仍活着时替换。

交互控制器首次创建 owner，后续调用 RebuildHostAgent。它先退旧 Agent，
再复制原 profile 给 Construct 消费；成功后仍在同一槽内。const 原 profile
留给子 Agent 派生，旧工具策略不丢。复制或恢复失败，主场槽置空；此时
has_agent 为假，宿主不得解引用旧借用。下一次成功构造仍用原槽。

重建只准在没有活动执行和 turn scope 时做。Root、Child 调重建口明确拒绝。
原命令表不在 handler 执行中销毁或替换。控制器的历史、Soul、后端、工具、
命令与收尾仍走原路；不增加线程或公共 SDK 声明。

原 owner 七案补真实旧借用验收：保存指针与引用，重建后经它们跑模型、工具
和历史；核源 profile 不变、复制失败空槽、后续同址恢复，以及活动 turn 拒绝。
原交互 EOF 案与共用 owner 案交远端 CI。本地不编译、不跑原生。
