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
#include "cli/terminal_frame.hpp"  // frame::RenderList:管道路选项清单(排版批 6)
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
    // P3(过渡批收编:ask_user 菜单渲染进调度口):回显行与工具确认菜单的
    // 显示半边同款纪律——经 RunUiSync 提交,统一提交锁内落笔;槽未接就地
    // 直走,与旧路一字不差。排版批 6:淡色回显走 row_muted 语义档(与旧
    // stats 同值,零视觉变化)。
    lubancode::cli::RunUiSync([&] {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        TermOut() << theme.row_muted << "• " << tr("ask_user.declined") << theme.reset << "\n";
        TermOut() << theme.row_muted << "  └─ " << question.question << " (";
        for (std::size_t i = 0; i < question.options.size(); ++i) {
            TermOut() << (i == 0 ? "" : " / ") << question.options[i].label;
        }
        TermOut() << ")" << theme.reset << "\n";
        if (discussion.has_value()) {
            TermOut() << theme.row_muted << "     " << tr("ask_user.discussion_recorded") << theme.reset
                      << " " << *discussion << "\n";
        }
        TermOut().flush();
    });
}

}  // namespace

std::expected<lubancode::tools::AskUserResponse, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme) {
    // P3(过渡批收编:ask_user 菜单渲染进调度口)——与工具确认菜单(P2)同款
    // 纪律:开屏前先排干会话级 UI 调度的余量(此前提交的事件画完再开问),
    // 问话的显示半边经 RunUiSync 提交(统一提交锁内落笔);菜单读键留在
    // 本线程——读键不能在 UI 线程/持锁进行。槽未接(单发/单测)就地直走,
    // 行为与旧路一字不差。
    lubancode::cli::RunUiSync([] {});
    // 交互菜单取得整块屏面所有权:脚注框收起 + 子代理状态块整块收走 +
    // ticker 挂起(零输出)+ 监听线程让出读权,全程一个作用域管到底;
    // 标题/问题/选项/提示行从正文末尾连着铺,等待期间无人改写这片区域。
    // 恢复点在函数末尾(菜单结果/取消提示之后),见 console_input.hpp
    // StreamFooterSuspendScope 注释。
    const lubancode::cli::StreamFooterSuspendScope footer_suspend;
    const bool interactive_menu =
        lubancode::platform::StdinIsInteractive() && lubancode::platform::ProbeStdoutConsole().is_console;
    lubancode::cli::RunUiSync([&] {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        TermOut() << "\n";
        if (!interactive_menu) {
            // 管道路(排版批 6):问话原样一行,选项清单走 frame::RenderList
            // ——标题嵌上边框(空 header 用面板默认题),编号进 label、描述
            // 进 hint(key_hint 档)。plain 主题退无框纯文本,零转义。
            TermOut() << question.question << "\n";
            std::vector<lubancode::cli::frame::ListRow> rows;
            for (std::size_t i = 0; i < question.options.size(); ++i) {
                rows.push_back(lubancode::cli::frame::ListRow{
                    std::to_string(i + 1) + ". " + question.options[i].label, std::string(),
                    question.options[i].description});
            }
            rows.push_back(lubancode::cli::frame::ListRow{
                std::to_string(question.options.size() + 1) + ". " + std::string(tr("ask_user.other")),
                std::string(), std::string()});
            rows.push_back(lubancode::cli::frame::ListRow{
                std::to_string(question.options.size() + 2) + ". " + std::string(tr("ask_user.discuss")),
                std::string(), std::string()});
            const std::string title = question.header.empty()
                                          ? std::string(tr("ask_user.panel_title"))
                                          : question.header;
            for (const std::string& line :
                 lubancode::cli::frame::RenderList(title, rows, theme, lubancode::cli::frame::Light())) {
                TermOut() << line << "\n";
            }
        }
        TermOut().flush();
    });

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
        // §六:空提交文案自带操作说明——多选教人"空格勾选、Enter 提交",
        // 单选保持旧口径。报错行另起一行,不再吞掉常驻按键提示(渲染侧
        // console_input 同步改)。
        menu_options.invalid_hint =
            tr(question.multi_select ? "ask_user.menu_select_one_multi" : "ask_user.menu_select_one");
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
            lubancode::cli::RunUiSync([&] {
                std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
                TermOut() << theme.error << tr("ask_user.invalid") << theme.reset << "\n";
            });
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
            lubancode::cli::RunUiSync([&] {
                std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
                TermOut() << theme.error << tr("ask_user.discuss_empty") << theme.reset << "\n";
            });
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
            lubancode::cli::RunUiSync([&] {
                std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
                TermOut() << theme.error << tr("ask_user.custom_empty") << theme.reset << "\n";
            });
        }
    }
    if (inline_custom_answer.has_value() && !inline_custom_answer->empty()) {
        answers.push_back(*inline_custom_answer);
    }

    lubancode::cli::RunUiSync([&] {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        // 排版批 6:✓ 标题走 frame_title(与 banner 同值)、箭头注记走
        // key_hint(与 stats 同值)——语义落位,视觉不动。
        TermOut() << theme.frame_title << "✓ "
                  << (question.header.empty() ? tr("ask_user.panel_title") : question.header) << theme.reset
                  << theme.key_hint << " ->" << theme.reset;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            TermOut() << (i == 0 ? " " : ", ") << answers[i];
        }
        TermOut() << "\n";
    });
    return lubancode::tools::AskUserResponse::Answered(std::move(answers));
}

}  // namespace lubancode::cli
