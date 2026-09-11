# v3 旧设计清理清单

[文档首页](../README.md) · [当前 v3 实现](../architecture/session-v3.md) · [字段合同](../architecture/trajectory-v3-schema.md) · [文档规范](documentation.md)

核查日期：2026-09-11。基线为当前工作区源码（版本入口 `src/app/version.hpp`）。本轮查代码和文档、标记后续清理项，不删除实现或用户存档。下面都是静态检查结果，不冒充本轮运行验收。

`V3-LEGACY` 标记沿用旧合同的代码或文档；`V3-GAP` 标记新版尚未接全之处。条目各有编号，清理时连同引用、测试与文档一起销项。不是所有 v2 都该删：插件 manifest v2、原生 ABI v2、JSON-RPC 2.0 与会话格式无关。

## 待清理与待接线

| 编号 | 位置与证据 | 状态、替代方向与销项条件 |
| --- | --- | --- |
| V3-LEGACY-01 | `src/trajectory/v3/session_switch.cpp::NewSessionV3WriteEnabled`；`session_manager.cpp`、`directory.cpp` 的 `main.jsonl` 分支 | 旧读写仍可达，显式 `0` 还会新建 v2。后续退役写口、格式开关与旧专属测试；旧档读取另定边界，不把它当死代码直接删 |
| V3-LEGACY-02 | `src/trajectory/event.hpp`、`recorder.*`、`replay.*`；`src/runtime/trajectory_session.*` 的 v2 桥 | `EventEnvelope/EventScope/RecordRequest` 仍有消费者。逐个改到 v3 消息/事件与投影后再收旧接口；canonical JSON、哈希与耐久基础设施仍共用，不整目录删除 |
| V3-LEGACY-03 | `src/app/commands/session_commands.cpp::HandleCompactCommand/TryRunCompact` 的 v2 分支；`src/agent/compact.cpp::CompactTurnPartitioned` | 旧四分区双账、archive/kept_indices 与 compact_v2 语义已非默认 v3 合同。保留旧场调用证据，待旧写口退役后清理；新版以 RunV3Compact、compact.applied、上下文链为准 |
| V3-LEGACY-04 | `src/agent/loop.cpp` 的 `tool_result_message.role = api::Role::User`，中断补配对也仍用 User | 四角色枚举/adapter 已落地，生产内部容器尚未收敛。替成独立 Tool 时须联改 turn 切分、配对、回调、恢复和 wire 映射；不能只换枚举 |
| V3-LEGACY-05 | `src/hooks/dispatcher.*`、`loader.*`、`protocol.*`、`outbox.*`；`docs/features/extensions/hooks.md` 的 legacy adapter | command hooks 与 Lua 中间件并存。按挂点迁移配置、权限、执行顺序、效果采用和恢复；Post/outbox 尚有调用，不能先删旧分派器 |
| V3-GAP-01 | `src/accounting/session_usage_reader.cpp::ListSessionStreams/ParseStream` 只列 main.jsonl、平铺子账，按旧信封解析 | `/usage` 离线读口未消费 v3 主账/递归子账。接 assistant usage owner 和来源去重后，才可宣称跨会话统计完整；不能把空样本当零消耗 |
| V3-GAP-02 | `src/telemetry/projector.cpp::Fold` 吃 EventEnvelope；`service.cpp::DiscoverStreams` 一带仍查 main.jsonl | OTLP 投影未迁 v3。先换流发现、上下文/请求/usage 取数与游标，再验重启补投、不重复累计；Collector 配置本身不证明数据链通 |
| V3-GAP-03 | `src/app/memory_ledger_bridge.cpp::RecordRecallInjection` 取 `ledger_.main()`，空值报 recall_snapshot_failed | v3 主场须接正式消息与上下文采用关系。新版 MemoryStore 版本、CAS、忘记屏障和读写权限拆轴见总设计 §4.71；现有 memory 模块不能算这份合同已完成 |
| V3-GAP-04 | `src/app_server/server.cpp` 的只读 thread/read、thread/resume；`session_main_path` 仍拼 main.jsonl | SessionService 入口已统一，协议仍是过渡读面。清理硬编码路径并接 session 来源游标、真正恢复执行与 2.0 合同；保留“只读预览”说明直到行为改变 |
| V3-GAP-05 | `src/workflow/journal.cpp`、`runtime.cpp`、`host_executors.cpp`；关联 Workflow TODO | 编排账与节点执行不能靠换事件名完成迁移。独立节点 session、nodeExecutionId/attempt、output.commit 与失败恢复落地后再清旧桥 |
| V3-GAP-06 | `src/runtime/trajectory_session.cpp` 的 v3 早退分支；schema 第十节 | 标题、审批档、环境、verification 等事实未全部落 v3。逐域定合同、写入、恢复、显示；不删早退后假称已持久化 |
| V3-GAP-07 | `RunV3CompactBranch` 明报 dry-run 未接线、`(void)midturn` 后未填 parentTurnId；`src/agent/loop.cpp` 仍有 AfterHardTrim | D3 内存换账已修，不能重复列为未修。剩余：干跑、主 turn 关联、provider overflow 恢复与共用 hard trim/预览降档链对齐。applied 后投影失败只报错，尚未强制挡住后续请求。补生产场景验收后销项 |
| V3-GAP-08 | 总设计 §4.67–4.71；现行 goal/loop/memory 各域与命令入口 | Goal、Loop、btw、Memory 新合同含独立实施清单。旧功能可用不等于新增持久事务已接；按各自实施项推进，不批量删除现行功能 |

| V3-GAP-09 | `src/trajectory/session_manager.cpp::ArchiveSessionDir/UnarchiveSessionDir/DeleteSessionDir` | 归档/取消归档仍要求 session.json，v3 无 manifest 时返回 session.not_found；删除仍用 main.jsonl 推导封口与末 hash。先接 v3 状态、锁、末行 hash 与引用边界，再承诺新版生命周期闭环 |

## 文档旧口径

| 页面 | 本轮处理 |
| --- | --- |
| 根中英文 README、架构总览、会话指南 | 改成 v3 默认、同账两类行、执行中落账与双投影；保留已查明的过渡边界 |
| 用户压缩指南 | 按 v3 重写；旧四分区算法留在架构历史页并加醒目标记 |
| `architecture/context/compaction.md` | V3-LEGACY-03：旧算法参考，不再称现行主路 |
| `architecture/query-data-flow.md` | 更新角色边界、文件格式和落盘时点；保留实际仍存在的 User 工具容器说明 |
| `architecture/trajectory-v3-schema.md` | 更新默认状态、四角色实现与 usage/写入缺口；早期盘点标明历史范围 |
| Hook、AppServer、Workflow、Memory、Telemetry 专题 | 顶部注明当前接线边界，链接新版入口；旧合同有调用方，暂留参考 |
| `development/workspace-storage-v2/` 三页 | V3-LEGACY-01/02：历史迁移快照，不能据行号和阶段结论指导当前删除 |
| v3 `session_switch.hpp` 头注 | 清除“默认关”“仅 1 开”等与函数相反的注释，保留现行开关准确语义 |

## 设计单的保留边界

`todos/session轨迹v3_消息主轴树链与四角色壳收敛设计.todo` 是新版设计总纲，早期验收记录保留日期，不删历史红项来伪造一次通过。D1/D2/D3 后续修复以当前源码和发行记录核实；§4.67–4.71 新增设计不能因总纲写“已完结”就算落地。LuaHook、AppServer、Workflow 关联单仍各有实施阶段。旧 workspace-storage-v2 快照已逐页标记，新设计单不因文件名含旧术语而整份作废。

## 清理顺序与验收

1. 先迁 usage、Telemetry、Memory、AppServer 与 Workflow 消费方，补 v3 原账夹具。
2. 再收敛内部 Tool 容器、旧 compact 和 command hook 挂点，验证请求 refs 与实际 wire 对齐。
3. 最后退役 v2 新写口及旧专属接口，按另行确定的旧档策略收读取侧；用户存档不随源码清理删除。
4. 每项检查生产调用、CMake、测试夹具、脚本与文档引用，不能拿一次 `rg v2` 结果批量删文件。

本轮文档校验运行 `bash scripts/check_docs.sh` 与 `git diff --check`。实现清理时再跑相应单测及 `scripts/tests/v3_accept_matrix.py`。该矩阵使用本地假模型后端，不是摘要质量评测。
