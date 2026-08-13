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

std::string WireToString(config::Wire wire) {
    return config::ProviderWireName(wire);
}

}  // namespace

// 剥掉尾部所有斜杠(base_url 不该带结尾 /,后面各 client 自己拼 /v1/messages
// 之类的路径,留着斜杠会拼出双斜杠)。/provider add 向导(provider_wizard.cpp)
// 复用这个函数,声明搬进了 setup_wizard.hpp。
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

namespace {

// 编号回落版单选:打印"1) label / 2) label ..."编号列表,读一行数字,空输入走
// default_index(0-based),超范围/非数字重问。EOF 返回 std::nullopt。返回选中
// 下标(0-based)。io.choose 未注入时 ReadChoice 走这条;单测也靠它跑编号路径。
std::optional<std::size_t> ChooseByNumber(WizardIO& io, const std::vector<WizardChoiceItem>& items,
                                          std::size_t default_index) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::string line = "  " + std::to_string(i + 1) + ") " + items[i].label;
        if (!items[i].description.empty()) {
            line += " - " + items[i].description;
        }
        io.print(line);
    }
    while (true) {
        const auto answer = ReadTrimmed(io, trf("wizard.choose_prompt_n", default_index + 1));
        if (!answer.has_value()) {
            return std::nullopt;
        }
        if (answer->empty()) {
            return default_index;
        }
        try {
            std::size_t consumed = 0;
            const int n = std::stoi(*answer, &consumed);
            if (consumed != answer->size() || n < 1 || static_cast<std::size_t>(n) > items.size()) {
                io.print(tr("wizard.choice.bad_range"));
                continue;
            }
            return static_cast<std::size_t>(n - 1);  // 转 0-based
        } catch (...) {
            io.print(tr("wizard.choice.not_number"));
        }
    }
}

}  // namespace

// 单选:优先走注入点 io.choose(生产 = 方向键菜单);未注入时回落到 ChooseByNumber
// (编号列表 + read_line)。空输入按 default_index(0-based),超范围/非数字重问,
// EOF 返回 std::nullopt。返回选中下标(0-based)。
std::optional<std::size_t> ReadChoice(WizardIO& io, const std::vector<WizardChoiceItem>& items,
                                      std::size_t default_index, const std::string& hint) {
    if (io.interactive && io.choose) {
        return io.choose(items, default_index, hint);
    }
    return ChooseByNumber(io, items, default_index);
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

    std::vector<WizardChoiceItem> items;
    items.reserve(models.size());
    for (const auto& m : models) {
        const std::string& label = m.display_name.empty() ? m.id : m.display_name;
        items.push_back({label, std::string{}});
    }
    const auto choice = ReadChoice(io, items, 0, tr("wizard.choose.hint"));
    if (!choice.has_value()) {
        return std::nullopt;
    }
    return models[*choice].id;  // 0-based
}

namespace {

// i18n:第一问选界面语言——选完立即 SetLanguage,向导后续文案(包括
// 存进配置的 language 字段)即用所选语言。列表 = 内置两种 + 语言包;
// 默认高亮落在"当前语言"(启动时按 env/系统探测出来的那个)上,直接
// 回车 = 保持现状。EOF 返回 std::nullopt。
std::optional<std::string> ResolveLanguage(WizardIO& io) {
    const std::vector<std::string> langs = AvailableLanguages();
    io.print(tr("wizard.lang.title"));
    std::vector<WizardChoiceItem> items;
    items.reserve(langs.size());
    std::size_t default_index = 0;
    for (std::size_t i = 0; i < langs.size(); ++i) {
        items.push_back({LanguageDisplayName(langs[i]), std::string{}});
        if (langs[i] == CurrentLanguage()) {
            default_index = i;
        }
    }
    const auto choice = ReadChoice(io, items, default_index, tr("wizard.choose.hint"));
    if (!choice.has_value()) {
        return std::nullopt;
    }
    return langs[*choice];  // 0-based
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
    std::vector<WizardChoiceItem> wire_items = {
        {tr("wizard.wire.opt1"), {}},
        {tr("wizard.wire.opt2"), {}},
        {tr("wizard.wire.opt3"), {}},
    };
    config::Wire wire = config::Wire::Anthropic;
    {
        const auto choice = ReadChoice(io, wire_items, 0, tr("wizard.choose.hint"));
        if (!choice.has_value()) {
            return std::nullopt;
        }
        wire = *choice == 1 ? config::Wire::Responses
                            : (*choice == 2 ? config::Wire::ChatCompletions : config::Wire::Anthropic);
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
