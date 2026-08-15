#include "hooks/loader.hpp"

#include <algorithm>

#include "hooks/hash.hpp"

namespace lubancode::hooks {

namespace {

// ${LUBANCODE_PROJECT_DIR}:项目根占位符,exec form 的命令与参数都替换。
// (PLUGIN_ROOT/PLUGIN_DATA 等 plugin 来源的占位符,等真有 plugin hook
// 装载源再实现,不先造空门。)
std::string SubstituteProjectDir(const std::string& value, const std::string& cwd) {
    constexpr const char* kPlaceholder = "${LUBANCODE_PROJECT_DIR}";
    if (value.find(kPlaceholder) == std::string::npos || cwd.empty()) {
        return value;
    }
    std::string out;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t hit = value.find(kPlaceholder, pos);
        if (hit == std::string::npos) {
            out += value.substr(pos);
            break;
        }
        out += value.substr(pos, hit - pos);
        out += cwd;
        pos = hit + std::string_view(kPlaceholder).size();
    }
    return out;
}

HookSourceKind ClassifySource(const std::string& source_path, const std::optional<std::string>& project_config_path,
                              const std::optional<std::string>& global_config_path, const std::string& cwd) {
    if (project_config_path.has_value() && source_path == *project_config_path) {
        return HookSourceKind::Project;
    }
    if (global_config_path.has_value() && source_path == *global_config_path) {
        return HookSourceKind::User;
    }
    // 兜底分级:配置文件路径在当前目录之下 = 项目级(保守取边:错分级成
    // project 顶多多问一次信任,错分级成 user 才是真漏)。
    if (!cwd.empty() && source_path.rfind(cwd, 0) == 0) {
        return HookSourceKind::Project;
    }
    return HookSourceKind::User;
}

std::string SourceLabel(HookSourceKind kind, const std::string& source_path) {
    return std::string(ToString(kind)) + " " + source_path;
}

config::HookHandlerConfig PrepareHandler(config::HookHandlerConfig handler, const std::string& cwd) {
    handler.command = SubstituteProjectDir(handler.command, cwd);
    for (auto& arg : handler.args) {
        arg = SubstituteProjectDir(arg, cwd);
    }
    handler.command_windows = SubstituteProjectDir(handler.command_windows, cwd);
    for (auto& arg : handler.args_windows) {
        arg = SubstituteProjectDir(arg, cwd);
    }
    if (handler.timeout_ms <= 0) {
        handler.timeout_ms = 30000;
    }
    return handler;
}

void PushDefinition(std::vector<HookDefinition>& out, HookEvent event, const std::string& matcher, bool regex,
                    config::HookHandlerConfig handler, HookSourceKind kind, const std::string& source_path,
                    int declaration_index, bool legacy, const HookTrustStore& trust, LoadedHooks& loaded) {
    HookDefinition def;
    def.event = event;
    def.matcher = matcher;
    def.regex = regex;
    def.source_kind = kind;
    def.source_path = source_path;
    def.source_label = SourceLabel(kind, source_path);
    def.declaration_index = declaration_index;
    def.legacy = legacy;

    def.definition_hash = ComputeDefinitionHash(handler);
    def.definition_hash_short = DefinitionHashShort(def.definition_hash);
    def.handler = std::move(handler);

    // 信任分级:user/managed 不走审查(文件在用户/管理员手里,能改它的
    // 人本来就有全部权限);project 未见当前 hash 的信任 = 未信任,dispatcher
    // 绝不起进程。禁用账对 user/project 都生效,managed 不可禁。
    def.disabled = kind != HookSourceKind::Managed && trust.IsDisabled(source_path, def.definition_hash);
    def.trusted = kind != HookSourceKind::Project || trust.IsTrusted(source_path, def.definition_hash);

    if (kind == HookSourceKind::Project && !def.trusted) {
        loaded.has_untrusted_project = true;
    }
    if (def.disabled) {
        loaded.has_disabled = true;
    }
    out.push_back(std::move(def));
}

}  // namespace

std::string ComputeDefinitionHash(const config::HookHandlerConfig& handler) {
    // 规范串:全部字段拼进一个 JSON 对象再 dump——顺序稳定,转义统一,
    // 不会因为手工拼串把分隔符撞进去。
    nlohmann::json canonical;
    canonical["type"] = handler.type;
    canonical["command"] = handler.command;
    canonical["args"] = handler.args;
    canonical["command_windows"] = handler.command_windows;
    canonical["args_windows"] = handler.args_windows;
    canonical["timeout_ms"] = handler.timeout_ms;
    canonical["async"] = handler.async;
    canonical["failure_policy"] = handler.failure_policy;
    return Sha256Hex(canonical.dump());
}

std::string HookCommandDisplay(const config::HookHandlerConfig& handler) {
    if (handler.args.empty() && handler.args_windows.empty()) {
        return handler.command;  // shell 字符串形式,原样展示
    }
    std::string out = handler.command;
    for (const auto& arg : handler.args) {
        out += " " + arg;
    }
    if (!handler.command_windows.empty() || !handler.args_windows.empty()) {
        out += "  [win: ";
        out += handler.command_windows.empty() ? handler.command : handler.command_windows;
        for (const auto& arg : handler.args_windows) {
            out += " " + arg;
        }
        out += "]";
    }
    return out;
}

LoadedHooks LoadHookDefinitions(const config::HooksConfig& hooks,
                                const std::optional<std::string>& project_config_path,
                                const std::optional<std::string>& global_config_path, const std::string& cwd,
                                const HookTrustStore& trust) {
    LoadedHooks loaded;
    std::vector<HookDefinition>& out = loaded.definitions;

    // ---- 旧四类 -> legacy adapter。守旧语义在 dispatcher 的 legacy 分支:
    // 任意非零退出仍拦(pre_tool)、LUBAN_TOOL_* 照导、固定 30 秒、shell 串。
    auto push_legacy = [&](const std::vector<config::HookEntry>& entries, HookEvent event, bool matcher_meaningful) {
        for (const auto& entry : entries) {
            const HookSourceKind kind =
                ClassifySource(entry.source_path, project_config_path, global_config_path, cwd);
            config::HookHandlerConfig handler;
            handler.command = entry.command;  // shell 字符串形式,exec form 不掺和
            handler.timeout_ms = 30000;       // 固定 30 秒不变
            handler.failure_policy = "warn";
            PushDefinition(out, event, matcher_meaningful ? entry.matcher : std::string(), false, handler, kind,
                           entry.source_path, 0, /*legacy=*/true, trust, loaded);
        }
    };
    push_legacy(hooks.pre_tool, HookEvent::PreToolUse, true);
    push_legacy(hooks.post_tool, HookEvent::PostToolUse, true);
    push_legacy(hooks.session_start, HookEvent::SessionStart, false);
    push_legacy(hooks.session_end, HookEvent::SessionEnd, false);

    // ---- schema 2 事件组。
    for (const auto& [event, groups] : hooks.events) {
        for (const auto& group : groups) {
            const HookSourceKind kind =
                ClassifySource(group.source_path, project_config_path, global_config_path, cwd);
            int handler_index = 0;
            for (const auto& raw_handler : group.hooks) {
                config::HookHandlerConfig handler = PrepareHandler(raw_handler, cwd);
                PushDefinition(out, event, group.matcher, group.regex, std::move(handler), kind, group.source_path,
                               handler_index, /*legacy=*/false, trust, loaded);
                ++handler_index;
            }
        }
    }

    // 稳定排序:来源(managed<user<project)→ 声明次序。日志与执行账按这个
    // 序排,不按谁先跑完谁先说话。
    std::stable_sort(out.begin(), out.end(), [](const HookDefinition& a, const HookDefinition& b) {
        if (SourceOrder(a.source_kind) != SourceOrder(b.source_kind)) {
            return SourceOrder(a.source_kind) < SourceOrder(b.source_kind);
        }
        return a.declaration_index < b.declaration_index;
    });
    // 跨来源去重记账:同事件下 definition hash 相同的多条定义,只执行第一
    // 条(不同来源纵然命令一样,来源账保留,执行只跑一次)。
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t j = i + 1; j < out.size(); ++j) {
            if (!out[j].deduped && out[j].event == out[i].event &&
                out[j].definition_hash == out[i].definition_hash) {
                out[j].deduped = true;
            }
        }
    }
    return loaded;
}

}  // namespace lubancode::hooks
