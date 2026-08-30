// 工具确认终端半边的实现(骨架拆解反弹·问题 1):函数体自
// app/turn_runner.cpp 原样搬来,行为一字未改——注释一并随行。判断半边
//(档位裁定/钩子表态/拼单)在 turn_runner 的 ConfirmToolUse;这里只认
// ToolConfirmRequest,画屏与问话不出这个抽屉。
#include "cli/tool_confirm_ui.hpp"

#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "cli/tool_display.hpp"
#include "config/config.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"

namespace lubancode::cli {

// 终端接线收尾单:本文件的 stdout 写全走输出端口(TermOut),散打清零。
using lubancode::cli::TermOut;

// 打印一段文本的前几行,超过就注明省略了多少行。给确认前的改动摘要用。
namespace {

void PrintFirstLines(const std::string& text, int max_lines) {
    std::vector<std::string> lines;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);  // 没有换行符的单行内容
    }
    const int total = static_cast<int>(lines.size());
    for (int i = 0; i < total && i < max_lines; ++i) {
        TermOut() << "      " << lines[static_cast<std::size_t>(i)] << "\n";
    }
    if (total > max_lines) {
        TermOut() << trf("confirm.detail.omitted", total) << "\n";
    }
}

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input) {
    if (name == "write_file") {
        const std::string path = input.value("path", std::string());
        const std::string content = input.value("content", std::string());
        TermOut() << trf("confirm.detail.path", path) << "\n";
        TermOut() << trf("confirm.detail.content", content.size()) << "\n";
        PrintFirstLines(content, 5);
    } else if (name == "edit_file") {
        const std::string path = input.value("path", std::string());
        const std::string old_s = input.value("old_string", std::string());
        const std::string new_s = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        TermOut() << trf("confirm.detail.path", path) << (replace_all ? tr("confirm.detail.replace_all") : "")
                  << "\n";
        TermOut() << tr("confirm.detail.old") << "\n";
        PrintFirstLines(old_s, 3);
        TermOut() << tr("confirm.detail.new") << "\n";
        PrintFirstLines(new_s, 3);
    } else if (name == "run_command") {
        const std::string command = input.value("command", std::string());
        const std::string shell = input.value("shell", std::string("powershell"));
        TermOut() << trf("confirm.detail.command", shell, command) << "\n";
        // 进程生命线单 P2:确认框至少展示 shell、cwd 与完整命令——用户
        // 确认的是"在哪跑什么",不是只看半张票。cwd 不填时也明示
        //(当前会话目录),别让人误以为进了别处。
        const std::string cwd = input.value("cwd", std::string());
        if (!cwd.empty()) {
            TermOut() << trf("confirm.detail.workdir", cwd) << "\n";
        }
        if (input.value("run_in_background", false)) {
            TermOut() << tr("confirm.detail.background") << "\n";
        }
    } else {
        TermOut() << trf("confirm.detail.args", input.dump()) << "\n";
    }
    TermOut().flush();
}

}  // namespace

void ShowAutomaticToolDiff(ToolDisplay& display, const std::string& tool_use_id, const std::string& name,
                           const nlohmann::json& input) {
    // UI-C:自动放行(--yes/yolo/auto 档的文件工具/选过 a)先算统一
    // diff,存进条目;不再铺一块马上擦掉的临时预览,免得白滚视口、
    // 滚失 Running 锚点。工具完成后在原锚点一次画出留存 diff。
    // 管道模式 ShowDiffPreview 内部直接返回,输出照旧是稳定纯文本。
    const bool file_tool = name == "write_file" || name == "edit_file";
    if (file_tool) {
        display.ShowDiffPreview(tool_use_id, name, input, /*automatic=*/true);
    }
}

bool AskToolConfirm(const ToolConfirmRequest& request) {
    // 单子字段别名:函数体与搬家前(turn_runner.cpp 的 ConfirmToolUse 问话
    // 半边)一字不差。
    const std::string& tool_use_id = request.tool_use_id;
    const std::string& name = request.name;
    const nlohmann::json& input = request.input;
    const bool auto_confirm = request.auto_confirm;
    std::set<std::string>& always_allowed_tools = *request.always_allowed;
    const Theme& theme = *request.theme;
    ToolDisplay& display = *request.display;
    const std::function<void(bool asked, bool allowed)>& approval_observer = request.approval_observer;
    const bool file_tool = name == "write_file" || name == "edit_file";

    // UI-B:真的要问了——条目先改成"待确认"态(黄灯 + 待确认),确认块
    // (参数详情 + [y/a/N] 提示)跟在条目下面;答完确认块整个擦掉,
    // 拒绝则条目原地改灰 Cancelled,允许则改回 Running 等终态。
    // UI-C:edit_file/write_file 在真控制台下,参数详情换成统一 diff
    // 预览(路径 + 行级 diff,- 红底 + 绿底),answered 后随确认块一起擦;
    // 管道模式沿用老的参数摘要,不打 diff。
    //
    // v0.22.5 修复:确认交互落笔前必须先把流式脚注框挂起——不挂起的话
    // 有两条真机实测过的病:1) 详情文字盖写在框的横线上,行尾旧字符不
    // 清、横线残留;2) [y/a/N] 提示打出来之后,被 footer 心跳线程那
    // 条 400ms 一次的 ticker(不管是不是在等确认都无条件重画脚注框)盖
    // 没了,屏上只剩流式期的输入框。用 RAII 作用域,一次性盖住
    // PrintConfirmDetails/ShowDiffPreview 两条路径 + 后面的 ReadLine +
    // 下面 "chose_always" 那次追加确认,答完(或提前 return)自动摘挂起
    // ——详见 cli/console_input.hpp StreamFooterSuspendScope 注释。
    // ("ask_user 被子代理状态遮挡"一单起,这枚作用域同时收走子代理
    // 状态块、挂起 ticker、让监听线程交出读权——确认菜单与 ask_user
    // 菜单同一套屏面所有权,不另开第二条路。)
    const lubancode::cli::StreamFooterSuspendScope footer_suspend;
    const int pending_idx = display.OnConfirmRequest(tool_use_id);
    // 审批悬起旁听(loop 单遗留:WaitingPermission 真接线):真要问用户了,
    // 先报 asked;答完在收尾处报 answered。装配层拿它推 scheduler 的
    // WaitingPermission 账(等审批不烧 iteration,悬起期间后续拍 coalesce)。
    if (approval_observer) {
        approval_observer(/*asked=*/true, /*allowed=*/false);
    }
    if (file_tool && display.is_console) {
        display.ShowDiffPreview(tool_use_id, name, input, /*automatic=*/false);
    } else {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        PrintConfirmDetails(name, input);
    }
    // M10:esc_rejects=true——按 Esc 直接返回 nullopt,走到下面拒绝
    // 分支,不留在输入行里继续等。
    bool allowed = false;
    bool chose_always = false;
    const bool interactive_menu =
        lubancode::platform::StdinIsInteractive() && lubancode::platform::ProbeStdoutConsole().is_console;
    if (interactive_menu) {
        // 方向键选择:本次允许 / 本会话总允许 / 拒绝。默认高亮"拒绝"
        //(安全,等同原 [y/a/N] 回车=N)。Esc/Ctrl+C/EOF 也按拒绝。
        std::vector<lubancode::cli::ChoiceMenuItem> items = {
            {tr("confirm.opt.allow_once"), {}},
            {tr("confirm.opt.always"), {}},
            {tr("confirm.opt.deny"), {}},
        };
        lubancode::cli::ChoiceMenuOptions opts;
        opts.hint = tr("confirm.menu.hint");
        opts.initial_cursor = 2;  // 默认"拒绝"
        const auto selected = lubancode::cli::ReadChoiceMenu(items, opts, theme);
        if (selected.has_value() && !selected->selected_indices.empty()) {
            const std::size_t idx = selected->selected_indices.front();
            if (idx == 0) {
                allowed = true;
            } else if (idx == 1) {
                always_allowed_tools.insert(name);
                allowed = true;
                chose_always = true;
            }
        }
    } else {
        // 管道/非交互:沿用 [y/a/N] 读一行(自动化场景照旧)。
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + tr("confirm.prompt") + theme.reset, theme,
            /*esc_rejects=*/true);
        if (answer.has_value()) {
            if (*answer == "a" || *answer == "A") {
                always_allowed_tools.insert(name);
                allowed = true;
                chose_always = true;
            } else {
                allowed = (*answer == "y" || *answer == "Y");
            }
        }
    }
    display.OnConfirmAnswered(pending_idx, allowed, tool_use_id);
    // 审批悬起旁听:答完了(asked=false 那一枚),allowed 是裁定。
    if (approval_observer) {
        approval_observer(/*asked=*/false, allowed);
    }

    // 按 a 之后多问一句:也永久写进项目 settings.local.json?管道/--yes 下
    // 跳过(只进会话集合)——那些场景没法交互再问一遍。真控制台才追问。
    if (chose_always && display.is_console && !auto_confirm) {
        bool persist_yes = false;
        if (interactive_menu) {
            std::vector<lubancode::cli::ChoiceMenuItem> p_items = {
                {tr("confirm.persist.no"), {}},
                {tr("confirm.persist.yes"), {}},
            };
            lubancode::cli::ChoiceMenuOptions p_opts;
            p_opts.hint = tr("confirm.persist.menu.hint");
            p_opts.initial_cursor = 0;  // 默认"否"
            const auto p_sel = lubancode::cli::ReadChoiceMenu(p_items, p_opts, theme);
            persist_yes =
                p_sel.has_value() && !p_sel->selected_indices.empty() && p_sel->selected_indices.front() == 1;
        } else {
            const std::optional<std::string> persist = lubancode::cli::ReadLine(
                theme.confirm + tr("settings.local.persist_prompt") + theme.reset, theme,
                /*esc_rejects=*/true);
            persist_yes = persist.has_value() && (*persist == "y" || *persist == "Y");
        }
        if (persist_yes) {
            OnToolAllowedPersist(name);
        }
    }
    return allowed;
}

void OnToolAllowedPersist(const std::string& name) {
    const std::string cwd = lubancode::platform::CurrentDirUtf8();
    const auto written = lubancode::config::AddAllowedToolToSettingsLocal(cwd, name);
    std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
    if (written.has_value()) {
        TermOut() << trf("settings.local.persisted", name) << "\n";
        // 首次落地 settings.local.json 时,顺带保证 .gitignore 挡住它
        // (追加了/已挡住/教用户手动加,都是一行反馈;空串 = 无需打)。
        const std::string gi = lubancode::config::EnsureGitignoreCoversSettingsLocal(cwd);
        if (!gi.empty()) {
            TermOut() << gi << "\n";
        }
    } else {
        TermOut() << trf("settings.local.persist_failed", written.error()) << "\n";
    }
}

}  // namespace lubancode::cli
