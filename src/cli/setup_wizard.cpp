#include "cli/setup_wizard.hpp"

#include <cctype>

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// 剥掉尾部所有斜杠(base_url 不该带结尾 /,后面各 client 自己拼 /v1/messages
// 之类的路径,留着斜杠会拼出双斜杠)。
std::string StripTrailingSlashes(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 && s[end - 1] == '/') {
        --end;
    }
    return s.substr(0, end);
}

// 读一行、剥空白;EOF 时返回 std::nullopt,调用方原样往上传。
std::optional<std::string> ReadTrimmed(WizardIO& io, const std::string& prompt) {
    io.print(prompt);
    const auto line = io.read_line();
    if (!line.has_value()) {
        return std::nullopt;
    }
    return Trim(*line);
}

// 反复问,直到读到非空字符串,或者 EOF(此时返回 std::nullopt)。
std::optional<std::string> ReadRequired(WizardIO& io, const std::string& prompt, const std::string& empty_hint) {
    while (true) {
        const auto answer = ReadTrimmed(io, prompt);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (!answer->empty()) {
            return answer;
        }
        io.print(empty_hint);
    }
}

// 编号选择,空输入按默认(1-based default_choice)处理,超范围/非数字重问。
// EOF 返回 std::nullopt。
std::optional<std::size_t> ReadChoice(WizardIO& io, const std::string& prompt, std::size_t count,
                                       std::size_t default_choice) {
    while (true) {
        const auto answer = ReadTrimmed(io, prompt);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (answer->empty()) {
            return default_choice;
        }
        try {
            std::size_t consumed = 0;
            const int n = std::stoi(*answer, &consumed);
            if (consumed != answer->size() || n < 1 || static_cast<std::size_t>(n) > count) {
                io.print(tr("wizard.choice.bad_range"));
                continue;
            }
            return static_cast<std::size_t>(n);
        } catch (...) {
            io.print(tr("wizard.choice.not_number"));
        }
    }
}

std::string WireToString(config::Wire wire) {
    return wire == config::Wire::Responses ? "responses" : "anthropic";
}

// model 这一步:输入非空就直接用;输入空就拉列表、编号选;拉取失败/列表为空
// 都回落到"手动输入,必须非空"。返回值为空表示 EOF,调用方原样往上传。
std::optional<std::string> ResolveModel(WizardIO& io, config::Wire wire, const std::string& base_url,
                                         const std::string& api_key) {
    io.print(tr("wizard.model.hint"));
    const auto first = ReadTrimmed(io, "model: ");
    if (!first.has_value()) {
        return std::nullopt;
    }
    if (!first->empty()) {
        return *first;
    }

    const auto list_result = io.fetch_models(wire, base_url, api_key);
    if (!list_result.has_value()) {
        io.print(trf("wizard.model.fetch_failed", list_result.error().message));
        io.print(tr("wizard.model.manual"));
        return ReadRequired(io, "model: ", tr("wizard.model.empty"));
    }

    const std::vector<api::ModelInfo>& models = *list_result;
    if (models.empty()) {
        io.print(tr("wizard.model.list_empty"));
        io.print(tr("wizard.model.manual"));
        return ReadRequired(io, "model: ", tr("wizard.model.empty"));
    }

    for (std::size_t i = 0; i < models.size(); ++i) {
        const std::string& label = models[i].display_name.empty() ? models[i].id : models[i].display_name;
        io.print("  " + std::to_string(i + 1) + ") " + label);
    }
    const auto choice = ReadChoice(io, tr("wizard.model.choose"), models.size(), 1);
    if (!choice.has_value()) {
        return std::nullopt;
    }
    return models[*choice - 1].id;
}

// i18n:第一问选界面语言——选完立即 SetLanguage,向导后续文案(包括
// 存进配置的 language 字段)即用所选语言。列表 = 内置两种 + 语言包;
// 默认编号落在"当前语言"(启动时按 env/系统探测出来的那个)上,直接
// 回车 = 保持现状。EOF 返回 std::nullopt。
std::optional<std::string> ResolveLanguage(WizardIO& io) {
    const std::vector<std::string> langs = AvailableLanguages();
    io.print(tr("wizard.lang.title"));
    std::size_t default_choice = 1;
    for (std::size_t i = 0; i < langs.size(); ++i) {
        io.print("  " + std::to_string(i + 1) + ") " + LanguageDisplayName(langs[i]));
        if (langs[i] == CurrentLanguage()) {
            default_choice = i + 1;
        }
    }
    const auto choice = ReadChoice(io, trf("wizard.lang.prompt", default_choice), langs.size(), default_choice);
    if (!choice.has_value()) {
        return std::nullopt;
    }
    return langs[*choice - 1];
}

}  // namespace

std::optional<WizardOutcome> RunSetupWizard(WizardIO& io) {
    io.print(tr("wizard.title"));
    io.print(tr("wizard.subtitle"));
    io.print("");

    // ---- 0) language(i18n 第一问,选完向导后续文案即用所选语言) ----
    std::string language;
    {
        const auto answer = ResolveLanguage(io);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        language = *answer;
        SetLanguage(language);
    }
    io.print("");

    // ---- 1) wire ----
    io.print(tr("wizard.wire.title"));
    io.print(tr("wizard.wire.opt1"));
    io.print(tr("wizard.wire.opt2"));
    config::Wire wire = config::Wire::Anthropic;
    {
        const auto choice = ReadChoice(io, tr("wizard.choose_prompt"), 2, 1);
        if (!choice.has_value()) {
            return std::nullopt;
        }
        wire = (*choice == 2) ? config::Wire::Responses : config::Wire::Anthropic;
    }
    io.print("");

    // ---- 2) base_url ----
    io.print(tr("wizard.base_url.title"));
    if (wire == config::Wire::Anthropic) {
        io.print("  https://api.minimaxi.com/anthropic");
    } else {
        io.print("  https://api.minimaxi.com/v1");
    }
    std::string base_url;
    {
        const auto answer = ReadRequired(io, "base_url: ", tr("wizard.base_url.empty"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        base_url = StripTrailingSlashes(*answer);
    }
    io.print("");

    // ---- 3) api_key ----
    std::string api_key;
    {
        const auto answer = ReadRequired(io, tr("wizard.api_key.prompt"), tr("wizard.api_key.empty"));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        api_key = *answer;
    }
    io.print("");

    // ---- 4) model ----
    std::string model;
    {
        const auto answer = ResolveModel(io, wire, base_url, api_key);
        if (!answer.has_value()) {
            return std::nullopt;
        }
        model = *answer;
    }
    io.print("");

    // ---- 5) 汇总 + 是否保存 ----
    io.print(tr("wizard.summary.title"));
    io.print("  language = " + language);
    io.print("  wire     = " + WireToString(wire));
    io.print("  base_url = " + base_url);
    io.print("  api_key  = " + config::MaskApiKey(api_key));
    io.print("  model    = " + model);
    io.print("");

    bool save = true;  // 默认保存
    {
        const auto answer = ReadTrimmed(io, trf("wizard.save_prompt", io.home_config_display_path));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (*answer == "n" || *answer == "N") {
            save = false;
        }
    }

    config::Config cfg;
    cfg.wire = wire;
    cfg.base_url = base_url;
    cfg.auth_token = api_key;
    cfg.model = model;
    cfg.language = language;
    cfg.max_context_chars = config::kDefaultMaxContextChars;

    return WizardOutcome{cfg, save};
}

}  // namespace lubancode::cli
