#include "app/hook_runtime.hpp"

#include <filesystem>
#include <memory>
#include <utility>

#include "cli/console_input.hpp"
#include "config/config.hpp"
#include "hooks/outbox.hpp"
#include "platform/paths.hpp"
#include "runtime/middleware_assembly.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::app {

namespace {

struct HookRuntimeState {
    std::unique_ptr<hooks::HookDispatcher> dispatcher;
    std::vector<std::string> startup_notices;
};

// 进程级单份。故意 new 了不删(leaky):退出路径上 session hooks 析构时还要
// 用它,静态析构次序在这里争不过正确性——进程退出自然回收。
HookRuntimeState& State() {
    static HookRuntimeState* state = new HookRuntimeState();
    return *state;
}

}  // namespace

std::vector<std::string> SetupHookRuntime(const config::ConfigResult& config_result) {
    HookRuntimeState& state = State();
    state.startup_notices.clear();

    auto dispatcher = std::make_unique<hooks::HookDispatcher>();

    // 信任账:放用户主目录(<home>/.lubancode/hook-trust.json),绝不写回仓库。
    const std::optional<std::string> trust_path = hooks::HookTrustStore::DefaultStorePath();
    auto [trust, load_error] = hooks::HookTrustStore::Load(trust_path);
    if (load_error.has_value()) {
        state.startup_notices.push_back(*load_error);
    }

    const std::string cwd = platform::CurrentDirUtf8();
    const hooks::LoadedHooks loaded = hooks::LoadHookDefinitions(
        config_result.config.hooks, config_result.project_config_file_path, config_result.global_config_file_path,
        cwd, trust);

    hooks::HookContext context;
    context.cwd = cwd;
    context.session_id = "unassigned";
    context.turn_id = "unassigned";
    context.permission_mode = HookPermissionModeText();

    const auto configured = dispatcher->Configure(loaded, std::move(trust), std::move(context));

    // 四层生命周期单 P2:可靠 Post 的 durable outbox。只在真有 hooks 定义
    // 时挂(没配 hooks 的用户零额外 I/O);账本打不开(目录建不出/盘只读)
    // 降级为零行为,提示一行——outbox 是可靠性增强,不是硬闸。
    if (!dispatcher->Empty()) {
        const auto outbox_path = hooks::HookOutbox::DefaultPath();
        if (outbox_path.has_value()) {
            auto outbox = hooks::HookOutbox::Open(*outbox_path);
            if (outbox != nullptr) {
                dispatcher->SetOutbox(std::move(outbox));
            } else {
                state.startup_notices.push_back("hooks outbox 账本打开失败(" + outbox_path->string() +
                                                "),可靠 Post 记账降级为内存账,不影响 hooks 执行。");
            }
        }
    }

    if (configured.has_untrusted_project) {
        int untrusted = 0;
        for (const auto& def : dispatcher->definitions()) {
            if (def.source_kind == hooks::HookSourceKind::Project && !def.trusted && !def.disabled) {
                ++untrusted;
            }
        }
        state.startup_notices.push_back("项目配置里有 " + std::to_string(untrusted) +
                                        " 条 hook 未经信任审查,已全部跳过(不会起进程)。"
                                        "用 /hooks 查看命令与 definition hash,审查后 trust 即生效;"
                                        "命令一改须重审。");
    }
    if (configured.has_disabled) {
        state.startup_notices.push_back("有 hook 处于禁用状态(/hooks 可看明细、可重新启用)。");
    }
    if (configured.definition_count > 0) {
        state.startup_notices.push_back("hooks 已装载 " + std::to_string(configured.definition_count) +
                                        " 条定义;/hooks 可查来源、命令、信任与最近运行记录。");
    }

    // LuaHook 单 P0-B:中间件核装配。内置槽位(§4.36 估算/容量,required)
    // + 项目/用户 Lua 包 -> 发布 -> SetMiddleware(接线缝)。CLI、one-shot
    // 与 app-server 共用这一只核:三入口的 PreUser/PostUser/PreRequest 都
    // 从 runtime::Run*Middleware 走它,不许各接一套。发布失败不挂核(零
    // 行为,老路径一字不变),提示报错。
    {
        runtime::MiddlewareAssemblyOptions middleware_options;
        middleware_options.project_hooks_root =
            std::filesystem::path(tools::Utf8ToPath(cwd)) / ".lubancode" / "hooks";
        if (const auto home = config::HomeLubancodeDir(); home.has_value()) {
            middleware_options.user_hooks_root = tools::Utf8ToPath(*home) / "hooks";
        }
        const runtime::MiddlewareAssemblyReport middleware_report =
            runtime::AttachMiddlewareRegistry(*dispatcher, middleware_options);
        for (const std::string& notice : middleware_report.notices) {
            state.startup_notices.push_back(notice);
        }
    }

    state.dispatcher = std::move(dispatcher);
    return state.startup_notices;
}

hooks::HookDispatcher* HookRuntime() {
    return State().dispatcher.get();
}

const std::vector<std::string>& HookStartupNotices() {
    return State().startup_notices;
}

void UpdateHookRuntimeContext(hooks::HookContext context) {
    if (State().dispatcher != nullptr) {
        State().dispatcher->UpdateContext(std::move(context));
    }
}

std::vector<std::string> AdoptBackgroundHookRecordNotices() {
    hooks::HookDispatcher* dispatcher = HookRuntime();
    if (dispatcher == nullptr) {
        return {};
    }
    hooks::HookDispatcher::ExternalAdoption adoption = dispatcher->AdoptExternalRecords();
    std::vector<std::string> notices;
    if (!adoption.records.empty()) {
        notices.push_back("后台子代理 hooks 落账 " + std::to_string(adoption.records.size()) +
                          " 条运行记录(/hooks runs 可查)");
    }
    for (auto& warning : adoption.warnings) {
        notices.push_back(std::move(warning));
    }
    return notices;
}

std::string HookPermissionModeText() {
    return cli::ConfirmModeMachineName(cli::CurrentConfirmMode());
}

}  // namespace lubancode::app
