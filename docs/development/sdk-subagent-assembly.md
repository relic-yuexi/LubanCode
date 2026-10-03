# SDK 前台子 Agent 装配合同

[开发手册](README.md) · [首批边界](sdk-subagents.md) · [执行组合](child-sdk-integration.md) · [子票审批](child-async-approval.md)

状态：公开装配实现候选。先接冻结计划、真实前台 owner 与公共消费者；严格采用门及同源远端证据收齐前，不勾交付。
实现须先合齐共用 owner、typed 子终态、当次取消、真实子票审批与严格父采用门，再取同一组合头远端证据。
第一轮父终态观察即使通过，也不能替严格采用门或本笔验收。

## 公开值与准入

宿主逐场显式开启，默认关。首批只准 main 派 foreground，深度固定为 1；父工具调用等本孩子退出。
`auto` 只能在本入口解析成 foreground。background、嵌套、worktree 隔离与恢复未完孩子暂不开放。
模式、层数、类型、名单和预算先裁完，再注册 task、开子账、起线程或建 worktree；越界明确拒绝。
孩子工具表不登记 `agent`，运行入口仍须复核 caller 深度，不能单靠模型 schema 挡递归。

拟在 `SessionOptions` 增可选标准值计划，保存精确内置类型、逐类型工具名单、模型与预算。
首批类型限 `general-purpose`、`Explore`；不扫描 HOME、cwd YAML、Package 或动态 agent catalog。
工具名单只准取本场已经准入并握手成功的真实工具名；别名、延迟发现和空名单不扩大权限。
自定义工具与 MCP 工具也须逐项选名；首批 Explore 取真实 ReadOnlyLocal/ReadOnlyRemote 分类与原角色名单交集。
requested/effective 分开冻结；Unknown 同名工具不能混入，交集为空便拒开。这批不开放任意 custom/MCP 的 Explore 角色。
模型省略才沿父模型；模型名单与预算字段名随实现 PR 二审，不在文档冻结。

用户已选宿主显式填预算：开启时每种准入类型必须填正整数步数和时长，不设开启默认值。
时长统一用整数秒；零、负数、浮点、缺项及转换到内部计数/时钟会溢出的值均拒开。
准入先核预算，再碰 task、子账或线程；超父场已设同维执行上限便拒，不能先起跑后裁。
父上限 0 只表示那项未设，不拿它当子无限许可；孩子仍须有显式正值。审批 timeout 与网络 timeout 不冒充执行时长帽。
调用省略预算沿宿主冻结值，显式值只准收窄；不能借 0、坏形状或溢出提升上限。同 ID 恢复沿冻结原值，显式变化拒开。

开启后保留 `agent` 工具名；与宿主自定义或 MCP 注册冲突便拒开。关闭时保持既有 SDK 工具面。
不复制父 Memory 召回、写入模块或逐 operation 回调；子提示不继承父场临时 Memory 召回片段。
孩子无真实 owned operation，invocation 继续缺席；父 operation 只作票归属与 cause，不能拼子五键。
首批子名单拒 `memory_save`，也不交父模块引用；另拒 `todo_write` 保留名，免得旧私有 todo 构造替换宿主工具。
宿主自定义工具仍按其公开执行合同负责自己的外部副作用；这笔不改 CLI 工具构造。

## 票据、报告与归属

沿原 `PendingApprovals`、事件与 `ResolveApproval` 发票、回答，不另造 pending 表或等答线程。
公开票增加可选 owned 子范围：真实宿主 Session/已受理 operation、宿主 run、直接父 SID/run、子 SID/run、cwd/floor、真实声明 turn/action/message。
票发生在 `ToolExecutionStarted` 之前，不能虚造 attempt。request ID 仍为本公开 Session 内不透明逐票键。
SID 与 run 都可跨父场同串；查询须回本 Session owner 和本 operation，不能凭孩子两串号跨场寻址。

子权限用显式 child-scoped 裁定口合既有规则、ModePolicy 与有效 floor，不递父 ordinary allowed。
硬拒绝、PreToolUse Ask、PermissionRequest 每次照裁；只有普通 Pass 才能借当前孩子 grant 免新票。
用户已定 `AcceptForSession` 只管当前孩子。grant 绑定宿主归属、直接父与子 SID/run、cwd/floor，不拿票 turn/action 当 grant 键。
每票 Wait 后撤 lease；grant 留到孩子最终退出、取消或 Close。父场、兄弟、后代和恢复 run 均不继承。
缺真实 owner、capability 或可撤 lease 便拒，不回落普通父票或同步确认。无票孩子退场也留下关闭标记，迟登记不能复活。

拟增本 operation 子报告查询与冻结计划查询，只返回标准库 owned 值，Close 后可查。
报告分开记真实子执行状态、子 append/Close 确认、父终态观察、正式工具结果选择与父上下文采用；不能合成一枚“完成”抹平缺口。
保留父真实 action/attempt、spawn/link 引用、孩子实际卷身份与终态摘要；task ID 和模型 call ID 只供配对，不作全局 owner。
执行已知与父模型已消费分开记。健康取消或父执行帽失败可在 Tool 消息已选择、接纳后收场，下一份父请求尚未发出。
只准保留 Inspector 真报的 `prepared_consumption_pending`：操作仍为 Cancelled 或明确执行帽 Failed，报告为 owned Incomplete，不能冒称 Validated。
成功操作仍须完整采用链；其它缺口、坏归属或坏 hash 不放行。观察、捕获或子 Close 未确认仍走 StopIndeterminate。
正文仅返既有策略下 preview；子完整账与工具原件留执行端。远端 Full 仍须 Node 许可和本场参数双开。
查询只读冻结值，不重放模型、工具，不补账，不以 TaskLedger 活态或宽松树遍历代替耐久报告。

## 资源与关场

每场派工 owner 归 `SessionResources` attachment，罩住协调器、角色工具 overlay、冻结计划与报告投影。
`AgentTool` 和转发表短借这份 owner；孩子沿共用 `ExecutionOwner` 持 wrapper、overlay、Agent 与 scoped turn。
父 Backend、原工具及 MCP client 仍归本场资源图；孩子不复制后端，不新建 MCP，不另开公开 Session 拼子循环。
首批父子串行借本场 Backend：`agent` 不入只读并行名单，父调用等孩子返回；不要求 Backend 支持并发。
监督、取消合流线程仍须真收柄。协调器强引用、业务终态或有界 detach 都不能证明资源借用已退。

Close 先拒新派工和新票、广播当次取消，再等 SDK worker、前台子调用及相关线程实际退出。
执行结束后 Finish 子账并确认 Close；scope/grant 最迟在 whole child 返回前退净，scoped callbacks 须早于所借桥、取消链与资源销毁退场。
本笔沿现有实际收尾次序；若要把退 grant 提前到 Finish 之前，须另取生产交错证据。未知副作用或交接失败令父轮 StopIndeterminate。
随后沿现有 `SessionService::Close` 封主账；SDK 清 options 中源回调，再释放 service：Agent → attachment → registry/MCP/backend → ledger/文件句柄。
attachment 若需在封账前收线程，须在 shutdown 阶段完成，不能等析构才第一次取消；不能改既有封账与释放文件次序。
构造失败同样先撤借用，再退候选资源；publisher、capture 销毁与线程 join 均在 API/账/协调器锁外。
不承诺强杀宿主 callback 或固定墙钟 Close；取消合作义务沿现有公共 Backend/Tool 合同。

## 冻结、恢复与交付门

新场冻结解释后计划：精确类型/工具面/模型/预算、depth=1、foreground、有效 cwd/floor 与版本；摘要接有效 system host binding。
先有界预检；恢复在既有 SessionLock 内、旧 system 转移或任何写账之前，复核计划和已握手工具面。
新场计划、binding 与有效 system 可靠落稳后，才准初始化公开执行并启动 worker；半落盘不是成功采用。
同 ID 恢复省略选项沿原计划，显式变化拒开；旧档无计划只准关闭。坏件、错归属和单边残留拒绝，不倒推补账。

严格父采用门须核真实 spawn/link → 子终态回执 → 父观察 → result store → formal selected → Tool 消息 → context admission → 实际父请求。
逐枚核本场、真实父 action/attempt、子卷身份、事件次序、正文与摘要，防跨场、跨 turn、重哈希冒领和观察降格。
raw capture 与正式结果各核其角色，不借二者都 selected 冒称双重采用。只观察到孩子终态仍不足以宣称父已采用。
除上述唯一未消费窗口，完整 final 缺报告或采用链便拒恢复；合法中断可保 Indeterminate 与明确缺口，禁止自动重派或重跑副作用。
已知取消与执行帽失败沿上述唯一未消费窗口恢复原终态；不发新模型、不重跑孩子，不从旧账捏 live 回执。
恢复先在构造前核已知 final，再在现有 opening 锁下复核；坏子卷或已采用材料缺件须先拒，不能等旧场写过新行才报错。
已知 final 逐项配对本场 SDK result，单件上限 64 MiB；再核真实 V3 turn 与最终消息归属，不借空报告躲过错 turn。
通用副作用未知沿 `sdk.side_effect.indeterminate` 留原错；实际 Memory 写未知保留原专码，不把普通工具失败抬成未知。

安装消费者只用公共头，真实 Submit 派子任务；验默认关、显式开、准入前拒越界、票归属、当前孩子 grant 与父 grant 不继承。
同项目两场、异项目各一场，复用去重键与模型 call ID，核真实 cwd、请求、工具、报告、取消和关闭不串场。
SID/run 同号跨父归属仍由既有真实子票册守住；公开夹具不假定跨项目发号全局唯一，也不捏造 SDK 孩子身份。
再验缺/零/负/溢出预算、超父帽准入前拒绝、真实步数/时长到帽、完整恢复、坏/半计划、错采用/自重哈希、无 final 中断与 checked Close。
装配/派工失败须保真实失败账；故障 cleanup 先放闸再等线程。
三平台 fresh 全量、六套 SDK focused/真实安装、Host、Worker、两 Runner、必需 ASan 和九份实际依赖图认同一组合头。
新增来源须精确登记、非零、无 skip；旧 CLI 子 Agent 与后台册照跑。公开入口、报告和恢复全过才勾交付。
本地只做静态、文档和纯数据检查；不 configure、编译、CTest 或运行原生夹具。

## 首轮原生红点诊断约

源 `c2cfd3ce` 的真实 Windows 捕获故障与 macOS 四场取消断言红了；原日志未记实际 Operation 或 live Finish/Close 值，不能倒填根因。
下一头只在这两处原断言失败前打印已有公开 owned 值：状态、原错、持久标记、报告采用缺口、实际子回执、请求与工具计数。
不改生产、原断言、预算、故障场景或固定十二册。诊断只凭当前场值，不读未来结果、私有 Writer 或凭据；旧原件独立封存，新头独立取三平台证据。

诊断源 `d674d3e0` 的 [macOS 组合腿](https://github.com/relic-yuexi/LubanCode/actions/runs/37072767233/job/111056228058) 已记实值：父 `Cancelled/result_persisted=true`、原错为空，子 `succeeded/committed/closed`，append/Close 无错，工具执行为零。
完整原日志 SHA256 `fa5719b41636d5dec7147687c4a3ad62c16a5476a06c831a6ee8be50ca762f74`，3287–3292 行；不再猜成 Finish/Close 丢确认。
这套四场夹具须先在共闸未释放时，逐 owner 确认孩子 0、1 的真实 Backend 已亲眼读到本次取消旗，再放孩子 2、3；父 Cancel 成功或别场 Close 完成都不能顶替它。
候选只收夹具确认闸；两名 owner 合用一段 20 秒等待，原四场到闸等待仍另用 20 秒。四场重叠、Cancelled/持久标记/工具零次/回执断言与失败 RAII 放闸照留，生产 CancelChain 轮询不改。Windows 原工具失败另查下段，生产不抢改。

`d674d3e0` 的 Windows SDK-only 原日志 SHA256 `939ab227abbdd124bdd160072e630d662071919254643a2b1f5f28f698f33bef`，3524–3530 行，已证父 `Failed/result_persisted=true`、子 `failed/committed/closed`；只到一枚 child 请求、一枚工具执行。
障碍 `active=false/obstacle_written=false`，setup 无错，原目录仍在、backup 不在；`before_child_final` 尚未走到，不能把这笔已知失败洗成捕获未知。
下一诊断只在父原 Check 拒绝前打印已交给 fake Backend 的公开 `tool_reply.text`，不改参数、返回、断言或正文；本夹具不含凭据。子首工具之后究竟哪一步失败，等真实 preview 说明，不凭临时路径长度猜定产品故障。

本分支随后只摘取结果仓原生 IO、原册精确登记与 Memory 写计划诊断八笔窄修；不带入组合中的 CAS 接口。
本头实际为 24 册 SDK focused、29 册必需 ASan，安装消费仍 20 场，公开子 Agent 原 12 案不减；ResultStore 原 17 案及 Windows 两枚路径标记照验。
旧 b58 的 Windows 真红与新组合的计划写入真红另封。诊断只补实际原子写阶段，未凭旧部分绿宣布故障已修；新头重取整套远端证据。
