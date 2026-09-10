// 中间件核装配(LuaHook 单 P0-B):builtin 槽位 + 项目/用户 Lua 包 ->
// 发布 -> HookDispatcher::SetMiddleware(接线缝)。本件归 runtime 库——
// lua_factory 用的 MakeLuaHookHandler 在 runtime 侧(engine 不反向引
// runtime,turn_runtime.cpp 头注的同一条链接纪律)。
//
// CLI 进程在 SetupHookRuntime 调;app-server 装配层同调(同一只核,三入口
// 共用 runtime::Run*Middleware,不许各接一套)。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hooks/dispatcher.hpp"

namespace lubancode::runtime {

struct MiddlewareAssemblyOptions {
    // 发现根:project 在 <cwd>/.lubancode/hooks,user 在 <home>/.lubancode/
    // hooks。空路径跳过该层。
    std::filesystem::path project_hooks_root;
    std::filesystem::path user_hooks_root;
};

struct MiddlewareAssemblyReport {
    bool attached = false;             // 发布成功并已 SetMiddleware
    std::uint64_t registry_revision = 0;
    int packages_loaded = 0;           // project + user 装载成功的包数
    std::vector<std::string> notices;  // 给用户看的提示(装载/发布失败)
};

// builtin 槽位(估算/容量,§4.36 required) + 项目/用户 Lua 包 → 发布 →
// SetMiddleware。发布失败(同层冲突/循环依赖/物化失败)不挂核:老路径
// 照旧,零行为,notice 报错。lua_factory 用生产适配器(MakeLuaHookHandler,
// 每次调用独立 state)。
MiddlewareAssemblyReport AttachMiddlewareRegistry(hooks::HookDispatcher& dispatcher,
                                                  const MiddlewareAssemblyOptions& options = {});

}  // namespace lubancode::runtime
