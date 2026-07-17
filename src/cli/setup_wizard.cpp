#include "cli/setup_wizard.hpp"

#include <cctype>

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
                io.print("编号不对,重新选一个。");
                continue;
            }
            return static_cast<std::size_t>(n);
        } catch (...) {
            io.print("没听懂,输个编号(直接回车用默认值)。");
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
    io.print("model:回车自动从接口获取列表,或者直接输入模型名。");
    const auto first = ReadTrimmed(io, "model: ");
    if (!first.has_value()) {
        return std::nullopt;
    }
    if (!first->empty()) {
        return *first;
    }

    const auto list_result = io.fetch_models(wire, base_url, api_key);
    if (!list_result.has_value()) {
        io.print("拉取模型列表失败: " + list_result.error().message);
        io.print("改成直接输入模型名:");
        return ReadRequired(io, "model: ", "model 不能为空,再输一遍。");
    }

    const std::vector<api::ModelInfo>& models = *list_result;
    if (models.empty()) {
        io.print("接口返回的模型列表是空的。");
        io.print("改成直接输入模型名:");
        return ReadRequired(io, "model: ", "model 不能为空,再输一遍。");
    }

    for (std::size_t i = 0; i < models.size(); ++i) {
        const std::string& label = models[i].display_name.empty() ? models[i].id : models[i].display_name;
        io.print("  " + std::to_string(i + 1) + ") " + label);
    }
    const auto choice = ReadChoice(io, "选择模型编号 [1]: ", models.size(), 1);
    if (!choice.has_value()) {
        return std::nullopt;
    }
    return models[*choice - 1].id;
}

}  // namespace

std::optional<WizardOutcome> RunSetupWizard(WizardIO& io) {
    io.print("=== lubancode 初次配置向导 ===");
    io.print("(base_url / api_key 没读到,先配一遍,配完直接进入会话)");
    io.print("");

    // ---- 1) wire ----
    io.print("接口格式:");
    io.print("  1) anthropic (Claude 系)");
    io.print("  2) responses (OpenAI 系)");
    config::Wire wire = config::Wire::Anthropic;
    {
        const auto choice = ReadChoice(io, "选择 [1]: ", 2, 1);
        if (!choice.has_value()) {
            return std::nullopt;
        }
        wire = (*choice == 2) ? config::Wire::Responses : config::Wire::Anthropic;
    }
    io.print("");

    // ---- 2) base_url ----
    io.print("base_url(必填),例如:");
    if (wire == config::Wire::Anthropic) {
        io.print("  https://api.minimaxi.com/anthropic");
    } else {
        io.print("  https://api.minimaxi.com/v1");
    }
    std::string base_url;
    {
        const auto answer = ReadRequired(io, "base_url: ", "base_url 不能为空,再输一遍。");
        if (!answer.has_value()) {
            return std::nullopt;
        }
        base_url = StripTrailingSlashes(*answer);
    }
    io.print("");

    // ---- 3) api_key ----
    std::string api_key;
    {
        const auto answer = ReadRequired(io, "api_key(必填): ", "api_key 不能为空,再输一遍。");
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
    io.print("配置汇总:");
    io.print("  wire     = " + WireToString(wire));
    io.print("  base_url = " + base_url);
    io.print("  api_key  = " + config::MaskApiKey(api_key));
    io.print("  model    = " + model);
    io.print("");

    bool save = true;  // 默认保存
    {
        const auto answer = ReadTrimmed(io, "保存到 " + io.home_config_display_path + "? [Y/n]: ");
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
    cfg.max_context_chars = config::kDefaultMaxContextChars;

    return WizardOutcome{cfg, save};
}

}  // namespace lubancode::cli
