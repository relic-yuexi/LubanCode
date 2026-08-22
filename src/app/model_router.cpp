#include "app/model_router.hpp"

#include <algorithm>
#include <utility>

#include "app/backend_stack.hpp"
#include "cli/line_editor.hpp"

namespace lubancode::app {

std::vector<lubancode::agent::ModelRoleSpec> BuildRoleSpecs(const lubancode::config::ConfigResult& config_result) {
    // 优先级:高级段(model_roles.<role>.model 非空)> shorthand 三字段。
    // 高级段带 provider/effort/窗口/输出上限,信息严格覆盖 shorthand;两者
    // 值不同时的冲突提示 MergeConfig 已记进 model_role_notices。
    const auto& config = config_result.config;
    const auto& sources = config_result.sources;

    const auto build = [&config, &sources](const char* role_name, const std::string& shorthand,
                                           const lubancode::config::ModelRoleRouteConfig& advanced,
                                           lubancode::config::Source shorthand_source,
                                           lubancode::config::Source advanced_source)
        -> lubancode::agent::ModelRoleSpec {
        lubancode::agent::ModelRoleSpec spec;
        if (!advanced.model.empty()) {
            spec.provider = advanced.provider;
            spec.model = advanced.model;
            spec.effort = advanced.effort;
            spec.context_window = advanced.context_window;
            spec.max_output_tokens = advanced.max_output_tokens;
            spec.source = std::string("model_roles.") + role_name;
            if (advanced_source != lubancode::config::Source::Default) {
                spec.source += "(" + lubancode::config::ToString(advanced_source) + ")";
            }
            return spec;
        }
        if (!shorthand.empty()) {
            spec.model = shorthand;
            spec.source = lubancode::config::ToString(shorthand_source);
        }
        return spec;
    };

    return {
        build("normal", config.normal_model, config.model_roles.normal, sources.normal_model, sources.model_roles),
        build("cheap", config.cheap_model, config.model_roles.cheap, sources.cheap_model, sources.model_roles),
        build("lao", config.lao_model, config.model_roles.lao, sources.lao_model, sources.model_roles),
    };
}

ModelRouterService::ModelRouterService(const lubancode::config::ConfigResult& config_result,
                                       lubancode::api::Backend& main_backend,
                                       std::shared_ptr<std::string> current_model,
                                       const std::string& active_provider)
    : config_result_(config_result),
      main_backend_(main_backend),
      current_model_(std::move(current_model)),
      active_provider_(active_provider) {}

lubancode::agent::ModelRouteTable ModelRouterService::Table() const {
    const auto specs = BuildRoleSpecs(config_result_);
    lubancode::agent::ModelRouteTable table =
        lubancode::agent::ResolveModelRoutes(specs[0], specs[1], specs[2],
                                             current_model_ ? *current_model_ : std::string(), active_provider_);
    // compact_model 兼容别名:只在 cheap_model 与高级段 cheap 都未配置时
    // 顶替压缩类任务(MergeConfig 已在两写冲突时提示"新字段优先",这里照
    // 优先级执行)。旧字段沿用当前 provider,不带 effort——它本来就只是一
    // 个模型名字符串(规格"不把模型字符串当完整路由"正是要补的账)。
    if (!config_result_.config.compact_model.empty() && table.cheap.fell_back_to_normal) {
        lubancode::agent::ModelRoute legacy = table.normal;
        legacy.model = config_result_.config.compact_model;
        legacy.source = "compact_model(旧字段,仅压缩)";
        legacy.fell_back_to_normal = false;
        table.compact_legacy_override = std::move(legacy);
    }
    return table;
}

lubancode::agent::ModelRoute ModelRouterService::RouteInfo(lubancode::agent::TaskKind kind) const {
    return Table().RouteFor(kind);
}

ModelRouterService::Routed ModelRouterService::Route(lubancode::agent::TaskKind kind) const {
    Routed routed;
    routed.route = RouteInfo(kind);
    routed.backend = BackendForProvider(routed.route.provider);
    return routed;
}

lubancode::api::Backend* ModelRouterService::BackendForProvider(const std::string& provider) const {
    if (provider.empty() || provider == active_provider_) {
        return &main_backend_;
    }
    const auto cached = provider_backends_.find(provider);
    if (cached != provider_backends_.end()) {
        return cached->second.get();
    }
    const lubancode::config::ProviderConfig* entry =
        lubancode::config::FindProvider(config_result_.config.providers, provider);
    if (entry == nullptr) {
        return nullptr;
    }
    // 跨 provider:按条目展开一份运行配置再造裸 client(与
    // ApplyConfiguredActiveProvider 同一套展开,但这里是派生配置,不管
    // 来源记账)。展开后的 extra_body/extra_headers/鉴权全按目标端来,
    // 不把当前端的私货带过去。
    lubancode::config::Config derived = config_result_.config;
    derived.wire = entry->wire;
    derived.base_url = entry->base_url;
    derived.auth_mode = entry->auth;
    derived.auth_token = lubancode::config::ProviderApiKey(*entry).value_or(std::string());
    derived.model = entry->model;
    derived.context_window_tokens = entry->context_window_tokens;
    derived.extra_body = entry->extra_body;
    derived.extra_headers = entry->extra_headers;
    derived.reasoning_replay = entry->reasoning_replay;
    derived.reasoning_delta_field = entry->reasoning_delta_field;
    derived.reasoning_replay_field = entry->reasoning_replay_field;
    auto backend = lubancode::app::BuildBackend(derived);
    lubancode::api::Backend* raw = backend.get();
    provider_backends_.emplace(provider, std::move(backend));
    return raw;
}

std::vector<std::string> FormatModelRolesTable(const lubancode::agent::ModelRouteTable& table) {
    struct Row {
        std::string role;
        std::string provider;
        std::string model;
        std::string effort;
        std::string source;
    };
    const auto make_row = [](const char* role, const lubancode::agent::ModelRoute& route) {
        Row row;
        row.role = role;
        row.provider = route.provider.empty() ? "(活跃端)" : route.provider;
        row.model = route.model.empty() ? "(未定)" : route.model;
        row.effort = route.effort.empty() ? "-" : route.effort;
        // 回落必须写明"回落到 normal",不许把同名再印一遍(规格"界面")。
        row.source = route.source.empty() ? "未定" : route.source;
        return row;
    };
    const std::vector<Row> rows = {
        make_row("cheap", table.cheap),
        make_row("normal", table.normal),
        make_row("lao", table.lao),
    };

    const auto pad = [](const std::string& text, std::size_t width) {
        const std::size_t chars = lubancode::cli::DisplayWidthUtf8(text);
        std::string out = text;
        out.append(chars < width ? width - chars : 0, ' ');
        return out;
    };
    struct Column {
        const char* header;
        std::string Row::*field;
    };
    const Column columns[] = {
        {"角色", &Row::role}, {"provider", &Row::provider}, {"model", &Row::model},
        {"effort", &Row::effort}, {"来源", &Row::source},
    };
    std::size_t widths[5] = {0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 5; ++i) {
        widths[i] = lubancode::cli::DisplayWidthUtf8(columns[i].header);
        for (const auto& row : rows) {
            widths[i] = std::max(widths[i], lubancode::cli::DisplayWidthUtf8(row.*columns[i].field));
        }
    }

    std::vector<std::string> lines;
    std::string header;
    for (std::size_t i = 0; i < 5; ++i) {
        header += pad(columns[i].header, widths[i] + 2);
    }
    lines.push_back(header);
    for (const auto& row : rows) {
        std::string line;
        for (std::size_t i = 0; i < 5; ++i) {
            line += pad(row.*columns[i].field, widths[i] + 2);
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

}  // namespace lubancode::app
