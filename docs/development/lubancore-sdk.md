# LubanCore C++ SDK（实验版）

本批提供真实会话闭环。`Runtime` 协调活会话关场，`Session` 句柄持执行资源，经现有
`SessionService` 接纳输入，再用 `Agent`、轨迹桥和工具表跑回合。
它不启动 CLI 或 AppServer 子进程。

## 拆分进度

本页记录已交付的窄 SDK。SDK-only 默认构建、普通安装、终端依赖拆分、SDK 与
AppServer 共用资源拥有、CLI 显式工具装配，以及四路回合临时接线已验收。
SDK 与 AppServer 共用会话执行对象，多会话隔离与寿命验收也已通过。第一阶段收工。
可信 C++ 扩展已由 [PR #255](https://github.com/relic-yuexi/LubanCode/pull/255) 交付，
沿用现有中间件核；每场实例、冻结装配、真实上下文接纳和整场卸载均已验收。
公开安装头为 `api.hpp`、`core.hpp`、`extensions.hpp`、`results.hpp`。
持久工具结果查询与双层投影已由 [#257](https://github.com/relic-yuexi/LubanCode/pull/257)
合入功能分支，本批远端 CI 已过。Worker [#245](https://github.com/relic-yuexi/LubanCode/pull/245)
已接公开接口，三平台各验真实搬迁进程 18 场，已合入功能分支 `a151d8b4`。
`LubanCore::Core` 是公开安装目标；仓库内部旧名
`lubancode_core` 仍带 CLI 实现，两者不能混用。

依赖瘦身首笔把 updater 与 miniz 归到 CLI 宿主目标；SDK-only 不定义它们，
组合构建也核 SDK 实际传递闭包。本笔仍待远端三平台验收，合同见
[更新器归宿主](sdk-updater-boundary.md)。渠道、Gateway、Lua 尚未拆完。

后端、基础工具与 MCP 已共用 `runtime/assembly`。SDK 与 AppServer 共用执行对象，
各自保留受理与排队；CLI、one-shot 仍用原会话栈。完整工具、插件、Hook 和记忆等
能力尚未统一迁入公开 SDK。
新增宿主可用下文 API，现有 CLI 迁移须保留原功能，不能靠删去功能来完成拆分。
验收清单见 [SDK 拆分计划](../../todos/LubanCore与CLI分离_核心库独立成宿主底座设计.todo)。

SDK 与 AppServer 已调用同一只 `SessionResources` 工厂，管理 backend、MCP 连接和
工具表。各宿主先交明确计划，再创建资源；失败候选按拥有次序释放。工具表先销毁，
被工具借用的 MCP 连接随后释放，后端最后释放。SDK 的必需组件策略与 AppServer 的
可选组件降级策略各自保留。AppServer 跨轮持同一 Agent，并在同 ID 恢复时灌入历史。

AppServer 停场超时会回 `thread.stop_pending`，保留会话账，拒收新操作；重试后等
执行线程退出，再封账并移除记录。封账失败回 `thread.close_failed`。最终 Shutdown
先取消所有回合、审批和浏览器任务，再等待借用退出；进程内回调仍须合作取消。

SDK、AppServer、CLI 与无界面执行器共用 `ScopedTurnBindings`。每轮保存旧接线，
挂本轮轨迹、投影与路由号；返回、取消或抛错时恢复旧接线。CLI Stop 续跑仍属同一轮。
清掉本轮借用时，长期 inbox、压力与 Soul 回调照常保留，已落下的执行事实也照常保留。

第一阶段已过这条线：新宿主只调公开 SDK，就能跑完整会话，不必复制内部运行栈。
`SessionExecution` 归拢资源、Agent、恢复历史与销毁次序，
由 `SessionService` 持有，SDK 与 AppServer 共用。显式空历史仍走恢复，装配失败不替换
旧执行对象；候选构造和析构都放在会话锁外。宿主各自决定受理、排队与恢复策略。
新增安装消费模式 `isolation` 在同一项目开两场，在不同项目各开一场，核模型、权限、
取消、关闭与活回调。三平台、SDK-only 与 ASan 新提交证据见下文 #253 记录。
共同 cwd 不加项目独占锁；共享文件冲突仍由宿主协调。

内部 `SessionExecution` 将 `AgentProfile` 复制给 Agent，再清源档案中的回调与 resolver。
失败也清源回调，模型等值数据留给宿主。关场须清掉每份回调，不能假定
`std::function` 移动后源就为空。Close 清理不占 Session 查询锁，可重入 `ReadOperation`
等非阻塞查询；后端、工具表仍须活着。建场仍持 Runtime 初始化锁，析构不得重入
`OpenSession`；清理中调用 `Close`、`WaitResult`、`Shutdown` 会报 `sdk.lifecycle.reentrant`。

SDK 建场时留住调用方交来的后端，再准备工具与资源。MCP 启动或恢复失败，先收工具
和回调，再放后端。后端析构也守生命周期重入门；阻塞关场请求明报
`sdk.lifecycle.reentrant`。后端引用只在初始化栈与本场资源间流转，不跨场共享。
前置参数无效、MCP 启动失败和恢复目标缺失各有公开消费回归。

关场先拒新活、唤醒全部审批，再广播取消，等主回合与进程内后台回调真正退出。
任务账面终态不代替线程退出。退出后才封账、释放 Agent、工具表、MCP 与后端；
Lua 宿主也须活到借用收尽。补齐恢复时缺失工具消息，不把已取消任务重排执行。
内部 Async 关停回归使用显式注入回调；公开 SDK 仍只开放前台命令，不据此声称
远端 Worker、独立 Runner 或公开后台任务已交付。

CLI 完整工具装配现接内部 `ToolAssemblyPlan`。交互模式与 one-shot 都先由
`ResolveCliToolAssemblyPlan` 折好 cwd、插件目录、信任账、Package 数据根和 PTC
画像路径，再调用原 `ToolRuntime`。缺席目录不退回主目录；静态路径须为绝对路径，
校验发生在起 MCP、扫描插件或创建目录之前。动态 Agent 根在每次扫描前校验。
MCP 固定使用启动 cwd，之后切换插件
cwd 不会搬动已运行的 MCP 进程。

`ToolRuntime` 保留 main/sub/explore 工具表及原资源拥有关系。backend 须活得比工具久；
技能清单按值留给解析器；权限、Agent 扫描根、Package 快照等供应商由宿主显式提供。
CLI 供应商仍按派发时刻读取当前权限与项目，保留切目录和改权限行为。
Agent 类型清单与按名派发现在同取当前 Package 快照，补上旧清单漏列包内 Agent 的缺口。

装配诊断保存 `code`、来源阶段、组件、参数与主/子表归属。CLI 在原发生点同步翻译、
上色和显示；子表重复报告只入诊断，不重复打印。展示 sink 只在构造期间调用，不保留。
构造中途抛错仍走原异常边界，不能从未构造成功的对象再取诊断。

这是内部宿主边界拆分。完整工具装配尚未进入公开 SDK；Package 环境变量展开、
工具内部状态根与进程环境访问仍沿用各自合同，不能据此声称整个工具图已无全局依赖。

## 构建与安装

搜索按场显式启用：`builtin_tools = {"search"}`。它复用 CLI 的 SearchTool，
保留 grep/glob、输出上限与取消；参数中省去、置空或清空 `path`，都落到本场 cwd。
相对路径也从本场 cwd 起算。绝对路径仍属可信本地工具，本批不加文件沙箱。
建场先检查 `<resource_root>/libexec/rg[.exe]`，过版本门后才启动 MCP 和模型。
SDK 不从 HOME、PATH 或宿主程序旁找替代品。合同见
[内置搜索接入](lubancore-builtin-search.md)。

要随 SDK 安装搜索后端，先按仓库 manifest 校验资源，再显式交给 CMake。
下面以 Linux x64 为例；macOS ARM64 用 `macos-arm64`，Windows x64 用 `windows-x64`。

```sh
bash scripts/fetch_ripgrep.sh --target rg-stage --platform linux-x64 --cache rg-cache
```

独立 SDK 构建须同时关闭 CLI、启用 SDK。下面先关闭测试，走默认 `ALL` 和普通安装，
不指定构建目标，也不筛安装组件。`sdk-prefix` 可换成所需安装目录。

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DLUBANCODE_BUILD_CLI=OFF -DLUBANCODE_BUILD_SDK=ON -DBUILD_TESTING=OFF -DLUBANCODE_BUNDLED_RG_DIR="$PWD/rg-stage/libexec"
cmake --build build-sdk --config Release --parallel 4
cmake --install build-sdk --config Release --prefix sdk-prefix
```

这套配置不定义 CLI、`lubancode_core`、`lubancode_app`、终端资源复制或 CLI 测试目标。
内部仍构建 runtime/engine 与其依赖；渠道、Gateway、updater、Lua 等旧层尚未细拆，
当前安装包不等于最小依赖包。CLI 默认开启；CLI 与 SDK 同时关闭会明确报错。
同时构建 CLI 与 SDK 时，可保留 CLI 默认值，按 `--component LubanCore` 单独安装 SDK。

要跑 SDK 专项，打开同一构建目录中的测试开关，再构建默认 `ALL`：

```sh
cmake -S . -B build-sdk -DLUBANCODE_BUILD_CLI=OFF -DLUBANCODE_BUILD_SDK=ON -DBUILD_TESTING=ON
cmake --build build-sdk --config Release --parallel 4
ctest --test-dir build-sdk -C Release -L sdk-focused --output-on-failure --no-tests=error
```

消费方只需 `find_package(LubanCore CONFIG REQUIRED)`，链接 `LubanCore::Core`，
包含 `<lubancore/core.hpp>`。公开头仅用标准库，不要求内部 `src` 或第三方头。
`LubanCore_RESOURCE_DIR` 指向安装后资源目录。验收程序见 `examples/sdk-consumer`。
宿主把此目录显式填入 `RuntimeOptions.resource_root`。搬迁整个安装目录后，资源根也跟着
更新；组件安装与 SDK-only 普通安装都会携带 rg、MIT 许可和固定版本 manifest。
未交 rg stage 时，SDK 仍可用于其他工具；启用 search 会在建场时报缺件。

当前输出共享库。C++23 编译器、标准库、编译配置须匹配；尚无跨工具链 ABI 承诺。
Windows SDK 构建统一使用动态 CRT（Release `/MD`、Debug `/MDd`），消费方也须相同。
默认关闭 SDK 时，原 CLI 发布构建仍沿用静态 CRT。运行时须能找到安装目录下共享库；
Windows 将 `bin` 加入 PATH，Linux 使用库搜索路径或应用自身 RPATH。

集成提交 `22e7ce1f` 已合入 PR #248。其验收头为 `d92c99e9`，见
[CI 36419112569](https://github.com/relic-yuexi/LubanCode/actions/runs/36419112569)：
Linux、macOS、Windows 均跑过 testing OFF/ON 默认构建、普通全安装、仓库外搬迁消费
5/5 与 SDK 专项 4/4；默认 CLI 全量分别为 654/654、657/657、656/656。
这些证据覆盖当时 SDK-only 与终端拆分，不替后续 SessionFactory 或宿主迁移验收。

随后 PR #249 与 #251 分别经 `a7fd7248`、`7f3cf7cb` 合入。#251 验收头为
`12686fa1`，见 [CI 36462878177](https://github.com/relic-yuexi/LubanCode/actions/runs/36462878177)：
三平台安装消费各 5/5、SDK 专项各 6/6、宿主兼容各 6/6；默认 CLI 全量 Linux
659/659、macOS 662/662、Windows 661/661，ASan 选择集 99/99。ASan 包含本批工具
装配与延迟工具、上批会话寿命和浏览器取消回归；LeakSanitizer 未开启，Playwright
浏览器验收未执行，TSan 按条件跳过。后续改动须另验新提交。

PR #252 验收头为 `7913fd30`，经 `9aa63142` 合入，见
[CI 36511121045](https://github.com/relic-yuexi/LubanCode/actions/runs/36511121045)：
三平台 SDK-only 与组合构建的消费测试各 5/5、SDK 专项各 7/7，宿主兼容各 7/7。
新临时接线册各跑原生 9 例，真实 CLI Stop 册各跑 1 例。全量 Linux 662/662、
macOS 665/665、Windows 664/664；ASan 101/101，六册寿命回归均核实际非零用例。
实际合并内容与受测内容一致；LSan、Playwright 与条件 TSan 限制沿用上文。
这些证据只覆盖 #252；后续会话执行对象与隔离测试另验。

PR #253 验收头为 `54c164c2`，经 `180fd160` 合入功能分支，见
[CI 36769261243](https://github.com/relic-yuexi/LubanCode/actions/runs/36769261243)：
三平台 SDK-only 与组合构建的安装消费各 6/6、SDK 专项各 8/8，宿主兼容各 7/7。
六组会话执行册均跑原生 11/11 例、366/366 条断言；三平台 AppServer/Lua 册各跑
6/6 例、513/513 条断言。公开消费实际跑过四场隔离、三条建场失败、后续健康建场、
小回调退场和同 ID 恢复；完整日志、JUnit 与登记清单均已核对，无 CTest 跳过或漏跑。

默认构建全量 Linux 664/664、macOS 667/667、Windows 666/666；ASan 103/103，
八册必需寿命回归均跑出非零原生用例。ASan 覆盖内部会话、Agent、工具与宿主寿命，
不含独立安装消费程序。LSan 未开启，Playwright 未安装，TSan 按条件跳过。
Ubuntu 24.04、Debian 11、Debian 12 烟测通过。受测合并提交 `41591788` 与实际合并
`180fd160` 同为树 `02ee0cbc`。这轮只验第一阶段窄 SDK，未含后续公开 C++ 扩展、
后台命令与远端 Worker。内部 Async 线程创建失败欠账仍见[总欠账单](../../todos/欠账与观察清单.todo)。

PR #255 验收头为 `9a965a7f`，经 `57745632` 合入功能分支，见
[CI 36828070978](https://github.com/relic-yuexi/LubanCode/actions/runs/36828070978) 与
[文档检查 36828070966](https://github.com/relic-yuexi/LubanCode/actions/runs/36828070966)：
三平台 SDK-only 与组合构建共六组，安装消费各 7/7、SDK 专项各 9/9。
SDK-only 走 testing OFF/ON 默认构建和全安装；组合构建另核 LubanCore 组件安装。
三份公开头均已核齐，外部消费只用安装产物。六组扩展册各跑原生 12/12 例、
591/591 条断言；会话执行册仍为 11/11 例、366/366 条断言。安装消费真实核过
拦截链、同目录两场、同身份恢复、小回调退出、工厂失败清理和健康会话续跑。

三平台宿主专项各 7/7，逐来源共 58 例、1173 条断言，含 AppServer/Lua
6/6 例、513/513 条断言。默认构建全量 Linux 666/666、macOS 669/669、
Windows 668/668，以完整 CI 控制台汇总为证；SDK、宿主与 ASan 则另核逐来源
完整日志、JUnit 和登记清单，不把全量汇总说成逐册原生日志证明。

ASan 105/105，十册必需寿命来源齐全且实跑非零用例；扩展册为 12/12 例、
591/591 条断言，中间件核为 24/24 例、212/212 条断言，未见地址错误诊断。
ASan 不含独立安装消费程序；LeakSanitizer 未开启，Playwright 未安装，TSan
按条件跳过。受测合并提交 `89d1adfd` 与实际合并 `57745632` 同为树 `4706b9f1`，
两者父提交均为 `aaa05496` 与 `9a965a7f`。这轮验可信 C++ 窄扩展，不代表高级
ExtensionRuntime、后台命令、远端 Worker 或 Node 结果同步已交付。本地未编译或运行原生测试。

## 调用次序

1. `Runtime::Create` 显式接收绝对路径 `data_root`、`resource_root`。
2. `OpenSession` 指定绝对 `cwd`、模型与系统提示。提供显式连接材料，或转交一只
   自定义 `Backend`。两条后端入口只能选一条。
3. `Subscribe` 返回拉式事件流。宿主自己调 `Next`，自行接入 UI 或网络循环。
4. `Submit(key, text)` 先把输入原件与受理事实落稳，再返回回执。回执不等于完成。
5. 审批事件携完整入参、工具名、调用号与 cwd。宿主调 `ResolveApproval` 答复；
   `PendingApprovals` 可补查尚未答复请求。超时、取消、关闭会唤醒挂起审批。
   `request_id` 绑定本场会话与持久轮次，须原样交回；外场或恢复前旧请求不能串答。
6. `ReadOperation` 查快照，`WaitResult` 等终态。`Cancel` 对尚未开始和正在执行的
   操作都生效。`Close` 拒新活，取消并收拢 worker，封账并放掉文件句柄。

同键同正文返回原操作；异正文报 `operation_conflict`。`Close`、`Shutdown` 可重复调用。
仍持有 Session 句柄时，关闭后可查 ID 和已有操作快照。丢掉最后一只句柄会关场，
等 worker 退出，再释放内存结果与配置；磁盘原件照常保留。Runtime 只留首条关场错误，
供 `Shutdown` 返回，不强留已丢弃会话。事件流有界，慢消费者溢出时报 `sdk.events.overflow`，
不会把缺事件说成完整回放；操作结果另从持久账查询。`EventStream::Close` 唤醒并等候
正在执行的 `Next` 退出。SDK 不替宿主开事件回调线程。

自定义 Backend、Tool 须合作检查取消旗。SDK 不强杀这些进程内回调，`Close` 会等其退出，
不会丢下 detached 线程再释放借用对象。不得在 Backend、Tool、工厂或扩展回调里销毁 Runtime 或 Session；
回调中调用 `Close`、`WaitResult`、`Runtime::OpenSession` 或 `Runtime::Shutdown` 会报 `sdk.lifecycle.reentrant`，
跨会话调用也受这条约束，免得两只 worker 互相等着 join。

## 工具、权限与并发边界

工具默认空表。可显式启用 `read_file`、`write_file`、`edit_file`、`run_command`、`search`，
也可注入自定义工具，或按服务与工具名单挂 MCP。内置实现沿用共用装配，不另写一套工具。
相对文件路径和命令 cwd 按会话目录解析，不调用进程级 chdir。命令只开放前台执行；
`run_in_background` 真值会明确拒绝。MCP 使用显式完整环境与会话 cwd，启动失败拒绝建场，
不会自动加载个人配置中的其他服务。自定义工具默认要求外部副作用审批。

一场会话串行处理输入。多场会话可在同一进程并发，也可共用 cwd；上下文、权限和操作账
各自持有，不加项目独占锁。共享文件仍可能互相覆盖，SDK 不声称跨会话文件事务隔离。
进程级语言文案只读取现有默认状态，SDK 不在建场时改全局语言。

当前不提供全量 CLI 装配：Lua、Package、子 Agent、CLI Hook 配置、动态换模型、目录切换、
自动记忆、Detached job、GPU 分配均不在这批能力内。`resource_root` 为显式资源入口；
这批四件本地工具不加载资源文件，也不从 home 自动搜配置。自定义 Backend 注入面仅支持
文本与工具调用；遇图片、思考或结构化结果会明报不支持。真实连接后端仍沿用原协议实现。

MCP 文本结果可接着送入下一轮模型请求。图片、音频和二进制块先存入本场 artifact 目录，
再由共用容量闸报 `tool_batch.unestimated_media_or_reasoning`，操作以 `Failed` 收场；
不会重跑工具，也不会继续发送模型请求。SDK 尚未提供媒体预算策略，不能拿文本字节估算
替媒体计价。原件仍可从本地会话目录读取。详见[媒体边界](../architecture/context/v3-action-summary.md#媒体边界)。

## 公开 C++ 扩展

本节记录 #255 已交付的可信 C++ 窄接口，验收与合并记录见上文。

`<lubancore/extensions.hpp>` 提供 `extensions::v1`。宿主把 `Registration` 放进
`SessionOptions::extensions`，每项带 `Manifest` 与工厂。建场先校验声明、依赖和冻结计划，
再调工厂；每场各造一只 `Instance`，不跨会话共用实例。空表沿用原 SDK 执行路。
安装消费示例的 `extensions` 模式只用公开头，演示真实会话拦截。

首批开放三处：`PreUser`、`PostUser`，以及 `PreRequest` 的只读 `Estimate`、`Capacity`。
`PreUser` 输入为 `{"prompt":string}`；调用 `Next::Call` 传同形候选才会采用改写。
不调 `Next` 就短路，正常返回省略输出时，沿用已消费 Next 的结果。`Denied` 与处理失败
分开记账；下游已经拒绝或失败，上游不能再把它洗成成功。可选项允许 `KeepOriginal`，
required 槽仍须失败关闭；高层替代项不能撤掉低层 required 约束。

`ContextAppend` 仅收 `{"text":string}`，只准 `PreUser`、`PostUser` 返回。采用后加来源提示，
另落 `origin=hook`、`display=hidden` 消息，接纳进真实 V3 上下文链。模型历史同序追加，
恢复也读这份正文。原输入仍存受理原件，正式 Human 消息存采用后正文。`PreUser` 拒绝时
不接纳用户消息；`PostUser` 拒绝时保留已接纳用户消息，停住后续模型请求。

`Estimate` 看冻结模型输入，返回公开头所列 EST1 测量对象。`Capacity` 返回
`AdmissionDecision`，形如 `{"decision":"allow","reason":"within budget"}`；还可选
`recover`、`reject`，SDK 当前会停住这两种结局。内置估算与容量 required 槽照常在场。
这批不开放请求正文改写、Action 挂点或全部 CLI Hook 装配。

`Context` 携会话、操作、派发与调用身份；turn、step、request 只填已经发出的号。
取消旗只借到 `Invoke` 返回。Next 副本共用一次许可；跨线程报 `hook.next.wrong_thread`，
重复消费和调用结束后再用也各报明确错误。保存 Next 不会强留会话、实例或后端。
observer 只读，在辅助线程运行，派发等其退出才返回；同一实例可能收到并发 observer，
扩展自行照管共享状态。工厂、Invoke 与扩展析构中，阻塞生命周期入口受同一重入闸约束。

`Close` 先停接、取消并等待实际回调退出，再销毁 Agent、扩展、工具、MCP 与后端。
关闭后仍可调 `DescribeExtensions` 读冻结计划。首批随整场关闭收回，不提供逐项卸载。
可信 C++ 插件持进程权限；这里没有动态库加载器、沙箱、强杀回调或自动回滚外部副作用。
`definition_hash` 由宿主声明，扩展实现或策略变更须更新身份，SDK 不替原生代码计算摘要。

会话目录内 `sdk-extension-plan.json` 存冻结计划。恢复先比声明与计划，再造新实例；
身份、来源、排序、匹配或策略不符，报 `sdk.extension.resume_mismatch`，不启动工厂。
旧会话没有此文件时，只准空扩展表恢复。恢复重建实例，不序列化任意 C++ 私有状态。
公开 ABI 仍属实验接口，须沿安装消费说明使用匹配工具链与 Windows CRT。

## 持久工具结果与宿主投影

`Session::ListToolResults(operation_id)` 列出已结束操作的工具结果身份。
`ReadToolResult` 收这份身份，读本场不可变原件；不收文件路径、offset 或滚动窗口。
身份绑住 Session、Operation、turn、工具调用、持久事件和具体 result。
终态发布前先冻结引用索引，恢复时先验 V3 账、重建索引，再启动执行线程。
查旧轮不借新轮 Agent，也不靠订阅事件补历史；关闭句柄仍可查询。
尚未结束、来源损坏或索引不可用均明报，不能把异常洗成空列表。
索引按终态冻结，每次建立仍读取、验算整份 V3 账。整账读入尚未设字节帽，未实现增量索引。
可信本地列表保留 `capture-*`、`res-*` 等具体版本，`tool_call_id` 沿用 V3 action ID，
不是 provider 原调用号。远端宿主须另筛正式版本；Worker IPC 只开放 selected 的 `res-*`。

`results::v1` 提供只读快照。文本通道各自保留身份、捕获状态和完整性证据；
缺件、坏 hash、读取失败和超限都有明确状态。原件先校验身份、字节数和摘要，
再交给宿主。二进制、`raw_payload` 与未支持媒介只出元信息，不顺引用抓附件。
模型预览含本地来源路径，不能直接拿来当远端界面正文。
本地读接口面向可信宿主；它不替宿主建立文件系统沙箱。
文本读取默认 1 MiB、最多 8 MiB，描述最多 8 MiB，总验件 IO 最多 64 MiB；
每操作最多 256 份结果，每份最多 64 通道。超限标状态，不将原件截短后冒充完整。

本场投影选择由 `SessionOptions::result_policy` 冻结。新场省略时用 preview、版本 1；
恢复省略时保留原选择，显式传值须与原选择相同。旧场没有策略文件时只按 preview、
版本 1 接纳，不能借恢复升成 full。这个选择不裁本地原件，也不限制模型接纳材料。

`ResultProjector` 另收可信宿主给出的 Node 许可。Node 许可缺省关闭，
本场缺省 preview；两层同时准许才出 full，越过 Node 许可则明拒。
投影还须匹配快照内实际会话身份与冻结选择，不能另造 full projector 抬高旧场。
工具、模型或查询参数均不能改这份选择。

多通道共用一份正文预算：先合成固定顺序文本、整段脱敏，再截一次 UTF-8 前缀。
任一参与文本捕获不全时，preview 只留元信息，full 明拒；二进制不转 base64。
超限正文与超限序列化帧也明拒，不能截短后仍称全文。
已生成投影可用专用格式保存并恢复；重投复用原定型记录，不重新换一段 preview。
保存格式内摘要只检损坏，不提供存储认证。已知秘密或规则变更须换 Node 策略版本。
这里没有网络连接、持久 outbox、ACK 或 Control 接线。
投影正文、出站闭合 JSON 与存盘格式各设 1 MiB 上限，JSON 转义也计入体积。
full 超限整份拒绝，不裁成 preview；这批未提供 full 分页或附件读取。

## 恢复与结果

`resume_session_id` 非空时，只恢复所指 V3 会话，ID 保持不变。源缺失、损坏、仍持活锁，
或源并非 V3，都报错；不会回落开新场。恢复会重排“已受理、未派发”输入；
“已派发、无终态”操作报 `Indeterminate`，不会自动重复外部副作用。
待执行输入原件丢失、损坏或不符合受理时的摘要，建场报 `sdk.resume.input_unavailable`；
不会挂着 `Accepted` 等到超时，也不会拿改过的正文重跑。
操作账有坏行、重复受理或缺少先前受理/派发事实，建场报 `sdk.resume.operation_ledger_invalid`。
恢复不能套用查询接口的“跳过坏行”规矩，否则会丢幂等键、重用操作号。

操作终态由 `SessionService` 写入原操作账。最终正文另存会话目录下 `sdk-results`，
其对应终态事实写稳后才发完成事件。工具完整原件仍走现有 V3 结果仓；SDK 不套远端
preview 同步策略。写盘失败会标明结果未可靠保存并停止继续执行，不回报假成功。
