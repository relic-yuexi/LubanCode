// turn_runner.hpp 的实现:PrintDivider/PromptAskUser/BuildCallbacks/RunTurn
// 的函数体全在这只 translation unit 里,控制台读取、状态画板、分界线、
// 图像输入等终端依赖不往公开头漏。

#include "app/turn_runner.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <typeinfo>
#include <utility>

#include "agent/compact.hpp"
#include "app/hook_runtime.hpp"
#include "cli/console_input.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "platform/console.hpp"
#include "ptc/ptc_tool.hpp"
#include "runtime/plugin_tool.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/command_safety.hpp"
#include "tools/run_command.hpp"
#include "tools/undo_file_edit.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"

namespace lubancode::app {

// M11(0.10.0):输入/输出分界线。用户回车提交、模型真要开始作答那一刻打
// 一条,回合结束的统计行之后再打一条,把一问一答从视觉上框出来——纯粹
// 是一条线,不带文字、不带花边。is_console 为假(管道/重定向)时直接
// 什么都不打,不污染被重定向的输出。宽度用 cli::DetectConsoleWidth()
// 现测,测不到就交给 cli::BuildDividerLine 自己按 80 列兜底。颜色用
// theme.stats(跟 token 统计行、cwd 提示同一档"淡色信息"),plain 主题下
// theme.stats 本来就是空串,着色形同虚设,不用另外判断。
void PrintDivider(const lubancode::cli::Theme& theme, bool is_console) {
    if (!is_console) {
        return;
    }
    const std::optional<int> width = lubancode::cli::DetectConsoleWidth();
    const bool plain = theme.reset.empty();
    // 0.21.x:分界线满终端宽——max_width 传终端宽自身,
    // min(console_width - 1, console_width) 恒等于 console_width - 1,去掉旧的
    // 80 列上限;探测失败按 80 兜底不变。
    const int detected = width.value_or(80);
    const std::string line = lubancode::cli::BuildDividerLine(detected, plain, detected);
    if (line.empty()) {
        return;
    }
    std::cout << theme.stats << line << theme.reset << "\n";
    std::cout.flush();
}

// turn 尾分界线(终端回合视觉收束单):"──── Worked for 6m 41s ────"。
// 每个用户 turn 恰一枚,落在正文/错误/打断提示之后、下一只 composer 之前。
// tone 按终态挑:正常 Worked、打断 Stopped、失败/预算耗尽 Failed。墙钟
// 由调用方按 steady_clock 算好递进来(毫秒)。
// is_console 为假(管道/重定向)也落——只是退成纯文案(不带横线装饰):
// 这是回合的时间账,automation 的 stdout 契约里该有它;开头那条裸分界线
// 沿老规矩只在真控制台打,两者口径不同(那条是装饰,这条是账)。
void PrintTurnFooter(const lubancode::cli::Theme& theme, bool is_console, std::int64_t wall_ms,
                     lubancode::cli::TurnFooterTone tone) {
    const bool plain = theme.reset.empty() || !is_console;
    const std::optional<int> width = lubancode::cli::DetectConsoleWidth();
    const int detected = width.value_or(80);
    const std::string text = lubancode::cli::FormatTurnFooterText(wall_ms, tone);
    const std::string line = lubancode::cli::BuildTurnFooterLine(text, detected, plain);
    if (line.empty()) {
        return;
    }
    std::cout << theme.stats << line << theme.reset << "\n";
    std::cout.flush();
}

// 鲁班图标:启动、/clear 后各打一次,纯装饰,不承载信息(信息在紧跟着的
// PrintBanner 里)。边框版,四行,配色沿用 PrintBanner 同一套语义色——
// 标题行跟版本行一样用 theme.banner(主色),副标题跟 cwd 提示行一样用
// theme.stats(淡色信息),不新开配色。管道/重定向模式(调用方按
// spinner_enabled/is_console 判断)不该打这些装饰字符,由调用方决定

// 打印一段文本的前几行,超过就注明省略了多少行。给确认前的改动摘要用。
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
        std::cout << "      " << lines[static_cast<std::size_t>(i)] << "\n";
    }
    if (total > max_lines) {
        std::cout << trf("confirm.detail.omitted", total) << "\n";
    }
}

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input) {
    if (name == "write_file") {
        const std::string path = input.value("path", std::string());
        const std::string content = input.value("content", std::string());
        std::cout << trf("confirm.detail.path", path) << "\n";
        std::cout << trf("confirm.detail.content", content.size()) << "\n";
        PrintFirstLines(content, 5);
    } else if (name == "edit_file") {
        const std::string path = input.value("path", std::string());
        const std::string old_s = input.value("old_string", std::string());
        const std::string new_s = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        std::cout << trf("confirm.detail.path", path) << (replace_all ? tr("confirm.detail.replace_all") : "")
                  << "\n";
        std::cout << tr("confirm.detail.old") << "\n";
        PrintFirstLines(old_s, 3);
        std::cout << tr("confirm.detail.new") << "\n";
        PrintFirstLines(new_s, 3);
    } else if (name == "run_command") {
        const std::string command = input.value("command", std::string());
        const std::string shell = input.value("shell", std::string("powershell"));
        std::cout << trf("confirm.detail.command", shell, command) << "\n";
        // 进程生命线单 P2:确认框至少展示 shell、cwd 与完整命令——用户
        // 确认的是"在哪跑什么",不是只看半张票。cwd 不填时也明示
        //(当前会话目录),别让人误以为进了别处。
        const std::string cwd = input.value("cwd", std::string());
        if (!cwd.empty()) {
            std::cout << trf("confirm.detail.workdir", cwd) << "\n";
        }
        if (input.value("run_in_background", false)) {
            std::cout << tr("confirm.detail.background") << "\n";
        }
    } else {
        std::cout << trf("confirm.detail.args", input.dump()) << "\n";
    }
    std::cout.flush();
}

std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::expected<std::vector<std::string>, std::string> PromptAskUser(
    const lubancode::tools::AskUserQuestion& question, const lubancode::cli::Theme& theme) {
    // 交互菜单取得整块屏面所有权:脚注框收起 + 子代理状态块整块收走 +
    // ticker 挂起(零输出)+ 监听线程让出读权,全程一个作用域管到底;
    // 标题/问题/选项/提示行从正文末尾连着铺,等待期间无人改写这片区域。
    // 恢复点在函数末尾(菜单结果/取消提示之后),见 console_input.hpp
    // StreamFooterSuspendScope 注释。
    const lubancode::cli::StreamFooterSuspendScope footer_suspend;
    const bool interactive_menu = lubancode::platform::StdinIsInteractive() &&
                                  lubancode::platform::ProbeStdoutConsole().is_console;
    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        std::cout << "\n";
        if (!question.header.empty()) {
            std::cout << theme.banner << question.header << theme.reset << "\n";
        }
        std::cout << question.question << "\n";
        if (!interactive_menu) {
            for (std::size_t i = 0; i < question.options.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << question.options[i].label;
                if (!question.options[i].description.empty()) {
                    std::cout << theme.stats << " - " << question.options[i].description << theme.reset;
                }
                std::cout << "\n";
            }
            std::cout << "  " << (question.options.size() + 1) << ". " << tr("ask_user.other") << "\n";
        }
        std::cout.flush();
    }

    std::vector<std::size_t> indexes;
    std::optional<std::string> inline_custom_answer;
    if (interactive_menu) {
        std::vector<lubancode::cli::ChoiceMenuItem> items;
        items.reserve(question.options.size() + 1);
        for (const auto& option : question.options) {
            items.push_back({option.label, option.description});
        }
        items.push_back({tr("ask_user.other"), {}});
        lubancode::cli::ChoiceMenuOptions menu_options;
        menu_options.multi_select = question.multi_select;
        menu_options.editable_index = items.size() - 1;
        menu_options.hint = tr(question.multi_select ? "ask_user.menu_multi_hint" : "ask_user.menu_hint");
        menu_options.invalid_hint = tr("ask_user.menu_select_one");
        menu_options.editable_hint = tr("ask_user.menu_edit_hint");
        const auto selected = lubancode::cli::ReadChoiceMenu(items, menu_options, theme);
        if (!selected.has_value()) {
            return std::unexpected(tr("ask_user.cancelled"));
        }
        indexes = selected->selected_indices;
        if (selected->custom_text.has_value()) {
            inline_custom_answer = TrimAscii(*selected->custom_text);
        }
    } else {
        for (;;) {
            const std::optional<std::string> raw = lubancode::cli::ReadLine(
                theme.confirm + tr(question.multi_select ? "ask_user.multi_prompt" : "ask_user.select_prompt") +
                    theme.reset,
                theme, /*esc_rejects=*/true);
            if (!raw.has_value() || TrimAscii(*raw).empty()) {
                return std::unexpected(tr("ask_user.cancelled"));
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
                        index > static_cast<int>(question.options.size() + 1)) {
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
            std::cout << theme.error << tr("ask_user.invalid") << theme.reset << "\n";
        }
    }

    std::vector<std::string> answers;
    for (const std::size_t index : indexes) {
        if (index < question.options.size()) {
            answers.push_back(question.options[index].label);
            continue;
        }
        for (;;) {
            const std::optional<std::string> custom = lubancode::cli::ReadLine(
                theme.confirm + tr("ask_user.custom_prompt") + theme.reset, theme,
                /*esc_rejects=*/true);
            if (!custom.has_value()) {
                return std::unexpected(tr("ask_user.cancelled"));
            }
            const std::string value = TrimAscii(*custom);
            if (!value.empty()) {
                answers.push_back(value);
                break;
            }
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << theme.error << tr("ask_user.custom_empty") << theme.reset << "\n";
        }
    }
    if (inline_custom_answer.has_value() && !inline_custom_answer->empty()) {
        answers.push_back(*inline_custom_answer);
    }

    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        std::cout << theme.stats << tr("ask_user.recorded") << theme.reset;
        for (std::size_t i = 0; i < answers.size(); ++i) {
            std::cout << (i == 0 ? " " : ", ") << answers[i];
        }
        std::cout << "\n";
    }
    return answers;
}

// ---------------------------------------------------------------------------
// P2(显示系统剥离单):确认问话的终端实现,包成 InteractionBroker 形状。
//
// ConfirmToolUse 是原来 BuildCallbacks 里那枚 on_tool_confirm 闭包的原文
// 搬家(一个字没改,注释一并随行):会话级确认档(yolo/auto/confirm)、
// settings.local.json 的 allow/deny 前缀、PreToolUse 归并决策、
// PermissionRequest 钩子、三档菜单/[y/a/N]、settings.local.json 追问,全
// 在这里。BuildCallbacks 的同步回调(async 缺位的回落路)与异步审批通道
// (async 主路)共用这一份——同一颗脑子,两条门。
//
// ReadyApprovalFuture 是"当场问完"的 future:结果在构造时就绪,WaitApproval
// 立即返回。远端前端(app-server/Web/Tauri)后续在同一枚
// on_tool_confirm_async 挂接点上换"登记 request_id、等 ResolveApproval"
// 的悬起实现,RunOneTool 与本文件都不用动。
// ---------------------------------------------------------------------------
namespace {

class ReadyApprovalFuture final : public lubancode::agent::InteractionFuture {
public:
    explicit ReadyApprovalFuture(lubancode::agent::ApprovalResponse response) : response_(std::move(response)) {}

    std::optional<lubancode::agent::ApprovalResponse> WaitApproval() override { return response_; }

private:
    lubancode::agent::ApprovalResponse response_;
};

}  // namespace

// ---------------------------------------------------------------------------
// P3(显示系统剥离单):终端装配的档位翻译。cli::CurrentConfirmMode() 读的
// 是 SharedEditor 那枚跨线程档位(Shift+Tab 流式切档),runtime 只认自己的
// 中立枚举——映射在这里,一处。
// ---------------------------------------------------------------------------
namespace {

lubancode::runtime::TurnRuntime::Options BuildTurnRuntimeOptions(
    bool auto_confirm, std::set<std::string>& always_allowed_tools, const std::vector<std::string>& allow_commands,
    const std::vector<std::string>& deny_commands, lubancode::hooks::HookDispatcher* hook_dispatcher) {
    lubancode::runtime::TurnRuntime::Options options;
    options.auto_confirm = auto_confirm;
    switch (lubancode::cli::CurrentConfirmMode()) {
        case lubancode::cli::ConfirmMode::Confirm:
            options.permission_mode = lubancode::runtime::PermissionMode::Confirm;
            break;
        case lubancode::cli::ConfirmMode::Auto:
            options.permission_mode = lubancode::runtime::PermissionMode::Auto;
            break;
        case lubancode::cli::ConfirmMode::Yolo:
            options.permission_mode = lubancode::runtime::PermissionMode::Yolo;
            break;
    }
    options.always_allowed = &always_allowed_tools;
    options.allow_commands = allow_commands;
    options.deny_commands = deny_commands;
    options.hook_dispatcher = hook_dispatcher;
    return options;
}

}  // namespace

// 确认问话的完整实现(原文自 BuildCallbacks 的旧闭包搬家,注释随行)。
// P3(显示系统剥离单):档位/黑名单/钩子表态的裁定已抽去
// runtime::EvaluatePermission 与 runtime::EmitPermissionRequest(纯逻辑,
// 可脱离终端单测),这里只剩"把决定落到画面上"的活:diff 预览、三档菜单、
// settings.local.json 追问。裁定次序的完整注释见 runtime/turn_runtime.hpp。
bool ConfirmToolUse(const std::string& tool_use_id, bool auto_confirm,
                    std::set<std::string>& always_allowed_tools, const lubancode::cli::Theme& theme,
                    lubancode::cli::ToolDisplay& display, const std::vector<std::string>& allow_commands,
                    const std::vector<std::string>& deny_commands,
                    lubancode::hooks::HookDispatcher* hook_dispatcher, const lubancode::agent::ToolHookDecision& pre,
                    bool has_permission_hooks, const std::string& name, const nlohmann::json& input,
                    const std::function<void(bool asked, bool allowed)>& approval_observer = {}) {
    const bool file_tool = name == "write_file" || name == "edit_file";

    // 裁定(纯逻辑,runtime 层):档位 + permissions 叠加 + PreToolUse 表态
    // -> 放行还是问。auto_confirm/--yes 与 yolo 在里头一并判。档位翻译复
    // 用 BuildTurnRuntimeOptions(Confirm/Auto/Yolo 的映射一处定)。
    const lubancode::runtime::TurnRuntime::Options core_options =
        BuildTurnRuntimeOptions(auto_confirm, always_allowed_tools, allow_commands, deny_commands, hook_dispatcher);
    lubancode::runtime::PermissionContext permission;
    permission.auto_confirm = core_options.auto_confirm;
    permission.mode = core_options.permission_mode;
    permission.always_allowed = core_options.always_allowed;
    permission.allow_commands = &core_options.allow_commands;
    permission.deny_commands = &core_options.deny_commands;
    const lubancode::runtime::PermissionVerdict verdict =
        lubancode::runtime::EvaluatePermission(permission, pre, name, input);

    if (verdict.action == lubancode::runtime::PermissionVerdict::Action::Allow) {
        // UI-C:自动放行(--yes/yolo/auto 档的文件工具/选过 a)也把统一
        // diff 预览打出来——用户看得见将要发生什么,但不停下等确认,
        // 打完即执行;执行完预览被 TrimBelow 擦掉,条目只留 +N -M。
        // 管道模式 ShowDiffPreview 内部直接返回,输出照旧是稳定纯文本。
        if (file_tool) {
            display.ShowDiffPreview(tool_use_id, name, input, /*trim_on_done=*/true);
        }
        return true;
    }

    // ---- 真要问用户了:PermissionRequest 钩子先表态(runtime 层发射)。----
    if (has_permission_hooks) {
        const lubancode::runtime::PermissionHookResult hook =
            lubancode::runtime::EmitPermissionRequest(hook_dispatcher, name, input);
        if (hook.reply == lubancode::runtime::PermissionHookReply::Deny) {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cout << theme.error << "[hook] PermissionRequest 钩子拒绝执行 " << name << ": " << hook.reason
                      << theme.reset << "\n";
            return false;
        }
        if (hook.reply == lubancode::runtime::PermissionHookReply::Allow) {
            if (file_tool) {
                display.ShowDiffPreview(tool_use_id, name, input, /*trim_on_done=*/true);
            }
            return true;
        }
        // 全不表态 -> 正常问用户(落下去)。
    }
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
        display.ShowDiffPreview(tool_use_id, name, input, /*trim_on_done=*/false);
    } else {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        PrintConfirmDetails(name, input);
    }
    // M10:esc_rejects=true——按 Esc 直接返回 nullopt，走到下面拒绝
    // 分支，不留在输入行里继续等。
    bool allowed = false;
    bool chose_always = false;
    const bool interactive_menu =
        lubancode::platform::StdinIsInteractive() && lubancode::platform::ProbeStdoutConsole().is_console;
    if (interactive_menu) {
        // 方向键选择:本次允许 / 本会话总允许 / 拒绝。默认高亮"拒绝"
        // (安全,等同原 [y/a/N] 回车=N)。Esc/Ctrl+C/EOF 也按拒绝。
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
            const std::string cwd = CurrentDirUtf8();
            const auto written = lubancode::config::AddAllowedToolToSettingsLocal(cwd, name);
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            if (written.has_value()) {
                std::cout << trf("settings.local.persisted", name) << "\n";
                // 首次落地 settings.local.json 时,顺带保证 .gitignore 挡住它
                // (追加了/已挡住/教用户手动加,都是一行反馈;空串 = 无需打)。
                const std::string gi = lubancode::config::EnsureGitignoreCoversSettingsLocal(cwd);
                if (!gi.empty()) {
                    std::cout << gi << "\n";
                }
            } else {
                std::cout << trf("settings.local.persist_failed", written.error()) << "\n";
            }
        }
    }
    return allowed;
}

// 交互循环、单发模式共用的回调:文本打字机打印(正文保持原色,不着色),
// 工具调用打一行提示,needs_confirm 的工具按 auto_confirm 决定是自动放行
// 还是问用户一句(三选:y 本次允许 / a 本会话总是允许该工具 / N 拒绝)。
// 确认逻辑本体在 ConfirmToolUse(见上),这里只接两条门:同步回落
// (on_tool_confirm,子代理/PTC 转发与单测走的旧路)与异步审批
// (on_tool_confirm_async,Broker 的终端实现)。
// always_allowed_tools 由调用方持有,跨多轮 Run() 保留,选过 a 的工具本
// 会话内不会再问。usage_stats 由调用方持有,只在这一次 Run() 范围内累计
// (RunTurn() 每次都会传一份新的进来)。registry 是这一轮实际在用的工具表——
// 如果里面注册了 "agent" 工具,这里顺带把这一轮现算好的确认/记账/打印
// 逻辑通过 SetHooks 灌给它,子代理被调用时就能用上同一套(详见
// tools/agent_tool.hpp 顶部注释)。
// display:UI-B(0.12.0)新增,这一轮的工具条目展示总管(建条目、原地
// 改写状态、管道模式的 [工具]/[工具完成] 稳定纯文本),todo_state 也归它
// 持有。回调层只管把事件原样转进去。
lubancode::agent::Callbacks BuildCallbacks(bool auto_confirm, std::set<std::string>& always_allowed_tools,
                                            const lubancode::cli::Theme& theme, lubancode::runtime::TurnUsageStats& usage_stats,
                                            lubancode::cli::ContextTracker& context_tracker,
                                            lubancode::tools::ToolRegistry& registry,
                                            lubancode::hooks::HookDispatcher* hook_dispatcher,
                                            ToolDisplay& display, StreamBodyTracker& body_tracker,
                                            const std::vector<std::string>& allow_commands,
                                            const std::vector<std::string>& deny_commands,
                                            const std::atomic<bool>* cancel_flag,
                                            lubancode::agent::WorkflowRecorder* recorder,
                                            lubancode::runtime::ToolTraceHub* trace_hub,
                                            lubancode::runtime::TurnCollector* view_collector,
                                            std::function<std::string(const std::string&, const nlohmann::json&)>
                                                mode_gate,
                                            std::function<void(bool asked, bool allowed)> approval_observer) {
    lubancode::agent::Callbacks callbacks;

    // Plan 模式(只读研究硬闸单):ModePolicy 接到 RunOneTool 的
    // on_mode_policy 挂点。空 gate = 没装 Plan 闸(单测/子代理旧路)。
    callbacks.on_mode_policy = mode_gate;

    // 逐枚追踪单:装了 hub 的轮次,recorder 吃 canonical trace 的投影
    // (hub.AttachProjection),不走 on_tool_start/on_tool_done 各自手打——
    // 一份事件,两路消费,次序不再分叉。
    const bool trace_projection_installed = trace_hub != nullptr;

    // hooks:dispatcher 为空指针或没有工具事件的定义是常态(没配 hooks 的
    // 用户占多数),这时候干脆不设这些回调——跟"没有 hooks 系统"时行为
    // 完全一样。配了就全走 dispatcher:来源相加、信任审查、并发归并都在
    // 那一层,这里只发事件、领决策。P3 起 payload 组装与归并映射在
    // runtime::Emit* 一处(turn_runtime.cpp),这里只接线。
    const bool has_tool_hooks = lubancode::runtime::HasToolHooks(hook_dispatcher);
    const bool has_permission_hooks = lubancode::runtime::HasPermissionHooks(hook_dispatcher);
    // PreToolUse 的归并决策要在确认回调里继续用(allow 跳过用户确认、
    // ask 强制问一句)——确认回调的签名不带它,靠这个共享槽传:RunOneTool
    // 先跑 PreToolUse 再问确认,槽里的决策就是当前这次工具调用的。子代理
    // 转发的是同一批 std::function(闭包随行),槽照常可用。
    auto pre_decision_slot = std::make_shared<lubancode::agent::ToolHookDecision>();
    if (has_tool_hooks) {
        callbacks.on_pre_tool_use_hook = [hook_dispatcher, pre_decision_slot](
                                             const std::string& /*tool_use_id*/, const std::string& name,
                                             const nlohmann::json& input) -> lubancode::agent::ToolHookDecision {
            lubancode::agent::ToolHookDecision decision =
                lubancode::runtime::EmitPreToolUse(hook_dispatcher, name, input);
            *pre_decision_slot = decision;
            return decision;
        };

        callbacks.on_post_tool_use_hook = [hook_dispatcher](
                                              const std::string& /*tool_use_id*/, const std::string& name,
                                              const nlohmann::json& input,
                                              const lubancode::tools::Tool::Result&
                                              result) -> std::vector<std::string> {
            return lubancode::runtime::EmitPostToolUse(hook_dispatcher, name, input, result);
        };

        // UI 相位:checking_hook/blocked 由 ToolDisplay 按 tool_use_id 路由到
        // 对应条目(P4;子代理工具的 id 在转发链上随行);等权限/运行的过渡
        // 由既有 OnConfirmRequest/终态渲染覆盖,不重复画。
        callbacks.on_tool_phase = [&display](const std::string& tool_use_id, const std::string& /*name*/,
                                            lubancode::agent::ToolPhase phase) {
            switch (phase) {
                case lubancode::agent::ToolPhase::CheckingHook:
                    display.OnHookCheckingText(tool_use_id);
                    break;
                case lubancode::agent::ToolPhase::Blocked:
                    display.OnHookMarkBlocked(tool_use_id);
                    break;
                case lubancode::agent::ToolPhase::WaitingPermission:
                case lubancode::agent::ToolPhase::Running:
                    break;
            }
        };
    }

    // M10:流式期间的 std::cout 写要拿 StdoutWriteMutex 跟监听线程错开——
    // 这条规矩没变,只是打印挪进了 StreamBodyTracker::OnDelta(锁在它里面
    // 拿):正文照旧逐字原样打,顺带给回合收束后的 markdown 重画记账;
    // 管道模式/plain 主题下 tracker 不启用,OnDelta 就是原来那三行。
    // 先收掉正在展示的思考折叠块(如果有),再加分隔,最后打正文。
    // view_collector(回合视觉收束):旁路入账,屏上一个字节不变。
    callbacks.on_text_delta = [&display, &body_tracker, view_collector](const std::string& text) {
        if (view_collector != nullptr) {
            view_collector->OnTextDelta(text, /*thinking=*/false);
        }
        if (display.HasActiveThinking()) {
            display.OnThinkingDone();
            body_tracker.OnToolBlockDone();  // 下一段正文前垫一空行,别粘在思考条目上
        }
        body_tracker.OnDelta(text);
    };

    // 思考增量(thinking/reasoning):首 delta 断开正文块 + PaintNew "思考中…",
    // 后续 delta 只攒正文不刷屏。结束时标题换成 "思考 Xs"。
    callbacks.on_thinking_delta = [&display, &body_tracker, view_collector](const std::string& text) {
        if (view_collector != nullptr) {
            view_collector->OnTextDelta(text, /*thinking=*/true);
        }
        if (!display.HasActiveThinking()) {
            body_tracker.OnBlockBreak();
        }
        display.OnThinkingDelta(text);
    };

    // UI-B:工具条目化渲染,建条目/画条目/管道行全在 ToolDisplay 里。
    // markdown:条目要开画了,正文当前块到此为止(保持原样,不重画)。
    // recorder:录一遍生成技能(0.25.x)的监听挂点——只在这里旁听事件,
    // 不改工具本身的执行路径;没在录(nullptr)零影响。
    callbacks.on_tool_start = [&display, &body_tracker, recorder, view_collector, trace_projection_installed](
                                  const std::string& tool_use_id,
                                                                  const std::string& name,
                                                                  const nlohmann::json& input) {
        if (recorder != nullptr && !trace_projection_installed) {
            recorder->RecordToolCall(name, input);
        }
        if (view_collector != nullptr) {
            view_collector->OnToolStarted(tool_use_id, name, input);
        }
        display.OnThinkingDone();  // 思考块若有,先收尾
        body_tracker.OnBlockBreak();
        display.OnToolStart(tool_use_id, name, input);
    };

    callbacks.on_tool_confirm = [auto_confirm, &always_allowed_tools, &theme, &display, &allow_commands,
                                  &deny_commands, hook_dispatcher, pre_decision_slot,
                                  has_permission_hooks, approval_observer](const std::string& tool_use_id,
                                                                          const std::string& name,
                                                                          const nlohmann::json& input) -> bool {
        return ConfirmToolUse(tool_use_id, auto_confirm, always_allowed_tools, theme, display, allow_commands,
                              deny_commands, hook_dispatcher, *pre_decision_slot, has_permission_hooks, name, input,
                              approval_observer);
    };

    // P2(显示系统剥离单):异步审批通道——同一份裁定与问话逻辑包成
    // InteractionBroker 的终端实现。终端这条路是"当场问完"的同步短路:
    // AskApproval 里直接跑完整段(档位裁定 + PermissionRequest 钩子 +
    // 三档菜单/[y/a/N] + settings.local.json 追问),Wait 立刻拿结果,
    // 行为与今日一字不差;远端前端(app-server/Web/Tauri)后续换成登记
    // request_id 悬起的实现,内核零改动。on_tool_confirm_async 缺位的旧
    // 路(子代理/PTC 转发、单测)不走这里,照旧同步、不许多线程化。
    callbacks.on_tool_confirm_async =
        [auto_confirm, &always_allowed_tools, &theme, &display, &allow_commands, &deny_commands, hook_dispatcher,
         pre_decision_slot, has_permission_hooks, approval_observer](const lubancode::agent::ApprovalRequest& request)
        -> std::shared_ptr<lubancode::agent::InteractionFuture> {
        const bool allowed =
            ConfirmToolUse(request.tool_use_id, auto_confirm, always_allowed_tools, theme, display, allow_commands,
                           deny_commands, hook_dispatcher, *pre_decision_slot, has_permission_hooks,
                           request.tool_name, request.input, approval_observer);
        lubancode::agent::ApprovalResponse response;
        response.decision = allowed ? lubancode::agent::ApprovalDecision::Accept
                                    : lubancode::agent::ApprovalDecision::Decline;
        return std::make_shared<ReadyApprovalFuture>(std::move(response));
    };

    // 回合视觉收束:step 边界入账(批次边界在下面)。零成本旁路,不设
    // 回调的既有消费方(单测)行为不变。
    callbacks.on_model_step_started = [view_collector](int step_index) {
        if (view_collector != nullptr) {
            view_collector->OnModelStepStarted(step_index);
        }
    };

    // 回合视觉收束:批次边界接线。同一条 assistant message 吐多枚 tool_use
    // 时,batch.started 先到——把整批条目都立成 Pending(绿点变黄灯"排队
    // 中"),工具一枚枚真开跑时逐枚点亮 Running。单子"工具批次"节:用户
    // 一眼能看出"模型这拍打算跑三件",也能看出卡在哪一件。单枚批次画面
    // 不变(登记随即被 start 覆盖)。
    callbacks.on_tool_batch_started = [&display, view_collector](int step_index, int batch_index,
                                                 const std::vector<std::string>& ordered_tool_use_ids) {
        if (view_collector != nullptr) {
            view_collector->OnToolBatchStarted(step_index, batch_index, ordered_tool_use_ids);
        }
        if (ordered_tool_use_ids.size() <= 1) {
            return;  // 一枚不算批:结构留在账里,画面只靠连续缩进与间距成块
        }
        display.OnBatchAnnounced(ordered_tool_use_ids);
    };

    callbacks.on_tool_batch_finished = [&display, view_collector](int batch_index, bool interrupted) {
        if (view_collector != nullptr) {
            view_collector->OnToolBatchFinished(batch_index, interrupted);
        }
        if (interrupted) {
            display.OnBatchSkipped();  // 还 Pending 的按 Skipped 定格,屏上不缺枚
        }
    };

    callbacks.on_tool_done = [&display, &body_tracker, recorder, view_collector, cancel_flag,
                              trace_projection_installed](
                                                                 const std::string& tool_use_id,
                                                                 const std::string& name,
                                                                 const lubancode::tools::Tool::Result& result) {
        if (recorder != nullptr && !trace_projection_installed) {
            recorder->RecordToolResult(name, result.is_error, result.content);
        }
        if (view_collector != nullptr) {
            // ESC 后补的合成结果(is_error 且 cancel 已置)按 Interrupted 记,
            // 不冒充跑过又失败;真失败照 Failed。
            std::optional<lubancode::runtime::TurnItemViewState> forced;
            if (result.is_error && cancel_flag != nullptr && cancel_flag->load()) {
                forced = lubancode::runtime::TurnItemViewState::Interrupted;
            }
            view_collector->OnToolFinished(tool_use_id, result.content, result.is_error, forced);
        }
        display.OnToolDone(tool_use_id, name, result);
        body_tracker.OnToolBlockDone();
    };

    callbacks.on_builtin_tool_start = [&display, &body_tracker](const std::string& tool_use_id,
                                                                 const std::string& name,
                                                                 const nlohmann::json& input) {
        display.OnThinkingDone();  // 思考块若有,先收尾
        body_tracker.OnBlockBreak();
        display.OnToolStart(tool_use_id, name, input);
    };
    callbacks.on_builtin_tool_done = [&display, &body_tracker](const std::string& tool_use_id,
                                                                const std::string& name,
                                                                const nlohmann::json& input,
                                                                const std::string& summary, bool is_error) {
        display.OnBuiltinToolDone(tool_use_id, name, input, lubancode::tools::Tool::Result{summary, is_error});
        body_tracker.OnToolBlockDone();
    };

    callbacks.on_usage = [&display, &usage_stats, &context_tracker, view_collector](const lubancode::api::UsageReport& report) {
        // 请求结束:思考块若无后续文本/工具接上(只思考不回答的极端情况),
        // 在这里收尾。有后续时 OnThinkingDone 是幂等空操作。
        display.OnThinkingDone();
        if (view_collector != nullptr) {
            view_collector->OnUsage(report);
        }
        usage_stats.Add(report);
        // ContextTracker 只认"最近一次请求"的真实用量,整个覆盖,不跟着
        // usage_stats 一起累加——语义区别见 cli/context_tracker.hpp 文件头。
        // ApplyUsage 兼管"provider 没回 usage"(四项全零)的语义:不清零、
        // 只把现有数字标成旧值;ESC/HTTP 错误路径压根走不到 on_usage,不会
        // 把旧数伪装成本次新值。
        context_tracker.ApplyUsage(report.usage);
        // usage 一到就把 context/tokens 两段发布给状态行数据源——只改数据
        // 不落笔(锁与重画事务在 cli::UpdateStatusLineContext 里),回合内
        // 状态栏跟着前进,不必等整轮收口回外层循环重建快照;外层重建与这里
        // 读的是同一只 tracker,同一笔数,不存在先新后旧。子代理的 usage 走
        // agent_tool 那份钩子(见下),不进这里、不碰 tracker——主 context
        // 不被独立子代理的上下文虚抬。
        lubancode::cli::UpdateStatusLineContext(
            context_tracker.UsagePercent(), static_cast<std::int64_t>(context_tracker.current_tokens()),
            static_cast<std::int64_t>(context_tracker.window_tokens()), !context_tracker.usage_stale(),
            // 缓存注记(缓存诊断单):cached_tokens 有则摆本场命中与命中率,
            // 没回就写"未报告"——同一个 0 不糊。空闲重建那路(InteractiveSession
            // 每圈)读同一只 helper,两处口径一致。
            lubancode::cli::BuildCacheNote(context_tracker, report.reported()));
    };

    // agent 工具(注册了的话)需要这一轮现算好的转发逻辑:确认回调直接
    // 转发父级那份(三档确认模式照管子代理);usage 累进 usage_stats(统计
    // 行的请求次数、输入输出 token 都要算上子代理那几次请求)但不动
    // context_tracker、也不发布状态行——子代理是完全独立的上下文,它的
    // 用量跟"主对话历史占用多大"是两回事,冲进去反而会把 /context 的数字
    // 带偏成子代理那次请求的大小,而不是主对话真实占用;子代理结束后主
    // 模型带着它的结论发下一次请求,按那次的 usage 再更新。
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry.Find("agent"));
        agent_tool != nullptr) {
        lubancode::tools::AgentTool::Hooks hooks;
        // P2(显示系统剥离单):确认转发走旧同步 on_tool_confirm——子代理
        // 的任务线程不该吃 async future(那要 Broker 悬起表配合,后台任务
        // "没人可问"的同步短路正是要保住的路径)。主轮回合有 async 就走
        // async,子代理转发保持同步,两不串。
        hooks.on_tool_confirm = callbacks.on_tool_confirm;
        // ESC/Ctrl+C 打断信号透传:没这一行,子代理内部工具循环永远拿到
        // nullptr,顶层怎么置位 cancel_flag 都传不进去——子代理会一路跑到
        // 自己的步数上限(max_steps_per_turn)或任务自然完成才停,ESC/Ctrl+C 对它形同虚设。
        hooks.cancel = cancel_flag;
        // UI-B:子代理内层工具也走条目样式(前缀缩进四空格),状态同样原地
        // 更新——启动靠 on_sub_tool_start,终态靠下面包了一层的 post_tool
        // 钩子(agent 工具没有单独的"子工具结束"回调,post_tool 正好在真
        // 执行完之后带着 Result 触发一次,借它回写;拒绝那条路在确认回调里
        // 已经定格,pre_tool 拦截另包一层)。
        hooks.on_sub_tool_start = [&display](const std::string& tool_use_id, const std::string& name,
                                            const nlohmann::json& input) {
            display.OnSubToolStart(tool_use_id, name, input);
        };
        hooks.on_usage = [&usage_stats, &display](const lubancode::api::UsageReport& report) {
            usage_stats.Add(report);
            display.agent_step_count += 1;  // 子代理每一次独立请求算一步,agent 条目终态摘要用
            // 子代理自己的 token/工具次数/耗时都记在 AgentTool 统一台账里,
            // 代理面板(footer 里那块)按修订号自己刷新,这里不再另记一本。
        };
        // M9:pre_tool/post_tool 钩子照旧转发给父级同一份;UI-B 在外面再包
        // 一层,给子工具条目回写终态/拦截态用。
        if (callbacks.on_pre_tool_hook) {
            hooks.on_pre_tool_hook = [&display, base = callbacks.on_pre_tool_hook](
                                          const std::string& tool_use_id, const std::string& name,
                                          const nlohmann::json& input) -> std::optional<std::string> {
                std::optional<std::string> blocked = base(tool_use_id, name, input);
                if (blocked.has_value()) {
                    display.OnSubBlocked(tool_use_id, *blocked);
                }
                return blocked;
            };
        }
        // hooks 第三步:新回调同样转发(deny 定格由 phase(Blocked) 走 display
        // 路由,这里不重复画)。
        hooks.on_pre_tool_use_hook = callbacks.on_pre_tool_use_hook;
        hooks.on_permission_request = callbacks.on_permission_request;
        hooks.on_tool_phase = callbacks.on_tool_phase;
        // Plan 模式:子代理同过 ModePolicy(单子:Explore 子代理拿同一
        // Plan mode + 更窄 allowlist,不因独立 context 逃闸)。
        hooks.on_mode_policy = callbacks.on_mode_policy;
        hooks.hook_dispatcher = hook_dispatcher;
        // 逐枚追踪单:子代理内层工具事件并轨进主会话的 trace hub。hub 的
        // OnTrace 自带锁与单 writer 落盘,子代理任务线程投递不会跟主
        // JSONL 交错(单子 agent/PTC 节)。parent_execution_id 延迟取值
        // (agent 工具真正开跑时才由 hub 钉)。
        if (trace_hub != nullptr) {
            hooks.on_tool_trace = [trace_hub](const lubancode::agent::ToolTraceEvent& event) {
                trace_hub->OnTrace(event);
            };
            hooks.parent_execution_id_getter = [trace_hub]() {
                return trace_hub->current_agent_execution();
            };
        }
        if (callbacks.on_post_tool_use_hook) {
            hooks.on_post_tool_use_hook = [&display, base = callbacks.on_post_tool_use_hook](
                                              const std::string& tool_use_id, const std::string& name,
                                              const nlohmann::json& input,
                                              const lubancode::tools::Tool::Result&
                                              result) -> std::vector<std::string> {
                const std::vector<std::string> feedback = base(tool_use_id, name, input, result);
                display.OnSubToolResult(tool_use_id, name, input, result);
                return feedback;
            };
        }
        hooks.on_post_tool_hook = [&display, base = callbacks.on_post_tool_hook](
                                       const std::string& tool_use_id, const std::string& name,
                                       const nlohmann::json& input,
                                       const lubancode::tools::Tool::Result& result) {
            if (base) {
                base(tool_use_id, name, input, result);
            }
            display.OnSubToolResult(tool_use_id, name, input, result);
        };
        agent_tool->SetHooks(std::move(hooks));
    }

    // PTC 工具(注册了的话):脚本里每一枚 stub 调用都要过这一轮的完整
    // 执行链——PreToolUse/权限/PostToolUse 原样转发(schema 复检与审计在
    // agent::RunOneTool 里),Esc 取消链透传。展示回调(on_tool_start/
    // on_tool_done)刻意不转发:规格 UI 节要求 PTC 只画一张卡、聚合行在
    // 结果文本里,不把 16 枚同构工具卡刷满屏;外层那枚
    // programmatic_tool_calling 调用照常走 display 的一条卡。
    if (auto* ptc_tool = dynamic_cast<lubancode::ptc::PtcTool*>(registry.Find("programmatic_tool_calling"));
        ptc_tool != nullptr) {
        lubancode::ptc::PtcTool::Hooks hooks;
        // P2:同 agent 工具,转发走旧同步回调(PTC 脚本线程不吃 async future)。
        hooks.on_tool_confirm = callbacks.on_tool_confirm;
        hooks.on_pre_tool_use_hook = callbacks.on_pre_tool_use_hook;
        hooks.on_permission_request = callbacks.on_permission_request;
        hooks.on_tool_phase = callbacks.on_tool_phase;
        hooks.on_post_tool_use_hook = callbacks.on_post_tool_use_hook;
        // Plan 模式:stub 调用同过 ModePolicy(单子明令)。
        hooks.on_mode_policy = callbacks.on_mode_policy;
        hooks.cancel = cancel_flag;
        ptc_tool->SetHooks(std::move(hooks));
    }

    // 插件工具(plugins 单第 7 步的 ESC 链):process 插件的 adapter
    // (进程超时/取消同一落锤路)与 Lua 工具(hook 里查旗掐死循环)每轮
    // 灌这一轮的取消旗。registry 是本轮实际在用的表(main/sub 都从这走)。
    // run_command 同链(进程生命线单 P1:前台命令的取消通道——ESC 不再
    // 只能等 120 秒超时)。
    if (cancel_flag != nullptr) {
        for (const auto& tool : registry.All()) {
            if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(tool.get());
                plugin_adapter != nullptr) {
                plugin_adapter->SetCancel(cancel_flag);
            }
            if (auto* lua_tool = dynamic_cast<lubancode::tools::LuaTool*>(tool.get()); lua_tool != nullptr) {
                lua_tool->SetCancel(cancel_flag);
            }
            if (auto* run_command = dynamic_cast<lubancode::tools::RunCommandTool*>(tool.get());
                run_command != nullptr) {
                run_command->SetCancel(cancel_flag);
            }
        }
    }

    return callbacks;
}


std::string ImageInputErrorText(const lubancode::cli::ImageInputError& error) {
    using Kind = lubancode::cli::ImageInputErrorKind;
    switch (error.kind) {
        case Kind::MissingPath: return tr("error.image.missing_path");
        case Kind::NotFound: return trf("error.image.not_found", error.path);
        case Kind::NotRegularFile: return trf("error.image.not_regular", error.path);
        case Kind::UnsupportedType: return trf("error.image.unsupported", error.path);
        case Kind::TooLarge: return trf("error.image.too_large", error.path);
        case Kind::ReadFailed: return trf("error.image.read_failed", error.path);
        case Kind::InvalidImage: return trf("error.image.invalid", error.path);
    }
    return trf("error.image.read_failed", error.path);
}

// 发一轮用户输入,走 agent loop(可能会有若干次工具调用来回),流式打字机
// 打印回复,结束后打一行 token 用量统计(暗色/淡色,plain 主题下就是空
// 前后缀)。always_allowed_tools 由调用方持有,记录本会话内选过"总是允许"
// 的工具。registry 是这一轮实际在用的工具表,传给 BuildCallbacks 好给里头
// 的 agent 工具(如果有)灌这一轮的转发钩子。
//
// M10:这里起一条 TurnInputListener,存活区间正好是"发出请求到本轮 Run()
// 结束"——ESC 打断、消息排队都靠它。真控制台之外(管道/重定向)监听器
// 构造函数自己判断不起线程,行为跟 0.7.0 完全一致。
// is_console:M11(0.10.0)新增,决定要不要打输入/输出分界线(管道/重定向
// 模式恒为假,分界线完全不出现,不污染被重定向的输出)。todo_state 同样
// M11 新增,转发给 BuildCallbacks 给 on_tool_done 用;留空指针表示这一轮
// 的 registry 没注册 todo_write(目前两个调用点都注册了,这个默认值只是
// 留个口子)。
// transcript:UI-B(0.12.0)新增,会话级工具条目存档(InteractiveLoop/
// AskOnce 各持有一份,跨多轮累积),UI-C/D 的 Ctrl+E 全文查看要用。
// transcript_expanded:UI-D(0.16.0)紧凑/详细会话级开关(Ctrl+O 翻转,
// InteractiveLoop 持有),详细态下新条目直接按展开版画;回合中切档还会
// 从线程安全快照把现有条目整组重打。AskOnce 不传(nullptr),恒紧凑。
// atomic<bool>:回合执行
// 期间监听线程(另一个线程)会写、Run() 所在的这个线程会读,真机驱动器
// 实测踩到过普通 bool 在这条跨线程路径上的可见性问题(写了但读的那一刻
// 还没看见),换成 atomic<bool> 用 load/store 的 acquire/release 语义堵上。
// allow_commands/deny_commands:settings.local.json 的 run_command 前缀白/黑
// 名单,原样递给 BuildCallbacks 的确认回调叠加判定(缺省空表 = 无叠加)。
RunTurnResult RunTurn(lubancode::agent::AgentLoop& loop, const std::string& user_input, bool auto_confirm,
                       std::set<std::string>& always_allowed_tools, const lubancode::cli::Theme& theme,
                       lubancode::cli::ContextTracker& context_tracker, lubancode::tools::ToolRegistry& registry,
                       lubancode::hooks::HookDispatcher* hook_dispatcher, bool is_console,
                       std::vector<lubancode::cli::TranscriptItem>& transcript,
                       std::shared_ptr<lubancode::tools::TodoListState> todo_state,
                       std::atomic<bool>* transcript_expanded,
                       const std::vector<std::string>& allow_commands,
                       const std::vector<std::string>& deny_commands,
                       lubancode::tools::AgentTool* completion_agent,
                       lubancode::agent::WorkflowRecorder* recorder, bool silent,
                       lubancode::runtime::TurnUsageStats* usage_out,
                       lubancode::runtime::ToolTraceHub* turn_trace_hub,
                       std::string thread_id_for_trace, std::string turn_id_for_trace,
                       lubancode::runtime::TurnView* turn_view_out,
                       std::function<std::string(const std::string&, const nlohmann::json&)> mode_gate,
                       std::function<void(bool asked, bool allowed)> approval_observer) {
    auto prepared_input = lubancode::cli::PrepareImageInput(user_input);
    if (!prepared_input.has_value()) {
        std::cerr << theme.error << tr("error.prefix") << ImageInputErrorText(prepared_input.error())
                  << theme.reset << "\n";
        return RunTurnResult{1};
    }
    for (const auto& image : prepared_input->attachments) {
        if (!silent) {
            std::cout << theme.stats << trf("image.attached", image.filename, image.width, image.height)
                      << theme.reset << "\n";
        }
    }
    const std::string background_results =
        completion_agent != nullptr ? completion_agent->DrainCompletionNotices() : std::string();

    // UserPromptSubmit:用户 prompt 送模型前。可阻断(continue=false/exit 2,
    // 这一轮不发模型、不算错误),可追加 developer context(原 prompt 不动,
    // 注入文本带来源标识单独成块,不串成一坨)。背景回流通知的"不可信参考
    // 资料"声明也走同一口(P3 起决策与组装在 runtime::ApplyUserPromptSubmit,
    // 这里只接阻断的收口与上屏)。
    const lubancode::runtime::PromptGate gate = lubancode::runtime::ApplyUserPromptSubmit(
        hook_dispatcher, user_input, background_results, prepared_input->message);
    if (gate.blocked) {
        std::cerr << theme.error << tr("error.prefix") << "UserPromptSubmit 钩子阻断本轮: " << gate.block_reason
                  << theme.reset << "\n";
        return RunTurnResult{0};
    }
    for (const std::string& ctx : gate.additional_context) {
        prepared_input->message.content.push_back(lubancode::api::TextBlock{ctx});
    }

    // 安全点(轮起):后台子代理投递的 hooks 记录在这里归并落账。告警走
    // stderr(静默档也要让人看见降级),信息行只在非静默档打。
    for (const std::string& notice : lubancode::app::AdoptBackgroundHookRecordNotices()) {
        if (!silent) {
            std::cout << theme.stats << "[hooks] " << notice << theme.reset << "\n";
        } else {
            std::cerr << "[hooks] " << notice << "\n";
        }
    }

    // (UserPromptSubmit 与背景回流声明已在上面 runtime::ApplyUserPromptSubmit
    // 一口收账,这里不再另发一遍。)

    lubancode::runtime::TurnUsageStats usage_stats;
    // 视图账(终端回合视觉收束单第 3 步:正文入账):TurnCollector 与
    // ToolDisplay 并行记账——屏上逐字不变(现有 painter 一根毛不动),
    // collector 攒 TurnView 给 Ctrl+L/resume 重放与将来的 TerminalTurnRenderer
    // 整轮重画。slash/本地校验失败的轮到不了这里,不造假账。
    lubancode::runtime::IdAuthority& view_ids = lubancode::runtime::ProcessIdAuthority();
    lubancode::runtime::TurnCollector view_collector(view_ids, view_ids.NextTurnId());
    view_collector.StartTurn(
        user_input, std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count());
    // 轮级核(P3):cancel 旗挪进 runtime::TurnRuntime——监听线程写
    // (request_interrupt)、Run 线程读(interrupted),acquire/release 语义
    // 原文照搬。ToolDisplay/BuildCallbacks/loop.Run 收它的地址,行为与
    // 从前那只裸 atomic<bool> 一字不差。
    lubancode::runtime::TurnRuntime turn_core(BuildTurnRuntimeOptions(auto_confirm, always_allowed_tools,
                                                                       allow_commands, deny_commands,
                                                                       hook_dispatcher));
    std::atomic<bool>& cancel_flag = turn_core.cancel;
    ToolDisplay display(transcript, theme, is_console, todo_state, &cancel_flag, transcript_expanded, silent);
    // markdown:正文两段式渲染的记账员。渲染只活在真控制台 + 彩色主题——
    // 管道/重定向(is_console 为假)和 plain 主题(theme.reset 空)全部
    // enabled=false,正文原样输出,一个字节不动。silent 档正文不上屏,
    // 只攒进台账(见 StreamBodyTracker 注释)。
    StreamBodyTracker body_tracker(theme, is_console && !theme.reset.empty(), silent);
    // hooks 上下文:这一轮的 turn_id 换新(session_id 等会话字段保持会话层
    // 设好的值),确认档可能被 Shift+Tab 切过,按当前值报。
    if (hook_dispatcher != nullptr && !hook_dispatcher->Empty()) {
        lubancode::hooks::HookContext turn_context = hook_dispatcher->context();
        turn_context.turn_id = lubancode::hooks::HookDispatcher::NextHookRunId();
        turn_context.permission_mode = lubancode::app::HookPermissionModeText();
        hook_dispatcher->UpdateContext(std::move(turn_context));
    }
    lubancode::agent::Callbacks callbacks =
        BuildCallbacks(auto_confirm, always_allowed_tools, theme, usage_stats, context_tracker, registry,
                        hook_dispatcher, display, body_tracker, allow_commands, deny_commands, &cancel_flag,
                        recorder, turn_trace_hub, &view_collector, mode_gate, approval_observer);
    if (turn_trace_hub != nullptr) {
        if (recorder != nullptr) {
            turn_trace_hub->AttachProjection(
                [recorder](const lubancode::agent::ToolTraceEvent& event) {
                    if (event.kind == lubancode::agent::ToolTraceEventKind::Scheduled) {
                        recorder->RecordToolCall(event.tool_name, nlohmann::json::object(), event.execution_id,
                                                 event.tool_use_id);
                    } else if (event.kind == lubancode::agent::ToolTraceEventKind::ExecutionFinished) {
                        recorder->RecordToolResult(event.tool_name,
                                                   event.outcome != lubancode::agent::ToolOutcome::Succeeded,
                                                   event.fallback_message, lubancode::agent::ToString(event.outcome),
                                                   event.error_code, event.execution_id);
                    }
                });
        }
        turn_trace_hub->Install(loop, callbacks, thread_id_for_trace, turn_id_for_trace);
        // 补偿关系边(单子第四期):undo_file_edit execute 后报"这枚补偿
        // 谁",finished 栅栏随账落 compensates。
        callbacks.on_tool_compensates = [&registry](const std::string& /*execution_id*/,
                                                    const std::string& tool_name) -> std::string {
            if (tool_name != "undo_file_edit") {
                return std::string();
            }
            const auto* tool = registry.Find(tool_name);
            if (tool == nullptr) {
                return std::string();
            }
            const auto* undo_tool = dynamic_cast<const lubancode::tools::UndoFileEditTool*>(tool);
            return undo_tool != nullptr ? undo_tool->last_compensates() : std::string();
        };
    }

    // 逐枚追踪单:hub 装进 callbacks(canonical 事件出口 + 消息落盘次序
    // 关口);recorder 挂成投影,只吃 execution_id/outcome/摘要。


    // markdown × M10:监听线程随时可能在流式正文当中插打 [已排队]/[已打断]
    // 整行——这几行不在 body_tracker 的行数账里,不通气的话收束重画会把
    // 排队回显擦掉、贴着缓冲区底时锚点还会错行。钩子在监听线程持
    // StdoutWriteMutex 时被调(跟 OnDelta 同一把锁),Stop()(join 完)之后
    // 立刻摘掉,绝不活过 body_tracker。
    lubancode::cli::SetStreamScreenPrintHook([&body_tracker] { body_tracker.InvalidateBlockAnchor(); });
    lubancode::cli::SetStreamScreenScrollHook([&display, &body_tracker](int rows) {
        display.OnScreenScrolledLocked(rows);
        body_tracker.OnScreenScrolledLocked(rows);
    });

    // 用户这一行已经提交、真要开始等模型作答了——分界线打在这儿,紧跟在
    // 提示符那一行之后、模型正文开始打字机输出之前。
    // 用户输入背景块(终端用户输入背景块单):真控制台的 user surface 已由
    // composer 收框那一笔落下(CollapseBoxOnSubmit 直接铺 FormatUserPromptBlock,
    // 与 resume/Ctrl+L 同源——同一笔 transaction 退 editing chrome、落背景,
    // 不先收成裸文本再回头涂),这里只按间距表垫一口块后气口(UserPrompt ->
    // 任意 = 1),信息收尾不贴脸。管道/重定向没有 composer 收框,保持稳定
    // 纯文本,不补这一口。
    PrintDivider(theme, is_console && !silent);
    if (is_console && !silent && !user_input.empty()) {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        for (int g = 0;
             g < lubancode::cli::GapBetween(lubancode::cli::BlockRole::UserPrompt,
                                            lubancode::cli::BlockRole::Thinking);
             ++g) {
            std::cout << "\n";
        }
        std::cout.flush();
    }

    // turn 墙钟(终端回合视觉收束单):起点是"用户输入过了本地校验、正式
    // 交给 turn runtime"那一刻(本函数顶上的 prepared/gate 都已过,这里就
    // 是起跑线);终点在 footer 落笔前。steady_clock,不受系统改钟影响;
    // 墙上时间只作日志字段。
    const std::chrono::steady_clock::time_point turn_wall_start = std::chrono::steady_clock::now();
    // turn 级 Working 活动条:认整个 turn,不认单次模型请求(单子第六节)。
    // 正文流、工具批次、下一次模型请求、重试都不熄、不归零;EndTurnActivity
    // 在收口时熄,同一只钟交给 Worked footer。
    if (is_console && lubancode::platform::SupportsScreenRepaint() && !silent) {
        lubancode::cli::BeginTurnActivity(
            lubancode::cli::tr("spinner.thinking"),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    // 0.21.x 流式脚注:流式期间在正文下方常驻一行"⎋ 打断 · 键入并回车 排队
    // 下一条",让用户看见能 ESC 打断、能键入排队(回归前屏上啥都没有)。只在
    // Windows 真控制台开——footer 要随时查光标位,POSIX 走 DSR 6n 会跟监听
    // 线程抢 stdin,跟 StreamBodyTracker 的重画一样诚实关掉。静默档不起
    // footer:屏幕此刻归用户正看的查看帧,main 的回流轮一个字节不铺。
    lubancode::cli::BeginStreamFooter(theme, is_console && lubancode::platform::SupportsScreenRepaint() && !silent);

    // 子代理状态的心跳:旧 AgentStatusBoard/AgentStatusPainter 那套 400ms
    // ticker 已删,前台/后台子代理状态全在 AgentTool 统一台账里、由 footer
    // 的代理面板画。长工具调用期间没有正文增量、没有按键,footer 不会自己
    // 醒——这里起一只轻量心跳线程,200ms 一拍调 RedrawStreamFooterLocked
    // (内部自己看挂起/事务计数,菜单占屏时零输出),Running 的耗时与灯才
    // 会走。只活 loop.Run() 这一段,Run() 一返回就停。
    // 回合视觉收束:心跳同管 turn 活动条——秒数从 turn_wall_start 现算
    // (一秒一跳),走字扫光沿 "Working" 七个字母缓扫(帧率 200ms 一拍,
    // 字符数与显示宽恒不变,不拿 -\|/ 换字符引起抖动)。
    const class FooterHeartbeat {
    public:
        explicit FooterHeartbeat(bool enabled, const std::chrono::steady_clock::time_point* turn_start,
                                 const std::atomic<bool>* cancel_ptr) {
            if (!enabled) {
                return;
            }
            thread_ = std::thread([this, turn_start, cancel_ptr] {
                try {
                    std::size_t frame = 0;
                    bool stopping_reported = false;
                    while (!stop_.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        if (stop_.load(std::memory_order_acquire)) {
                            return;
                        }
                        // TurnActivityActive/Set/Update 都自带 StdoutWriteMutex。
                        // 旧代码先在这里攥锁，再调它们，MSVC 会以
                        // resource_deadlock_would_occur 报同线程二次上锁；异常
                        // 又从 std::thread 顶漏出，整进程便以 0xC0000409 快退。
                        // 活动态只走自带锁的公开口。非活动态才在本层拿锁，
                        // 调用约定为“调用方已持锁”的 Redraw...Locked。
                        if (lubancode::cli::TurnActivityActive()) {
                            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                                     std::chrono::steady_clock::now() - *turn_start)
                                                     .count();
                            // ESC 真置了 cancel:活动条换 Stopping(终态落账后才
                            // 退场,不瞬间消失让人以为已停、后台却还在跑)。
                            if (cancel_ptr != nullptr && cancel_ptr->load(std::memory_order_acquire)) {
                                if (!stopping_reported) {
                                    lubancode::cli::SetTurnActivityInterruptRequested();
                                    stopping_reported = true;
                                }
                            }
                            lubancode::cli::UpdateTurnActivityElapsed(frame, elapsed);
                        } else {
                            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
                            if (lubancode::cli::RepaintSuspendedLocked()) {
                                continue;  // 菜单占屏/挂起:零输出,秒数照走(账不丢)
                            }
                            lubancode::cli::RedrawStreamFooterLocked();
                        }
                        ++frame;
                    }
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "\n[footer-heartbeat] %s\n", e.what());
                    std::fflush(stderr);
                } catch (...) {
                    std::fprintf(stderr, "\n[footer-heartbeat] unknown exception\n");
                    std::fflush(stderr);
                }
            });
        }
        ~FooterHeartbeat() {
            stop_.store(true, std::memory_order_release);
            if (thread_.joinable()) {
                thread_.join();
            }
        }
        FooterHeartbeat(const FooterHeartbeat&) = delete;
        FooterHeartbeat& operator=(const FooterHeartbeat&) = delete;

    private:
        std::atomic<bool> stop_{false};
        std::thread thread_;
    } footer_heartbeat(is_console && lubancode::platform::SupportsScreenRepaint() && !silent, &turn_wall_start,
                      &cancel_flag);

    // 监听线程:流式期间的面板按键、排队/打断都在它手里(键位优先级见
    // TurnInputListener::ThreadMain)。
    lubancode::cli::TurnInputListener listener(
        cancel_flag, theme, transcript_expanded,
        [&display](bool expanded) { return display.FormatSnapshotForToggleLocked(expanded); });

    // 回合级异常兜底(宽窄转换异常单):流式/工具链路里任何 std::exception
    // ——历史病灶是 path 窄转换的 system_error(1113),文案正是"No mapping
    // for the Unicode character..."——不再穿透顶层把整场会话掀了。转成本
    // 回合的失败收口:走下面共用的错误路径(listener 停、footer 收、错误
    // 上屏),交回 status=1;外层会话继续跑,prompt 回得来,已入 history
    // 的部分(用户消息)照常落盘。异常类型一并打出来,真机再出事有姓有名。
    std::expected<lubancode::agent::RunOutcome, std::string> result;
    try {
        result = loop.Run(std::move(prepared_input->message), callbacks, &cancel_flag);
    } catch (const std::exception& e) {
        result = std::unexpected(trf("error.unexpected", e.what()) + std::string(" [") + typeid(e).name() + "]");
    }

    // Run() 已经返回,不管是不是被打断——先收输入线程，保证它不再碰转录
    // 快照；也保证下一次 ReadLine() 前不再抢控制台输入。心跳线程随后收。
    listener.Stop();
    lubancode::cli::SetStreamScreenPrintHook(nullptr);  // 线程已 join,摘钩,别让它抓着局部引用过夜
    // streaming→idle 的交接是一笔事务,次序钉死不许倒:
    //   1) 收 streaming footer(EndStreamFooter 里的 EraseStreamFooterLocked,
    //      按上一帧的账整框擦净、光标拨回正文续写位);
    //   2) 写最终正文(body_tracker.FinalizeRepaint,markdown 收束重画);
    //   3) 统计行 + 尾分界线(顺序打印,窗口自然跟行);
    //   4) 主循环回到 ReadLine 画 idle composer/代理坞/状态栏,首帧经
    //      EnsureViewportRowsLocked(帧账的"保锚可见"原语)把整帧纳入可视区
    //      ——长缓冲平移视口、贴底滚内容,不许靠改字号/滚轮/Ctrl+End 救场。
    // 本函数只管前三步;第四步在 console_input 的 composer 首帧里,帧账
    // 一处收口(规格《多智能体真机回归_可视区重排与查看态》)。
    lubancode::cli::EndStreamFooter();
    lubancode::cli::SetStreamScreenScrollHook(nullptr);

    RunTurnResult out;
    // 0.28.x:流式期间排队的消息不在这里搬运了——监听线程直接写会话层
    // SteeringQueue,投递由 InteractiveSession 的会话泵在安全点接手。

    // markdown 两段式的后一段:回合正常收束(没报错、没被 ESC 打断)才把
    // 最后一块正文按渲染版重画;半截话/报错现场保持原样,不赌。
    if (result.has_value() && !result->cancelled) {
        body_tracker.FinalizeRepaint();
    }

    // 静默档收尾:正文一个字都没上过屏,但一个字也不能丢——整段归档成一条
    // transcript 条目(工具条目与思考块本来就进了台账),回 main 重铺/Ctrl+E
    // 聚焦查看全都能看见。打断/报错的半截话照归,状态如实标。
    if (silent) {
        std::string silent_body = body_tracker.TakeSilentBody();
        if (!silent_body.empty()) {
            lubancode::cli::TranscriptItem item;
            item.id = static_cast<int>(transcript.size()) + 1;
            item.kind = lubancode::cli::TranscriptKind::Tool;
            item.tool_name = "assistant";
            item.title = tr("transcript.assistant_bg_title");
            item.status = !result.has_value() ? lubancode::cli::TranscriptStatus::Error
                          : result->cancelled ? lubancode::cli::TranscriptStatus::Interrupted
                                              : lubancode::cli::TranscriptStatus::Ok;
            item.start_time = item.end_time = std::chrono::steady_clock::now();
            item.full_output = std::move(silent_body);
            // 紧凑档摘要:正文头两行,每行掐 120 码点——渲染层还会按终端宽
            // 截,这里先兜住 Ctrl+E(不截宽)那一路。
            std::size_t cursor = 0;
            for (int taken = 0; taken < 2 && cursor < item.full_output.size(); ++taken) {
                std::size_t cut = item.full_output.find('\n', cursor);
                const std::string line = item.full_output.substr(cursor, cut == std::string::npos
                                                                            ? std::string::npos
                                                                            : cut - cursor);
                if (!line.empty()) {
                    item.summary_lines.push_back(
                        lubancode::cli::TruncateUtf8Codepoints(line, 120));
                }
                if (cut == std::string::npos) {
                    break;
                }
                cursor = cut + 1;
            }
            transcript.push_back(std::move(item));
        }
    }

    if (!silent) {
        std::cout << "\n";
    }

    // turn 收口的共用半段(终端回合视觉收束单):熄活动条、算墙钟、收
    // 视图账。三条路(错误早退/打断/正常)都从这里过——footer 恰一枚,
    // 不再从中途裸退。
    const auto finish_turn_chrome = [&](lubancode::cli::TurnFooterTone tone) {
        const long long activity_seconds = lubancode::cli::EndTurnActivity();
        (void)activity_seconds;  // Working 秒数与 footer 同钟(同一只 steady 钟),
                                 // 单测钉口径;这里不再二次对账,免得双写。
        const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - turn_wall_start)
                                 .count();
        using lubancode::runtime::TurnItemViewState;
        TurnItemViewState view_status = TurnItemViewState::Succeeded;
        switch (tone) {
            case lubancode::cli::TurnFooterTone::Stopped:
                view_status = TurnItemViewState::Interrupted;
                break;
            case lubancode::cli::TurnFooterTone::Failed:
                view_status = TurnItemViewState::Failed;
                break;
            case lubancode::cli::TurnFooterTone::Worked:
                break;
        }
        view_collector.FinishTurn(view_status, wall_ms, /*approval_wait=*/0);
        if (turn_view_out != nullptr) {
            *turn_view_out = view_collector.view();  // 会话层存档:Crtl+L/resume 重放用
        }
        // 静默档(查看态回流)不落:屏幕此刻归用户正看的查看帧。
        if (!silent) {
            PrintTurnFooter(theme, is_console, wall_ms, tone);
        }
    };

    if (!result.has_value()) {
        // 错误必须让人看见(本地兼容端 Effort 诊断单:xhigh 那次瞬时 exit 1,
        // transport 错误被屏上重画搅得若有若无)。三道保险:拿
        // StdoutWriteMutex 跟 footer/心跳线程错开;两个流都 flush(stderr
        // 无缓冲但 Windows 终端重定向下未必);行前垫换行,别粘在残留帧上。
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            std::cerr << "\n"
                      << theme.error << tr("error.prefix") << result.error() << theme.reset << "\n";
            std::cerr.flush();
            std::cout.flush();
        }
        out.status = 1;
        finish_turn_chrome(lubancode::cli::TurnFooterTone::Failed);  // Failed after Xs
        return out;
    }
    if (result->hit_step_limit) {
        // 主循环的步数硬闸(0.30.x 起从"报错"改为"预算耗尽"):loop 把它当
        // RunOutcome 而不是 error 交回来,这里按老口径打一行、记 status,不
        // 影响子代理那边按 budget_exhausted 收账带走部分结果。
        std::cerr << theme.error << tr("error.prefix")
                  << trf("error.step_limit", result->steps_used) << theme.reset << "\n";
        out.status = 1;
        finish_turn_chrome(lubancode::cli::TurnFooterTone::Failed);  // 预算耗尽也是 Failed
        return out;
    }
    out.cancelled = result->cancelled;
    // 输出预算耗尽也是失败收场(footer 词干用 Failed after),单立旗子
    // 免得跟 cancelled 互相盖。
    bool turn_failed = false;

    // 输出预算耗尽且正文为空(reasoning 吃光 max_tokens):明报,不留一片
    // 空白。规格根因四:结构化失败页——实际上限、已续次数、usage 是否
    // 报告、思考检查点、四条去路。usage 拆账顺带摆出来——output 里
    // reasoning 占多少一目了然;provider 没拆账(reasoning 字段缺席)就按
    // "未拆账"说,不猜 0。
    if (result->length_empty_output) {
        const std::int64_t reasoning_total = usage_stats.reasoning_tokens();
        std::cout << theme.error << trf("agent_outcome.output_budget.head", result->output_budget.continuations_used)
                  << theme.reset << "\n";
        if (result->output_budget.limit_tokens > 0) {
            std::cout << theme.stats << trf("agent_outcome.output_budget.limit", result->output_budget.limit_tokens)
                      << theme.reset << "\n";
        } else {
            std::cout << theme.stats << tr("agent_outcome.output_budget.limit_unset") << theme.reset << "\n";
        }
        std::cout << theme.stats
                  << trf("agent_outcome.output_budget.continuations", result->output_budget.continuations_used)
                  << theme.reset << "\n";
        std::cout << theme.stats
                  << tr(result->output_budget.usage_reported ? "agent_outcome.output_budget.usage_reported"
                                                             : "agent_outcome.output_budget.usage_not_reported")
                  << theme.reset << "\n";
        if (result->output_budget.thinking_bytes > 0) {
            std::cout << theme.stats
                      << trf("error.length_empty_reasoning_bytes", result->output_budget.thinking_bytes)
                      << theme.reset << "\n";
        }
        std::cout << theme.stats
                  << (reasoning_total > 0
                          ? trf("error.length_empty_reasoning", reasoning_total)
                          : tr("error.length_empty_no_split"))
                  << theme.reset << "\n";
        std::cout << theme.stats << tr("agent_outcome.output_budget.escapes") << theme.reset << "\n";
        std::cout.flush();
        out.status = 1;  // 一个字都没回,按失败收场——但话说清楚了,不是哑巴 1
        turn_failed = true;  // 收口 tone 用 Failed after,不用 Worked 糊
    }

    // Stop:主回合正常收束(没被打断、没撞预算、没报错)准备停时触发。
    // 钩子 continue=false = "还不能停,再续一轮":续跑理由作为带标识的
    // continuation prompt 入账,不伪装成用户输入;stop_hook_active 防咬尾,
    // 最多续一次。续跑轮没有输入监听(ESC 打断靠外层循环兜底),如实注明。
    if (hook_dispatcher != nullptr && !hook_dispatcher->Empty() &&
        hook_dispatcher->HasHandlersFor(lubancode::hooks::HookEvent::Stop) && !result->cancelled) {
        const auto last_assistant_text = [&loop]() {
            for (auto it = loop.history().rbegin(); it != loop.history().rend(); ++it) {
                if (it->role != lubancode::api::Role::Assistant) {
                    continue;
                }
                for (auto block = it->content.rbegin(); block != it->content.rend(); ++block) {
                    if (const auto* text = std::get_if<lubancode::api::TextBlock>(&*block)) {
                        return text->text;
                    }
                }
            }
            return std::string();
        }();

        bool stop_hook_active = false;
        for (int round = 0; round < 2; ++round) {
            lubancode::hooks::HookPayload payload;
            payload.event = lubancode::hooks::HookEvent::Stop;
            payload.fields["stop_hook_active"] = stop_hook_active;
            payload.fields["last_assistant_message"] = last_assistant_text;
            const auto merged = hook_dispatcher->Emit(lubancode::hooks::HookEvent::Stop, payload);
            if (!merged.blocked || stop_hook_active) {
                break;  // 没人拉闸,或已经续过一次(不许无限续)
            }
            std::cout << theme.stats << "[stop 钩子] 要求再收口一轮: " << merged.block_reason << theme.reset
                      << "\n";
            const auto continuation = loop.Run("[stop 钩子续跑,非用户输入] " + merged.block_reason, callbacks);
            if (!continuation.has_value() || continuation->cancelled || continuation->hit_step_limit) {
                break;  // 续跑轮报错/被打断/撞预算:如实停,不带病硬续
            }
            out.cancelled = out.cancelled || continuation->cancelled;
            stop_hook_active = true;
        }
    }

    // 安全点(轮收):后台子代理这轮攒下的 hooks 记录归并落账,报信一行。
    for (const std::string& notice : lubancode::app::AdoptBackgroundHookRecordNotices()) {
        if (!silent) {
            std::cout << theme.stats << "[hooks] " << notice << theme.reset << "\n";
        } else {
            std::cerr << "[hooks] " << notice << "\n";
        }
    }

    // 统计降噪(终端回合视觉收束单第七节):输入/缓存/输出/请求数/context
    // 长行不再每轮全摊——context 与缓存命中常驻底部状态栏(UpdateStatusLine
    // Context 已在 on_usage 局部发布),紧凑态 footer 只写总耗时;详细态
    // (Ctrl+O)才展开这行。管道/重定向(is_console 为假)没有状态栏,长行
    // 照打——稳定纯文本输出是 automation 的契约,不能静默吞。
    const bool stats_verbose = (transcript_expanded != nullptr && transcript_expanded->load()) || !is_console;
    if (usage_stats.request_count() > 0 && !silent && stats_verbose) {
        // 0.17.0:token 数字统一 k 化(cli::FormatTokenCount),超过 10k 的
        // 数字不再铺一长串数位。i18n:整行进表(stats.line),缓存那一节
        // 先拼好塞进 {1}。
        // 口径(前缀缓存守恒单):"输入"= TotalInputTokens(非缓存输入 +
        // 缓存读 + 缓存写)——DeepSeek 49k hit + 1k miss 显示为 50k 输入,
        // 不是 1k,也不是 99k。命中率分母只取输入,不带 output。
        // 缓存节按四态说话(缓存诊断单):not_reported / disabled /
        // enabled_no_hit / hit,同一个 0 不糊——"服务端未启用"只有
        // /doctor cache 从 metrics 读到 enable_prefix_caching=False 才说
        // (结论记在 ContextTracker),否则 0 命中如实带"是否启用未验证"。
        const std::string cache_part = [&]() {
            if (!usage_stats.any_reported()) {
                return tr("stats.cache_not_reported");
            }
            if (usage_stats.cache_read_tokens() > 0) {
                const int hit_percent = usage_stats.cache_hit_percent();
                return trf("stats.cache", lubancode::cli::FormatTokenCount(usage_stats.cache_read_tokens()),
                           hit_percent >= 0 ? std::to_string(hit_percent) : std::string("?"));
            }
            const auto server_enabled = context_tracker.server_prefix_caching();
            if (server_enabled.has_value() && !*server_enabled) {
                return tr("stats.cache_disabled");
            }
            return server_enabled.has_value() ? tr("stats.cache_no_hit_enabled")
                                              : tr("stats.cache_no_hit_unverified");
        }();
        std::cout << theme.stats
                  << trf("stats.line", lubancode::cli::FormatTokenCount(usage_stats.total_input_tokens()),
                         cache_part, lubancode::cli::FormatTokenCount(usage_stats.output_tokens()),
                         usage_stats.request_count(), context_tracker.UsagePercent())
                  << theme.reset << "\n";
    }
    // 回合正常结束(不是上面那条 !result.has_value() 的报错早退)——统计行
    // 之后再打一条分界线,跟开头那条首尾呼应,把这一问一答框完整。
    // usage 台账出账(模型分工第一期):整轮的逐步 usage 交给调用方记进
    // 分角色账本(normal 档),compact/抽取那几笔后台采样另走各处的
    // BackgroundCallAccounting,不混进这里。
    if (usage_out != nullptr) {
        *usage_out = usage_stats;
    }
    // turn 尾分界线(终端回合视觉收束单):Worked for X(正常)/ Stopped
    // after X(ESC 打断)/ Failed after X(输出预算耗尽)。统计行在前、footer
    // 在后,跟开头那条首尾呼应——但带上了总耗时,用户看得见这一轮到底
    // 忙了多久。
    finish_turn_chrome(turn_failed        ? lubancode::cli::TurnFooterTone::Failed
                       : out.cancelled    ? lubancode::cli::TurnFooterTone::Stopped
                                          : lubancode::cli::TurnFooterTone::Worked);
    return out;
}

}  // namespace lubancode::app
