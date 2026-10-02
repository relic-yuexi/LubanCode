# 子结果严格历史采用合同

[开发手册](README.md) · [父终态观察](child-parent-observation.md) · [子终态回执](child-terminal-receipt.md) · [SDK 子 Agent 合同](sdk-subagents.md)

先交合同，再写实现。本笔从父观察源 `67ed74c8` 分出私有分支。
前笔尚待鲜 CI，不能拿 D 或组合绿件替它验收。
本笔只加中立、只读历史校准，不开放 SDK 子 Agent 或恢复自动补账。
不跑模型、工具或 helper，不开 writer，不改日志或结果仓，不重放孩子。

## 输入、持值与四种结论

调用方借一份已验父 `V3Ledger`，明传真实父目录与 turn/action/attempt。
借用只在这次调用内有效。结果持值，含各段真实引用、摘要和校准结论；
不持 writer、backend、账卷或文件句柄，不把 child SID/run 当全局名字。
父账所有材料须来自 common reader/verifier，调用方负责这份原件保证。
本笔不重读整份父账，不给长父场补一份整卷 64 MiB 限额。

| 结论 | 含义 |
| --- | --- |
| `NotApplicable` | 指定本场 action 没有前笔版本化子观察材料，沿旧工具规则；不假称采用通过 |
| `Incomplete` | 真子链尚未完成，缺后续保存、接纳或真实请求；保缺口，不补跑、不冒成功 |
| `Rejected` | 已有材料矛盾、坏 owner、坏件、越界或超帽；不能采用 |
| `Validated` | 母来源、子终态、真实 raw、父 local 结果、历史接纳和后续真实 prepared 全链齐备 |

只有 `Validated` 表示完整链已校准。仅验过 raw 或 admission 不能标 accepted。
普通旧工具不添子卷要求；旧低版本观察明确不适用或未完成，不能猜新字段。
有完整观察却材料损坏，明拒；无 final 或无下一请求等合法崩溃窗留未完成。
这四值是内部读取结论，不替 SDK operation 终态，也不自动恢复执行许可。

## 四份事实各管一段

1. **父 producer 原始确认。** 前笔真 producer 写下 execution、appendConfirmation、
   seal 与 display schema。历史校准确认它确实来自这枚母场观察，且与下面材料吻合。
   这份存档仍属可信父 producer 声明，不是独立 OS Close 认证。
2. **子持久终态。** 同一份 bounded 子 bytes 复用 common reader/verifier，
   核终态五键、唯一末 `session.ended`、reason/closeQuality 与首 system 母来源。
   append 后才发生 Close；子卷本身重建不出 Close 回执，也没有完整 typed execution。
   不从 hash、reason 或 clean 字样猜出 live Cancelled、StartupRejected 或 Close 结果。
3. **真实 raw 捕获。** 母观察之后的 raw capture persisted 归本 action/attempt，
   真实 named artifact bytes/hash 与观察 raw 文本/结构 SHA、字节数一致。
   进入 PostHook 后可改文；原始返回与有效结果分两版，各核各账。
4. **历史采用。** effective persisted → local selected → tool message → context admission →
   后续真实 conversation `model.request.prepared`。每一步都要实存引用和次序。
   compact 后退出当前链也沿历史 revision 校准，不能只问当前 context。

历史材料不足就留 `Incomplete`；互相冲突就 `Rejected`。
不能从 live registry、缓存或摘要补造缺掉的历史事实。
逐型独立重建执行与 Close 须另补中立持久报告，不在这笔临时添永真接口。

## 一次完整采用链

指定 action 必须在本父 session/run、真 turn/step 内声明；attempt 是这次实际尝试。
provider call-id 仅作声明材料，不能把另一轮同串旧 action 搬进来。
真实 parent spawn 五键须指母账那枚 spawn，childSessionRef/task/parentActionRef 对齐。
首 child system 必须引用这枚 spawn 与当前母 action，逐行 session/run 归这份子卷。
母观察 checkpoint 仍保 schema 四键；内部 display 五键与末终态同源。

raw persisted 后才有有效版本。selected 只引本父 local persisted 字符串，
不许跨场五键数组；本笔不得绕过 SDK `sourceResultEventRefs` 现有拒跨场门。
selected 与 tool message 归同一 action/attempt/turn，次序严格；
tool role 与本场 action 号对齐；V3 `message.tool_call_id` 存 local action，
provider 原号沿声明与现行 wire 映射校准。采用现行 summary/preview 规则。
普通有效正文沿同一中立 `PreviewFromPersistedMaterials`、`BuildToolPreview` 重建；
summary 沿纯 ledger 校原正文、候选、实际请求与来源。极端 output_index 当前
未存六键身份，明回 `Incomplete`，不能凭路径猜摘要后标完整。
后续真实 prepared 必须在采用之后，system/revision/输入链通过共用
`CheckPreparedAgainstChain`；须确实含这一支 tool message 或合法派生版本。
有 message 没 admission，或 admission 后没有下一真实请求，都不算模型已消费。

## 固定读帽与路径

| 材料 | 上限 |
| --- | --- |
| 本 action 实际子 JSONL | 64 MiB；131072 条；每行 4 MiB |
| named artifact 单件 | 64 MiB |
| 一次校准 artifact 实际累计读取 | 256 MiB；最多 64 个不同引用 |
| child/named artifact 相对路径 | 1024 个 UTF-8 字节；拒 NUL、坏编码、绝对路径、点段与越界 |

子卷真实 canonical 路径留在本父 `subagents`；目录根也须留在父场。
结果件只沿本父 `artifacts` named 引用，不读 cwd 或其他场，不遍历整树。
已打开 regular file 后有界读，同份 bytes 校 SHA/字节数；超帽最多多读一字节便拒。
累计帽按实际读取记账，不用 stat 估值代替；同引用复查可复用 owned bytes。
缺件、悬空链接、目录/FIFO 或坏摘要不得化成空串成功。
这是有限 IO 与归属校准，不宣称整树能挡所有外部置换竞态。

## 实现与远端证据门

共用 `FoldToolActions`、纯 `ProjectResultPreview`、历史 `revision_chains`、
`CheckPreparedAgainstChain` 与前笔 bounded reader；不另写 schema/hash parser。
SDK 和普通 reader 不换默认策略；只添私有只读入口和明确值结论。
失败不推动 Writer，不发请求，不补 capture、observation 或 tool message。

真 AgentTool/loop 先产出全链与两份账，再校准正路；PostHook 改文、compact 后历史链另验。
坏 owner、同名 provider/跨父同名孩子、raw/structured/artifact SHA 和字节矛盾、
selected 晚于 message、错 role/call-id、未采用、错 prepared revision 各验明拒。
改账反例须先经过 common reader/Verify；不能把 reader 先拒伪卷算成新校准验收。
每次调用前后核模型/工具计数和所有原件字节不变，合法未完成窗不重放。
实际来源 `tests/unit/runtime/test_child_history_adoption.cpp` 固定 8 案，
focused 必需来源共 21 册、ASan 共 26 册。两门逐来源登记、JUnit 与完整
LastTest 实跑非零，并核 8 枚 `[child-adoption-path]` 标记各一次。
旧父观察 8 案、12 枚通用标记和 Unix 别名标记全部保留。
`tests/support/child_observation_fixture.hpp` 只归实际内部测试；SDK-only 纯数据
边界册核其测试豁免、错误宿主目标和经夹具偷带 App 头。
公开 SDK 安装消费者不宣称已经有 child API；本笔真实验收走中立实际运行栈。

本地只查源码、文档、AST 和纯数据；configure、编译、CTest 与原生探针均走远端 CI。
共享功能分支与 main 不动。
