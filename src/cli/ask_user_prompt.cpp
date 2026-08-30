// ask_user 终端问询的实现(骨架拆解反弹·问题 1):函数体自
// app/turn_runner.cpp 的 PromptAskUser 原样搬来,行为一字未改——注释一并
// 随行。
#include "cli/ask_user_prompt.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "platform/console.hpp"

namespace lubancode::cli {

using lubancode::cli::TermOut;

namespace {

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

void PrintAskUserDeclined(const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme,
                          const std::optional<std::string>& discussion = std::nullopt) {
    std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
    TermOut() << theme.stats << "• " << tr("ask_user.declined") << theme.reset << "\n";
    TermOut() << theme.stats << "  └─ " << question.question << " (";
    for (std::size_t i = 0; i < question.options.size(); ++i) {
        TermOut() << (i == 0 ? "" : " / ") << question.options[i].label;
    }
    TermOut() << ")" << theme.reset << "\n";
    if (discussion.has_value()) {
        TermOut() << theme.stats << "     " << tr("ask_user.discussion_recorded") << theme.reset << " "
                  << *discussion << "\n";
    }
    TermOut().flush();
}

}  // namespace

std::expected<lubancode::tools::AskUserResponse, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme) {
    // 交互菜单取得整块屏面所有权:脚注框收起 + 子代理状态块整块收走 +
    // ticker 挂起(零输出)+ 监听线程让出读权,全程一个作用域管到底;
    // 标题/问题/选项/提示行从正文末尾连着铺,等待期间无人改写这片区域。
    // 恢复点在函数末尾(菜单结果/取消提示之后),见 console_input.hpp
    // StreamFooterSuspendScope 注释。
    const lubancode::cli::StreamFooterSuspendScope footer_suspend;
    const bool interactive_menu =
        lubancode::platform::StdinIsInteractive() && lubancode::platform::ProbeStdoutConsole().is_console;
    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        TermOut() << "\n";
        if (!interactive_menu) {
            if (!question.header.empty()) {
                TermOut() << theme.banner << question.header << theme.reset << "\n";
            }
            TermOut() << question.question << "\n";
            for (std::size_t i = 0; i < question.options.size(); ++i) {
                TermOut() << "  " << (i + 1) << ". " << question.options[i].label;
                if (!question.options[i].description.empty()) {
                    TermOut() << theme.stats << " - " << question.options[i].description << theme.reset;
                }
                TermOut() << "\n";
            }
            TermOut() << "  " << (question.options.size() + 1) << ". " << tr("ask_user.other") << "\n";
            TermOut() << "  " << (question.options.size() + 2) << ". " << tr("ask_user.discuss") << "\n";
        }
        TermOut().flush();
    }

    std::vector<std::size_t> indexes;
    std::optional<std::string> inline_custom_answer;
    if (interactive_menu) {
        std::vector<lubancode::cli::ChoiceMenuItem> items;
        items.reserve(question.options.size() + 2);
        for (const auto& option : question.options) {
            items.push_back({option.label, option.description});
        }
        items.push_back({tr("ask_user.other"), {}});
        items.push_back({tr("ask_user.discuss"), {}});
        lubancode::cli::ChoiceMenuOptions menu_options;
        menu_options.multi_select = question.multi_select;
        menu_options.editable_index = question.options.size();
        menu_options.immediate_submit_index = items.size() - 1;
        menu_options.separator_before_index = items.size() - 1;
        menu_options.question_panel = lubancode::cli::ChoiceMenuQuestionPanel{
            question.header.empty() ? tr("ask_user.panel_title") : question.header,
            question.question,
        };
        menu_options.hint = tr(question.multi_select ? "ask_user.menu_multi_hint" : "ask_user.menu_hint");
        menu_options.invalid_hint = tr("ask_user.menu_select_one");
        menu_options.editable_hint = tr("ask_user.menu_edit_hint");
        lubancode::cli::ReadExitReason menu_exit = lubancode::cli::ReadExitReason::Cancel;
        const auto selected = lubancode::cli::ReadChoiceMenu(items, menu_options, theme, &menu_exit);
        if (!selected.has_value()) {
            if (menu_exit == lubancode::cli::ReadExitReason::Esc) {
                PrintAskUserDeclined(question, theme);
                return lubancode::tools::AskUserResponse::Declined();
            }
            return std::unexpected(tr("ask_user.cancelled"));
        }
        indexes = selected->selected_indices;
        if (selected->custom_text.has_value()) {
            inline_custom_answer = TrimAscii(*selected->custom_text);
        }
    } else {
        for (;;) {
            lubancode::cli::ReadExitReason read_exit = lubancode::cli::ReadExitReason::Cancel;
            const std::optional<std::string> raw = lubancode::cli::ReadLine(
                theme.confirm + tr(question.multi_select ? "ask_user.multi_prompt" : "ask_user.select_prompt") +
                    theme.reset,
                theme, /*esc_rejects=*/true, /*composer=*/false, &read_exit);
            if (!raw.has_value()) {
                if (read_exit != lubancode::cli::ReadExitReason::Esc) {
                    return std::unexpected(tr("ask_user.cancelled"));
                }
                PrintAskUserDeclined(question, theme);
                return lubancode::tools::AskUserResponse::Declined();
            }
            if (TrimAscii(*raw).empty()) {
                PrintAskUserDeclined(question, theme);
                return lubancode::tools::AskUserResponse::Declined();
            }

            indexes.clear();
            std::stringstream parts(*raw);
            std::string part;
            bool valid = true;
            while (std::getline(parts, part, ',')) {
                part = TrimAscii(std::move(part));
                try {
                    std::size_t consumed = 0;
                    const int index = std::stoi(part, &consumed);
                    if (consumed != part.size() || index < 1 ||
                        index > static_cast<int>(question.options.size() + 2)) {
                        valid = false;
                        break;
                    }
                    const std::size_t zero_based = static_cast<std::size_t>(index - 1);
                    if (std::find(indexes.begin(), indexes.end(), zero_based) != indexes.end()) {
                        valid = false;
                        break;
                    }
                    indexes.push_back(zero_based);
                } catch (...) {
                    valid = false;
                    break;
                }
            }
            if (!question.multi_select && indexes.size() != 1) {
                valid = false;
            }
            if (valid && !indexes.empty()) {
                break;
            }
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << theme.error << tr("ask_user.invalid") << theme.reset << "\n";
        }
    }

    const std::size_t discuss_index = question.options.size() + 1;
    if (std::find(indexes.begin(), indexes.end(), discuss_index) != indexes.end()) {
        for (;;) {
            lubancode::cli::ReadExitReason read_exit = lubancode::cli::ReadExitReason::Cancel;
            const std::optional<std::string> discussion = lubancode::cli::ReadLine(
                theme.confirm + tr("ask_user.discuss_prompt") + theme.reset, theme,
                /*esc_rejects=*/true, /*composer=*/false, &read_exit);
            if (!discussion.has_value()) {
                if (read_exit != lubancode::cli::ReadExitReason::Esc) {
                    return std::unexpected(tr("ask_user.cancelled"));
                }
                PrintAskUserDeclined(question, theme);
                return lubancode::tools::AskUserResponse::Declined();
            }
            const std::string value = TrimAscii(*discussion);
            if (!value.empty()) {
                PrintAskUserDeclined(question, theme, value);
                return lubancode::tools::AskUserResponse::Discuss(value);
            }
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << theme.error << tr("ask_user.discuss_empty") << theme.reset << "\n";
        }
    }

    std::vector<std::string> answers;
    for (const std::size_t index : indexes) {
        if (index < question.options.size()) {
            answers.push_back(question.options[index].label);
            continue;
        }
        for (;;) {
            lubancode::cli::ReadExitReason read_exit = lubancode::cli::ReadExitReason::Cancel;
            const std::optional<std::string> custom = lubancode::cli::ReadLine(
                theme.confirm + tr("ask_user.custom_prompt") + theme.reset, theme,
                /*esc_rejects=*/true, /*composer=*/false, &read_exit);
            if (!custom.has_value()) {
                if (read_exit != lubancode::cli::ReadExitReason::Esc) {
                    return std::unexpected(tr("ask_user.cancelled"));
                }
                PrintAskUserDeclined(question, theme);
                return lubancode::tools::AskUserResponse::Declined();
            }
            const std::string value = TrimAscii(*custom);
            if (!value.empty()) {
                answers.push_back(value);
                break;
            }
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << theme.error << tr("ask_user.custom_empty") << theme.reset << "\n";
        }
    }
    if (inline_custom_answer.has_value() && !inline_custom_answer->empty()) {
        answers.push_back(*inline_custom_answer);
    }

    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        TermOut() << theme.banner << "✓ "
                  << (question.header.empty() ? tr("ask_user.panel_title") : question.header) << theme.reset
                  << theme.stats << " ->" << theme.reset;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            TermOut() << (i == 0 ? " " : ", ") << answers[i];
        }
        TermOut() << "\n";
    }
    return lubancode::tools::AskUserResponse::Answered(std::move(answers));
}

}  // namespace lubancode::cli
