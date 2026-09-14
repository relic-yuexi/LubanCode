// connection_snapshot.hpp 的实现。纯函数,不碰盘不读环境——配置与来源
// 由调用方(RunAppServerMode 启动一次/单测直喂)递进,冻结即冻结。
#include "app_server/connection_snapshot.hpp"

#include <algorithm>
#include <utility>

#include "hooks/hash.hpp"  // Sha256Hex(与 trajectory/prompt_assembler 同一把)

namespace lubancode::app_server {

namespace {

// 角色路由的取值来源标签(角色格的 model 从哪一级来)。shorthand 走
// sources.<role>_model;advanced 段整段回退,用 sources.model_roles。
std::string RoleSourceText(config::Source source) { return config::ToString(source); }

// 一格角色的快照形状:{model, source};model 空 = 该格未配置(落 null)。
nlohmann::json RoleEntry(const std::string& model, config::Source source) {
    if (model.empty()) {
        return nullptr;
    }
    return nlohmann::json{{"model", model}, {"source", RoleSourceText(source)}};
}

}  // namespace

std::string MaskEndpointUrl(const std::string& base_url) {
    const std::size_t scheme_end = base_url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) {
        return std::string();  // 没有 scheme 段没法安全切,宁可空着不记原文
    }
    // authority 到第一个 '/'、'?' 或 '#' 为止;userinfo(@)理论上不该出现在
    // base_url 里,真出现了也随 authority 截断段一起带走前的整段剥掉。
    std::size_t authority_end = base_url.find('/', scheme_end + 3);
    const std::size_t query = base_url.find('?', scheme_end + 3);
    if (query != std::string::npos && (authority_end == std::string::npos || query < authority_end)) {
        authority_end = query;
    }
    const std::size_t fragment = base_url.find('#', scheme_end + 3);
    if (fragment != std::string::npos &&
        (authority_end == std::string::npos || fragment < authority_end)) {
        authority_end = fragment;
    }
    if (authority_end == std::string::npos) {
        authority_end = base_url.size();
    }
    // userinfo(名字:密码@)剥掉——带了也不外泄。
    const std::string authority =
        base_url.substr(scheme_end + 3, authority_end - (scheme_end + 3));
    const std::size_t at = authority.rfind('@');
    const std::string host_port = at == std::string::npos ? authority : authority.substr(at + 1);
    if (host_port.empty()) {
        return std::string();
    }
    return base_url.substr(0, scheme_end + 3) + host_port;
}

std::string DescribeSecretReference(const config::Config& config, const config::ConfigSources& sources) {
    // 环境变量优先级压过一切(config.cpp 合并序):sources 还停在 env 级,
    // 说明 provider 应用没盖掉它(或压根没有 provider),钥匙真来自那枚变量。
    if (sources.auth_token == config::Source::LubanEnv) {
        return "env:LUBAN_API_KEY";
    }
    if (sources.auth_token == config::Source::LubancodeEnv) {
        return "env:LUBANCODE_API_KEY";
    }
    // provider 生效:引用按 provider 声明面描述(key_env 的变量名/inline/
    // none),只字不提值。
    if (!config.active_provider.empty()) {
        const config::ProviderConfig* provider =
            config::FindProvider(config.providers, config.active_provider);
        if (provider != nullptr) {
            switch (config.auth_mode) {
                case config::ProviderAuthMode::None:
                    return "provider:" + provider->name + ":none";
                case config::ProviderAuthMode::Env:
                    return "provider:" + provider->name + ":env:" + provider->key_env;
                case config::ProviderAuthMode::Inline:
                    return "provider:" + provider->name + ":inline";
            }
        }
    }
    // 配置文件兜底:inline(展示侧另有打码,这里连打码值都不带)。
    if (sources.auth_token == config::Source::ProjectConfigFile ||
        sources.auth_token == config::Source::GlobalConfigFile) {
        return "config-file:inline";
    }
    return "none";
}

std::string ConnectionConfigVersion(const config::Config& config) {
    // 料单:非敏感连接事实,一件不多一件不少。auth_token/api_key 一概不进
    // (§八 177"不记录可离线猜测密钥的内容哈希"——哈希料里没有钥匙,
    // 拿版本反推不出钥匙;换钥匙版本不动,是刻意语义)。
    const nlohmann::json facts = nlohmann::json{
        {"wire", config::ProviderWireName(config.wire)},
        {"base_url", config.base_url},
        {"model", config.model},
        {"provider", config::BoundProviderName(config, config.active_provider)},
        {"auth_mode", config::ProviderAuthModeName(config.auth_mode)},
        {"compact_model", config.compact_model},
        {"normal_model", config.normal_model},
        {"cheap_model", config.cheap_model},
        {"lao_model", config.lao_model},
        {"model_roles",
         nlohmann::json{{"normal", config.model_roles.normal.model},
                        {"cheap", config.model_roles.cheap.model},
                        {"lao", config.model_roles.lao.model}}},
    };
    return hooks::Sha256Hex(facts.dump());
}

nlohmann::json FreezeConnectionSnapshot(const config::Config& config, const config::ConfigSources& sources) {
    const bool advanced = !config.model_roles.Empty();
    const bool shorthand = !advanced && (!config.normal_model.empty() || !config.cheap_model.empty() ||
                                          !config.lao_model.empty());
    nlohmann::json roles = nlohmann::json{
        {"routing", advanced ? "advanced" : (shorthand ? "shorthand" : "off")},
    };
    if (advanced) {
        roles["normal"] = RoleEntry(config.model_roles.normal.model, sources.model_roles);
        roles["cheap"] = RoleEntry(config.model_roles.cheap.model, sources.model_roles);
        roles["lao"] = RoleEntry(config.model_roles.lao.model, sources.model_roles);
    } else {
        roles["normal"] = RoleEntry(config.normal_model, sources.normal_model);
        roles["cheap"] = RoleEntry(config.cheap_model, sources.cheap_model);
        roles["lao"] = RoleEntry(config.lao_model, sources.lao_model);
    }
    roles["compact"] = RoleEntry(config.compact_model, sources.compact_model);

    const std::string endpoint = MaskEndpointUrl(config.base_url);
    nlohmann::json snapshot = nlohmann::json{
        {"wire", config::ProviderWireName(config.wire)},
        {"model", config.model},
        {"provider", config::BoundProviderName(config, config.active_provider)},
        {"endpoint", endpoint.empty() ? nlohmann::json() : nlohmann::json(endpoint)},
        {"secretRef", DescribeSecretReference(config, sources)},
        {"configVersion", ConnectionConfigVersion(config)},
        {"sources",
         nlohmann::json{{"wire", config::ToString(sources.wire)},
                        {"baseUrl", config::ToString(sources.base_url)},
                        {"model", config::ToString(sources.model)},
                        {"apiKey", config::ToString(sources.auth_token)}}},
        {"roles", std::move(roles)},
    };
    return snapshot;
}

std::string FormatEffectiveConfigDiagnostics(const EffectiveConfigInput& input) {
    if (input.config == nullptr || input.sources == nullptr) {
        return std::string();
    }
    const config::Config& config = *input.config;
    const config::ConfigSources& sources = *input.sources;

    std::string out;
    const auto line = [&out](std::string text) {
        out += "[app-server] effective-config ";
        out += std::move(text);
        out += '\n';
    };

    // 根路径与托管档位(§四.111"根路径"):个人布局明说,不冒充应用根。
    line("参数根=" + (input.config_root.empty() ? std::string("(个人布局)") : input.config_root) +
         " 数据根=" + (input.data_root.empty() ? std::string("(个人布局)") : input.data_root) +
         " 应用根=" + (input.app_root_active ? "1" : "0") + " 托管=" + (input.managed ? "1" : "0"));

    // 来源清单:本次合并实际吃到的配置文件(应用根语义下全局档即参数根
    // config.json;项目级档托管模式不读,#67 裁剪,这里如实展示读到什么)。
    line("配置来源 项目级=" + input.project_config_path.value_or(std::string("无")) +
         " 全局=" + input.global_config_path.value_or(std::string("无")));

    // 连接面(取值来源逐字段标注——验证个人 provider/compact/角色模型没有
    // 意外覆盖本应用配置,看来源标签即知,§八 179)。
    const std::string endpoint = MaskEndpointUrl(config.base_url);
    line("wire=" + config::ProviderWireName(config.wire) + "(" + config::ToString(sources.wire) + ")" +
         " model=" + (config.model.empty() ? std::string("(未配置)") : config.model) + "(" +
         config::ToString(sources.model) + ")" + " 端点=" +
         (endpoint.empty() ? std::string("(未配置)") : endpoint) + "(" +
         config::ToString(sources.base_url) + ")" + " 密钥引用=" +
         DescribeSecretReference(config, sources));

    const bool advanced = !config.model_roles.Empty();
    const bool shorthand = !advanced && (!config.normal_model.empty() || !config.cheap_model.empty() ||
                                         !config.lao_model.empty());
    std::string roles = "provider=" + config::BoundProviderName(config, config.active_provider) +
                        " 角色路由=" + (advanced ? std::string("advanced") : (shorthand ? std::string("shorthand") : std::string("关")));
    const auto role_text = [](const char* name, const std::string& model) {
        return model.empty() ? std::string() : std::string(" ") + name + "=" + model;
    };
    if (advanced) {
        roles += role_text("normal", config.model_roles.normal.model);
        roles += role_text("cheap", config.model_roles.cheap.model);
        roles += role_text("lao", config.model_roles.lao.model);
    } else {
        roles += role_text("normal", config.normal_model);
        roles += role_text("cheap", config.cheap_model);
        roles += role_text("lao", config.lao_model);
    }
    roles += role_text("compact", config.compact_model);
    line(std::move(roles));

    // 部署档与功能状态(profile/hash;无档=显式零工具默认档,如实说)。
    if (input.harness != nullptr) {
        line("部署档=" + (input.profile_path.empty() ? std::string("(未记路径)") : input.profile_path) +
             " sha256=" + (input.profile_sha256.empty() ? std::string("(未记)") : input.profile_sha256) +
             " 档名=" + input.harness->name);
        line("features mcp=" + std::to_string(input.harness->FeatureEnabled("mcp") ? 1 : 0) +
             " skills=" + std::to_string(input.harness->FeatureEnabled("skills") ? 1 : 0) +
             " plugins=" + std::to_string(input.harness->FeatureEnabled("plugins") ? 1 : 0) +
             " 工具面=" +
             (input.harness->tools.mode == HarnessToolPolicy::Mode::Only
                  ? ("only(" + std::to_string(input.harness->tools.allow.size()) + ")")
                  : (input.harness->tools.mode == HarnessToolPolicy::Mode::None ? "none" : "inherit")));
    } else {
        line("部署档=无(显式零工具默认档) features mcp=0 skills=0 plugins=0 工具面=only(0)");
    }
    return out;
}

}  // namespace lubancode::app_server
