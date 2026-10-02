# SDK 项目 memory_save

本批让新宿主显式准许模型同步写项目正式记忆。只迁 `memory_save` 的项目 upsert。复用内置工具解析、验证和 `ProjectCommit` 提交门；CLI 仍排队并启动原有 helper。不开用户层、不自动提取、不把正式保存改成候选。

## 公开值与开场

`SessionOptions::memory_write` 接 `memory::v1::WriteOptions{enabled}`。空默认关；显式 `false` 也关。开写不会开启 recall。`DescribeMemoryWrite()` 返回冻结计划；`GetMemorySaves(operation_id)` 返回本场逐次调用报告，不含记忆正文。operation ID 只在 Session 内唯一，网络寻址须用 `(session_id, operation_id)`。

新场把计划存进会话目录，并把摘要接到有效 system root 的 `hostBindings.memoryWrite`。同 ID 恢复时，省略选项沿存档；显式变化拒开。旧场没有计划，只能省略选项继续关写。静态计划检查早于 MCP。恢复持有会话独占 owner，再验有效 system 采用链和逐次调用报告；模型和工具此时还不能执行。可信本地写许可不冒充 ACL 或托管 Worker 授权。

## 谁执行、谁拥有

每场注册一只 `memory_save`，同步 target 持有本场模块。宿主把真实 session、operation、turn 送入共用 TurnWiring；主执行线程从已开账 action 取真实 action ID、attempt，再按值交给工具。并行工具线程只读这份副本，不回读正在追加的桥。模型参数不能改这些身份。提交键绑定这五项，不能只用模型 call ID。取消谓词只借当前调用 atomic 旗；提交门不存指针，不起监控线程。

SDK target 先解析并验证正式 SaveRequest，再用中立 MemoryLedgerBridge 写 `memory.save.requested`。只有 PowerLoss 提交成功、真实 source ref 非空，才准提交门碰 topic。事件记实际调用身份与 `save_request_sha256`。取得真实引用后，再算含 target/source/session 的 gate `request_sha256`，两项分别核准；失败零 topic mutation。旧桥空字符串和无 writer fallback 不适用这条路。

工具描述要求核实事实、明确偏好。这是模型指引。硬校验沿现有正式 SaveRequest：字段、ID、scope 和反馈置信度等约束；旧校验没强制每条 fact 附证据，也没拒绝 inferred preference。本批不暗中收紧 CLI。附带证据仍走提交门原有读取与指纹检查。

## 逐次回执与未知写入

本场报告存 session、operation、turn、action、attempt、请求事件引用、请求摘要、项目身份、记忆 ID、路径、正文摘要、提交阶段、状态和错误。正文留原不可变 snapshot，不经公开 query 回传。调用请求与结果都须耐久确认。已确认相同键只核验并重新 flush 原 result；不再写 topic。冲突、坏件、只有 intent 没 result，均不重放。

`NotStarted` 明报没开始 topic mutation；`Committed` 只在所有应有阶段和 result 耐久确认后成立；`Indeterminate` 明报部分写入或结果未确认。共用工具 Result 增最小显式 execution control，默认 Continue。SDK 未知写入终止同批尚未启动的后续执行和后续模型请求，已经启动的并行工具全部等其退出。整回合 `Indeterminate`、`result_persisted=false`，不借旧崩溃 outcome 或 tool-result 仓失败码冒充业务状态。后续自动提交也停住，留宿主核查。

恢复须以已验证 V3 账核实际 requested/action/采用结果，核冻结计划、报告与 gate intent/result/snapshot 的请求 SHA、正文 SHA、阶段和 owner。没有最终报告的合法中断留未知态，不重跑旧 op；完整成功回合缺件、坏件或冒领报告拒恢复。哈希是完整性核对，不当身份认证。公开 query 返回 owned 值，Close 后仍可读。

工具开写、请求、保存回执、选中事件与 tool 消息逐枚核场身份和实际 run，不能只认首行 owner。完整回合还须核正式 tool 消息已进入有效上下文链。消息与选中事件同属本回合和真实 action；次序须为保存回执、选中事件、tool 消息。消息的 `tool_call_id` 指真实 action，不能借其它角色或晚到事件冒领采用事实。同 ID 恢复沿原场账 run，不凭进程重启重发身份。

## 关场与验收

本场 worker 执行同步 gate。Close 发原有取消旗，等实际调用退出，再沿共用执行对象收 Agent、资源和账。target 此时不再借 writer。文件系统调用没有固定墙钟保证。不开 CLI helper，不复制另一套会话栈。

远端三平台验实际公开 Submit→memory_save→结果→终态与移位安装 consumer；原 recall 14 册、四场隔离和旧 CLI 项目保存照跑。新册须核默认关、显式开、恢复计划、坏输入、requested 先于 topic、同键与冲突、提交各阶段失败、未知不再续模型、取消/Close、回执校准和旧不可变 snapshot。ASan 精确收源与非零用例。本地只跑纯数据、AST、文档检查；不配置、编译或执行原生。

公开报告每 operation 最多 64 次保存、单件最多 512 KiB；恢复最多枚举 4096 项，总读取最多 64 MiB。原生 V3/operation 账沿现有边界，本批不许诺全账 IO 帽。Inspect 只读不可变回执与 snapshot，仍短暂持项目锁；不创建 memory/lifecycle/operation 目录，不写 topic/receipt；原有项目锁仍建删自己那座锁目录。
