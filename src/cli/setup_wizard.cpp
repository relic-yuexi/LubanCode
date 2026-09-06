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

std::string WizardTrim(const std::string& s) {
    return Trim(s);
}

// 画一帧(向导重排单):注入了 draw_frame(TTY 面板)就整帧交给它;没注入
// (管道/单测/不支持原地重画的终端)退化朴素逐行——分隔线、标题+步骤号、
// 正文、错误、输入提示、footer 逐行打印,步骤号/返回/错误一个不少。
void DrawWizardFrame(WizardIO& io, const WizardFrame& frame) {
    if (io.draw_frame) {
        io.draw_frame(frame);
        return;
    }
    io.print("------------------------------------------------------------");
    std::string header = frame.title;
    if (!frame.progress.empty()) {
        header += header.empty() ? frame.progress : "  " + frame.progress;
    }
    if (!header.empty()) {
        io.print(header);
    }
    io.print("");
    for (const std::string& line : frame.body) {
        io.print(line);
    }
    if (!frame.error.empty()) {
        io.print("! " + frame.error);
    }
    if (!frame.prompt.empty()) {
        io.print(frame.prompt);
    }
    if (!frame.footer.empty()) {
        io.print(frame.footer);
    }
    io.print("------------------------------------------------------------");
}

// 读一个导航事件:注入了 read_event(TTY 面板)就交给它;没注入回落
// read_line——nullopt 算 Eof,其余算 Submitted。朴素路上没有 Esc/Ctrl+C
// 可言,Back 与 Cancelled 只在注入端出现。
WizardInputEvent ReadWizardEvent(WizardIO& io) {
    if (io.read_event) {
        return io.read_event();
    }
    const auto line = io.read_line();
    if (!line.has_value()) {
        return WizardInputEvent{WizardInputEvent::Kind::Eof, std::string()};
    }
    return WizardInputEvent{WizardInputEvent::Kind::Submitted, Trim(*line)};
}

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
// EOF 返回 std::nullopt。返回选中下标(0-based)。setup 向导不关心取消是哪个
// 键,cancel_kind 传 nullptr(向导重排单加的口子,provider 向导才用)。
std::optional<std::size_t> ReadChoice(WizardIO& io, const std::vector<WizardChoiceItem>& items,
                                      std::size_t default_index, const std::string& hint) {
    if (io.interactive && io.choose) {
        return io.choose(items, default_index, hint, nullptr);
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

std::optional<SetupEntryOutcome> RunSetupEntryWizard(WizardIO& io) {
    const std::vector<std::string> languages = AvailableLanguages();
    std::vector<WizardChoiceItem> language_items;
    language_items.reserve(languages.size());
    std::size_t language_default = 0;
    for (std::size_t i = 0; i < languages.size(); ++i) {
        language_items.push_back({LanguageDisplayName(languages[i]), std::string()});
        if (languages[i] == CurrentLanguage()) {
            language_default = i;
        }
    }

    DrawWizardFrame(io, WizardFrame{
                            .title = tr("setup.entry.title"),
                            .progress = tr("setup.entry.language_progress"),
                            .body = {tr("setup.entry.language_body")},
                            .choice_rows = static_cast<int>(language_items.size()) + 1,
                        });
    const auto language_choice =
        ReadChoice(io, language_items, language_default, tr("wizard.choose.hint"));
    if (!language_choice.has_value()) {
        return std::nullopt;
    }
    const std::string language = languages[*language_choice];
    SetLanguage(language);

    DrawWizardFrame(io, WizardFrame{
                            .title = tr("setup.entry.title"),
                            .progress = tr("setup.entry.method_progress"),
                            .body = {tr("setup.entry.method_body"), tr("setup.entry.method_hint")},
                            .choice_rows = 3,
                        });
    const std::vector<WizardChoiceItem> actions = {
        {tr("setup.entry.add"), tr("setup.entry.add_desc")},
        {tr("setup.entry.skip"), tr("setup.entry.skip_desc")},
    };
    const auto action = ReadChoice(io, actions, 0, tr("wizard.choose.hint"));
    if (!action.has_value()) {
        return std::nullopt;
    }
    return SetupEntryOutcome{
        .action = *action == 0 ? SetupEntryAction::AddProvider : SetupEntryAction::Skip,
        .language = language,
    };
}

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
    // 第四项(Gemini 原生)的文案不走 i18n 表:wire 名 + 官方端点本就是
    // 中英通吃的字面量,新键等 i18n 表那册一并补,这里先直书。
    std::vector<WizardChoiceItem> wire_items = {
        {tr("wizard.wire.opt1"), {}},
        {tr("wizard.wire.opt2"), {}},
        {tr("wizard.wire.opt3"), {}},
        {"google-generate-content (Gemini)", {}},
    };
    config::Wire wire = config::Wire::Anthropic;
    {
        const auto choice = ReadChoice(io, wire_items, 0, tr("wizard.choose.hint"));
        if (!choice.has_value()) {
            return std::nullopt;
        }
        switch (*choice) {
            case 1:
                wire = config::Wire::Responses;
                break;
            case 2:
                wire = config::Wire::ChatCompletions;
                break;
            case 3:
                wire = config::Wire::GoogleGenerateContent;
                break;
            default:
                break;
        }
    }
    io.print("");

    // ---- 2) base_url ----
    io.print(tr("wizard.base_url.title"));
    if (wire == config::Wire::Anthropic) {
        io.print("  https://api.minimaxi.com/anthropic");
    } else if (wire == config::Wire::GoogleGenerateContent) {
        io.print("  https://generativelanguage.googleapis.com");
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

    return WizardOutcome{cfg, save};
}

}  // namespace lubancode::cli
