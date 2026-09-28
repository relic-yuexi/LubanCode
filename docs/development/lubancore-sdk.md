# LubanCore C++ SDK（实验版）

本批提供真实会话闭环。`Runtime` 协调活会话关场，`Session` 句柄持执行资源，经现有
`SessionService` 接纳输入，再用 `Agent`、轨迹桥和工具表跑回合。
它不启动 CLI 或 AppServer 子进程。

## 拆分进度

本页记录已交付的窄 SDK。SDK-only 默认构建、普通安装与终端依赖拆分已验收；
第一阶段仍须收口会话资源拥有、SessionFactory 与现有宿主迁移，之后再接远端。
`LubanCore::Core` 是公开安装目标；仓库内部旧名
`lubancode_core` 仍带 CLI 实现，两者不能混用。

后端、基础工具与 MCP 已共用 `runtime/assembly`。CLI、one-shot、AppServer
仍有各自的会话装配；完整工具、插件、Hook 和记忆等能力尚未统一迁入公开 SDK。
新增宿主可用下文 API，现有 CLI 迁移须保留原功能，不能靠删去功能来完成拆分。
验收清单见 [SDK 拆分计划](../../todos/LubanCore与CLI分离_核心库独立成宿主底座设计.todo)。

CLI 完整工具装配现接内部 `ToolAssemblyPlan`。交互模式与 one-shot 都先由
`ResolveCliToolAssemblyPlan` 折好 cwd、插件目录、信任账、Package 数据根和 PTC
画像路径，再调用原 `ToolRuntime`。缺席目录不退回主目录；已给路径须为绝对路径，
校验发生在起 MCP、扫描插件或创建目录之前。MCP 固定使用启动 cwd，之后切换插件
cwd 不会搬动已运行的 MCP 进程。

`ToolRuntime` 保留 main/sub/explore 工具表及原资源拥有关系。backend 须活得比工具久；
技能清单按值留给解析器；权限、Agent 扫描根、Package 快照等供应商由宿主显式提供。
CLI 供应商仍按派发时刻读取当前权限与项目，保留切目录和改权限行为。

装配诊断保存 `code`、来源阶段、组件、参数与主/子表归属。CLI 在原发生点同步翻译、
上色和显示；子表重复报告只入诊断，不重复打印。展示 sink 只在构造期间调用，不保留。
构造中途抛错仍走原异常边界，不能从未构造成功的对象再取诊断。

这是内部宿主边界拆分。完整工具装配尚未进入公开 SDK；Package 环境变量展开、
工具内部状态根与进程环境访问仍沿用各自合同，不能据此声称整个工具图已无全局依赖。

## 构建与安装

独立 SDK 构建须同时关闭 CLI、启用 SDK。下面先关闭测试，走默认 `ALL` 和普通安装，
不指定构建目标，也不筛安装组件。`sdk-prefix` 可换成所需安装目录。

```sh
cmake -S . -B build-sdk -DCMAKE_BUILD_TYPE=Release -DLUBANCODE_BUILD_CLI=OFF -DLUBANCODE_BUILD_SDK=ON -DBUILD_TESTING=OFF
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

当前输出共享库。C++23 编译器、标准库、编译配置须匹配；尚无跨工具链 ABI 承诺。
Windows SDK 构建统一使用动态 CRT（Release `/MD`、Debug `/MDd`），消费方也须相同。
默认关闭 SDK 时，原 CLI 发布构建仍沿用静态 CRT。运行时须能找到安装目录下共享库；
Windows 将 `bin` 加入 PATH，Linux 使用库搜索路径或应用自身 RPATH。

集成提交 `22e7ce1f` 已合入 PR #248。其验收头为 `d92c99e9`，见
[CI 36419112569](https://github.com/relic-yuexi/LubanCode/actions/runs/36419112569)：
Linux、macOS、Windows 均跑过 testing OFF/ON 默认构建、普通全安装、仓库外搬迁消费
5/5 与 SDK 专项 4/4；默认 CLI 全量分别为 654/654、657/657、656/656。
这些证据覆盖当时 SDK-only 与终端拆分，不替后续 SessionFactory 或宿主迁移验收。

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
不会丢下 detached 线程再释放借用对象。不得在 Backend/Tool 回调里销毁 Runtime 或 Session；
回调中调用 `Close`、`WaitResult` 或 `Runtime::Shutdown` 会报 `sdk.lifecycle.reentrant`，
跨会话调用也受这条约束，免得两只 worker 互相等着 join。

## 工具、权限与并发边界

工具默认空表。首批可显式启用 `read_file`、`write_file`、`edit_file`、`run_command`，
也可注入自定义工具，或按服务与工具名单挂 MCP。内置实现沿用共用装配，不另写一套工具。
相对文件路径和命令 cwd 按会话目录解析，不调用进程级 chdir。命令只开放前台执行；
`run_in_background` 真值会明确拒绝。MCP 使用显式完整环境与会话 cwd，启动失败拒绝建场，
不会自动加载个人配置中的其他服务。自定义工具默认要求外部副作用审批。

一场会话串行处理输入。多场会话可在同一进程并发，也可共用 cwd；上下文、权限和操作账
各自持有，不加项目独占锁。共享文件仍可能互相覆盖，SDK 不声称跨会话文件事务隔离。
进程级语言文案只读取现有默认状态，SDK 不在建场时改全局语言。

当前不提供全量 CLI 装配：Lua、Package、子 Agent、Hook 配置、动态换模型、目录切换、
自动记忆、Detached job、GPU 分配均不在这批能力内。`resource_root` 为显式资源入口；
这批四件本地工具不加载资源文件，也不从 home 自动搜配置。自定义 Backend 注入面仅支持
文本与工具调用；遇图片、思考或结构化结果会明报不支持。真实连接后端仍沿用原协议实现。

MCP 文本结果可接着送入下一轮模型请求。图片、音频和二进制块先存入本场 artifact 目录，
再由共用容量闸报 `tool_batch.unestimated_media_or_reasoning`，操作以 `Failed` 收场；
不会重跑工具，也不会继续发送模型请求。SDK 尚未提供媒体预算策略，不能拿文本字节估算
替媒体计价。原件仍可从本地会话目录读取。详见[媒体边界](../architecture/context/v3-action-summary.md#媒体边界)。

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
