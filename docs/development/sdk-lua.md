# SDK 显式 standalone Lua 合同

本批尚未实现或验收。基线为渠道宿主分支 `9a252c6a`。首笔只接宿主明确选定的 standalone `.lua` 工具；SDK 默认关闭。Package、manifest-backed Lua、Lua 中间件能力束另开小批，原 CLI 三路装配照旧。

## 公开什么

拟在 `SessionOptions` 加显式 Lua 选择值。宿主给绝对目录、精确相对脚本路径、预期完整工具名，以及正数指令预算、内存帽、墙钟预算；不收进程默认 cwd、HOME 或 CLI 配置作为兜底。不扫描或执行未选脚本，也不替宿主改名。

同份已打开普通文件字节过固定读取帽、UTF-8/NUL 与根内路径检查，再交现有 `LuaTool::LoadFromScript`。实际工具名、说明和 object schema 都验过，才注册进本场工具表；完整名须与宿主声明一致。缺件、坏类型、重名、与 builtin/custom/MCP 名冲突或任一必选件加载失败，整场拒开，不跳过坏件装半张表。

选名最多 128 件，单脚本最多 1 MiB，累计实际读取最多 16 MiB，冻结计划最多 256 KiB。先校三项执行预算均为正且可由内部类型表示，再构造 state；不以零值偷偷关帽。入口指纹只覆盖选定源码与声明，不冒称 Lua 堆快照。

首笔只用现有 `Whitelisted` 画像：字符串、表、数学、UTF-8、字符串编译与时间函数。`dofile/loadfile/require`、进程与文件 API、Trusted 全库、Host HTTP/Secret 均不开放。此处是本批能力范围，不把 Lua VM 或文件检查当 OS 隔离。

## 真正可见、真正可调

原 `LuaTool::deferred()` 为 true。现有 `Agent::BuildToolDefinitions` 只在宿主设 `tool_filter` 时过滤，只在 `native_deferred_tools` 启用时标 Deferred。公开 SDK 当前均未开启，故所选 Lua 按普通工具定义出现在真实首个模型请求，沿原 `RunOneTool` 执行；不复制工具执行器，不为本批另造搜索或激活协议。CLI 原延迟工具发现与激活不改。

工具名仍沿 `plugin__<文件stem>__<脚本name>`。保留 External 审批、真实 schema 验参、raw/formal/selected 与主 V3 采用链。未经显式子 Agent profile 准入，不自动给孩子或 Explore 多挂工具；本批验收以主场真实调用为准。

## 共用装载与预算

`src/tools/lua_tool.hpp/.cpp` 已有 VM、LuaGuard 和转换件，SDK 复用它们。落公开装配前先补三处中立收口：

1. `LoadFromScript` 改用已有 `OpenLuaLibraries`。现实现直接开全库，只处理 Pure，传 Whitelisted 尚未真正生效；修后 CLI 原 Pure/Trusted 仍走同一条件与原库集合。
2. 临时 `lua_State` 用 RAII 持有。编译、顶层初始化、定义转换或 C++ 异常都关闭一次；成功一次移交 LuaTool。失败不留下半只 VM。
3. 顶层初始化与每次执行同设指令账、内存帽和墙钟 deadline。初始化不跑模型或工具。调用接本次真实取消旗，返回与异常时清掉 guard 借旗及 deadline；不用监控线程或新停止系统。预算沿现有 instruction hook 检查，不承诺硬实时中断任意 C 函数。

旧 CLI 未请求墙钟预算时保持旧零值行为；本批 SDK 必须显式给正预算。脚本返回值仍走原 Lua 到 Tool::Result 转换，不另定义 Lua 专属运行栈。

## 谁拥有，怎么关场

本场 registry 直接持有每件 LuaTool；每件工具独占 VM、guard 和互斥锁。省掉 CLI `EmbeddedLuaRuntime` 的目录扫描和借用 adapter，仍调用同一 VM 实现。不同场不共用 VM 或可变 globals；同一 VM 调用串行。

取消沿 `ToolExecutionContext.cancel` 传入，只借到这次调用退出。同 state 等锁时与拿锁后须核本次取消，不读上一回合残留地址。关闭沿现有 Session worker、ExecutionOwner 和 SessionResources：停接、置取消旗、等真实调用退场，清回合借用与 Agent，再销 registry/VM。禁止 detach 假收工，不把尚在调用的 VM 先关掉。

描述与开场冻结信息若提供查询，只返回 owned 值；闭场后照样可读。公开头不暴露 `lua_State`、LuaTool、CLI ToolRuntime 或裸 callback 借用。

## 同 ID 恢复

冻结声明含目录与精确入口、预期/实际工具名、源码摘要、schema、Whitelisted 画像和三项预算。默认关闭也须有明确新场声明。聚合现有 opening gate，不能替换 Skills/Memory/Subagents 等绑定。新场 under-owner 落冻结计划，成功才写 system 绑定；恢复省略沿旧计划，显式变化、缺件、入口漂移、坏声明或坏绑定拒绝，不静默升级旧无计划会话。

静态材料检查可在 MCP 之前；独占 owner 下恢复校准必须在工具、模型和续账之前。当前资源装配可能先连 MCP，本合同不虚称整条开场已在 MCP 之前，也不允许为读取失败去调用脚本工具。

恢复从同份冻结源码构造新 VM，不回放历史工具，不恢复先前 Lua globals/闭包。运行中保留本场 VM 状态；跨 Close 的 Lua 状态持久化另批。旧工具结果与真实模型输入仍从既有 V3 raw/formal/selected/context/prepared 原链读取，不拿源码 fingerprint 冒充运行状态或 Close 回执。

## Package 留待后笔

现 `src/package/component.hpp/.cpp` 带 ChannelManifest、Workflow、Agent、Plugin parser；`mounting.hpp/.cpp` 折四张材料表；`code_mounting.cpp` 又暂存 process/Lua/MCP 并整包发布。它们目前由 CLI core 持有。直接搬入 SDK 会带回渠道宿主等依赖，不能借一处公开路径宣布已完成 Package。

下一笔先划纯解析/分析值与宿主执行发布边界，复用原 parser、整包 invalid、引用/信任/停用与覆盖规则；不复制 schema，不造静库环。之后逐件接公开 Skills/Profile/Agent 材料，再接 manifest Lua/受控能力与 Package code 事务。CLI 命令、reload 钉旧快照、code 须新会话和坏包诊断都保留。

## 远端验收

三平台安装消费者只调公开 SDK：默认关闭、精确选件、首个真实 Backend 请求有定义、真实 Submit 调脚本并进入下一请求；同项目两场与异项目两场各自 VM；审批、取消、Close；初始化/转换失败不遗留 VM；同 ID 新 VM 恢复不重跑旧工具；漂移/缺坏/重复声明拒启，未选脚本不读取。

原 CLI Lua/manifest/Package 来源册继续实际运行。新增原生来源与非零 case 数纳入 focused/ASan；最终数按真实源码登记，不先拿计划数冒实跑。九份实际 FileAPI 继续核 source owner 和 SDK 闭包，Package/渠道/Gateway/更新器仍禁回；Lua 当前已链接，本批不宣称已经瘦出默认 SDK。

本地只查代码、文档与纯数据。configure、编译、CTest、项目原生进程一概交远端 CI。
