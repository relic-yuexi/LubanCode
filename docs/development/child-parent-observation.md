# 父 V3 子终态观察与未知止损

[开发手册](README.md) · [子终态回执](child-terminal-receipt.md) · [前台组合验收](child-sdk-integration.md) · [SDK 子 Agent 合同](sdk-subagents.md)

先写合同，再实现。本笔基线为组合头 `4413b315`，含 D 旧消费者迁移与 C 诊断；
这批鲜 CI 尚未验收。
本笔只接 main 向前台孩子派工时的父 V3 观察与失败止损。
公开 SDK 子 Agent、后台、嵌套、异步审批、严格恢复采用与自动补账另笔交付。
不另建 SDK 运行栈，不重写 ResultStore，不改远端默认 preview。

## 父场持有与开跑前门

父轮持绑定与首次观察/捕获回执；shared registry 只持子终态值副本。
绑定冻结真实父 Session/run、当前 turn/action、已接纳声明消息、实际 attempt、
原 spawn task、父 spawn 五键与 childSessionRef。provider call-id 只查当前轮簿。
旧轮同名 provider ID 不续用旧绑定；空、错场、错 run、陌生或已退轮 action 明拒。

子 SID 与 run 都可能跨父场同串。V3 不写 session.json，现主 run 发号只扫旧 manifest；
子 run 又拼本场计数与主 run。因此寻址须携父 Session/stream 归属。
同串 registry 键只回本 owner 值；真实子路径、父 spawn 来源与五键共同核验。
不能拿 SID/run 字符串当全局身份，也不能让模型参数补这些字段。

本笔信任真实 native producer 已 Committed 的母 spawn 回执与声明冻结值。
Attach 再核当前 writer/books、action 与 attempt；不重读整份父卷，不给长父场添读帽。
母来源只经内部生产者传值，模型、外部材料与恢复表不能注入这份绑定。
这不是母五键独立盘读校准；完整父卷证据由原生验收核，严格恢复另笔补。

实际模式由 AgentTool 派工口解析并持值传入 spawn 装配。
`auto`、旧 `run_in_background`、会话缺省与 Agent profile 都参与这次解析；
不能只看模型参数，不能在装配层重新猜。新门只管 main 的真实 foreground。
后台、嵌套与未接轨迹路径须显式判为不适用，沿原路，不因新绑定门误拒。

`AttachChildRun` 须回显式成功或错误，不能 void 跳过后仍开子 Agent。
CLI spawn 装配在真子账已开、已 link 后检查绑定。绑定失败时停止派工，
对已开的真子桥调用一次 `Finish(StartupRejected, reason)` 并检查真实 Close，
缓存/上交原生回执，随后销毁桥、wrapper 与已占资源；不启动子模型或工具。
失败经 owned spawn failure 传回实际 AgentTool，沿原任务/隔离房收尾。
未确认 append/Close 明传 StopIndeterminate；确认的启动拒绝保原普通失败。
不冒称回滚，不重试 Finish，不让清理错误抹掉最初绑定错误。

## 读件与计算上限

新观察验证固定以下帽，不改普通 reader 或工具默认预算：

| 材料 | 固定上限 |
| --- | --- |
| 单份实际子 JSONL | 64 MiB，超帽最多多读 1 字节后拒绝 |
| JSONL 非空记录 | 131072 条；每行 4 MiB |
| 原始父工具文本及其结构材料 | 各 64 MiB；超帽不截断成成功观察 |
| 子相对 journal 路径 | 1024 个 UTF-8 字节；拒 NUL、坏 UTF-8、绝对路径与越界 |
| 新观察打开父结果仓时的目录枚举 | 65536 项，先计数再筛选 |

用已打开 regular file 的有界读取，再从同一份 owned bytes 校准 native hash/seq/schema。
有界 reader 共用纯 ledger 的 summary/selection 校准，不探结果 artifact；
普通 `ExpandResultPreview` 仍核真实文件与 hash。纯投影不宣称外部 blob 完整。
不做 status 后无界重读，也不把普通 WalkSessionTree 冒充严格源校准。
子路径须 canonical 留在本父 session 的 subagents 目录；不存在、悬空链接、坏件明拒。
`subagents` 自身也须留在真实父目录内，不能随目录链接指到另一场再自认合法根。
这是读帽与路径归属检查，不宣称整个目录树能挡所有外部置换竞态。
无整棵会话树遍历，不读其他父场；JSONL line/记录帽在 JSON 解析前检查。
观察只验本次绑定孩子，校准原生 terminal 五键确指唯一最后一枚 session.ended，
源各行 session/run、首枚 system 的 spawnEventRef 与 parentActionRef/taskId 须对上冻结值。

## 次序与首次回执

次序钉为：真实 child Finish → 父工具执行终态 → Capture 收最终原始返回 →
核子卷/父来源 → PowerLoss 追加 subagent.observed → 原 raw capture →
原 PostHook、有效结果、selected、tool message 与 context admission。

`childCheckpointRef` 保现有四字段形状。版本化内部 display 材料另记验过的
terminal 五键、真实 execution/append/Close，以及原始父文本 SHA256/字节数。
正文仍只存本场原结果仓；raw capture 内容须对上观察 hash，不复制全文到事件。
selected 仍只收父 local persisted ID，不能塞子五键跨会话对象。
本笔正路实跑下一真实 model.request.prepared，只作当前运行回归，
不宣称已经交付严格恢复采用或公开 SDK 子结果查询。

观察与 capture 各缓存第一次尝试，成功、拒绝、未知都不再写第二遍。
保原生 append 与独立 Close；执行成功不等于父场已经确认交接。
Cancelled + CloseFailed 保 Cancelled/已提交五键/CloseFailed，不能落完整正向观察。
执行 Indeterminate 即使终态 append/Close 完整，也不能冒任务成功。

## 未知态必须走到真实出口

中立工具回执只添默认 false 的 owned 未确认旗；普通工具沿旧路。
真实子已执行，父 observation/capture 随后未确认时，明确升 StopIndeterminate。
不套 crash 合成 UnknownAfterStart，不只回普通 is_error。
loop 的 rewrite/批次 Failed 出口须保 sticky unknown 与真实错误，
交回 RunOutcome.side_effect_indeterminate，沿共用 Drive/Stop/CLI/SDK 聚合。
进 rewrite 前已未知时撤掉 ActionSummary backend；含真实绑定 child 的本批
在 rewrite 内首次硬失败，也须立刻撤掉它。后置 sibling 可以保存已执行原件，
不能再发摘要模型。真实 `res-` 文件号解析异常只在含绑定 child 的批次
收成 typed Failed + unknown；保第一原错，不重写 capture，不改普通工具异常策略。
只收已经在途完成件；不启同批尚未开工工具，不发下一模型，不重复 capture/append。
只保存未知态，不能推动成功 Writer 状态，也不能用旧缓存补造完整采用。

回合退场先撤借用，再清绑定。registry 不握父 writer，不续旧 spawn callback 寿命。
Close 沿原真实调用收栈；不添监控线程、detach 或新取消系统。

## 远端验收与分账

新 native 来源实跑：子 success 后 observation I/O unknown、子 success 后 capture unknown、
sticky unknown 遇 rewrite Failed/批次 commit Failed、正确观察到下一真实请求的正路，
以及取消 CloseFailed、Attach 拒绝后的真桥收尾、provider ID 跨轮复用与父场同名隔离。
新册固定八案。十二条完成标记钉住实际路径：正路采用、观察 I/O、捕获、
rewrite/commit 回执、真实仓名溢出、前置与批内摘要止损、健康摘要、取消关闭、
缺子卷与纯 ledger 摘要。首次 capture 重查在活轮内完成，不假称退轮后仍有调用口。
故障放在真实 writer/结果仓边界，不拿 reader 先拒伪卷冒充宿主校准。
旧两案组合/两路径标记、D 七案/五标记、E 十案、C 七案和普通 CLI 门全部保留。
新增来源列入 SDK focused 二十份、ASan 二十五份必需来源；
新册八案与十二标记各恰一次，登记、JUnit、LastTest 与非零原生数须齐。
Unix 另钉真实外部目录别名反例及完成标记；Windows 不假称无需权限即可建链接。
格式分账为 explicit-v3；普通非子工具保默认 false，V2 回归沿旧路径。

本地只查源码、文档、AST 与纯数据；configure、编译、CTest 与原生故障进程都留远端。
源码实现后先静审，再提交远端 CI；共享功能分支与 main 不动。
