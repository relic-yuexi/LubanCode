# SDK 项目 Memory 只读召回合同

本批尚未验收。公开入口为 `SessionOptions::memory`、`memory::v1::RecallOptions`、
`Session::DescribeMemory()` 与 `Session::GetMemoryRecall(operation_id)`；公开值只含
标准库类型。新场省略便关闭。可信本地宿主显式启用本场项目召回；不增加 Runtime
许可开关，不冒称 Managed ACL 已接线。CLI 全局配置与原召回路照常。

Memory 根取 Runtime 的 data_root 和共有工作区门牌，不沿 HOME 发现，不 chdir。
同项目多场共用磁盘主题，各持召回配置、报告与观察者。首笔只读项目层，learn Off，
不挂 memory_save，不起 CLI memory-worker，不另调模型。用户层、候选、写队列、
管理命令和自动提取另批。参数坏则在 MCP、模型启动前拒开。

每条新的人类 operation 只召回一次。查询取准入后实际输入，归属用本场 operation、
turn 和 session。自动事件、恢复史与重复受理不重跑检索。缺库或零命中可交空报告；
坏库、坏路径、读失败和超帽须明报，不能冒充零命中。
operation_id 只在本场唯一，跨场可以同名。wire 须按 `(session_id, operation_id)`
寻址；`GetMemoryRecall` 始终查这只 Session 的映射，同名不算跨场读取。

默认 section 与正文预算为 8192 字节、3 条；参数上限为 65536 字节、32 条，完整上下文另限 128 KiB。
读取另收固定帽：catalog 4 MiB，主题各 16 KiB，
条目 1024、目录枚举 4096、catalog 与主题累计 24 MiB；证据各 16 MiB、累计 64 MiB。
帽按实际读取字节算。严格支路不修 catalog，不写项目共享 trace-last；CLI 保留原默认。
只读和路径检查不构成 OS 沙箱，也不消除并发换树竞态。
护栏文案沿共有语言表；本批不公开逐场语言参数，也不承诺文案语言隔离。

catalog 只定条目名单，筛选、证据与排序均认实际 topic；严格 reader 在宽容默认值
生效前核原始 policy 类型与值。错层、空 policy、坏日期、坏指纹表和坏 evidence 明拒。
缺件与悬空链接分开判；打开后再核 regular-file，防目录、设备或 FIFO 冒作正文。

完整召回段归入本轮真实 ContextRuntime 输入，再沿共有历史执行口发送。正文、来源和
护栏都须能从 V3 采用链重建，同轮不重复注入。逐条召回事实保留实际片段指纹，并引用
完整段消息。片段最多 512 字节便内联，较大者沿共有 BlobStore 落内容寻址仓，采用前
核真实 blob 字节与指纹。SDK 调中立账桥，不另搭运行栈。库后来变化只影响未来召回。
已有 blob 损坏而本次快照准备失败时，不再调用模型，本次不采用 ContextRuntime 输入，
也不落召回事实；旧采用史保留。报告如实记 failed 与 snapshot_failed，引用留空。
账健康门已失败，操作须留 Indeterminate/result_persisted=false，不能冒称持久 Failed。

本场计划与摘要归会话目录，绑定沿共有开场门聚合，保留 Skills 绑定。同 ID 恢复时，
省略 Memory 参数沿存档；显式值须匹配。已有报告归 `sdk-memory-recalls`，按 operation
查询自己的值，不能回读项目“最近一次”报告。Close 先 join 主 worker，再释放模块和
观察者；闭场查询不再摸活 writer、不重新召回。未落稳、未采用和部分失败均须留下真状态。

报告每件最多 512 KiB；恢复枚举最多 4096 项，报告正文累计最多 64 MiB，按真实读取
字节收帽。片段读取受选中条数和载荷总预算约束。完成凭据 SDK result 单件沿现有
64 MiB 边界核身份和 complete；本批不许诺整个 V3 与操作账都有读取帽。
已宣称完整的 durable final 缺报告、错归属、错采用、错正文或降格冒领，拒绝恢复。
dispatch 后尚无 final，或明确持久不完整，恢复仍为 Indeterminate，旧操作不重跑，
报告缺口单独报错。报告先落稳并核真，之后才写完整完成账。

Entry.selected 只说检索选了这段，failed 时也可能为 true；实际采用须看 state 与
context_message_id。state=failed 且带引用，说明有部分采用，事实收尾未齐。
score、理由与筛选旗标归检索诊断；V3 校准核归属、采用、选中身份、片段和完整正文，
不冒称能用后来已变化的库倒算所有旧分数。

验收走远端三平台：安装消费者只调公共 SDK；真读项目库，查零命中、坏件与超帽，
核实际请求文本、V3 重建、四场重叠、取消、关闭、同 ID 恢复及报告归属。本地不 configure、
编译、CTest 或运行原生程序。首笔通过也只交只读召回，不等于 Memory 模块全数迁完。
