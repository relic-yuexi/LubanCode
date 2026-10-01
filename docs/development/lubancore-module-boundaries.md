# LubanCore 可选模块划分草案

状态：开工切面，尚未冻结新 target/API 名。顺序按 [架构分期](../../todos/LubanCore系统架构设计.todo)：身份与 Worker 协议先行，SDK 能力按小 PR 并行；各组共用 [Managed 身份合同](lubancore-managed-identity.md)，不自造主体、资源根或授权表。

当前公开 SDK 已接 read_file、write_file、edit_file、前台 run_command。`LubanCore::Core` 仍私有链接 runtime/engine，构建依赖包含 Lua、渠道、Gateway、updater 等。公开头只用标准 C++，不等于构建依赖已经瘦身；源码列表也不能证明所有对象都进入最终动态库。

| 模块 | 公开什么 | 谁持有、怎样收场 | 本批切面 |
| --- | --- | --- | --- |
| 会话核心 | Runtime、Session、Turn、恢复、审批、取消、结果 | 共用 SessionExecution/SessionResources；停接单、取消、唤醒、join 后再放资源 | 保留当前合同，不另造运行栈 |
| 身份与授权 | 中立上下文、固定资源、Managed 视图与强制门 | 宿主认证；核心逐操作核当前权限；可信拥有者负责清理 | Managed 必经，不能作为可关插件或让 YOLO 绕过 |
| 本地文件工具 | 已有读写改；首个新增项为 search | 本场工具表、cwd、搜索进程与 I/O 线程 | 独立小 PR；rg 从可信 resource_root 定位；cwd 不是沙箱 |
| 前台命令 | 已有 run_command | 本场命令 owner；取消/关闭等真实进程和线程退出 | 不混 Detached，不借全局 chdir 切场 |
| Memory/Skills | 先读、召回与冻结 Skills，再接写入 | 冻结本场访问范围；写队列另定执行 owner | 现写队列借 CLI executable --memory-worker 与进程级 supervisor，不能照搬为 SDK 每场 owner |
| 子 Agent/Action | 建孩子、预算、路由、结果与取消 | 父场持孩子，孩子持自己的执行对象；先停新生、再取消/join | 权限只能收窄；共用核心装配 |
| Lua/Package | 可选加载、包内容与宿主能力 | 包内容可只读共用；Lua state/可变数据归本场，owner 活过借用工具 | Lua OFF 不关闭可信 C++ 中间件；Package/Skills 不绑死 Lua |
| 后台工作/Runner 适配 | 明确 Session-owned 和 Runner-owned 工作 | 各自取消、关闭、持久未知态与恢复 | 先补线程创建失败清理；不能用一枚隐式开关混三种后台机制 |
| 模型/MCP 适配 | 内置 HTTP wire、显式 MCP、可注入 Backend/Tool | 后端、MCP、凭据、环境与调用取消归本场 | 后续可选构建；网络类型不进公开头 |
| CLI/Worker/服务宿主 | 装配、认证、路由、展示与传输 | 宿主管连接/进程，公开 SDK 管会话 | CLI 逐项迁移并保留功能；协议独立管版本 |

每项先写一页“公开什么、谁拥有、怎样关场”，再写代码。建场时明确选入能力，声明与配置按场冻结。本场关闭前不做单项热卸载；动态库、强沙箱、外部副作用回滚另批。

## 两处寿命欠账

现 `SessionResources::Attach` 先于工具表销毁，适合回调 attachment。Lua 工具借 Lua runtime；runtime 须活过工具，不能直接塞进现有 Attach。回调 attachment 与能力 owner 要分清，并在卸载前取消/join 真实任务。

Memory 写队列按 state_root 共用进程级 supervisor，当前还会起 CLI 隐藏 worker。读/召回与 Skills 可先迁；写入队列须另定拥有者、进程入口及租户/项目根。不得复制 CLI 内部运行栈或偷偷依赖可执行文件名。

`ObservationBoundary` 仍为进程 singleton，read_file/search 会读它。后续逐场观察合同不能当成现已隔离；这层也不代身份授权。

## 并行与验收

公开上下文、Session 所有权/关闭、workspace/Memory 身份、顶层 CMake/CI/install 各留一位写入者。能力组只改自己窄装配与适配；先定共同资源合同，再接依赖瘦身，免得不同 PR 各造默认目录和 Policy。

模块 OFF 与本场未选入分开验：前者查构建/拉取闭包，后者查会话能力。OFF 闭包先在 SDK-only、CLI=OFF 检查；CLI=ON 合构可保留 CLI 所需依赖。分别查构建源/FetchContent、导出 usage requirements、最终动态库 imports，不能只查公开头。

模块 ON 须跑远端三平台真实安装、移位消费和会话行为。每项核同 cwd 多场、不同项目、权限、模型、事件、取消、恢复与关闭；后台任务不留悬空引用。身份与 OS 文件/进程隔离分开验，不能据 tenant 目录声称沙箱已交付。本地不 configure、编译或执行原生程序。
