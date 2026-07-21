#include "cli/provider_wizard.hpp"

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

// 名字这一步:prefill 非空且校验通过就直接用,跳过问答;否则(prefill 为空,
// 或者校验没过)进交互循环,反复问直到给出合法且不重名的名字,或者 EOF。
std::optional<std::string> ResolveProviderName(WizardIO& io, const std::string& prefill,
                                                const std::vector<config::ProviderConfig>& existing) {
    if (!prefill.empty()) {
        const auto valid = config::ValidateProviderName(prefill, existing);
        if (valid.has_value()) {
            return prefill;
        }
        io.print(trf("provider_wizard.name.prefill_invalid", prefill, valid.error()));
    }

    while (true) {
        const auto answer = ReadRequired(io, tr("provider_wizard.name.prompt"), tr("provider_wizard.name.empty"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        const auto valid = config::ValidateProviderName(*answer, existing);
        if (!valid.has_value()) {
            io.print(valid.error());
            continue;
        }
        return *answer;
    }
}

// base_url 这一步:非空 + http/https 粗验,不过就重问。
std::optional<std::string> ResolveBaseUrl(WizardIO& io) {
    while (true) {
        const auto answer = ReadRequired(io, "base_url: ", tr("wizard.base_url.empty"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        const std::string url = StripTrailingSlashes(*answer);
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            return url;
        }
        io.print(tr("provider_wizard.base_url.bad_scheme"));
    }
}

// api_key 这一步:直接贴 key 就落盘(provider.api_key);留空则改问
// key_env(环境变量名,默认 ANTHROPIC_AUTH_TOKEN)。返回值:
// first = 落盘用的 api_key(可能是空串,表示走 key_env 那条路);
// second = key_env(始终非空,api_key 非空时这个字段实际不生效,但仍
// 保留默认值,不留空)。
struct ApiKeyStep {
    std::string api_key;
    std::string key_env;
};

std::optional<ApiKeyStep> ResolveApiKeyStep(WizardIO& io) {
    const auto pasted = ReadTrimmed(io, tr("provider_wizard.api_key.prompt"));
    if (!pasted.has_value()) {
        return std::nullopt;
    }
    if (!pasted->empty()) {
        return ApiKeyStep{*pasted, "ANTHROPIC_AUTH_TOKEN"};
    }

    const auto key_env = ReadTrimmed(io, tr("provider_wizard.key_env.prompt"));
    if (!key_env.has_value()) {
        return std::nullopt;
    }
    return ApiKeyStep{"", key_env->empty() ? "ANTHROPIC_AUTH_TOKEN" : *key_env};
}

// 推理档位这一步:可选,留空跳过(空串 = 不设置)。候选跟 /think 的内置
// 映射表(src/api/anthropic/client.cpp 的 BuildThinkingJson)对齐——
// none/low/medium/high/xhigh/max,但不做强校验,responses 协议本就是
// 原样透传任意字符串,这里跟 /think 一个待遇。
std::optional<std::string> ResolveReasoningEffort(WizardIO& io) {
    io.print(tr("provider_wizard.effort.hint"));
    return ReadTrimmed(io, tr("provider_wizard.effort.prompt"));
}

// 额外请求参数这一步:可选,直接回车跳过(= 不设置 extra_body,留空
// object)。跟 effort 那种"随便留个字符串,错了发请求时才炸"不一样——
// 这里输了东西就当场校验:必须能解析成 JSON object,解析失败或者不是
// object 都打一行错误重问,不把坏配置放过去。
std::optional<nlohmann::json> ResolveExtraBody(WizardIO& io) {
    io.print(tr("provider_wizard.extra_body.hint"));
    while (true) {
        const auto answer = ReadTrimmed(io, tr("provider_wizard.extra_body.prompt"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (answer->empty()) {
            return nlohmann::json::object();
        }
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(*answer);
        } catch (const nlohmann::json::parse_error& e) {
            io.print(trf("provider_wizard.extra_body.invalid_json", e.what()));
            continue;
        }
        if (!parsed.is_object()) {
            io.print(tr("provider_wizard.extra_body.not_object"));
            continue;
        }
        return parsed;
    }
}

}  // namespace

std::optional<ProviderWizardOutcome> RunProviderAddWizard(WizardIO& io, const std::string& name_prefill,
                                                            const std::vector<config::ProviderConfig>& existing) {
    io.print(tr("provider_wizard.title"));
    io.print("");

    config::ProviderConfig provider;

    // ---- 1) 名字 ----
    {
        const auto answer = ResolveProviderName(io, name_prefill, existing);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.name = *answer;
    }
    io.print("");

    // ---- 2) base_url ----
    {
        const auto answer = ResolveBaseUrl(io);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.base_url = *answer;
    }
    io.print("");

    // ---- 3) wire(跟初次配置向导同一套文案,复用现成的两个选项) ----
    io.print(tr("wizard.wire.title"));
    io.print(tr("wizard.wire.opt1"));
    io.print(tr("wizard.wire.opt2"));
    {
        const auto choice = ReadChoice(io, tr("wizard.choose_prompt"), 2, 1);
        if (!choice.has_value()) {
            return std::nullopt;
        }
        provider.wire = (*choice == 2) ? config::Wire::Responses : config::Wire::Anthropic;
    }
    io.print("");

    // ---- 4) api_key / key_env ----
    {
        const auto answer = ResolveApiKeyStep(io);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.api_key = answer->api_key;
        provider.key_env = answer->key_env;
    }
    io.print("");

    // ---- 5) model:拿刚问出来的 base_url/wire,试着用刚给的 key 在线拉列表 ----
    {
        const std::string fetch_key = provider.api_key.empty() ? config::ProviderApiKey(provider).value_or("")
                                                                 : provider.api_key;
        const auto answer = ResolveModel(io, provider.wire, provider.base_url, fetch_key);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.model = *answer;
    }
    io.print("");

    // ---- 6) 推理档位(可选) ----
    {
        const auto answer = ResolveReasoningEffort(io);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.model_reasoning_effort = *answer;
    }
    io.print("");

    // ---- 7) 额外请求参数(可选) ----
    {
        const auto answer = ResolveExtraBody(io);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        provider.extra_body = *answer;
    }
    io.print("");

    // ---- 8) 汇总 + 确认 ----
    io.print(tr("provider_wizard.summary.title"));
    io.print("  name     = " + provider.name);
    io.print("  wire     = " + config::ProviderWireName(provider.wire));
    io.print("  base_url = " + provider.base_url);
    if (provider.api_key.empty()) {
        io.print("  key_env  = " + provider.key_env);
    } else {
        io.print("  api_key  = " + config::MaskApiKey(provider.api_key));
    }
    io.print("  model    = " + provider.model);
    io.print("  effort   = " +
              (provider.model_reasoning_effort.empty() ? tr("provider_wizard.effort.unset")
                                                        : provider.model_reasoning_effort));
    io.print("  extra_body = " + (provider.extra_body.empty()
                                      ? tr("provider_wizard.extra_body.unset")
                                      : trf("provider_wizard.extra_body.summary", provider.extra_body.size())));
    io.print("");

    bool save = true;  // 默认确认
    {
        const auto answer = ReadTrimmed(io, tr("provider_wizard.confirm.prompt"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (*answer == "n" || *answer == "N") {
            save = false;
        }
    }

    return ProviderWizardOutcome{provider, save};
}

}  // namespace lubancode::cli
