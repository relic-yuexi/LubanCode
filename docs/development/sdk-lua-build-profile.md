# SDK Lua 构建画像

本笔只让 SDK-only 构建去掉 Lua。默认画像仍带 Lua；CLI、Hook、现有 SDK Lua 行为照旧。基线为 `f011f7b1`。

## 构建与接口

- 新选项 `LUBANCORE_WITH_LUA` 默认 `ON`。`OFF` 只接受 `LUBANCODE_BUILD_CLI=OFF`、`LUBANCODE_BUILD_SDK=ON`；其余组合在下载依赖前拒绝。
- `OFF` 不调用 Lua FetchContent，不创建 `lubancode_lua`，不编译下表六件，不链接 Lua。保留中立中间件、HTTP、Secret 合同。
- `include/lubancore/lua.hpp`、`SessionOptions::lua`、`Session::DescribeLua()` 公开声明和布局逐字保留。安装配置导出 `LubanCore_WITH_LUA`，不添加 C++ ABI。

| 原 owner | OFF 排除源码 |
|---|---|
| engine | `src/tools/lua_tool.cpp` |
| runtime | `src/runtime/plugin_lua.cpp`、`src/runtime/plugin_lua_host.cpp`、`src/runtime/plugin_lua_manifest.cpp`、`src/runtime/middleware_assembly.cpp`、`src/runtime/hook_package_check.cpp` |

首笔不支持 CLI 与瘦 SDK 同树构建。共用 engine/runtime 仍会把 Lua 递进 DLL；独立 provider archives 另立合同。

## 会话、恢复与收场

只隔开 `SessionLua::Load` 真加载口。`Prepare`、`Plan`、`CheckBinding`、`Open` 保原冻结计划、持久回执、身份、绑定和恢复规矩；关闭 Lua 也继续留账。

`OFF` 新建显式选择和恢复已开启 Lua 来源都返回 `sdk.lua.build_unavailable`。不读所选脚本、不建 VM、不调用模型或工具、不采用该来源、不改原账。关闭 Lua 来源继续按同一 ID 恢复。坏计划、坏绑定、旧账缺失规矩仍走原错误，不能偷偷降成关闭。

真实工具/VM 仍只在 `ON` 转入原 Session registry，随原资源次序收场。`OFF` 只持关闭快照和声明账，Close 后仍可 Describe；不留后台资源。

## 固定验收与 CI

新增 `test_lubancore_lua_build_profile.cpp` 六案，完成标记为 `[sdk-lua-build-path]` 加 `default-off`、`selection`、`disabled-resume`、`frozen-plan`、`isolation`、`lifetime`。同一册在 ON/OFF 都跑实际 SDK 会话；不得让 OFF 静默跳过整个册。

安装消费者新增 `sdk.consumer.lua_build_profile`，三项绝对 argv：消费者、`lua-build-profile`、状态根；末尾标记 `[sdk-lua-build-consumer] complete`。跨画像另用 ON/OFF 两只已迁位消费者，分进程依次跑 `lua-build-seed-off`、`lua-build-resume-off`、`lua-build-seed-on`、`lua-build-reject-on`；每阶段保完整 argv、返回码、完成标记、同头构建上下文和原账字节证据。它们不冒充一只进程能加载两套 SDK。

ON 保原 Lua 九案、保护六案、安装 Lua 三路及全部 CLI/Hook 原生册。新增后 focused 52、安装 30。OFF 明列另一名单：仅排原 `lubancore_lua`、`lua_protected` 两册和安装 Lua 三路，保其余来源，再跑新六案与消费者；focused 50、安装 27。默认 ASan 追加新六案来源，required 58；根任务接三平台矩阵与两份选择器。

三平台远程 CI 检查：ON 默认回归；OFF SDK-only testing ON/OFF、正常 ALL 构建、迁位安装消费；公开头逐字不变；File API 全图无 Lua target/六件源码/原生 Lua include；OFF 新鲜配置喂无效 `FETCHCONTENT_SOURCE_DIR_LUA`，证明没走 Lua fetch；两画像跨进程恢复。纯数据正反案须拒错画像、漏源、旧计数、相对或替换 argv、空案、缺标记、重复/跳过 JUnit 与不匹配 LastTest。

本地只查文本、Python、AST、YAML。configure、编译、CTest 与原生运行一概交远程 CI。源码审查不能替代上述原生证据。
