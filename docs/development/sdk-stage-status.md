# SDK 阶段进度与验收边界

核查日期：2026-10-05。已合基线为功能分支 `6e2702b2`；[#332](https://github.com/relic-yuexi/LubanCode/pull/332) 的已验源 `0066ac3e` 与合入头同树。

目标：新宿主只调用公开 SDK，便能运行完整会话，不必复制内部运行栈。CLI 逐项迁入，原功能保住。现已能嵌入；CLI 主入口和 one-shot 仍用内部装配，尚未全部对齐。

## 八阶段现状

| 阶段 | 已合范围 | 接下来补什么 |
| --- | --- | --- |
| 收口当前批 | Worker #245、Runner #246、SDK #325、Job 日志 #332 已合功能分支；旧子 PR 已收口 | 下一笔 Web／可选 Lua 等组合在已有 CI 分支验收，未添实施 PR |
| SDK 完整化 | 五件内置工具；显式 Skills；项目 Memory Recall/Save 和片段 CAS；前台深度一子 Agent；主 Action；strict standalone Lua；Package 清单分析 | 其余 CLI 工具、自动 Memory、Skills 管理、Package 挂载、后台与嵌套子任务、公开 Job、CLI 主入口迁移 |
| 四口 SPI | Session 真接 EventSink；召回片段真接 Memory Blob provider；公开 PolicyProvider 原语 | 完整 JournalStore/BlobStore，替换结果与恢复读写，接真实 Session 授权；本阶段不引数据库 |
| 依赖瘦身 | updater、Release 查询、渠道、Gateway 留宿主；Package 只带中立 parser | 可关闭 Lua 的编译画像已在私有候选通过，仍待合入；继续检查依赖闭包 |
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

## 下一笔候选

当前组合已保留 #332 的 `0066ac3e` 窄修复，接入
[有界 web_fetch](sdk-web-fetch.md)。宿主显式选择工具并冻结每场上限；默认不开放。
公开配置与能力快照只含 SDK／标准库值，内部单次请求 Transport 留替换口。
模型只能收窄预算，Close 等请求真退出；不支持 gzip 时明确拒绝编码，不添解压依赖。
原 CLI 19 案保住，另补两案；SDK 六案和安装消费核实际 HTTP、预算、取消与四场隔离，
HTTP 夹具另核线程退净。源码和纯数据已查，这份新组合仍待自己的远程 CI。

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

ASan 真跑了 161 册，其中 159 过、两册失败；1827 CASE 中两案败，71195 断言中 11 条败。新 Jobs 公开十案、私有七案和不可变结果八案均实过，四份写后未知回执逐字段保住；TSan 十四册实过。这些局部结果不能验收整树。Lua 恢复坏声明仍遭拒绝，但会话身份先过 Jobs 绑定门，旧夹具硬认 Lua 错误码；子会话捕获夹具堵着旧固定临时名，独占临时件改名后未再触发故障。后源只修明确预检期待和真实 no-replace 拒绝点，保原硬断言，不改生产顺序。

probe 后源改用独立、标准库私有夹具，复制原源码，在 CI 临时目录单独配置和编译，再从真实 File API 取实际可执行件并移位。产品测试开关、SDK 闭包和原 CMake 图照旧。配置或编译失败也保有界原 reply；成功后仍严核源码、目标与 artifact 归属。上传的原图先记 `not_evaluated`，不能拿“已保存”充作“已通过”。安装消费还留真实 helper 与 jobs 头文件原字节。

旧 Windows ready 首读失败另补[原件与清场诊断](workspace-racer-ready-evidence.md)：保存首次字节、真实句柄与 argv，失败前收同一只 helper。六 CASE、原前缀断言、锁状态和预算照留；没有重读求绿，也未断言根因。这些后源改动只过源码与纯门，仍须新一轮远端验证。
