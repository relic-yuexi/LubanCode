#include "tools/ask_user.hpp"

#include <utility>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

AskUserTool::AskUserTool(AskUserHandler handler) : handler_(std::move(handler)) {}

std::string AskUserTool::name() const {
    return "ask_user";
}

std::string AskUserTool::description() const {
    // 文案在 src/prompts/tools/<语言>/ask_user.md,兜底是迁移前的原文。
    return ToolText("ask_user", "description",
                    "当任务缺少会改变实现方向的用户选择时,用选择题向用户询问。一次可问 1 到 4 题,"
                    "每题给 2 到 4 个备选项;界面会自动追加“自己填写”。不要拿它询问可自行查明的细节。");
}

nlohmann::json AskUserTool::input_schema() const {
    nlohmann::json option = nlohmann::json::object();
    option["type"] = "object";
    option["properties"] = nlohmann::json::object();
    option["properties"]["label"] = {{"type", "string"}};
    option["properties"]["description"] = {{"type", "string"}};
    option["required"] = nlohmann::json::array({"label"});

    nlohmann::json options = nlohmann::json::object();
    options["type"] = "array";
    options["minItems"] = 2;
    options["maxItems"] = 4;
    options["items"] = std::move(option);

    nlohmann::json question = nlohmann::json::object();
    question["type"] = "object";
    question["properties"] = nlohmann::json::object();
    question["properties"]["header"] = {
        {"type", "string"},
        {"description", ToolText("ask_user", "param.questions.header", "简短题头,建议不超过 12 个字")}};
    question["properties"]["question"] = {
        {"type", "string"}, {"description", ToolText("ask_user", "param.questions.question", "完整问题")}};
    question["properties"]["options"] = std::move(options);
    question["properties"]["multi_select"] = {
        {"type", "boolean"}, {"description", ToolText("ask_user", "param.questions.multi_select", "是否允许多选,默认 false")}};
    question["required"] = nlohmann::json::array({"question", "options"});

    nlohmann::json questions = nlohmann::json::object();
    questions["type"] = "array";
    questions["minItems"] = 1;
    questions["maxItems"] = 4;
    questions["items"] = std::move(question);

    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    schema["properties"] = nlohmann::json::object();
    schema["properties"]["questions"] = std::move(questions);
    schema["required"] = nlohmann::json::array({"questions"});
    return schema;
}

Tool::Result AskUserTool::execute(const nlohmann::json& input) {
    const auto questions_it = input.find("questions");
    if (questions_it == input.end() || !questions_it->is_array()) {
        return {"questions 必须是数组", true};
    }
    if (questions_it->empty() || questions_it->size() > 4) {
        return {"questions 一次须有 1 到 4 题", true};
    }
    if (!handler_) {
        return {"当前入口不能与用户交互", true};
    }

    nlohmann::json answers = nlohmann::json::array();
    for (std::size_t qi = 0; qi < questions_it->size(); ++qi) {
        const nlohmann::json& raw = (*questions_it)[qi];
        if (!raw.is_object()) {
            return {"questions[" + std::to_string(qi) + "] 必须是 object", true};
        }
        AskUserQuestion question;
        if (const auto it = raw.find("header"); it != raw.end()) {
            if (!it->is_string()) {
                return {"questions[" + std::to_string(qi) + "].header 必须是字符串", true};
            }
            question.header = it->get<std::string>();
        }
        if (const auto it = raw.find("question");
            it == raw.end() || !it->is_string() || it->get<std::string>().empty()) {
            return {"questions[" + std::to_string(qi) + "].question 必须是非空字符串", true};
        } else {
            question.question = it->get<std::string>();
        }
        const auto options_it = raw.find("options");
        if (options_it == raw.end() || !options_it->is_array() || options_it->size() < 2 ||
            options_it->size() > 4) {
            return {"questions[" + std::to_string(qi) + "].options 须有 2 到 4 项", true};
        }
        for (std::size_t oi = 0; oi < options_it->size(); ++oi) {
            const nlohmann::json& option = (*options_it)[oi];
            if (!option.is_object() || !option.contains("label") || !option["label"].is_string() ||
                option["label"].get<std::string>().empty()) {
                return {"questions[" + std::to_string(qi) + "].options[" + std::to_string(oi) +
                            "].label 必须是非空字符串",
                        true};
            }
            AskUserOption parsed{option["label"].get<std::string>(), {}};
            if (const auto it = option.find("description"); it != option.end()) {
                if (!it->is_string()) {
                    return {"questions[" + std::to_string(qi) + "].options[" + std::to_string(oi) +
                                "].description 必须是字符串",
                            true};
                }
                parsed.description = it->get<std::string>();
            }
            question.options.push_back(std::move(parsed));
        }
        if (const auto it = raw.find("multi_select"); it != raw.end()) {
            if (!it->is_boolean()) {
                return {"questions[" + std::to_string(qi) + "].multi_select 必须是布尔值", true};
            }
            question.multi_select = it->get<bool>();
        }

        auto selected = handler_(question);
        if (!selected.has_value()) {
            return {selected.error(), true};
        }
        if (selected->empty()) {
            return {"用户没有选择答案", true};
        }
        answers.push_back({{"question", question.question}, {"answers", *selected}});
    }

    return {nlohmann::json{{"answers", std::move(answers)}}.dump(), false};
}

}  // namespace lubancode::tools
