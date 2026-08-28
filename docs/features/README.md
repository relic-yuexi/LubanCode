# 功能目录

[文档首页](../README.md) · [参考手册](../reference/README.md) · [架构说明](../architecture/README.md)

这里按用户眼中的功能分门。精确字段去参考手册；源码状态机去架构区。

| 模块 | 内容 |
| --- | --- |
| [会话](sessions/README.md) | history、session、恢复、导出、token 与缓存 |
| [上下文压缩](context/compaction.md) | 结构压缩、语义摘要、分层归并与回放 |
| [终端界面](terminal/README.md) | composer、排队、转录、diff 与子代理面板 |
| [app-server](app-server/README.md) | 无界面后台协议、SSH 远程项目、审批与 diff 事件 |
| [项目指令](project-instructions/README.md) | `/init`、AGENTS 层级、覆盖与大小边界 |
| [Provider 目录](providers/catalog.md) | 厂家预设、缓存、Schema 与在线更新 |
| [扩展](extensions/README.md) | Skills、Plugins、MCP、LSP 与 Hooks |
| [Hooks](extensions/hooks.md) | 事件、协议、信任、决策归并与后台语义 |
| [PTC](tools/ptc.md) | 程序化工具调用、runner、预算与回落 |
| [Workflow](workflows/README.md) | 安装、运行、恢复与工作流设计原语 |
| [自进化闭环](evolution/README.md) | Package 候选、评测、批准、晋升与回滚的闭环契约 |

项目记忆横跨用户行为与内部数据流。使用命令见
[会话与上下文](sessions/README.md)，完整设计见
[记忆架构](../architecture/memory/design.md)。
