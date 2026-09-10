// 中间件核装配实现(LuaHook 单 P0-B)。
#include "runtime/middleware_assembly.hpp"

#include <memory>
#include <utility>

#include "hooks/middleware.hpp"
#include "hooks/middleware_builtins.hpp"
#include "hooks/middleware_loader.hpp"
#include "runtime/plugin_lua_host.hpp"  // MakeLuaHookHandler:生产 lua 工厂

namespace lubancode::runtime {

MiddlewareAssemblyReport AttachMiddlewareRegistry(hooks::HookDispatcher& dispatcher,
                                                  const MiddlewareAssemblyOptions& options) {
    using hooks::middleware::MiddlewareDispatcher;
    using hooks::middleware::MiddlewarePool;
    MiddlewareAssemblyReport report;
    MiddlewarePool::Options pool_options;
    pool_options.lua_factory = [](const hooks::middleware::LuaHandlerSpec& spec,
                                  const hooks::middleware::HandlerLimits& limits) {
        return MakeLuaHookHandler(spec, limits);
    };
    MiddlewarePool pool(std::move(pool_options));

    // 内置槽位先入池(builtin 是默认项;§4.36:估算/容量 required)。
    hooks::middleware::AddBuiltinRequestSlots(pool);

    // 项目层先装、用户层后装:同键用户层胜出(§三来源层级)。
    if (!options.project_hooks_root.empty()) {
        auto loaded = hooks::middleware::LoadHookPackages(pool, options.project_hooks_root,
                                                          hooks::middleware::SourceLayer::Project,
                                                          "project " + options.project_hooks_root.string());
        report.packages_loaded += loaded.packages_loaded;
        for (const auto& error : loaded.errors) {
            report.notices.push_back("项目 hook 包未装载: " + error);
        }
    }
    if (!options.user_hooks_root.empty()) {
        auto loaded = hooks::middleware::LoadHookPackages(pool, options.user_hooks_root,
                                                          hooks::middleware::SourceLayer::User,
                                                          "user " + options.user_hooks_root.string());
        report.packages_loaded += loaded.packages_loaded;
        for (const auto& error : loaded.errors) {
            report.notices.push_back("用户 hook 包未装载: " + error);
        }
    }

    auto published = pool.Publish();
    if (!published.has_value()) {
        report.notices.push_back("中间件注册表发布失败(" + published.error().code +
                                 "): " + published.error().message + ";本轮不挂中间件核,老 hook 路照旧。");
        return report;
    }
    report.attached = true;
    report.registry_revision = (*published)->revision();
    dispatcher.SetMiddleware(std::make_shared<MiddlewareDispatcher>(std::move(*published)));
    if (report.packages_loaded > 0) {
        report.notices.push_back("Lua hook 中间件已装载 " + std::to_string(report.packages_loaded) +
                                 " 个包(registryRevision=" + std::to_string(report.registry_revision) + ")。");
    }
    return report;
}

}  // namespace lubancode::runtime
