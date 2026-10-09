# SDK 阶段进度与验收边界

2026-10-09 续批：第 2 步[显式 web_search](sdk-web-search.md) 已随
[#335](https://github.com/relic-yuexi/LubanCode/pull/335) 合入 `194f587b`，与验收源
`d596e8e0` 同树。远端全量 Windows 791、Linux 789、macOS 792 项均过；
三平台独立 SDK Lua ON/OFF 各 70/68 项均过，九册安装消费各 15 次真实 HTTP
均过；ASan 169 项过，Workflow TSan 本片未触发。74 册产物及原日志已封存。
[SDK 开场线程失败清理](sdk-opening-start-cleanup.md) 已随
[#336](https://github.com/relic-yuexi/LubanCode/pull/336) 合入 `8a19edf4`，与验收源
`49060155` 同树。全量 Windows 791、Linux 789、macOS 792 项均过，三平台
测试名单与 #335 相同；独立 SDK ON/OFF 各 70/68 项、生命周期 12 案及五条
真实失败路径、九套安装消费均过。ASan 170 来源、1881 CASE、77461 断言过，
745 份源码及输入哈希与实际 Git 对象吻合；74 册产物已封存。未跑本地 CI。
[采样参数与终止原因](sdk-model-sampling.md) 已随
[#337](https://github.com/relic-yuexi/LubanCode/pull/337) 合入 `adab6fb3`，与验收源
`108eed10` 同树。全量 Windows 793、Linux 791、macOS 794 项均过；只增两条
采样登记，旧案例未减。独立 SDK ON/OFF 各 71/69 来源、九套安装消费各 37/34
案例均过，实际 DLL 拒绝非法终止原因。三平台真实 Memory 抽取核过三种截断。
ASan 171 来源、1887 CASE、77578 断言过；746 份输入哈希与 Git 对象吻合。
74 册产物及 1925 份索引原件已封存，未跑本地 CI。
[旁路采样看门狗关场](sdk-sampling-lifetime.md) 已随
[#338](https://github.com/relic-yuexi/LubanCode/pull/338) 合入 `068db5d3`，与验收源
`639a9dc2` 同树。全量 Windows 795、Linux 793、macOS 796 项均过，旧名单未减。
三平台 SDK ON/OFF 各 72/70 来源、九套安装消费各 37/34 项及各 15 次真实 HTTP
均过；SDK DLL 内六条故障路径与 CLI 采样 25 案均过。ASan 172 来源、1899 CASE、
78519 断言过；747 份测试源码、附件及配置输入与 Git 原件吻合。77 册产物、1941 份索引原件
逐大小和 SHA256 封存。TSan 本片跳过，不计通过。未跑本地 CI。
[Memory 抽取共享底座](sdk-memory-extraction-core.md) 已随
[#339](https://github.com/relic-yuexi/LubanCode/pull/339) 合入 `52b71aa8`，与验收源
`5c463e9b` 同树。Windows/Linux/macOS 全 CLI 各 798/796/799 项均过，旧名单未减。
三平台 SDK ON/OFF 各 73/71 项、九套安装消费各 37/34 项及各 15 次真实 HTTP
均过；实际 SDK DLL 与 CLI 内核两路各七案及标记齐。ASan 174 来源、1913 CASE、
79290 断言过；749 份测试源码、私有附件及四份配置输入与 Git 原件吻合，清单
不覆盖全部生产源码。77 册产物、1940 份原件与封存副本逐大小和 SHA256 核过。
TSan 本片跳过，不计通过。未跑本地 CI。同步共享底座已交，自动 Memory owner、
候选管理、写回与完整持久身份及来源仍欠账。
下一片[Memory 抽取线程启动失败](sdk-memory-worker-start-cleanup.md) 合同先提交；失败槽、原 CLI 收账与五案已接，ASan 两道选择器及执行门已补，待本源远端验收，不计交付。
CLI 主入口与 one-shot 尚未迁完，SDK 与 CLI 全面对齐仍欠账。下文保留历史原账。

核查日期：2026-10-06。已合基线为功能分支 `fd76b6a5`；[#333](https://github.com/relic-yuexi/LubanCode/pull/333) 的已验源 `c522a51c` 与合入头同树。

目标：新宿主只调用公开 SDK，便能运行完整会话，不必复制内部运行栈。CLI 逐项迁入，原功能保住。现已能嵌入；CLI 主入口和 one-shot 仍用内部装配，尚未全部对齐。

## 2026-10-06 批次记录：收口后再推进

当前只留总 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)。
功能分支停在已验 `fd76b6a5`。候选沿原 CI 分支收齐，不添实施 PR。

修复源 `a23f98e6` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37452164239)
已自然结束，整轮失败。原生全量 Linux 787、Windows 789、macOS 790 项均过；
九套 SDK、ASan 与 TSan 也过。Windows 三道后置纯数据检查读取 UTF-8 YAML 时
误用 cp1252，统一在字节 27 报解码错误。三份 extractor 已生成通过原件，不能
据此抹掉后续失败。首失败 19 件／5617043 字节已逐件复核、封存；摘要
`c0db4433643b849fe8ee4d7a335b09574b06650c081df20fbb4099d4250130ab`。

下一候选合入[显式 UTF-8 读取](ci-evidence-utf8.md)：只改三份纯测试的九处文本
读取，原字节、失败门、CASE 与预算保留。原十五项纯测试及 cp1252 默认读取
重放均过；C++ 新组合仍待本源远端 CI。未合功能分支，未添 PR。

上一源 `e79eea52` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37439210845)
已自然结束，整轮失败。1048 件原件逐大小与 SHA256 复核后封账；共
20707282 字节，摘要为
`c35878f0b1a59a536e19b7a1d8b426cb420e14776706bc606dea7b38cf1aba54`。
六套安装消费各 37 项中 36 过。不可变结果写入已报未知，后续新操作仍成功，
原验收拦住这条漏口；focused、OFF 与全量随后跳过。ASan 实跑 168 册，
167 过，同一公开结果案失败。TSan 实跑 14 册，13 过；workflow 五案开场
均报 `named_result.plan_path_rejected`，保留实际路径，未采宿主别名指向。
不能断唯一起因，也不能把开场失败写成 race。Windows 两套子场与恢复种子
当次实际通过；局部通过不算整批验收。

本候选保留 [SDK 入参测算](sdk-model-input-projection.md)、
[File 预览路径兼容](sdk-file-result-display-path.md)、
[原截止断言分组](sdk-owned-deadline-doctest.md)，以及失败时才输出的
[子会话终态](sdk-child-terminal-diagnostic.md)和
[workflow 开场诊断](workflow-session-open-diagnostic.md)。
测算共用实际 `Generate` 转换；SDK 输入、provider wire、请求输出帽分别记范围。
主请求只留指纹，摘要保真实材料。旧预算、断言和公开后端 ABI 照留。

这轮收三处窄修：

- [未知发布执行门](sdk-named-publication-fence.md)同时守新输入、已排队输入和后台命令启动。
  晚 Job 报未知，仍保父场已确认终态；同轮排队命令不得越过许可再启动。
  这条门只管当前 owner，尚未交付跨进程未知发布恢复。
- [Journal 材料门](sdk-journal-owner-operation-material-gate.md)改核实际操作账、输入与结果文件。
  原独立读回错用了 Jobs 专属绑定事件。旧档缺材料仍拒；新 CI 须留下实际生产文件。
- [File 路径门](sdk-named-owned-paths.md)收在自持会话根内，根和场内链接继续拒。
  宿主祖先沿既有 LocalTrusted 合同。Windows 原路径先留住 `..` 再查边界；
  真正相对路径验收在当前测试目录另建独立目录，不要求 Temp 与测试目录同盘。

路径补六场原生验收源码，核真实 SDK 会话、同 ID 恢复、File 材料与政策、
关闭后读取，以及根、内部目录、叶和悬空链接。Windows 造真实目录 junction；
POSIX 造文件和目录 symlink，不能据此声称 Windows 文件 symlink 也已验。
原 Named 10／8、workflow 五案、所有原预算和案例保住。

`a23f98e6` 实际六套 Lua ON 各 69 册 focused／552 CASE／37 项安装消费；
三套 OFF 各 67 册／537 CASE／34 项消费。ASan 实跑 169 册，76 册重点来源
与 26 份编译支持件已核；1869 CASE／75185 断言通过。TSan 十四册通过。
这些事实只属该失败源，下一份 C++ 组合仍须重跑三平台与消毒器。

下一片已在同一私有候选合并，源码交叉审查通过，尚待本源远端验收：
[十处场内 live 读取](sdk-journal-live-read-capture.md)取真正 Writer 捕获；
[开场五模块](sdk-prepare-journal-capture.md)共用本次惰性 File 捕获与同一只读视图。
模块首错与原装配顺序照留。小份来源指纹随原恢复请求入锁，锁后另取真实材料，
先核原引用，再比指纹，末后交原恢复适配器与续写接口；指纹不代替原生锚和 EOF 检查。
有效却归属别场的主账保原模块首错；真实账在两次捕获间变动，则拒绝续写。
原私有模块调用未带共享 owner 时，仍用原 File 读取。

[Policy 回调退场](sdk-policy-callback-lifecycle.md)复用 SDK 原阻塞门。
提供者调用、通知、退订和最后一份捕获销毁均入门；异步线程退捕获也照办。
通知只负责失效，原跨订阅与外部排空次序照留。公开声明未扩，只补四行寿命说明。
阻塞调用报重入并不会延后对象析构；宿主须留住仍在使用的场、Runtime 和事件流。

[CLI 单次提问 owner](cli-oneshot-execution-owner.md)已接入这批私有组合。
AskOnce 借用原 Spinner 与 ToolRuntime，Agent 改由共用 ExecutionOwner 创建和退场。
它没有把借用资源冒充自持 SessionResources，也没有将 CLI 后端折成文本 SDK DTO。
原请求、权限、工具、RunTurn、受理与关账照留；源码独审通过，原生尚待远端验收。
完整 JournalStore、托管授权和 CLI 主入口迁移仍未完成。完整 AskOnce 端到端
证据须另补，当前 owner 案不能代替整条命令验收。

[交互 CLI owner](cli-interactive-execution-owner.md)也已收入私有组合。独审拦下
首版重建后命令表仍借旧 Agent 指针的漏口；[稳定槽修复](cli-stable-execution-owner.md)
已过独审。owner 只创建一次，后续在原槽重建；旧命令借用成功重建后仍同址。
复制或恢复失败则留空，宿主仍须守原串行与空槽纪律。原七 CASE 全部保留，
新增实际旧指针、引用和闭包调用验收；这份新源码尚待远端执行。

[逐次模型发送门](sdk-model-send-gate.md)位于真实 AgentLoop，每次首发、恢复重试
和后续模型步都在提交 sent 预算前检查。拒绝、异常和取消保原账，机密失败串不外泄。
它尚未绑定 Policy 或原发起者；摘要、Compact、采样与后端内部网络重试仍不受此门管。

[Managed 新场底层准入](sdk-managed-session-admission.md)把真正锁和原持久回执
整束交给 Manager，首条 V3 写完整创建身份。失败和 Close 后均禁 Local 回落；
候选先收 Writer 和附属写帽，再放锁，非空残账继续保留。现有 Local 场拒隐式转换。
这笔只完成内部存储链；公开 Managed SDK、执行授权、同 ID 恢复与旧账迁入仍未交付。

## 八阶段现状

| 阶段 | 已合范围 | 接下来补什么 |
| --- | --- | --- |
| 收口当前批 | Worker #245、Runner #246、SDK #325、Job 日志 #332、Web／可选 Lua 等组合 #333 已合功能分支；旧子 PR 已收口 | Jobs／不可变结果组合在已有 CI 分支验收；当前只留总 Draft #234 |
| SDK 完整化 | 五件原内置工具、有界 web_fetch 和 Todo；显式 Skills；项目 Memory Recall/Save 和片段 CAS；前台深度一子 Agent；主 Action；strict standalone Lua；Package 清单分析与根 inventory；公开 RAG 参考例 | 其余 CLI 工具、自动 Memory、Skills 管理、Package 挂载、后台与嵌套子任务、公开 Job、CLI 主入口迁移 |
| 四口 SPI | Session 真接 EventSink；召回片段真接 Memory Blob provider；公开 PolicyProvider 原语 | 完整 JournalStore/BlobStore，替换结果与恢复读写，接真实 Session 授权；本阶段不引数据库 |
| 依赖瘦身 | updater、Release 查询、渠道、Gateway 留宿主；Package 只带中立 parser；Lua 可从 SDK 依赖闭包关闭，ON/OFF 三平台均已验收并合入 | 继续检查依赖闭包与端侧最小装配 |
| 身份与治理 | 身份值、授权 action、可撤销 PolicyProvider 和订阅合同 | 新托管会话 ownership、执行前重查、恢复归属、查询与审批隔离；旧账迁入另批 |
| 公开服务 | AppServer 内部网页、WebSocket；本地可信 Worker IPC | 正式 Worker 协议、HTTP/SSE、持久 outbox/ACK 和游标；gRPC 按需 |
| 分布式与存储 | 本地 Worker；独立实验 Runner；两端部署合同 | 网络登记、心跳、鉴权、存储与队列适配；池化和 Sandbox 池各守前置门 |
| 扩展与编排 | 可信 C++ 窄中间件；前台子 Agent 与主 Action | 动态库、热卸载、沙箱、工作流与多 Agent 的公开装配 |

上述范围只算已经合入功能分支。接口存在不等于真实执行已接管，私有候选通过也不等于交付。GPU 占用和网络连通交用户处理；同项目可开多场，不能把 Session 当成独占工作目录。

## 已合基线

[#325 原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357534) 和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37149357428) 均过。六套 SDK 各有 28 场安装移位消费、48 册 focused、414 条原生 CASE；全量 Linux 744、Windows 746、macOS 747；ASan 实跑 147 册，54 册重点来源通过。

859 件材料逐大小与 SHA256 复核，封账摘要为 `e82c9e15e01e0150287bb03ebb1c10f2326a6038a323fd3f5e450b7c98822278`。全量结论取实际日志与保留原件；并未上传每册完整 JUnit/LastTest。LSan 关闭，浏览器和 TSan 按路径跳过。SDK 插桩不能替 Worker/Runner 插桩。

## 已收口：#332

[#332](https://github.com/relic-yuexi/LubanCode/pull/332) 源 `0066ac3e` 已合进功能分支 `6e2702b2`。它补[真实 Job 来源绑定](sdk-job-operation-binding-v1.md)与 [V3 日志回执](v3-journal-witness.md)，并同步 main `3b973ffe`。Job 历史只读 PassiveHold；首次未确认追加和首次 checked Close 持值，不靠重复 Close 改口。这批仍属内部前置，尚未开放后台 Job 或完整 JournalStore。

本源[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915212)和[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37276915234)整轮均过。六套 SDK 各 50 册 focused／426 CASE／28 场安装移位消费；全量 Linux 748、Windows 750、macOS 751；ASan 实跑 149 册、56 册重点门、1742 CASE／65795 断言。559 件材料逐件复核，封账摘要 `b4618090fd1026b3c538de9a4dbb5995f7402e4a202ec4525d9865c7778a473c`。scoped PowerShell 包装器三枚 cmdlet 改用模块限定名，原管道、格式、错误流和退出码保住；原第六案末补 legacy/scoped 对象表格逐字对照。Windows 两套 focused exact 实耗 2242／2299ms，两份实际对象表格均 76 字节且逐字相等；全量双源码也保登记、JUnit 与 LastTest。审查、评论与线程无待办，合入前重查源头和目标未漂移。

前源 `52fa7d6d` 的[原生 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651084)整轮失败；[文档 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37250651072)通过。它留下这些实际结果：

- 三平台全量 Linux 748、Windows 750、macOS 751 与 ASan 149/56 均过。新 Job/V3 两册各六案保实际 argv、非零断言与原始日志。
- 五套 SDK focused 通过；Windows SDK-only 50 册中 49 过、一册失败。六套安装移位消费各 28 项通过。
- 失败册 `sdk.focused.run_command_execution_limits` 六案五过、一案失败。`powershell-exact` 实际登记 `RUN_SERIAL=true`，仍等到 15054ms 超时。同次 shell-entry、wrapper-ready 确认；user-block-entry 未确认，probe 起步和结束记录均缺。原帽仍为 15000ms，未加时限、预热或重跑求绿。
- 监督器五案通过。健康拍夹具只由手动拍驱动，消除背景拍先改状态、稍后入通知时断言抢跑；生产监督器未改。
- 失败原件已封 568 件，摘要 `96ce13da2b5e4d8131638ec2dc9372f7c3a90064ac1f6ea6b2a96462773fec60`。Windows 全量另保完整实际登记、JUnit 和 LastTest；ASan 保 raw File API、149 册实跑与 56 册重点门。

入口观测只收窄待查区间，尚不能断唯一起因。全量较后通过不能盖掉这次失败。新源修复已单独验收，失败原件继续封存。

## 已收口：#333

这批保留 #332 的 `0066ac3e` 窄修复，接入
[有界 web_fetch](sdk-web-fetch.md)。宿主显式选择工具并冻结每场上限；默认不开放。
公开配置与能力快照只含 SDK／标准库值，内部单次请求 Transport 留替换口。
模型只能收窄预算，Close 等请求真退出；不支持 gzip 时明确拒绝编码，不添解压依赖。
原 CLI 19 案保住，另补两案；SDK 六案和安装消费核实际 HTTP、预算、取消与四场隔离，
HTTP 夹具另核线程退净。公开 Todo、Package 根 inventory、可关闭 Lua 与安装后的 RAG 参考例同批收口。

源 `c522a51c` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37309659950) 15 项全过，[#333](https://github.com/relic-yuexi/LubanCode/pull/333) 已正常合成 `fd76b6a5`，合入树精确相同。六套 Lua ON 各 59 来源／486 CASE／34 场安装消费；三套 OFF 各 57 来源／471 CASE／31 场消费。全量 Linux 766、Windows 768、macOS 769；ASan 实跑 158 来源、65 重点门、1802 CASE／69955 断言；TSan 十四场 workflow。九套 Web、RAG 与原始图均核过实际执行，三份跨镜像包各跑五笔命令。1061 件原件逐大小、来源与 SHA256 复核，摘要 `c0ac5d8b6cde2c1c8441e13629aed29f2bf77733ac253c2ede6d52227d851b53`。LSan 关闭；Linux/macOS 全量以真实 job 完成日志为据，没有逐来源完整 JUnit。这批绿灯不借给新 Jobs／不可变结果／SPI。

首组合 `92f93572` 的[远程 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37284807084)在重复 Location 安装消费报普通网络错，macOS 33／34 通过；HTTP 第 21 笔后停止，服务正常退净。原日志未记底层 CPR 错误码。查 curl 源码，部分版本会在第二枚头进入回调前拒绝协议；这条差异能解释现象，不能据此断定唯一成因。

窄修只保底层实际失败状态，并将 `NetworkFailed`、CPR `WEIRD_SERVER_REPLY` 和真实跳转状态同时成立时归为 `redirect_invalid`。没有状态、304、普通状态和其它网络码不套；取消、时限与字节帽仍先判。不解析错误文案，不拿残留响应跟随跳转。原案、HTTP 夹具和预算照留，23 项纯门、文档与独立源码复核通过。新组合 `57d70c91` 已同步功能分支，[本源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37288325660)整轮失败：Windows combined 四场生命周期案撞 Memory 活锁；Web 九套原生与安装消费均核过实际 30 笔 HTTP 和正常退场。

本源预期 ON 六套各 55 册 focused／458 CASE／34 场消费，OFF 三套各
53 册／443 CASE／31 场消费；全量 Linux 758、Windows 760、macOS 761；
ASan 154 册实跑、61 册重点来源及 19 份支持件。预期不能代替实际原件。

私有源 `37219c98` 的[整轮 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37245678268) 已通过，仍未合入功能分支。

| 模块 | 合同与已验范围 |
| --- | --- |
| [Package 盘点](sdk-package-inventory.md) | 显式根、有界目录与文件清单；不挂载、不执行、不替宿主授信 |
| Lua ON/OFF | 默认 ON；OFF 真去编译依赖；三平台跨画像恢复，在读取所选 Lua 脚本、创建 VM 和调用模型前拒绝启用的 Lua 来源 |
| [主场 Todo](sdk-todo-write.md) | 每场自持清单，整表替换；恢复保历史、重新起空表；CLI 提醒与展示留宿主 |
| [Agentic RAG 参考例](sdk-agentic-rag-example.md) | 仅公开 SDK 和标准库；宿主注入检索工具，真实模型工具回环；不增核心检索 API 或向量库 |

ON 六套各 54 册 focused、452 条原生 CASE、33 项安装消费；OFF 三套各 52 册、437 条 CASE、30 项消费。三平台跨画像各五条真实命令；九套独立 RAG 示例从四份复制源码单独构建和执行。全量 Linux 756、Windows 758、macOS 759；ASan 实跑 153 册，60 册重点来源通过，另核 18 份附件和实际 PCH。

876 件材料已封，摘要 `d1ec767ac5539b1b0c58fe6081f44db582fa29ac99f91a9bf89c03d109ab0264`。本地旧组合 `640c0f71` 只另加监督器夹具修复。上面的新组合还接了 scoped PowerShell 修复、Web 工具和验收门；不得借 `37219c98` 绿灯验收。

操作日志退场、ResultPolicy 开场锁与 Managed ownership 另留私有前置；真实 Managed 授权尚未接管。[后台 Job 合同](sdk-background-jobs.md)先定取消、许可和 Close 次序，尚未公开执行。

本地下一组合已收入[Owned 登记截止](sdk-owned-job-deadline.md)：正预算从真实登记追加起算，排队、采用和授权复核不重开时长；晚入线程保 Started 意图，实记命令未调用，不编业务 raw 或 Post。到期观察携真实父交付五引用；Hold 能核回未派发便取消的已确认交付，伪引用、错序和矛盾标记拒绝。旧零预算与 Legacy 函数保原行为，恢复仍只读，不重建执行截止。

这笔只在实际调用前收窄原有相对 process timeout；平台启动与 kill／capture／join 照原语义，不承诺截止那刻已退净。六条真实原生路径核登记／排队、授权耗时、运行预算、晚线程、启动抛错／Close，以及严格恢复。CI 另核完整 argv、六案、非零断言和 owned 终态事实；没有 command 就不能编 timeout，已调用就须有正预算，配额与线程必须真退净。三平台 SDK ON 预期各 56 册／464 CASE，OFF 各 54／449；安装消费仍为 34／31；全量预期 760／762／763，ASan 155 册／62 重点来源／19 支持件。源码与纯门已查，这份组合尚未完成原生验收。公共 SDK Job 仍未开启。

`6900159b` [首轮远端](https://github.com/relic-yuexi/LubanCode/actions/runs/37291725260)已失败，暴露两项接线漏处：Lua 画像纯册仍写旧名册 55，实际已增至 56；截止案两行 `REQUIRE_MESSAGE` 消息含裸条件式，编译报错。下一组合更新名册、只给条件式添括号；六案、预算及业务行为不改。三套 combined 和 ASan 均未执行 focused 或全量原生案，不能记作截止逻辑已通过；三套 combined 安装消费各 34 项及独立 RAG 示例实际通过。

下一组合还接[Managed 开场预留](managed-opening-reservation.md)与[归属侧记](managed-session-ownership.md)。内部 owner 先原子新建空目录、拿真实 SessionLock、缓存本次首笔耐久发布，再逐字重核归属，最后才建正文子目录。未知发布与失败残留留原样，不借重试补成已确认。LocalTrusted 开场、候选、祖先、管理与删除路径拒绝有标记或归属不明的账。原始 SDK 预读与完整 Policy 尚未接管，不能称公开 Managed 已交付。两册分别六案、十案，真实锁、未知发布、旧本地恢复、晚标记与祖先拒绝均须远端验收。

同批还接[Memory 项目交接](memory-project-commit-handoff.md)。`57d70c91` Windows 四场原件显示同项目第二场归属正确，却撞活锁而未写；原件没记锁释放，不能唯一认定慢盘或残锁。补同一实现场内的逐项目可取消队列，整段写入仍须拿原 OwnerLock，先放磁盘锁再交棒。异项目各自前进；坏锁照拒；等待取消、回调异常和同线程重入均不得悬票。原 20×100ms 外部重试、OwnerLock 与旧 24 案不变，另加六案核真实写入与线程退净。

这份开场／交接组合预期 ON 各 59 focused／486 CASE，OFF 各 57／471；消费仍 34／31。两册 Managed 与一册交接各注册 original 和 focused，全量较 `6900159b` 增六项，预期 Linux 766、Windows 768、macOS 769；ASan 158 册／65 重点门／19 支持件。这里只列验收名册，实际登记、执行、关场与完整 CI 仍须新源给证据。

`532ed159` 随后在启动前失败。GitHub 报分类步骤“表达式超过 21000 字符”，作业、附件与原生执行均为零。新修把 PR base、push before 两枚提交参数搬至 step 环境变量；分类脚本只读变量，整段不再含 GitHub 插值。路径、选择器、预算与原生案数照旧，另加纯门拦住把模板插值搬回长脚本。须等修正源真正启动并完成三平台验收。

修正源 `a7165eec` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37297587597)已失败。三套 SDK-only 又停在 Todo 纯门：这册仍写 ON 55／OFF 53，实际名册已到 59／57；RAG 同类旧数也须改齐。三套 combined、ASan 与 TSan 均在开场预留两行消息报编译错，字符串拼接也须整段加括号；focused／全量／ASan／TSan 原生均未执行。三平台安装消费各 34 项与独立 RAG 示例实际通过，不能替 focused 记过关。下一源只改两处名册数和两行消息括号，消费、CASE、生产逻辑与预算不变；整套 SDK 纯门本地 282 项通过，没有配置、编译或执行原生程序。

`80c8cd5a` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37299684343)已越过三套 SDK-only 数据门。macOS 编出 SDK 测试程序后，编译来源边界门拒绝三册新 shared 测试：CMake 已接入，独立白名单仍缺 Managed ownership、开场预留与 Memory 交接。`81ed65f2` 补这三条精确路径并[重跑远端](https://github.com/relic-yuexi/LubanCode/actions/runs/37302239817)；testing ON 且唯属 SDK reference target 才放行，testing OFF、其它 target 和近名新增来源照拒。边界、依赖闭包及开场／交接纯门 116 项通过。

同源 `80c8cd5a` Linux combined focused 59 源／486 CASE 实际通过；macOS 59 源中57源通过，Memory 交接与 SDK memory_save 两源失败。Managed 归属六案、开场预留十案和后台 deadline 六案在这两平台均实过。macOS 新项目的 memory 末段尚未建，旧 equivalent 错误分类令异项目两场及新交接两案报身份失败，归属未串场。下一源先核叶目录是否存在，再比 memory 或 workspace 身份，权限与未知错误仍拒；原24案、新六案、预算与磁盘锁不改。六份 ON 安装消费各34项和独立 RAG 示例已过，不能替失败 focused 记全绿；OFF、全量及消毒器继续按本源收原件。这处生产修正须另跑远端。

## 后续装配规矩

存储 SPI 前先补[结果仓不可变发布](result-immutable-publication.md)。File 默认实现用独占临时件和原生 no-replace 发布，正式名碰撞便拒，不能先查 exists 再 rename。逐枚保文件与直接父目录的实际确认；祖先链未确认、部分发布及未知回执照实留账。首次未知封 Store 后续写入，不读回升级；确定零发布的失败可继续。只交内部 File 前置，没有公开 JournalStore／BlobStore 或数据库。旧 AtomicWriteFile、预览／编号算法和原 CASE 保留，另添八案；三平台 focused／全量与 ASan 须在新组合实跑。

结果仓前置现与 Command Jobs 合成同一候选，复用已有 CI 分支验收，不添子 PR。新结果册同时注册 original 与 focused；边界名单只准精确来源，故障注入册共用平台资源锁。两条分类分支、两份 ASan 选择器、首轮内联名册及实际路径／argv／CASE门一并核对；旧结果仓资源锁仍在原分支，新锁不遭后一次赋值覆盖。源码与纯门通过，原生尚待这份组合实跑。

每项先写短合同：公开什么，谁持资源，取消怎样传，关场怎样等借用退出，恢复依据哪份事实。只交实际接通的一段，不先铺一排空接口。

工具、模型、存储、事件和策略各留窄 seam。宿主显式配置能力，Session 冻结本场配置；关闭等在途调用真退出，未知回执保未知。未来网络、数据库和插件实现接这些 seam，不能让核心依赖具体 UI 或基础设施。

默认只出 preview。Full 须 Node 许可和每场参数同时开启。子 Agent 默认关闭，宿主显式给预算，许可只管当前子场。

保持一张实施 PR，收完再开下一笔；功能分支到 main 继续走 Draft [#234](https://github.com/relic-yuexi/LubanCode/pull/234)。用户已授权自行收口，仍须逐源审查和远端三平台 CI。源码、Git、纯数据和文档可本地检查；配置、编译、CTest 与原生执行全交远端。

## Command Jobs 接线候选

公开源码封于 `2d582d03`，这批从 `c522a51c` 合入并补 CI 接线，尚未合功能分支，也没有本组合远端通过证据。默认不开 Job；显式预算与真正 run_command 声明接到同一 Session owner。已接原父调用五引用、真实 Job Operation binding、只管本 Job 的审批、唯一 host worker 完成泵、取消与 Close 真 join，以及同 ID、同 run 的 Hold 只读恢复。ReadJobPreview 是可信本地缓存，不能当出站投影。

Jobs 两册17 CASE 加结果仓八案，共增三册25 CASE。合树静态名册为 SDK Lua ON/OFF 62/60 册、511/496 CASE，安装消费35/32场；全量预期 Linux/Windows/macOS 772/774/775。ASan 当前选择式选出重点68册、执行来源161册、wildcard编译正文161份、附件23份；这里列接线范围，不冒充实跑。公开 Blob／Journal SPI 尚未接入。

CI 会留安装/移位消费者原 argv、显式真实 probe 的 File API 归属与字节指纹、公开十路径、私有七路径，以及四份写后未知 typed 原件。注册、JUnit、LastTest 三者须相符，失败也留原件。原 deadline 六 CASE 和 CLI 命令路径不改；本地仅跑纯数据、AST、脚本语法与文档检查。详见 [Command Jobs 合同](sdk-command-jobs.md)。

### 首轮组合实测与窄修

`605d200f` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37312694121)整轮失败，241 件原证冻结，摘要 `defb4ccbc2f70540f1301e21ab4b20f2ceb26184d443db0496e3e8baac9f037b`。六套安装消费停在私有 probe 准备：SDK-only 原本关闭测试，图内没有该 target；Linux/macOS combined 只编 SDK 库，没有 probe 可执行件；Windows combined 图带真实 ZERO_CHECK 再生依赖，遭旧“零依赖”门拒。消费、focused、全量与 OFF 本轮均未执行。

ASan 真跑了 161 册，其中 159 过、两册失败；1827 CASE 中两案败，71195 断言中 11 条败。新 Jobs 公开十案、私有七案和不可变结果八案均实过，四份写后未知回执逐字段保住；TSan 十四册实过。这些局部结果不能验收整树。Lua 恢复坏声明仍遭拒绝，但会话身份先过 Jobs 绑定门，旧夹具硬认 Lua 错误码；子会话捕获夹具堵着旧固定临时名，独占临时件改名后未再触发故障。后源只修[明确预检期待和真实 no-replace 拒绝点](sdk-lua-child-precheck-repair.md)，保原硬断言，不改生产顺序。

probe 后源改用独立、标准库私有夹具，复制原源码，在 CI 临时目录单独配置和编译，再从真实 File API 取实际可执行件并移位。产品测试开关、SDK 闭包和原 CMake 图照旧。配置或编译失败也保有界原 reply；成功后仍严核源码、目标与 artifact 归属。上传的原图先记 `not_evaluated`，不能拿“已保存”充作“已通过”。安装消费还留真实 helper 与 jobs 头文件原字节。

旧 Windows ready 首读失败另补[原件与清场诊断](workspace-racer-ready-evidence.md)：保存首次字节、真实句柄与 argv，失败前收同一只 helper。六 CASE、原前缀断言、锁状态和预算照留；没有重读求绿，也未断言根因。这些后源改动只过源码与纯门，仍须新一轮远端验证。

`a9d3f16e` 的[后源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37320718673) 又抓到两处错误，不能算通过。四条 Linux/macOS 安装消费者实际各 35 项中 34 项过；四场隔离已关场，随后的缺会话恢复错报 `sdk.job.plan_invalid`，违背旧 `sdk.session.open_failed`。Windows 独立探针配置与编译均成功，原始图同时列指定 EXE 和同目录 PDB，旧门把总产物数当可执行件数而拒绝。两份首次失败原件分别冻结 69 件与 29 件；没有补造未生成的消费总验收。

新候选只补[缺失分类](sdk-command-job-missing-resume.md)与[真实产物选择](sdk-command-probe-artifacts.md)。缺 workspace 或明确缺 session 沿旧错误码拒开，已有坏材料仍严查；原 private 案追加八次真实 Runtime 准入反例，旧七 CASE、消费者原断言与预算保留。Windows 精确选指定 EXE，可附同目录同名 PDB，POSIX 保单无后缀产物；生产与上传后复核共用同一门，原图字节不动。新的原生结果仍须独立交远端 CI。

`7a5321c8` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37421453525)整轮失败。唯一失败在 Windows SDK-only ON：focused 62 源中 61 过；Owned deadline 六案五过，438 行想读 `queued`，实际已为 `cancelled`。原六份终态事实仍齐；Windows OFF 未执行。其它五套 ON 各 62 源／511 CASE、两套 OFF 各 60／496、八套安装消费 35／32、三平台全量 772／774／775、ASan 161 来源／1827 CASE／71878 断言及 TSan 十四场 workflow 均过，不能替失败通道验收。1257 件原件共 49246151 字节，逐件复核后冻结，摘要 `6f1a67089a5b601af8b890867ac8db5137d597c787ea30d6c346431be1ab7faa`。首次失败 15 件与原封条 `e48f85c030b8904c61850ec654d9f7a5773afc3b5709cf5de1e1988761c45b05` 继续保留。

后源按[排队观察合同](sdk-owned-queue-observation.md)保住原 2000ms 正预算和全部截止、零调用、父引用、恢复与退场检查。另加零注册预算真票，硬查堵在活进程后排队，再取消退场。正票快照若已取消，还须核 Register 前实际时钟已过最早截止；其它状态照拒。生产、原六 CASE 和预算不变。纯数据门已核，原生仍交新源远端 CI。

## NamedResults 接线候选

[named 结果合同](sdk-named-result-blobs.md)先于实现封存。源码 `1a92b2ca` 接通一个 Session 所有 named 结果入口：原始捕获、正式结果、Job 准入、Job 完成和列表材料共用 Store、编号与整份写 lease。宿主显式提供 Provider；默认仍用 File。结果投影、读面与同 ID 恢复沿真实句柄取材料，不造本地镜像。外部仓只认本场冻结 namespace，不能替 Session 授权，也不能兼任 Memory CAS 或 Journal。

写入首次未知便封后续写。确定尚未调用 Provider 的失败另记；原生 File 确认和宿主声明分开留值。Close 等在途执行和写借用退出，只读句柄仍可存活，不能拉住 Writer 或 SessionService。Runtime 开场离开注册锁，Shutdown 等所有已准入开场完成或退场；Provider 回调递归读和阻塞生命周期调用在拿锁前拒绝。

独立源码审查已核 37 条变更与 767 份旧原生测试锚，未查出阻塞项；File 旧流读、预览、编号与异常语义保留。公开十案用安装 SDK/STL 与真实第二根字节，私有八案再核原生回执、开场身份、未知持值和退场。自动摘要另保真实 V3 证据与后续模型请求确认；安装消费者没有内部主账读面，不能冒称同样检查。

这份接线合入 `5498f954` 排队观察修正，旧预算与终态断言保留。安装消费多一场 named-results，SDK focused 多两册、18 CASE；新名册须逐项核 actual argv、JUnit、LastTest、公开头／helper 原字节和编译归属。此处只记源码候选，尚未合功能分支；整套三平台和 ASan 要在新源远端 CI 实跑，旧绿灯不借给它。

集成源 `bb9fb3f1` 的[远端 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37429745834)整轮失败。八条原生构建均停在 `named_result_blobs.cpp:26`：`string_view` 不能隐式转给只收 `const string&` 的 UTF-8 检查。消费者、focused、全量、NamedResults、ASan 和 TSan 均未执行；ASan 名册 163／70／25 只是计划。219 件失败原件共 4713248 字节已封，摘要 `9076087630e461d7643b96e53fc4516ef8e268a11f76f371d8f45787401a9776`。

窄修 `68e1fcf2` 只加显式 `std::string(value)`，其余 3036 份文件未动，源码与实际接口逐字核过。[新源 CI](https://github.com/relic-yuexi/LubanCode/actions/runs/37431764520)复用原分支，旧轮结束并封账后正常快进。它已越过这处编译错误，整轮仍失败；实际范围见本页“当前批”。未添实施 PR，未在本地编译。

## Journal 所有权接线候选

[主场 Journal 合同](sdk-journal-owner.md)先于实现封存。当前先归拢真实 V3 File 账，保旧 JournalWriter 和 File anchor。AppendLease 独持共享状态，原生追加与内存更新共守写门；首次未知先封、再解锁、最后退引用。原生已提交而内存更新抛错，另记 semantic unknown，不改称尚未写入。读句柄只持捕获字节和原生文件锚，不能吊住 Session 或 Writer。

同 ID 恢复在真实 SessionLock 内只捕获一份材料，投影与 continuation 共用它，严核原对象、完整前缀与 EOF。旧 RecoveryView 字符串接口尚多留一份主账字节；原 128MiB 帽管逐份读取，没冒称总驻留上限。SDK 各项 Prepare/restore 尚未全归这处读面，公开 JournalProvider 要等这条线收齐。本阶段不引数据库。

源码 `9093af67` 已闭合独审抓出的两处夹具错误：doctest 消息三元式加括号；一次真实工具调用按 action/attempt 分组，恰核 raw capture 与 formal 两份，恢复和关场后逐原身份、元数据与字节比对。原七案内部验收、三场 SDK/STL 宿主验收、一场私有主账核验留着。独审只算源码通过，尚无远端原生结论。

这批正接编译归属、安装移位、ASan 与同次全量原件。宿主先开场、跑完整回环、Close，再换 Runtime 恢复同一 ID，核零重放，再跑一回；同项目另开一场核历史与执行不串。受控验收账、原 raw/formal 文件随失败也保存，保存不等于通过。公开 SDK 不添内部 Writer ABI，完整 canonical/hash/Prepared chain 仍交实际原生 guard 验证。
