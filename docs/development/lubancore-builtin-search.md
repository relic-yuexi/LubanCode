# LubanCore 内置搜索接入合同

状态：本批开工合同，基线功能分支 `02eda80a`。只补公开 SDK 的 `search`；实现、安装与远端证据收齐前不记已交付。读写改文件、前台 `run_command` 已公开，CLI 原功能照留。

## 公开什么

宿主在 `SessionOptions::builtin_tools` 显式选 `search`，缺省仍为空。沿用现有 grep/glob、schema、结果帽和捕获合同，不另写搜索引擎，不公开内部 registry、runner 或 JSON 类型。

`SessionOptions::cwd` 固定本场搜索根。path 缺省、null 或空串均用本场 cwd；相对路径从本场 cwd 起算。错误类型、NUL、坏 UTF-8 明拒；不改进程 cwd。绝对路径沿可信本地合同，cwd 绑定不代目录沙箱。同目录可开多场。

资源固定在 `<RuntimeOptions::resource_root>/libexec/rg`，Windows 为 `rg.exe`。宿主显式给绝对资源根；SDK 不借 HOME、PATH、宿主 exe 旁或在线下载补缺件。只有选中 search 才探测版本；开场前用既有有界版本探针准备本场 runner，缺件、不可执行、错版本和 spawn 失败沿 `search_backend_*` 明报，先于 MCP 与会话执行启动。

SDK component 与普通安装将 manifest/hash 校验过的 rg 放在 `${CMAKE_INSTALL_DATADIR}/lubancore/libexec`，默认即 `share/lubancore/libexec`。安装消费者明确传移位后的 `share/lubancore`。未分期 rg 的开发构建可继续生成 SDK；未选 search 时不要求资源存在。CLI 保留原有三层 rg 发现，不借 SDK 默认根改 CLI 行为。

## 谁拥有

每场 `SessionResources::registry` 持搜索工具；工具持本场显式 runner 和准备结果。每次 execute 持一只短命 rg 及其读线程，沿现有 ChildProcess、取消和输出帽收口。Backend、MCP、扩展和工具表沿共用 SessionExecution 装配，不添第二套会话栈。

搜索保持只读、无审批；写改文件与命令审批类别照旧。现有进程级 `ObservationBoundary` 仍参与过滤；本批不把它称作按 Session 隔离的授权表。尚无 Managed 强制执行门，动作级授权接口也不代参数策略。

## 怎样关场

停新活、广播取消、唤醒审批，等待实际执行、rg 进程树及读线程退出，再封账、清回调、销毁 Agent、扩展、工具、MCP、Backend。关闭后仅留既有查询快照；失败、取消、超时不能写成成功或无命中。后台 job 不在本批。

## 远端验收

本地只查源码、文档和纯数据；不 configure、编译、CTest 或执行原生程序。每个新源头都收 Linux/macOS/Windows × SDK-only/组合六套安装移位、focused 与旧会话门，保三平台 host、Worker、Runner 和全量回归。

- 公共头与 `LubanCore::Core` 安装消费真实 rg；grep/glob、Unicode、缺省/空/相对 path、无命中、截断均查正文，不只查工具名。
- 同 cwd 两场、不同项目各一场，进程 cwd 另指诱饵项目；各场只查绑定目录，资源根与准备缓存不串场。
- 缺件、不可执行、错版本及失败准备先于 MCP/模型；给 HOME/PATH 放可用件也不能补救错误资源根。未选 search、未知/重复工具与既有四件行为照验。
- 取消与 Close 收齐实际子进程和线程；共享 ASan 明列本批必验来源，原件、登记、JUnit 与完整日志相合。LSan、TSan、条件跳过按实际结果说明。

本批退出线：新宿主只调公开 SDK，显式选五件本地工具，在给定项目与资源根跑完整搜索会话。Memory、Skills、子 Agent/Action、Lua/Package、Detached 各另立小 PR。
