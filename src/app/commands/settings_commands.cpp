// settings_commands.hpp 的实现:模型/供应商/配置/语言/技能/更新命令的函数体。
#include "app/commands/settings_commands.hpp"

#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)

#include <algorithm>
#include <cctype>
#include <iostream>

#include "app/version.hpp"
#include "app/model_router.hpp"
#include "app/runtime_profile.hpp"
#include "app/turn_runner.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"  // P0-3:/copy 读 ReplayState 投影

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "api/types.hpp"
#include "cli/console_input.hpp"
#include "cli/keymap.hpp"
#include "cli/markdown.hpp"
#include "cli/terminal_port.hpp"
#include "platform/clipboard.hpp"
#include "platform/console.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/provider_switch.hpp"
#include "cli/provider_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/theme.hpp"
#include "cli/wizard_panel.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/settings_local.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "tools/skill_loader.hpp"

namespace lubancode::app {


using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::TermOut;
using lubancode::cli::TermErr;
using lubancode::cli::tr;
using lubancode::cli::trf;

namespace {

// 去首尾空白(原先经 turn_runner.hpp 的公开声明间接可用;骨架拆解反弹·
// 问题 1 把那族帮手搬走后,这里立自己的文件内副本,与 memory_commands/
// model_commands 同款)。
std::string TrimAscii(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

// WizardPanel 给选择菜单留 12 行。短菜单至多 11 项再带一行 hint；长菜单
// 固定拿两行画搜索栏与 hint，中间十行翻页。三处共用一把尺，不能各算各的。
constexpr int kWizardChoiceReserveRows = 12;
constexpr std::size_t kWizardChoiceSearchRows = static_cast<std::size_t>(kWizardChoiceReserveRows - 2);
constexpr std::size_t kWizardChoicePlainRows = static_cast<std::size_t>(kWizardChoiceReserveRows - 1);

}  // namespace

void PrintLubanIcon(const lubancode::cli::Theme& theme) {
    TermOut() << theme.banner << "╭───────────────────────╮" << theme.reset << "\n";
    TermOut() << theme.banner << "│  鲁 班 code           │" << theme.reset << "\n";
    TermOut() << theme.stats << "│  匠心运斤 · 代码成器  │" << theme.reset << "\n";
    TermOut() << theme.banner << "╰───────────────────────╯" << theme.reset << "\n";
}

// 交互模式启动横幅:一眼看全版本、wire、当前模型、工作目录,两行,不啰嗦。
void PrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme) {
    const std::string wire_str = lubancode::config::ProviderWireName(config.wire);
    const bool connected = !config.base_url.empty() && !config.model.empty() &&
                           (!config.auth_token.empty() ||
                            config.auth_mode == lubancode::config::ProviderAuthMode::None);
    TermOut() << theme.banner << "lubancode " << kVersion << "  ";
    if (connected) {
        TermOut() << "[" << wire_str << "] " << config.model;
    } else {
        TermOut() << "[" << tr("banner.not_connected") << "]";
    }
    TermOut() << theme.reset << "\n";
    TermOut() << theme.stats << "cwd: " << CurrentDirUtf8() << "  ·  "
              << tr(connected ? "banner.hint" : "setup.session.hint") << theme.reset << "\n";
}

// 换会话边界或 provider 后重开一张干净屏面。调用方先判定真控制台，
// 免得 ANSI 清屏序列混进管道输出。
void ClearAndPrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme) {
    lubancode::platform::ClearScreen();
    PrintLubanIcon(theme);
    PrintBanner(config, theme);
}

// 给向导(初次配置 / provider add)造一份完整 WizardIO:print/read_line/fetch_models
// 接真实 IO,choose 接 ReadChoiceMenu(↑↓ 方向键,初始高亮落在默认项,回车即选中),
// interactive 让 ReadChoice 在管道/重定向时回落编号。向导重排单再加两个注入
// 点:draw_frame 接 WizardPanel(TTY 原地面板,试填不落 transcript;不可用时
// 自动退化朴素逐行),read_event 把 Esc/Ctrl+C/EOF 翻成导航事件。两处向导
// 共用,免得注入逻辑漂移。
lubancode::cli::WizardIO MakeInteractiveWizardIO(const lubancode::cli::Theme& theme) {
    lubancode::cli::WizardIO io;
    // 面板归 WizardIO 的闭包共有(shared_ptr):io 活多久面板活多久,向导
    // 走完面板析构自动收掉整块区域,transcript 只剩调用方的收尾行。
    auto panel = std::make_shared<lubancode::cli::WizardPanel>();
    const bool panel_active = lubancode::cli::WizardPanel::Available();
    io.print = [](const std::string& line) {
        TermOut() << line << "\n";
        TermOut().flush();
    };
    // prompt 已经由向导自己通过 print 打出来了,这里传空串,别让 ReadLine 再打一遍。
    io.read_line = []() -> std::optional<std::string> { return lubancode::cli::ReadLine(""); };
    io.fetch_models = [](lubancode::config::Wire wire, const std::string& base_url, const std::string& api_key) {
        return lubancode::api::ListModels(wire, base_url, api_key);
    };
    io.interactive = lubancode::platform::StdinIsInteractive() &&
                     lubancode::platform::ProbeStdoutConsole().is_console;
    if (panel_active) {
        io.draw_frame = [panel](const lubancode::cli::WizardFrame& frame) {
            // 选择帧给选项数+1 行预留(菜单还带一行提示),面板把 footer 画在
            // 预留区之下,菜单画进预留区,滚屏风险一并堵住。
            const int requested = frame.choice_rows > 0 ? frame.choice_rows : kWizardChoiceReserveRows;
            const int reserve_rows = frame.prompt.empty()
                                         ? (std::min)(kWizardChoiceReserveRows, (std::max)(2, requested))
                                         : 0;
            panel->Draw(frame, reserve_rows);
        };
    }
    io.read_event = [panel, panel_active]() -> lubancode::cli::WizardInputEvent {
        lubancode::cli::ReadExitReason reason = lubancode::cli::ReadExitReason::Submitted;
        const std::optional<std::string> line = panel_active ? panel->ReadText(&reason)
                                                             : lubancode::cli::ReadLine(
                                                                   "", lubancode::cli::Theme{},
                                                                   /*esc_rejects=*/true,
                                                                   /*composer=*/false, &reason);
        using Kind = lubancode::cli::WizardInputEvent::Kind;
        if (!line.has_value()) {
            return lubancode::cli::WizardInputEvent{
                reason == lubancode::cli::ReadExitReason::Esc ? Kind::Back : Kind::Cancelled, std::string()};
        }
        return lubancode::cli::WizardInputEvent{Kind::Submitted, lubancode::cli::WizardTrim(*line)};
    };
    io.choose = [&theme, panel, panel_active](
                    const std::vector<lubancode::cli::WizardChoiceItem>& items, std::size_t default_index,
                    const std::string& hint,
                    lubancode::cli::WizardInputEvent::Kind* cancel_kind) -> std::optional<std::size_t> {
        std::vector<lubancode::cli::ChoiceMenuItem> menu_items;
        menu_items.reserve(items.size());
        for (const auto& it : items) {
            menu_items.push_back({it.label, it.description});
        }
        lubancode::cli::ChoiceMenuOptions opts;
        opts.hint = hint;
        opts.initial_cursor = default_index;  // 初始高亮落在默认项,回车即选中默认
        // 面板只留了 kWizardChoiceReserveRows 行。超过短单容量便走搜索分页，
        // 且选项窗口锁在十行内；搜索栏、筛后结果与 hint 才能始终留在屏上。
        opts.search_threshold = kWizardChoicePlainRows;
        opts.max_visible_rows = kWizardChoiceSearchRows;
        lubancode::cli::ReadExitReason reason = lubancode::cli::ReadExitReason::Submitted;
        const auto selected = lubancode::cli::ReadChoiceMenu(menu_items, opts, theme, &reason);
        if (cancel_kind != nullptr && !selected.has_value()) {
            *cancel_kind = reason == lubancode::cli::ReadExitReason::Esc
                               ? lubancode::cli::WizardInputEvent::Kind::Back
                               : (reason == lubancode::cli::ReadExitReason::Cancel
                                      ? lubancode::cli::WizardInputEvent::Kind::Cancelled
                                      : lubancode::cli::WizardInputEvent::Kind::Eof);
        }
        if (!selected.has_value() || selected->selected_indices.empty()) {
            return std::nullopt;  // Esc/Ctrl+C/EOF
        }
        return selected->selected_indices.front();
    };
    return io;
}
bool HandleUpdateCommand(const std::string& args, int connect_timeout_ms, int request_timeout_secs) {
    std::string action = TrimAscii(args);
    for (char& ch : action) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (!action.empty() && action != "check") {
        TermOut() << tr("cmd.update.usage") << "\n";
        return false;
    }

    TermOut() << tr("cmd.update.checking") << "\n";
    TermOut().flush();
    const auto checked = lubancode::config::CheckForUpdate(
        std::string(kVersion), connect_timeout_ms, request_timeout_secs);
    if (!checked.has_value()) {
        TermOut() << trf("cmd.update.failed", checked.error()) << "\n";
        return false;
    }
    if (!checked->update_available) {
        TermOut() << trf("cmd.update.current", checked->current_version, checked->latest_version) << "\n";
        return true;
    }

    TermOut() << trf("cmd.update.available", checked->current_version, checked->latest_version) << "\n"
              << trf("cmd.update.release", checked->release_url) << "\n"
              << tr("cmd.update.install_hint") << "\n";
    return true;
}

// /skills 命令:列出扫描到的技能;一个都没有时打印两处目录路径,顺带说明
// 怎么造一份(SKILL.md 起手 frontmatter 的最小样例)。
void PrintSkillsCommand(const std::vector<lubancode::tools::SkillMeta>& skills, const std::string& project_dir,
                         const std::optional<std::string>& home_dir) {
    if (skills.empty()) {
        TermOut() << trf("cmd.skills.empty", project_dir,
                          home_dir.has_value() ? *home_dir : tr("path.no_home"))
                   << "\n";
        return;
    }
    TermOut() << trf("cmd.skills.header", skills.size()) << "\n";
    const std::vector<std::string> preferred_order = {"项目级", "主目录级", "官方"};
    std::set<std::string> printed;
    auto print_group = [&](const std::string& source) {
        std::vector<const lubancode::tools::SkillMeta*> group;
        for (const auto& skill : skills) {
            if (skill.source_level == source) group.push_back(&skill);
        }
        if (group.empty()) return;
        printed.insert(source);
        TermOut() << "\n" << source << " · " << group.size() << "\n";
        for (const auto* skill : group) {
            TermOut() << "  " << skill->name << "\n"
                      << "    "
                      << (skill->description.empty() ? tr("cmd.skills.no_desc") : skill->description) << "\n";
        }
    };
    for (const auto& source : preferred_order) print_group(source);
    for (const auto& skill : skills) {
        if (printed.count(skill.source_level) == 0) print_group(skill.source_level);
    }
    TermOut() << "\n" << tr("cmd.skills.manage_hint") << "\n";
}

// /skill 的参数只认第一个单词作动词,余下整段留给 URL、本地路径或技能名。命令
// 本身住 main.cpp,文件落盘与网络都压进 config::skill_store,这里不碰细节。
std::pair<std::string, std::string> SplitSkillCommandArgs(const std::string& args) {
    std::size_t begin = 0;
    while (begin < args.size() && std::isspace(static_cast<unsigned char>(args[begin])) != 0) {
        ++begin;
    }
    std::size_t word_end = begin;
    while (word_end < args.size() && std::isspace(static_cast<unsigned char>(args[word_end])) == 0) {
        ++word_end;
    }
    std::string word = args.substr(begin, word_end - begin);
    for (char& ch : word) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    std::size_t rest_begin = word_end;
    while (rest_begin < args.size() && std::isspace(static_cast<unsigned char>(args[rest_begin])) != 0) {
        ++rest_begin;
    }
    std::size_t rest_end = args.size();
    while (rest_end > rest_begin && std::isspace(static_cast<unsigned char>(args[rest_end - 1])) != 0) {
        --rest_end;
    }
    return {std::move(word), args.substr(rest_begin, rest_end - rest_begin)};
}
std::string JoinSkillNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) {
            out += ", ";
        }
        out += name;
    }
    return out;
}
bool HandleSkillCommand(const std::string& args, const std::filesystem::path& global_skills_root,
                        const std::filesystem::path& project_skills_root) {
    if (global_skills_root.empty()) {
        TermOut() << tr("cmd.skill.no_home") << "\n";
        return false;
    }
    const auto [verb, value] = SplitSkillCommandArgs(args);
    if (verb == "list") {
        const auto global = lubancode::config::ListStoredSkills(global_skills_root);
        const auto project = lubancode::config::ListStoredSkills(project_skills_root);
        if (!global.has_value()) {
            TermOut() << trf("cmd.skill.error", "/skill list", global.error()) << "\n";
            return false;
        }
        if (!project.has_value()) {
            TermOut() << trf("cmd.skill.error", "/skill list", project.error()) << "\n";
            return false;
        }
        if (global->empty() && project->empty()) {
            TermOut() << tr("cmd.skill.list_empty") << "\n";
            return false;
        }

        TermOut() << tr("cmd.skill.list_header") << "\n";
        const auto print_entries = [](const std::vector<lubancode::config::StoredSkill>& entries,
                                      const std::string& scope) {
            for (const auto& skill : entries) {
                const std::string source =
                    skill.source_url.has_value() ? trf("cmd.skill.remote", *skill.source_url,
                                                       skill.installed_at.value_or(std::string()))
                                                 : tr("cmd.skill.local");
                TermOut() << "  - " << skill.name << " [" << scope << "; " << source << "]\n"
                          << "      " << skill.dir_path << "\n";
            }
        };
        print_entries(*project, tr("cmd.skill.scope_project"));
        print_entries(*global, tr("cmd.skill.scope_global"));
        return false;
    }
    if (verb == "install") {
        if (value.empty()) {
            TermOut() << tr("cmd.skill.usage") << "\n";
            return false;
        }
        const auto installed = lubancode::config::InstallSkillSource(
            global_skills_root, value, lubancode::config::FetchRemoteSkillUrl);
        if (!installed.has_value()) {
            TermOut() << trf("cmd.skill.error", "/skill install", installed.error()) << "\n";
            return false;
        }
        TermOut() << trf("cmd.skill.install_done", JoinSkillNames(installed->installed_names)) << "\n";
        return true;
    }
    if (verb == "update") {
        const auto records = lubancode::config::LoadRemoteSkillRecords(global_skills_root);
        if (!records.has_value()) {
            TermOut() << trf("cmd.skill.error", "/skill update", records.error()) << "\n";
            return false;
        }
        std::vector<lubancode::config::RemoteSkillRecord> chosen;
        for (const auto& record : *records) {
            if (value.empty() || record.name == value) {
                chosen.push_back(record);
            }
        }
        if (chosen.empty()) {
            TermOut() << tr("cmd.skill.update_none") << "\n";
            return false;
        }

        bool changed = false;
        for (const auto& record : chosen) {
            lubancode::config::SkillInstallOptions options;
            options.overwrite = true;
            options.only_names = {record.name};
            const auto updated = lubancode::config::InstallRemoteSkills(
                global_skills_root, record.source_url, lubancode::config::FetchRemoteSkillUrl, options);
            if (!updated.has_value()) {
                TermOut() << trf("cmd.skill.error", "/skill update " + record.name, updated.error()) << "\n";
                continue;
            }
            TermOut() << trf("cmd.skill.update_done", JoinSkillNames(updated->installed_names)) << "\n";
            changed = true;
        }
        return changed;
    }
    if (verb == "remove") {
        if (value.empty()) {
            TermOut() << tr("cmd.skill.usage") << "\n";
            return false;
        }
        const auto removed = lubancode::config::RemoveStoredSkill(global_skills_root, value);
        if (!removed.has_value()) {
            TermOut() << trf("cmd.skill.error", "/skill remove", removed.error()) << "\n";
            return false;
        }
        TermOut() << trf("cmd.skill.remove_done", value) << "\n";
        return true;
    }
    TermOut() << tr("cmd.skill.usage") << "\n";
    return false;
}

// /think(/effort 同义)命令:不带参数看当前档位,带参数切档位(本会话
// 生效)。档位声明三层找:模型目录条目 → provider 配置 → 都没有明说
// "未经能力验证"(Effort 诊断单)。"不填"是正式状态,文案写"未发送参数",
// 不偷偷映射成任何档。
void HandleThinkCommand(const std::string& args, const std::shared_ptr<std::string>& current_think,
                         const lubancode::config::ModelCatalogEntry* entry,
                         const std::vector<std::string>& provider_levels, const std::string& think_param) {
    const std::vector<std::string> hint_lines = lubancode::config::ThinkLevelHintLines(entry);
    const std::string param_name = think_param.empty() ? std::string("reasoning_effort") : think_param;
    // 思考关不掉的明说(MiniCPM5 真机巡检单 P1):目录声明 always_think/
    // off_unsupported 的模型,none 档照旧把关闭请求发出去(不硬塞私有模板
    // 参数),但切换前后都要亮这句——别让状态栏只挂一枚 none 便算数。
    // 诊断行直接拼字,与落线形状行同一风格,不走 i18n 键。
    const auto note_off_unsupported = [&entry]() {
        if (lubancode::config::ClassifyThinkOffDeclaration(entry) !=
            lubancode::config::ThinkOffDeclaration::DeclaredUnsupported) {
            return;
        }
        TermOut() << "目录声明:此模型思考关不掉(always_think/off_unsupported)。none 档仍会发送关闭请求,"
                     "但此端点未证实可关,生效与否以 /doctor effort 三回对照为准。\n";
    };
    const auto is_none_level = [](const std::string& level) {
        if (level.size() != std::string("none").size()) {
            return false;
        }
        for (std::size_t i = 0; i < level.size(); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(level[i])));
            if (c != "none"[i]) {
                return false;
            }
        }
        return true;
    };
    if (args.empty()) {
        TermOut() << trf("cmd.think.current",
                         current_think->empty() ? std::string(tr("config.think.unset")) : *current_think)
                  << "\n";
        if (!hint_lines.empty()) {
            TermOut() << trf("cmd.think.catalog_header", entry->slug) << "\n";
            for (const auto& line : hint_lines) {
                TermOut() << line << "\n";
            }
        } else if (!provider_levels.empty()) {
            // 模型不在目录(或目录没声明),provider 配置声明了就列它——
            // 本地兼容端的主路:声明在 providers[].supported_think_levels。
            TermOut() << trf("cmd.think.provider_header", param_name) << "\n";
            for (const auto& level : provider_levels) {
                TermOut() << "  - " << level << "\n";
            }
        } else {
            TermOut() << tr("cmd.think.unverified") << "\n";
        }
        if (is_none_level(*current_think)) {
            note_off_unsupported();  // "切换前"也明说:当前就挂在 none 上
        }
        TermOut() << tr("cmd.think.doctor_hint") << "\n";
        return;
    }
    *current_think = args;
    TermOut() << trf("cmd.think.switched", args);
    if (!hint_lines.empty()) {
        if (!lubancode::config::ThinkLevelDeclared(*entry, args)) {
            TermOut() << tr("cmd.think.undeclared");
        }
        TermOut() << "\n";
    } else if (!provider_levels.empty()) {
        const bool declared = std::any_of(provider_levels.begin(), provider_levels.end(),
                                          [&args](const std::string& level) {
                                              // ASCII 大小写不敏感,跟目录那张表一个待遇。
                                              if (level.size() != args.size()) {
                                                  return false;
                                              }
                                              for (std::size_t i = 0; i < level.size(); ++i) {
                                                  const char a =
                                                      static_cast<char>(std::tolower(static_cast<unsigned char>(level[i])));
                                                  const char b =
                                                      static_cast<char>(std::tolower(static_cast<unsigned char>(args[i])));
                                                  if (a != b) {
                                                      return false;
                                                  }
                                              }
                                              return true;
                                          });
        TermOut() << (declared ? tr("cmd.think.provider_declared") : tr("cmd.think.provider_undeclared"))
                  << "\n";
    } else {
        TermOut() << tr("cmd.think.unverified_send") << "\n";
    }
    if (is_none_level(args)) {
        note_off_unsupported();  // "切换后"明说:切上去这一下就说清
    }
}

// ---------------------------------------------------------------------------
// /think history(Kimi 保留式思考单 P1):跨轮保留式思考的配置入口。
// 决策纯函数在前(单测钉各案),命令壳只打印与落账。
// ---------------------------------------------------------------------------

ThinkHistorySwitch DecideThinkHistorySwitch(lubancode::api::ReasoningHistoryMode current,
                                             lubancode::api::ReasoningHistoryMode requested,
                                             const lubancode::api::ReasoningConfig& reasoning,
                                             const std::string& effort) {
    ThinkHistorySwitch out;
    out.mode = current;
    const lubancode::api::ReasoningHistorySupport support =
        lubancode::api::ReasoningHistorySupportFor(reasoning);
    if (requested == lubancode::api::ReasoningHistoryMode::ProviderDefault) {
        out.applied = true;
        out.mode = lubancode::api::ReasoningHistoryMode::ProviderDefault;
        switch (support) {
            case lubancode::api::ReasoningHistorySupport::RequestControl:
                out.notes.push_back(
                    "已切回 provider default:新请求不再发 thinking.keep,不宣称跨轮 Preserved Thinking;"
                    "同一 Turn 的工具循环仍按 tool_episode 回传本枚 Turn 的思考。");
                break;
            case lubancode::api::ReasoningHistorySupport::ServerFixed:
                // 试图"关 history"的明报(P1 第 4 条):K3/K2.7 的保留由服务端
                // 固定开启,客户端没有关它的旋钮——说了,不猜。
                out.notes.push_back(
                    "此模型的 preserved thinking 由服务端固定开启,模型不支持关闭;"
                    "历史回传仍按 always 全量送回,与这个选择无关。");
                break;
            case lubancode::api::ReasoningHistorySupport::None:
                out.notes.push_back(
                    "此模型不支持 Preserved Thinking;default 即其常态(历史思考留在本地,不随请求送回)。");
                break;
        }
        return out;
    }
    // requested == All
    switch (support) {
        case lubancode::api::ReasoningHistorySupport::RequestControl: {
            if (lubancode::api::ReasoningEffortIsOff(effort, reasoning)) {
                // 冲突明报,不猜(P1 第 3 条):保留建立在思考开启之上,用户又
                // 关了思考——拒绝切换,指路让用户自己定夺。
                out.notes.push_back(
                    "冲突:当前思考处于关闭档(none),跨轮保留建立在思考开启之上,不能既关思考又要保留。"
                    "先把 /think 切回开思考的档,或维持 /think history default。");
                return out;
            }
            out.applied = true;
            out.mode = lubancode::api::ReasoningHistoryMode::All;
            out.notes.push_back(
                "已开跨轮保留:请求将同发 thinking.type=\"enabled\" 与 thinking.keep=\"all\","
                "历史回传升为 always——纯对话段的思考也随下一份请求原字节送回。");
            return out;
        }
        case lubancode::api::ReasoningHistorySupport::ServerFixed:
            out.applied = true;
            out.mode = lubancode::api::ReasoningHistoryMode::All;
            out.notes.push_back(
                "此模型的 preserved thinking 由服务端固定开启,wire 上没有可请求的保留字段;"
                "选择已记档(切到 K2.6 这类可选保留的模型时照常生效)。");
            return out;
        case lubancode::api::ReasoningHistorySupport::None:
            // K2.5 与无方言旧端(P1 第 5 条):当场报不支持,拒绝落账。
            out.notes.push_back("此模型不支持 Preserved Thinking,开不了 history all。");
            return out;
    }
    return out;
}

bool RevalidateThinkHistoryMode(const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                                const std::shared_ptr<std::string>& current_think,
                                const lubancode::config::ModelCatalogEntry* entry) {
    if (current_think_history == nullptr ||
        *current_think_history != lubancode::api::ReasoningHistoryMode::All) {
        return false;  // default 对任何模型都是合法状态,不用动
    }
    const lubancode::api::ReasoningConfig empty_reasoning;
    const lubancode::api::ReasoningConfig& reasoning = entry != nullptr ? entry->reasoning : empty_reasoning;
    const lubancode::api::ReasoningHistorySupport support =
        lubancode::api::ReasoningHistorySupportFor(reasoning);
    if (support == lubancode::api::ReasoningHistorySupport::ServerFixed) {
        return false;  // 固定开启:All 由服务端兜底,wire 无字段,选择合法保留
    }
    if (support == lubancode::api::ReasoningHistorySupport::RequestControl &&
        !lubancode::api::ReasoningEffortIsOff(*current_think, reasoning)) {
        return false;  // 可选保留且思考开着:合法,不动
    }
    // 到这里只剩两种失效:模型不支持(K2.5/目录外),或可选保留但思考被
    // 目录默认/继承档位关了。回 default 并明说——不硬带,也不猜。
    *current_think_history = lubancode::api::ReasoningHistoryMode::ProviderDefault;
    if (support == lubancode::api::ReasoningHistorySupport::None) {
        TermOut() << "history: 当前模型不支持 Preserved Thinking,跨轮保留已回落 default"
                     "(历史思考仍留在本地会话,不删)。"
                  << "\n";
    } else {
        TermOut() << "history: 当前模型思考处于关闭档,与跨轮保留冲突,已回落 default;"
                     "要保留先把 /think 切回开思考的档再 /think history all。"
                  << "\n";
    }
    return true;
}

// 历史保留能力的人话行(/think history 裸敲与 /think 裸敲共用)。
std::string ThinkHistorySupportLine(const lubancode::api::ReasoningConfig& reasoning) {
    switch (lubancode::api::ReasoningHistorySupportFor(reasoning)) {
        case lubancode::api::ReasoningHistorySupport::RequestControl:
            return "模型能力: 可选跨轮保留(请求发 thinking.keep=\"all\")";
        case lubancode::api::ReasoningHistorySupport::ServerFixed:
            return "模型能力: 服务端固定开启保留,不可关闭(wire 无请求字段)";
        case lubancode::api::ReasoningHistorySupport::None:
            return "模型能力: 不支持 Preserved Thinking";
    }
    return std::string();
}

std::string ThinkHistoryModeName(lubancode::api::ReasoningHistoryMode mode) {
    return mode == lubancode::api::ReasoningHistoryMode::All ? "all" : "default";
}

void HandleThinkHistoryCommand(const std::string& value,
                               const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                               const std::shared_ptr<std::string>& current_think,
                               const lubancode::config::ModelCatalogEntry* entry,
                               lubancode::runtime::SessionRuntime* session_runtime) {
    const lubancode::api::ReasoningConfig empty_reasoning;
    const lubancode::api::ReasoningConfig& reasoning = entry != nullptr ? entry->reasoning : empty_reasoning;
    if (value.empty()) {
        // 裸敲:亮当前形状——模式、模型能力、下一份请求会怎么落线。
        TermOut() << "历史保留: " << ThinkHistoryModeName(*current_think_history) << "\n";
        TermOut() << ThinkHistorySupportLine(reasoning) << "\n";
        TermOut() << "用法: /think history default|all(all = 请求 thinking.keep=\"all\",历史回传升为 always)"
                  << "\n";
        return;
    }
    lubancode::api::ReasoningHistoryMode requested = lubancode::api::ReasoningHistoryMode::ProviderDefault;
    if (value == "all") {
        requested = lubancode::api::ReasoningHistoryMode::All;
    } else if (value != "default") {
        TermOut() << "认不得的档: " << value << "(只认 default|all)\n";
        return;
    }
    const ThinkHistorySwitch decision =
        DecideThinkHistorySwitch(*current_think_history, requested, reasoning, *current_think);
    for (const auto& note : decision.notes) {
        TermOut() << note << "\n";
    }
    if (!decision.applied || decision.mode == *current_think_history) {
        return;  // 拒绝或同档重复:不落账
    }
    *current_think_history = decision.mode;
    // (P0-6:旧存档的 think_history_v1 事件行已删;选择只进会话活值。)
}

// 把模型目录条目应用到会话状态:/model 切换(两个 explicit 都传 false,
// 目录声明了就用)和交互模式启动(explicit 按 Source 判断,用户显式配过的
// 不动)共用这一段。改 current_think / 会话窗口 / base_instructions,干了
// 什么就打一行;模型不在目录时 ComputeCatalogApplication 给回一份"全空"
// 的应用——think/窗口不动,base_instructions 清空(旧模型的指令不再发),
// 一切回退现状,不打任何多余的话。
void ApplyModelCatalog(const lubancode::config::ModelCatalog& catalog, const std::string& slug,
                        bool think_explicit, bool window_explicit,
                        const std::shared_ptr<std::string>& current_think,
                        lubancode::cli::ContextTracker& context_tracker,
                        const std::shared_ptr<std::string>& current_model_instructions) {
    const auto apply =
        lubancode::config::ComputeCatalogApplication(catalog, slug, think_explicit, window_explicit);
    if (apply.think.has_value()) {
        *current_think = *apply.think;
        TermOut() << trf("catalog.apply_think", *apply.think) << "\n";
    }
    if (apply.context_window_tokens.has_value()) {
        context_tracker.set_window_tokens(*apply.context_window_tokens);
        TermOut() << trf("catalog.apply_window", *apply.context_window_tokens) << "\n";
    }
    if (*current_model_instructions != apply.base_instructions) {
        *current_model_instructions = apply.base_instructions;
        if (!apply.base_instructions.empty()) {
            TermOut() << trf("catalog.apply_instructions", slug) << "\n";
        }
    }
}

// /model roles:三角色路由短表(模型分工第一期)。回落行写明
// "回落到 normal",不把同名重印一遍(规格"界面"节)。
void PrintModelRolesTable(const lubancode::agent::ModelRouteTable* roles_table) {
    if (roles_table == nullptr) {
        TermOut() << tr("cmd.model.roles_unavailable") << "\n";
        return;
    }
    TermOut() << tr("cmd.model.roles_header") << "\n";
    for (const std::string& line : lubancode::app::FormatModelRolesTable(*roles_table)) {
        TermOut() << "  " << line << "\n";
    }
}

// /model 裸敲的清单选择(全注释见头文件):交互菜单/非交互编号选一项,
// 只返回 id——不切换、不碰配置,提交统一走 runtime::CommandService::SetModel,
// 与带参直切同一条路。
std::optional<std::string> ChooseModelId(const lubancode::runtime::ModelQueryResult& query,
                                         const lubancode::config::ModelCatalog& catalog) {
    std::size_t default_idx = 0;
    std::vector<std::string> ids;
    std::vector<lubancode::cli::ChoiceMenuItem> items;
    ids.reserve(query.models.size());
    items.reserve(query.models.size());
    for (std::size_t i = 0; i < query.models.size(); ++i) {
        const auto& m = query.models[i];
        const bool current = m.id == query.current_model;
        if (current) default_idx = i;
        const auto* entry = catalog.FindBySlug(m.id);
        std::string label;
        if (entry != nullptr && !entry->display_name.empty()) {
            // 目录条目的 display_name 优先,后面括号带上 slug——选完切换
            // 用的还是 API 模型名,展示名和真名对得上号。
            label = entry->display_name + "(" + m.id + ")";
        } else {
            label = m.display_name.empty() ? m.id : m.display_name;
        }
        // 端点相性标记(ccmoon 巡检单 P1):Realtime 模型混进菜单时挂
        // 醒目标记,不当普通可用项。判词边界见 ClassifyModelEndpoint。
        if (lubancode::config::ClassifyModelEndpoint(entry, m.id) ==
            lubancode::config::ModelEndpointKind::Realtime) {
            label += " [Realtime]";
        }
        ids.push_back(m.id);
        items.push_back({label, current ? tr("cmd.model.current") : std::string{}});
    }
    std::size_t idx = default_idx;
    const bool interactive_menu = lubancode::platform::StdinIsInteractive() &&
                                  lubancode::platform::ProbeStdoutConsole().is_console;
    if (interactive_menu) {
        lubancode::cli::ChoiceMenuOptions opts;
        opts.hint = tr("confirm.menu.hint");
        opts.initial_cursor = default_idx;
        const auto sel = lubancode::cli::ReadChoiceMenu(items, opts, lubancode::cli::Theme{});
        if (!sel.has_value()) {
            TermOut() << tr("cmd.model.cancelled") << "\n";
            return std::nullopt;
        }
        idx = sel->selected_indices.empty() ? default_idx : sel->selected_indices.front();
    } else {
        for (std::size_t i = 0; i < items.size(); ++i) {
            TermOut() << "  " << (i + 1) << ") " << items[i].label
                      << (items[i].description.empty() ? "" : "  " + items[i].description) << "\n";
        }
        const std::optional<std::string> selection = lubancode::cli::ReadLine(
            trf("cmd.model.choose", default_idx + 1), {}, /*esc_rejects=*/true);
        if (!selection.has_value()) {
            TermOut() << tr("cmd.model.cancelled") << "\n";
            return std::nullopt;
        }
        if (!selection->empty()) {
            try {
                std::size_t consumed = 0;
                const int n = std::stoi(*selection, &consumed);
                if (consumed != selection->size() || n < 1 || static_cast<std::size_t>(n) > ids.size()) {
                    TermOut() << tr("cmd.model.bad_number") << "\n";
                    return std::nullopt;
                }
                idx = static_cast<std::size_t>(n - 1);
            } catch (...) {
                TermOut() << tr("cmd.model.not_number") << "\n";
                return std::nullopt;
            }
        }
    }
    return ids[idx];
}
void PrintProviderList(const std::vector<lubancode::config::ProviderConfig>& providers,
                       const lubancode::config::Config& current_config,
                       const std::string& active_provider) {
    if (providers.empty()) {
        TermOut() << tr("cmd.provider.empty") << "\n";
        return;
    }
    TermOut() << tr("cmd.provider.header") << "\n";
    for (const auto& provider : providers) {
        const std::string model = provider.model.empty() ? tr("cmd.provider.model_unset") : provider.model;
        const bool is_current = provider.name == active_provider ||
                                (active_provider.empty() && provider.wire == current_config.wire &&
                                 provider.base_url == current_config.base_url && provider.model == current_config.model);
        const std::string current = is_current ? tr("cmd.provider.current") : "";
        // 鉴权三态:none 写"无需鉴权";env 提变量名;inline 露打码 key。
        std::string auth_display;
        switch (provider.auth) {
            case lubancode::config::ProviderAuthMode::None:
                auth_display = tr("cmd.provider.auth_none");
                break;
            case lubancode::config::ProviderAuthMode::Inline:
                auth_display = "api_key=" + lubancode::config::MaskApiKey(provider.api_key);
                break;
            case lubancode::config::ProviderAuthMode::Env:
                auth_display = "key_env=" + provider.key_env;
                break;
        }
        std::string extra;
        if (!provider.model_reasoning_effort.empty()) {
            extra += trf("cmd.provider.extra_effort", provider.model_reasoning_effort);
        }
        if (provider.native_web_search) {
            extra += tr("cmd.provider.extra_web_search");
        }
        // extra_body/extra_headers 只提示"配了几键",绝不把 JSON 原文糊到
        // 屏幕上——那玩意可能一大坨,也可能藏着不方便随手示人的字段值。
        if (!provider.extra_body.empty()) {
            extra += trf("cmd.provider.extra_body_hint", provider.extra_body.size());
        }
        if (!provider.extra_headers.empty()) {
            extra += trf("cmd.provider.extra_headers_hint", provider.extra_headers.size());
        }
        TermOut() << trf("cmd.provider.line", provider.name, lubancode::config::ProviderWireName(provider.wire),
                          provider.base_url, model, provider.context_window_tokens, auth_display, extra, current)
                  << "\n";
    }
}

// /provider add 向导:跟 RunInitialSetupWizard(初次配置向导)同一套 WizardIO
// 建法——接 std::cout / cli::ReadLine / api::ListModels,不碰真实 IO 之外的
// 任何东西。问出来的是一条 ProviderConfig,写盘复用一行式旧用法同一条路径
// (AddProviderToGlobalConfig)。用户中途 EOF、或者最后一问回答 n,都当"整个
// 添加动作被取消"处理:不改 config.providers、不写盘。
std::optional<std::string> RunProviderAddWizardInteractive(const std::string& name_prefill,
                                                            lubancode::config::Config& config,
                                                            const lubancode::cli::Theme& theme) {
    lubancode::cli::WizardIO io = MakeInteractiveWizardIO(theme);

    if (lubancode::config::ProviderCatalogCacheIsStale()) {
        TermOut() << tr("provider_catalog.refreshing") << "\n";
        const auto refreshed = lubancode::config::RefreshProviderCatalog();
        if (!refreshed.has_value()) {
            TermOut() << trf("provider_catalog.refresh_failed", refreshed.error()) << "\n";
        }
    }
    const lubancode::config::ProviderCatalog provider_catalog = lubancode::config::LoadProviderCatalog();
    for (const auto& warning : provider_catalog.warnings) {
        TermOut() << trf("provider_catalog.warning", warning) << "\n";
    }
    const auto outcome =
        lubancode::cli::RunProviderPresetWizard(io, provider_catalog, name_prefill, config.providers);
    if (!outcome.has_value() || !outcome->save_requested) {
        TermOut() << tr("cmd.provider.add_cancelled") << "\n";
        return std::nullopt;
    }

    const auto saved = lubancode::config::AddProviderToGlobalConfig(outcome->provider);
    if (!saved.has_value()) {
        TermOut() << trf("cmd.provider.add_failed", saved.error()) << "\n";
        return std::nullopt;
    }
    config.providers.push_back(outcome->provider);
    TermOut() << trf("cmd.provider.added", outcome->provider.name, *saved) << "\n";
    return outcome->provider.name;
}

// /provider:添端只写全局配置；项目级若自行写了 providers，加载时仍按既有
// "整段压过"规则优先。切端时换 client、提示词平台段与模型连接，旧历史
// 保留不动；成功后把端名写回配置，下次启动照旧选中。
bool ExecuteProviderSwitch(const std::string& switch_name, const std::string& switch_model,
                           lubancode::config::Config& config, std::string& active_provider,
                           RebuildableBackend& real_backend, std::string& session_wire,
                           const std::shared_ptr<std::string>& current_model,
                           const std::shared_ptr<std::string>& current_think,
                           const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                           lubancode::cli::ContextTracker& context_tracker,
                           const std::shared_ptr<std::string>& current_model_instructions,
                           const lubancode::config::ModelCatalog& catalog,
                           lubancode::agent::PromptOptions& prompt_options,
                           const std::function<void(bool)>& rebuild_loop, bool is_console,
                           const lubancode::cli::Theme& theme,
                           const std::optional<std::string>& active_provider_write_path,
                           lubancode::config::Source& active_provider_source) {
    const lubancode::config::ProviderConfig* provider =
        lubancode::config::FindProvider(config.providers, switch_name);
    if (provider == nullptr) {
        TermOut() << trf("cmd.provider.not_found", switch_name) << "\n";
        return false;
    }
    // 项目配置显式写了 active_provider 就继续写回项目；其余场景
    // 记到用户全局配置。只存名字，不复制密钥或 endpoint。
    const auto remembered = [&]() -> std::expected<std::string, std::string> {
        if (active_provider_write_path.has_value()) {
            const auto written = lubancode::config::UpdateActiveProviderInConfigFile(
                *active_provider_write_path, provider->name);
            if (!written.has_value()) {
                return std::unexpected(written.error());
            }
            return *active_provider_write_path;
        }
        return lubancode::config::SetActiveProviderInGlobalConfig(provider->name);
    }();

    lubancode::config::ApplyProviderToRuntimeConfig(config, *provider);
    if (!switch_model.empty()) {
        config.model = switch_model;
    }
    *current_model = config.model;
    active_provider = provider->name;
    session_wire = lubancode::config::ProviderWireName(config.wire);
    real_backend.Rebuild(config);
    prompt_options.wire = lubancode::config::ProviderWireName(config.wire);
    context_tracker.set_window_tokens(config.context_window_tokens);
    // 校验、取 key、重建后端都成功了才清。清完先按新配置重画横幅，
    // 随后的目录应用与切换提示仍留在屏上；Agent 历史照旧保留。
    if (is_console) {
        ClearAndPrintBanner(config, theme);
    }
    ApplyModelCatalog(catalog, *current_model, /*think_explicit=*/false, /*window_explicit=*/true,
                      current_think, context_tracker, current_model_instructions);
    // provider 配了 model_reasoning_effort 就按 /think 同一套机制应用
    // (直接改 current_think,下一次请求就带上);没配就不动——不管
    // ApplyModelCatalog 刚才有没有按模型目录动过档位,都维持现状。
    // 放在 ApplyModelCatalog 之后:provider 的显式配置该压过目录默认。
    if (!provider->model_reasoning_effort.empty()) {
        *current_think = provider->model_reasoning_effort;
        TermOut() << trf("cmd.provider.effort_applied", provider->name, provider->model_reasoning_effort)
                  << "\n";
    }
    // 切模型重校验(P1):换过去的模型不认 history all(或与关思考档冲突)
    // 就回落 default 并明说,再重建——不把 K2.6 的 keep 状态硬带给 K3/K2.5。
    RevalidateThinkHistoryMode(current_think_history, current_think,
                               catalog.FindByProviderAndSlug(*current_model, *current_model));
    rebuild_loop(/*preserve_history=*/true);
    TermOut() << trf("cmd.provider.switched", provider->name, provider->base_url) << "\n";
    if (remembered.has_value()) {
        active_provider_source = active_provider_write_path.has_value()
                                     ? lubancode::config::Source::ProjectConfigFile
                                     : lubancode::config::Source::GlobalConfigFile;
        TermOut() << trf("cmd.provider.remembered", provider->name) << "\n";
    } else {
        TermOut() << trf("cmd.provider.remember_failed", remembered.error()) << "\n";
    }
    return true;  // 切换本身成了;"记住"写盘失败只另打一行,不算切换失败
}
void HandleProviderCommand(const std::string& args, lubancode::config::Config& config,
                           std::string& active_provider, RebuildableBackend& real_backend,
                           std::string& session_wire,
                           const std::shared_ptr<std::string>& current_model,
                           const std::shared_ptr<std::string>& current_think,
                           const std::shared_ptr<lubancode::api::ReasoningHistoryMode>& current_think_history,
                           lubancode::cli::ContextTracker& context_tracker,
                           const std::shared_ptr<std::string>& current_model_instructions,
                           const lubancode::config::ModelCatalog& catalog,
                           lubancode::agent::PromptOptions& prompt_options,
                           const std::function<void(bool)>& rebuild_loop, bool is_console,
                           const lubancode::cli::Theme& theme,
                           const std::optional<std::string>& active_provider_write_path,
                           lubancode::config::Source& active_provider_source) {
    const lubancode::cli::ParsedProviderCommand command = lubancode::cli::ParseProviderCommand(args);

    // 切换的正式执行:预检过了才进来(鉴权齐备或显式给了 model 覆盖)。
    // Switch 快捷路径与 SwitchInteractive 选择器共用这一段,免得两处漂移。
    const auto execute_switch = [&](const std::string& switch_name, const std::string& switch_model) {
        ExecuteProviderSwitch(switch_name, switch_model, config, active_provider, real_backend,
                              session_wire, current_model, current_think, current_think_history,
                              context_tracker, current_model_instructions, catalog, prompt_options,
                              rebuild_loop, is_console, theme, active_provider_write_path,
                              active_provider_source);
    };

    // 缺密钥的补救页(向导重排单):选中缺 key 的 provider 不退出选择器,
    // 原地换一页可处理的状态。返回 true = 鉴权已齐可以接着切;false = 返回
    // 列表(选择与筛选词由调用方还原)或干脆取消。
    const auto remediate_missing_auth = [&](const std::string& name) -> bool {
        while (true) {
            const lubancode::config::ProviderConfig* provider =
                lubancode::config::FindProvider(config.providers, name);
            if (provider == nullptr) {
                return false;
            }
            const lubancode::config::ProviderAuthResolution auth =
                lubancode::config::ResolveProviderAuth(*provider);
            if (auth.status != lubancode::config::ProviderAuthResolution::Status::Missing) {
                return true;  // 补齐了(env/inline/none 任一路)
            }
            const bool inline_missing = provider->auth == lubancode::config::ProviderAuthMode::Inline;
            std::vector<lubancode::cli::ChoiceMenuItem> items = {
                {tr("provider_remedy.opt_input_key"), tr(inline_missing ? "provider_remedy.hint_inline"
                                                                          : "provider_remedy.hint_env")},
                {tr("provider_remedy.opt_change_env"), {}},
                {tr("provider_remedy.opt_no_auth"), {}},
                {tr("provider_remedy.opt_howto"), {}},
                {tr("provider_remedy.opt_back"), {}},
            };
            TermOut() << trf("provider_remedy.title", name) << "\n";
            TermOut() << (inline_missing ? trf("provider_remedy.body_inline", name)
                                         : trf("provider_remedy.body_env", name, auth.env_name)) << "\n";
            lubancode::cli::ChoiceMenuOptions opts;
            opts.hint = tr("provider_remedy.footer");
            lubancode::cli::ReadExitReason reason = lubancode::cli::ReadExitReason::Submitted;
            const auto selected = lubancode::cli::ReadChoiceMenu(items, opts, theme, &reason);
            if (!selected.has_value() || selected->selected_indices.empty()) {
                return false;  // Esc 返回列表;Ctrl+C 取消
            }
            const std::size_t pick = selected->selected_indices.front();
            if (pick == 0) {
                // 现在输入 API key:先讲明保存去处(只供本次会话 / 写入用户
                // 配置),写盘按明文 key 风险提示办。
                std::vector<lubancode::cli::ChoiceMenuItem> where = {
                    {tr("provider_remedy.key_session"), tr("provider_remedy.key_session_desc")},
                    {tr("provider_remedy.key_persist"), tr("provider_remedy.key_persist_desc")},
                };
                lubancode::cli::ChoiceMenuOptions where_opts;
                where_opts.hint = tr("provider_remedy.footer");
                const auto where_sel = lubancode::cli::ReadChoiceMenu(where, where_opts, theme);
                if (!where_sel.has_value() || where_sel->selected_indices.empty()) {
                    continue;  // 当没选,回补救页
                }
                const std::optional<std::string> key =
                    lubancode::cli::ReadLine(tr("cmd.provider.auth_inline_prompt"));
                if (!key.has_value() || key->empty()) {
                    continue;
                }
                const bool persist = where_sel->selected_indices.front() == 1;
                // 内存这份走语义化 setter(问题 4):与落盘侧
                // SetProviderAuthInlineInGlobalConfig 同一套字段改法,不再
                // find_if 之后逐字段直改。
                lubancode::config::SetProviderAuthInline(config.providers, name, *key);
                if (persist) {
                    const auto saved = lubancode::config::SetProviderAuthInlineInGlobalConfig(name, *key);
                    if (!saved.has_value()) {
                        TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                        return false;
                    }
                    TermOut() << trf("provider_remedy.key_saved", name,
                                     lubancode::config::MaskApiKey(*key), *saved) << "\n";
                } else {
                    TermOut() << trf("provider_remedy.key_session_only",
                                     lubancode::config::MaskApiKey(*key)) << "\n";
                }
                continue;  // 回页顶复查:现在 Ready 了,直接返回 true
            }
            if (pick == 1) {
                // 改用另一个环境变量:只存变量名;当前进程取不到值就照实说
                // "未设置",不假装修好了。
                const std::optional<std::string> env_name =
                    lubancode::cli::ReadLine(tr("cmd.provider.auth_env_prompt"));
                if (!env_name.has_value() || env_name->empty()) {
                    continue;
                }
                const auto saved = lubancode::config::SetProviderAuthEnvInGlobalConfig(name, *env_name);
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return false;
                }
                // 内存这份同步换(问题 4:setter 封字段改法,不逐字段直改)。
                lubancode::config::SetProviderAuthEnv(config.providers, name, *env_name);
                const std::optional<std::string> value = lubancode::platform::GetEnvVar(env_name->c_str());
                TermOut() << (value.has_value() && !value->empty()
                                  ? trf("provider_wizard.auth.env.note_set", *env_name)
                                  : trf("provider_wizard.auth.env.note_unset", *env_name)) << "\n";
                continue;
            }
            if (pick == 2) {
                // 设为无需鉴权:明确写回 provider,重启仍认得,不是"忽略这次错误"。
                const auto saved = lubancode::config::SetProviderAuthModeInGlobalConfig(
                    name, lubancode::config::ProviderAuthMode::None);
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return false;
                }
                // 内存这份同步换(问题 4:setter 封字段改法,不逐字段直改)。
                lubancode::config::SetProviderAuthNone(config.providers, name);
                TermOut() << trf("provider_remedy.none_saved", name, *saved) << "\n";
                continue;
            }
            if (pick == 3) {
                // 查看设置方法:按当前终端给一条命令,说清设完要不要重启。
                const std::string env_for_howto =
                    auth.env_name.empty() ? provider->key_env : auth.env_name;
#ifdef _WIN32
                TermOut() << trf("provider_remedy.howto_powershell", env_for_howto) << "\n";
                TermOut() << trf("provider_remedy.howto_cmd", env_for_howto) << "\n";
#else
                TermOut() << trf("provider_remedy.howto_posix", env_for_howto) << "\n";
#endif
                TermOut() << tr("provider_remedy.howto_restart") << "\n";
                continue;
            }
            return false;  // 返回 provider 列表
        }
    };
    // /provider add 向导的收尾自动切:保存成功那一刻切过去(配完即用,
    // 不再要用户手动 /provider switch 一道)。切不动就如实提示并保持旧
    // 连接,不留半切换状态:新端没配默认模型、或缺密钥又在补救页退出,
    // 都只报一行,原连接分毫不动。Add 命令与两处 switch 面板的"添加新
    // provider"入口共用。
    const auto add_via_wizard = [&](const std::string& name_prefill) {
        const auto added = RunProviderAddWizardInteractive(name_prefill, config, theme);
        if (!added.has_value()) {
            return;  // 取消或写盘失败:不改连接
        }
        const lubancode::config::ProviderConfig* provider =
            lubancode::config::FindProvider(config.providers, *added);
        if (provider == nullptr || provider->model.empty()) {
            TermOut() << trf("cmd.provider.add_kept_connection", *added) << "\n";
            return;
        }
        if (lubancode::config::ResolveProviderAuth(*provider).status ==
                lubancode::config::ProviderAuthResolution::Status::Missing &&
            !remediate_missing_auth(*added)) {
            return;  // 补救页退出:保持旧连接
        }
        execute_switch(*added, "");
    };
    // /provider edit 的正式执行(容错单):进同一套向导面板改旧 provider,
    // 确认才写盘,取消不动配置。改的正好是当前活跃端时,整套重新应用——
    // 与 execute_switch 同一条路,不另立第二套字段镜像。
    const auto run_edit = [&](const std::string& name) {
        const lubancode::config::ProviderConfig* provider =
            lubancode::config::FindProvider(config.providers, name);
        if (provider == nullptr) {
            TermOut() << trf("cmd.provider.not_found", name) << "\n";
            return;
        }
        lubancode::cli::WizardIO io = MakeInteractiveWizardIO(theme);
        const auto outcome = lubancode::cli::RunProviderEditWizard(io, *provider);
        if (!outcome.has_value() || !outcome->save_requested) {
            TermOut() << tr("cmd.provider.edit.cancelled") << "\n";
            return;
        }
        const auto saved = lubancode::config::ReplaceProviderInGlobalConfig(name, outcome->provider);
        if (!saved.has_value()) {
            TermOut() << trf("cmd.provider.edit.save_failed", saved.error()) << "\n";
            return;
        }
        // 内存里的这份跟着换,后续 execute_switch / list 看到的都是新值。
        const auto it = std::find_if(config.providers.begin(), config.providers.end(),
                                     [&](const lubancode::config::ProviderConfig& p) { return p.name == name; });
        if (it != config.providers.end()) {
            *it = outcome->provider;
        }
        TermOut() << trf("cmd.provider.edit.saved", name, *saved) << "\n";
        if (active_provider == name) {
            execute_switch(name, "");  // 重新应用整套配置,立即生效
        }
    };

    switch (command.action) {
        case lubancode::cli::ProviderCommandAction::List:
            PrintProviderList(config.providers, config, active_provider);
            return;
        case lubancode::cli::ProviderCommandAction::Refresh: {
            TermOut() << tr("provider_catalog.refreshing") << "\n";
            const auto refreshed = lubancode::config::RefreshProviderCatalog();
            if (!refreshed.has_value()) {
                TermOut() << trf("provider_catalog.refresh_failed", refreshed.error()) << "\n";
            } else if (refreshed->not_modified) {
                TermOut() << tr("provider_catalog.refresh_current") << "\n";
            } else {
                TermOut() << trf("provider_catalog.refresh_ok", refreshed->revision, refreshed->cache_path) << "\n";
            }
            return;
        }
        case lubancode::cli::ProviderCommandAction::Add: {
            const bool needs_connection =
                config.base_url.empty() || config.model.empty() ||
                (config.auth_token.empty() &&
                 config.auth_mode != lubancode::config::ProviderAuthMode::None);
            if (command.wizard) {
                // 裸敲 /provider add,或者 /provider add <名字>(名字先给上,
                // 跳过向导第一问)——走分步向导,不进下面的一行式解析路径。
                // 保存成功即切过去(向导收尾自动切):空配置会话与已有连接
                // 一视同仁,切不动就保旧连接(add_via_wizard 里判定)。
                add_via_wizard(command.name);
                return;
            }
            const auto wire = lubancode::config::ParseProviderWire(command.wire);
            if (!wire.has_value()) {
                TermOut() << trf("cmd.provider.add_failed", wire.error()) << "\n";
                return;
            }
            std::size_t window = lubancode::config::kDefaultContextWindowTokens;
            if (!command.window.empty()) {
                const auto parsed_window = lubancode::config::ParseContextWindowTokens(command.window);
                if (!parsed_window.has_value()) {
                    TermOut() << trf("cmd.provider.add_failed", parsed_window.error()) << "\n";
                    return;
                }
                window = *parsed_window;
            }
            lubancode::config::ProviderConfig provider{
                .name = command.name,
                .base_url = command.base_url,
                .wire = *wire,
                .key_env = command.key_env,
                .api_key = command.key,
                .model = command.model,
                .model_reasoning_effort = command.effort,
                .context_window_tokens = window,
            };
            const auto valid = lubancode::config::ValidateProviderConfig(provider);
            if (!valid.has_value()) {
                TermOut() << trf("cmd.provider.add_failed", valid.error()) << "\n";
                return;
            }
            if (lubancode::config::FindProvider(config.providers, provider.name) != nullptr) {
                TermOut() << trf("cmd.provider.add_failed", trf("cmd.provider.exists", provider.name)) << "\n";
                return;
            }
            const auto saved = lubancode::config::AddProviderToGlobalConfig(provider);
            if (!saved.has_value()) {
                TermOut() << trf("cmd.provider.add_failed", saved.error()) << "\n";
                return;
            }
            config.providers.push_back(std::move(provider));
            TermOut() << trf("cmd.provider.added", command.name, *saved) << "\n";
            if (needs_connection) {
                const lubancode::config::ProviderConfig* added =
                    lubancode::config::FindProvider(config.providers, command.name);
                if (added != nullptr) {
                    if (lubancode::config::ResolveProviderAuth(*added).status ==
                            lubancode::config::ProviderAuthResolution::Status::Missing &&
                        !remediate_missing_auth(command.name)) {
                        return;
                    }
                    // 钥匙撞车单:一行式贴 key + 默认 key_env 并存、变量里有值——
                    // 切过去前叫一声(变量赢,打码),别让刚贴的 key 悄悄失效。
                    // (provider 本体已被 push_back move 走,用盘上找回来的 added。)
                    if (const std::optional<std::string> key_warning =
                            lubancode::config::ProviderAuthConflictWarning(*added)) {
                        TermOut() << *key_warning << "\n";
                    }
                }
                execute_switch(command.name, "");
            }
            return;
        }
        case lubancode::cli::ProviderCommandAction::Switch: {
            const lubancode::config::ProviderConfig* provider =
                lubancode::config::FindProvider(config.providers, command.name);
            const bool can_panel = is_console && lubancode::platform::StdinIsInteractive();
            if (provider == nullptr) {
                // 找不到名字:TTY 下不开死胡同,开已筛选的列表把"不存在"贴在
                // 面板内;非 TTY 照旧一行报错。
                if (can_panel) {
                    const auto picked = lubancode::cli::RunProviderSwitchPicker(
                        config.providers, active_provider, command.name,
                        trf("cmd.provider.not_found", command.name), {}, theme);
                    if (picked.pick == lubancode::cli::ProviderSwitchPick::Edit) {
                        run_edit(picked.name);  // e 快捷键:原地进编辑向导
                        return;
                    }
                    if (picked.pick == lubancode::cli::ProviderSwitchPick::Named) {
                        const lubancode::config::ProviderConfig* chosen =
                            lubancode::config::FindProvider(config.providers, picked.name);
                        if (chosen != nullptr) {
                            if (lubancode::config::ResolveProviderAuth(*chosen).status ==
                                    lubancode::config::ProviderAuthResolution::Status::Missing &&
                                !remediate_missing_auth(picked.name)) {
                                return;
                            }
                            // 钥匙撞车单:切过去前把撞车叫出来(变量赢,打码)。
                            if (const std::optional<std::string> key_warning =
                                    lubancode::config::ProviderAuthConflictWarning(*chosen)) {
                                TermOut() << *key_warning << "\n";
                            }
                        }
                        execute_switch(picked.name, "");
                    } else if (picked.pick == lubancode::cli::ProviderSwitchPick::AddNew) {
                        add_via_wizard("");
                    }
                    return;
                }
                TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                return;
            }
            // 鉴权三态:预检吃 ResolveProviderAuth 这一份共享结果——none 直接过,
            // env 缺变量/inline 缺 key 在 TTY 下进补救页,非 TTY 按模式报错。
            const lubancode::config::ProviderAuthResolution auth =
                lubancode::config::ResolveProviderAuth(*provider);
            if (auth.status == lubancode::config::ProviderAuthResolution::Status::Missing) {
                if (can_panel) {
                    if (!remediate_missing_auth(command.name)) {
                        return;  // 取消/返回:当前 provider、模型、后端与配置都不动
                    }
                } else {
                    if (provider->auth == lubancode::config::ProviderAuthMode::Inline) {
                        TermOut() << trf("cmd.provider.key_missing_inline", provider->name) << "\n";
                    } else {
                        TermOut() << trf("cmd.provider.key_missing", provider->name, auth.env_name) << "\n";
                    }
                    return;
                }
            }
            // 钥匙撞车单:切过去之前两把钥匙并存且不一致就叫一声(变量赢,打码)。
            if (const std::optional<std::string> key_warning =
                    lubancode::config::ProviderAuthConflictWarning(*provider)) {
                TermOut() << *key_warning << "\n";
            }
            execute_switch(command.name, command.model);
            return;
        }
        case lubancode::cli::ProviderCommandAction::SwitchInteractive: {
            // 裸敲 /provider switch:意图是换一家,不倒总帮助。TTY 开原地面板;
            // 非 TTY 只给 switch 专用短用法,不打印 add/remove/set 全家桶。
            const bool can_panel = is_console && lubancode::platform::StdinIsInteractive();
            if (!can_panel) {
                TermOut() << tr("cmd.provider.switch.usage_short") << "\n";
                return;
            }
            std::string filter;
            std::string cursor_name;  // 补钥页返回列表时"刚才的选择仍在"
            while (true) {
                std::string notice;
                const auto picked = lubancode::cli::RunProviderSwitchPicker(
                    config.providers, active_provider, filter, notice, cursor_name, theme);
                filter = picked.filter;
                cursor_name.clear();
                if (picked.pick == lubancode::cli::ProviderSwitchPick::Cancelled) {
                    return;  // 取消不改当前 provider,不写配置
                }
                if (picked.pick == lubancode::cli::ProviderSwitchPick::AddNew) {
                    add_via_wizard("");
                    return;
                }
                if (picked.pick == lubancode::cli::ProviderSwitchPick::Edit) {
                    run_edit(picked.name);  // e 快捷键:原地进编辑向导
                    return;
                }
                const lubancode::config::ProviderConfig* provider =
                    lubancode::config::FindProvider(config.providers, picked.name);
                if (provider == nullptr) {
                    continue;  // 列表里有名字却找不到:极少见,回列表
                }
                if (lubancode::config::ResolveProviderAuth(*provider).status ==
                    lubancode::config::ProviderAuthResolution::Status::Missing) {
                    cursor_name = picked.name;
                    if (!remediate_missing_auth(picked.name)) {
                        continue;  // 返回列表:选择与筛选词都还原
                    }
                }
                // 钥匙撞车单:同 Switch 分支,切过去前把撞车叫出来(变量赢,打码)。
                if (const std::optional<std::string> key_warning =
                        lubancode::config::ProviderAuthConflictWarning(*provider)) {
                    TermOut() << *key_warning << "\n";
                }
                execute_switch(picked.name, "");
                return;
            }
        }
        case lubancode::cli::ProviderCommandAction::Set: {
            // 认三个可设字段:native_web_search(开关)、extra_body(整段
            // JSON,替换语义)、extra_header(单条 HTTP 头,替换/删除)。
            // 字段名不对、值不合法,都跟 Add 分支同一个套路——套 set_failed
            // 报个更具体的原因,不写盘、不改内存。
            if (command.field == "native_web_search") {
                const auto enabled = lubancode::config::ParseBoolToggle(command.value);
                if (!enabled.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", enabled.error()) << "\n";
                    return;
                }
                // 先改内存里这份:SetProviderNativeWebSearch 顺带当"名字存不存在"
                // 的判断——找不到就原样不动、返回 false,不往下走落盘那一步。
                if (!lubancode::config::SetProviderNativeWebSearch(config.providers, command.name, *enabled)) {
                    TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved =
                    lubancode::config::SetProviderNativeWebSearchInGlobalConfig(command.name, *enabled);
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                TermOut() << trf("cmd.provider.set_ok", command.name, command.field, *enabled ? "on" : "off", *saved)
                          << "\n";
                // 改的正好是当前活跃端:顶层镜像字段跟着同步、重建 backend,别让
                // "刚改完当前端却要等下次 /provider switch 才生效"这种反直觉
                // 体验发生——跟 Switch 分支改完就 Rebuild 是同一个道理。
                if (active_provider == command.name) {
                    config.native_web_search = *enabled;
                    real_backend.Rebuild(config);
                    TermOut() << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            if (command.field == "extra_body") {
                // command.value 是原始文本;空串或者 "{}" 都当"清空"处理——
                // 跟 native_web_search 不一样,这里没有"合不合法的值"这一说,
                // 只有"合不合法的 JSON"。
                nlohmann::json parsed = nlohmann::json::object();
                const std::string trimmed_value = command.value;
                if (!trimmed_value.empty() && trimmed_value != "{}") {
                    nlohmann::json candidate;
                    try {
                        candidate = nlohmann::json::parse(trimmed_value);
                    } catch (const nlohmann::json::parse_error& e) {
                        TermOut() << trf("cmd.provider.set_failed",
                                          trf("cmd.provider.extra_body_invalid_json", e.what()))
                                  << "\n";
                        return;
                    }
                    if (!candidate.is_object()) {
                        TermOut() << trf("cmd.provider.set_failed", tr("cmd.provider.extra_body_not_object")) << "\n";
                        return;
                    }
                    parsed = std::move(candidate);
                }
                if (!lubancode::config::SetProviderExtraBody(config.providers, command.name, parsed)) {
                    TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved = lubancode::config::SetProviderExtraBodyInGlobalConfig(command.name, parsed);
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                TermOut() << trf("cmd.provider.set_ok", command.name, command.field,
                                  parsed.empty() ? tr("provider_wizard.extra_body.unset")
                                                  : trf("provider_wizard.extra_body.summary", parsed.size()),
                                  *saved)
                          << "\n";
                if (active_provider == command.name) {
                    config.extra_body = parsed;
                    real_backend.Rebuild(config);
                    TermOut() << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            if (command.field == "extra_header") {
                if (command.header_name.empty()) {
                    TermOut() << trf("cmd.provider.set_failed", tr("cmd.provider.extra_header_name_missing")) << "\n";
                    return;
                }
                if (!lubancode::config::SetProviderExtraHeader(config.providers, command.name, command.header_name,
                                                                command.value)) {
                    TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved = lubancode::config::SetProviderExtraHeaderInGlobalConfig(
                    command.name, command.header_name, command.value);
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                TermOut() << trf("cmd.provider.set_ok", command.name, command.header_name,
                                  command.value.empty() ? tr("provider_wizard.extra_body.unset") : command.value,
                                  *saved)
                          << "\n";
                if (active_provider == command.name) {
                    if (command.value.empty()) {
                        config.extra_headers.erase(command.header_name);
                    } else {
                        config.extra_headers[command.header_name] = command.value;
                    }
                    real_backend.Rebuild(config);
                    TermOut() << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            if (command.field == "auth") {
                // /provider set <名字> auth none|env|inline(向导重排单)。切到
                // env/inline 时若还缺变量名/key,接着开相应输入页补齐,不落
                // 半截配置;none 必须由用户显式敲这命令,谁也不许凭地址猜。
                const auto mode = lubancode::config::ParseProviderAuthMode(command.value);
                if (!mode.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", mode.error()) << "\n";
                    return;
                }
                const lubancode::config::ProviderConfig* target =
                    lubancode::config::FindProvider(config.providers, command.name);
                if (target == nullptr) {
                    TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                std::string prompted_env;
                std::string prompted_key;
                std::expected<std::string, std::string> saved;
                if (*mode == lubancode::config::ProviderAuthMode::Env && target->key_env.empty()) {
                    const std::optional<std::string> key_env =
                        lubancode::cli::ReadLine(tr("cmd.provider.auth_env_prompt"));
                    if (!key_env.has_value() || key_env->empty()) {
                        TermOut() << tr("cmd.provider.auth_aborted") << "\n";
                        return;
                    }
                    prompted_env = *key_env;
                    saved = lubancode::config::SetProviderAuthEnvInGlobalConfig(command.name, prompted_env);
                } else if (*mode == lubancode::config::ProviderAuthMode::Inline && target->api_key.empty()) {
                    const std::optional<std::string> key =
                        lubancode::cli::ReadLine(tr("cmd.provider.auth_inline_prompt"));
                    if (!key.has_value() || key->empty()) {
                        TermOut() << tr("cmd.provider.auth_aborted") << "\n";
                        return;
                    }
                    prompted_key = *key;
                    saved = lubancode::config::SetProviderAuthInlineInGlobalConfig(command.name, prompted_key);
                } else {
                    saved = lubancode::config::SetProviderAuthModeInGlobalConfig(command.name, *mode);
                }
                if (!saved.has_value()) {
                    TermOut() << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                // 内存这份跟着换(补过的变量名/key 一并同步),活跃端立即生效。
                // 问题 4:按补了什么走对应 setter,auth+字段一次切齐,不再
                // SetProviderAuthMode 之后 find_if 逐字段补丁。四路与原先的
                // 终态逐一相同(env/inline 未补 = 只换模式,其余连字段一起)。
                if (!prompted_env.empty()) {
                    lubancode::config::SetProviderAuthEnv(config.providers, command.name, prompted_env);
                } else if (!prompted_key.empty()) {
                    lubancode::config::SetProviderAuthInline(config.providers, command.name, prompted_key);
                } else if (*mode == lubancode::config::ProviderAuthMode::None) {
                    lubancode::config::SetProviderAuthNone(config.providers, command.name);
                } else {
                    lubancode::config::SetProviderAuthMode(config.providers, command.name, *mode);
                }
                const lubancode::config::ProviderConfig* fresh =
                    lubancode::config::FindProvider(config.providers, command.name);
                if (fresh != nullptr && active_provider == command.name) {
                    const lubancode::config::ProviderAuthResolution auth =
                        lubancode::config::ResolveProviderAuth(*fresh);
                    config.auth_mode = fresh->auth;
                    config.auth_token =
                        auth.status == lubancode::config::ProviderAuthResolution::Status::Ready
                            ? *auth.key
                            : std::string();
                    real_backend.Rebuild(config);
                }
                TermOut() << trf("cmd.provider.set_ok", command.name, command.field,
                                 lubancode::config::ProviderAuthModeName(*mode), *saved)
                          << "\n";
                if (active_provider == command.name) {
                    TermOut() << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            TermOut() << trf("cmd.provider.set_failed", trf("cmd.provider.set_unknown_field", command.field))
                      << "\n";
            return;
        }
        case lubancode::cli::ProviderCommandAction::Remove:
            if (!lubancode::cli::CanRemoveProvider(active_provider, command.name)) {
                TermOut() << trf("cmd.provider.remove_active", command.name) << "\n";
                return;
            }
            if (lubancode::config::FindProvider(config.providers, command.name) == nullptr) {
                TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                return;
            }
            if (const auto removed = lubancode::config::RemoveProviderFromGlobalConfig(command.name);
                removed.has_value()) {
                config.providers.erase(std::remove_if(config.providers.begin(), config.providers.end(),
                                                      [&](const lubancode::config::ProviderConfig& provider) {
                                                          return provider.name == command.name;
                                                      }),
                                       config.providers.end());
                TermOut() << trf("cmd.provider.removed", command.name, *removed) << "\n";
            } else {
                TermOut() << trf("cmd.provider.remove_failed", removed.error()) << "\n";
            }
            return;
        case lubancode::cli::ProviderCommandAction::Edit: {
            // /provider edit <名字>(容错单):TTY、管道都进向导(向导自己在
            // 管道下退化为朴素逐行,自动化可脚本驱动);名字找不着时 TTY 开
            // 已筛选的选择列表把"不存在"贴在面板内,非 TTY 一行报错。
            const lubancode::config::ProviderConfig* provider =
                lubancode::config::FindProvider(config.providers, command.name);
            if (provider == nullptr) {
                const bool can_panel = is_console && lubancode::platform::StdinIsInteractive();
                if (can_panel) {
                    const auto picked = lubancode::cli::RunProviderSwitchPicker(
                        config.providers, active_provider, command.name,
                        trf("cmd.provider.not_found", command.name), {}, theme,
                        /*edit_on_enter=*/true);
                    if (picked.pick == lubancode::cli::ProviderSwitchPick::AddNew) {
                        add_via_wizard("");
                    } else if (picked.pick == lubancode::cli::ProviderSwitchPick::Named ||
                               picked.pick == lubancode::cli::ProviderSwitchPick::Edit) {
                        run_edit(picked.name);
                    }
                    return;
                }
                TermOut() << trf("cmd.provider.not_found", command.name) << "\n";
                return;
            }
            run_edit(command.name);
            return;
        }
        case lubancode::cli::ProviderCommandAction::EditInteractive: {
            // 裸敲 /provider edit:意图是改一家已有的,复用 switch 选择器面板,
            // Enter 语义换成"编辑"。非 TTY 只给 edit 专用短用法,不倒总表。
            const bool can_panel = is_console && lubancode::platform::StdinIsInteractive();
            if (!can_panel) {
                TermOut() << lubancode::cli::ProviderSubcommandUsageLine("edit") << "\n";
                return;
            }
            const auto picked = lubancode::cli::RunProviderSwitchPicker(
                config.providers, active_provider, "", {}, {}, theme,
                /*edit_on_enter=*/true);
            if (picked.pick == lubancode::cli::ProviderSwitchPick::AddNew) {
                add_via_wizard("");
            } else if (picked.pick == lubancode::cli::ProviderSwitchPick::Named ||
                       picked.pick == lubancode::cli::ProviderSwitchPick::Edit) {
                run_edit(picked.name);
            }
            return;
        }
        case lubancode::cli::ProviderCommandAction::Invalid: {
            // 子命令容错(容错单):拼错(swtich)给"是不是想敲 X"+ X 的专用
            // 短用法;已知子命令敲错参(refresh now)给"参数不对"+ 短用法;
            // 无近邻 TTY 给短提示+最常用三行,非 TTY 给一行。任何路径都不再
            // 倒 13 行总表——总表留给 /help。
            const std::string lowered_word = [&]() {
                std::string word = command.bad_word;
                for (char& c : word) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                return word;
            }();
            const auto nearest = lubancode::cli::NearestProviderSubcommand(lowered_word);
            if (nearest.has_value()) {
                if (*nearest == lowered_word) {
                    TermOut() << tr("cmd.provider.bad_args") << "\n";
                } else {
                    TermOut() << trf("cmd.provider.typo_hint", command.bad_word, *nearest) << "\n";
                }
                TermOut() << lubancode::cli::ProviderSubcommandUsageLine(*nearest) << "\n";
                return;
            }
            const bool tty = is_console && lubancode::platform::StdinIsInteractive();
            if (tty) {
                TermOut() << trf("cmd.provider.unknown_sub.tty", command.bad_word) << "\n";
            } else {
                TermOut() << tr("cmd.provider.unknown_sub.pipe") << "\n";
            }
            return;
        }
    }
}

// /language 命令(i18n):裸敲列可选语言(内置两种 + 语言包)编号选;带参数
// 直接按语言码切。切换即时生效(会话级),有配置文件就问一句要不要写回
// (沿用 /model 那套 UpdateLanguageInConfigFile),没有就提示只在本会话生效。
void HandleLanguageCommand(const std::string& args, std::optional<std::string>& config_file_path) {
    namespace cli = lubancode::cli;
    std::string chosen;

    if (!args.empty()) {
        if (!cli::HasLanguage(args)) {
            TermOut() << trf("cmd.language.unknown", args) << "\n";
            return;
        }
        chosen = args;
    } else {
        const std::vector<std::string> langs = cli::AvailableLanguages();
        std::size_t current_idx = 0;
        std::vector<lubancode::cli::ChoiceMenuItem> items;
        items.reserve(langs.size());
        for (std::size_t i = 0; i < langs.size(); ++i) {
            const bool is_current = langs[i] == cli::CurrentLanguage();
            if (is_current) current_idx = i;
            items.push_back({cli::LanguageDisplayName(langs[i]),
                             is_current ? tr("cmd.language.current_mark") : std::string{}});
        }
        std::size_t idx = current_idx;
        TermOut() << tr("cmd.language.list_header") << "\n";
        const bool interactive_menu = lubancode::platform::StdinIsInteractive() &&
                                      lubancode::platform::ProbeStdoutConsole().is_console;
        if (interactive_menu) {
            lubancode::cli::ChoiceMenuOptions opts;
            opts.hint = tr("confirm.menu.hint");
            opts.initial_cursor = current_idx;
            const auto sel = lubancode::cli::ReadChoiceMenu(items, opts, lubancode::cli::Theme{});
            if (!sel.has_value()) {
                return;
            }
            idx = sel->selected_indices.empty() ? current_idx : sel->selected_indices.front();
        } else {
            for (std::size_t i = 0; i < items.size(); ++i) {
                TermOut() << "  " << (i + 1) << ") " << items[i].label
                          << (items[i].description.empty() ? "" : "  " + items[i].description) << "\n";
            }
            const std::optional<std::string> selection = cli::ReadLine(trf("cmd.language.choose", current_idx + 1));
            if (!selection.has_value()) {
                return;
            }
            if (!selection->empty()) {
                try {
                    std::size_t consumed = 0;
                    const int n = std::stoi(*selection, &consumed);
                    if (consumed != selection->size() || n < 1 || static_cast<std::size_t>(n) > langs.size()) {
                        TermOut() << tr("cmd.language.bad_number") << "\n";
                        return;
                    }
                    idx = static_cast<std::size_t>(n - 1);
                } catch (...) {
                    TermOut() << tr("cmd.language.bad_number") << "\n";
                    return;
                }
            }
        }
        chosen = langs[idx];
    }

    cli::SetLanguage(chosen);
    TermOut() << trf("cmd.language.switched", cli::LanguageDisplayName(chosen)) << "\n";

    if (config_file_path.has_value()) {
        const std::optional<std::string> answer =
            cli::ReadLine(trf("cmd.write_config_prompt", *config_file_path));
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateLanguageInConfigFile(*config_file_path, chosen);
            if (updated.has_value()) {
                TermOut() << trf("cmd.write_config.updated", *config_file_path) << "\n";
            } else {
                TermOut() << trf("cmd.write_config.failed", updated.error()) << "\n";
            }
        }
    } else {
        TermOut() << tr("cmd.session_only") << "\n";
    }
}

// --config、/config 共用:打印最终生效的配置和每个字段的来源。session_model
// 有值时(/config 场景)额外打一行"本会话实际在用的 model"——/model 切换
// 只影响会话内存,不一定跟 config.model(四级合并出来的那份)一致。
// catalog 非空时(现在两个调用点都传)追加两行:模型目录路径 + 条目数,
// 以及"当前模型(会话在用的那个,没有就看 config.model)命没命中目录"。
void PrintConfigDiagnostics(const lubancode::config::ConfigResult& result,
                             const std::optional<std::string>& session_model,
                             const lubancode::config::ModelCatalog* catalog,
                             const lubancode::config::SettingsLocal* settings) {
    const auto& config = result.config;
    const auto& sources = result.sources;
    const std::string wire_str = lubancode::config::ProviderWireName(config.wire);

    TermOut() << tr("config.header") << "\n\n";
    TermOut() << "  wire               = " << wire_str << "  [" << lubancode::config::ToString(sources.wire) << "]\n";
    TermOut() << "  base_url           = " << (config.base_url.empty() ? tr("config.not_set") : config.base_url)
              << "  [" << lubancode::config::ToString(sources.base_url) << "]\n";
    // api_key 行(钥匙撞车单):方括号说钥匙来路——变量名/inline/压过关系,
    // 前缀打码,与实际发送的钥匙对得上账。只在激活 provider 条目的解析结果
    // 确实是当前 auth_token 时才换(被 LUBANCODE_API_KEY 等压过、或没有
    // 条目的纯顶层写法,照旧标来源层级,别张冠李戴)。
    std::string api_key_bracket = lubancode::config::ToString(sources.auth_token);
    if (const lubancode::config::ProviderConfig* active = lubancode::config::FindProvider(
            config.providers, config.active_provider);
        active != nullptr) {
        const lubancode::config::ProviderAuthResolution auth = lubancode::config::ResolveProviderAuth(*active);
        const bool matches_token =
            (auth.status == lubancode::config::ProviderAuthResolution::Status::Ready &&
             auth.key.has_value() && *auth.key == config.auth_token) ||
            (auth.status == lubancode::config::ProviderAuthResolution::Status::Missing &&
             config.auth_token.empty());
        if (matches_token) {
            using lubancode::config::ProviderAuthResolution;
            if (auth.status == ProviderAuthResolution::Status::Missing) {
                api_key_bracket = trf("config.api_key.missing", active->key_env);
            } else if (auth.source == ProviderAuthResolution::KeySource::EnvVar) {
                if (auth.conflict) {
                    api_key_bracket = trf("config.api_key.env_over_inline", active->key_env,
                                          lubancode::config::MaskApiKey(*auth.key),
                                          lubancode::config::MaskApiKey(active->api_key));
                } else if (!active->api_key.empty()) {
                    api_key_bracket = trf("config.api_key.env_same_as_inline", active->key_env);
                } else {
                    api_key_bracket =
                        trf("config.api_key.from_env", active->key_env, lubancode::config::MaskApiKey(*auth.key));
                }
            } else if (auth.source == ProviderAuthResolution::KeySource::Inline) {
                if (active->auth == lubancode::config::ProviderAuthMode::Env) {
                    // env 模式回落:inline 兜底,点名变量未设,别让人以为变量在生效。
                    api_key_bracket = trf("config.api_key.from_inline_env_unset",
                                          lubancode::config::MaskApiKey(*auth.key), active->key_env);
                } else {
                    api_key_bracket =
                        trf("config.api_key.from_inline", lubancode::config::MaskApiKey(*auth.key));
                }
            }
        }
    }
    TermOut() << "  api_key            = "
              << (config.auth_mode == lubancode::config::ProviderAuthMode::None
                      ? tr("cmd.provider.auth_none")
                      : lubancode::config::MaskApiKey(config.auth_token))
              << "  [" << api_key_bracket << "]\n";
    TermOut() << "  model              = " << (config.model.empty() ? tr("config.not_set") : config.model) << "  ["
              << lubancode::config::ToString(sources.model) << "]\n";
    TermOut() << "  active_provider    = "
              << (config.active_provider.empty() ? tr("config.not_set") : config.active_provider) << "  ["
              << lubancode::config::ToString(sources.active_provider) << "]\n";
    if (lubancode::config::EnvironmentOverridesActiveProvider(config, sources, config.active_provider)) {
        TermOut() << "  provider_binding   = env override / unbound\n";
    }
    TermOut() << "  max_context_chars  = " << config.max_context_chars << "  ["
              << lubancode::config::ToString(sources.max_context_chars) << "]\n";
    TermOut() << "  max_steps_per_turn = " << config.max_steps_per_turn
              << (config.max_steps_per_turn == 0 ? tr("config.steps.unlimited") : "") << "  ["
              << lubancode::config::ToString(sources.max_steps_per_turn) << "]\n";
    // 输出预算(规格根因一):写明实际值与来源——unset 交服务端默认
    // (chat/responses 不发字段;anthropic 必填,client 落公开兜底),三级
    // 声明(config > provider > 模型目录)哪级说了算就在这里点名。子代理
    // 单列一行:默认与 main 同一份,subagent 段显式覆盖才不同。
    {
        const std::string effective_model = session_model.value_or(config.model);
        const lubancode::agent::AgentRuntimeProfile main_profile =
            lubancode::app::BuildMainRuntimeProfile(config, catalog, effective_model);
        const lubancode::agent::AgentRuntimeProfile subagent_profile =
            lubancode::app::BuildSubagentRuntimeProfile(main_profile, config);
        const auto budget_line = [](const lubancode::agent::AgentRuntimeProfile& profile) {
            if (!profile.max_output_tokens.has_value()) {
                return tr("config.output.unset");
            }
            return trf("config.output.tokens", *profile.max_output_tokens);
        };
        TermOut() << "  max_output_tokens  = "
                  << budget_line(main_profile) << "  ["
                  << lubancode::app::OutputBudgetSourceText(
                         main_profile.max_output_tokens_source, /*subagent_override=*/false)
                  << "]\n";
        TermOut() << "  max_output_tokens (subagent) = "
                  << budget_line(subagent_profile) << "  ["
                  << lubancode::app::OutputBudgetSourceText(
                         subagent_profile.max_output_tokens_source,
                         config.subagent.max_output_tokens.has_value())
                  << "]\n";
        TermOut() << "  length_continuations = " << config.agent.length_continuations << "  ["
                  << lubancode::config::ToString(sources.agent) << "]\n";
    }
    TermOut() << "  theme              = " << config.theme << "  [" << lubancode::config::ToString(sources.theme)
              << "]\n";
    TermOut() << "  status_panel       = ";
    for (std::size_t i = 0; i < config.status_panel.items.size(); ++i) {
        if (i > 0) {
            TermOut() << ",";
        }
        TermOut() << config.status_panel.items[i];
    }
    TermOut() << "  [" << lubancode::config::ToString(sources.status_panel) << "]\n";
    // i18n:language 空 = 跟系统,顺带亮出此刻实际生效的语言码。
    TermOut() << "  language           = "
              << (config.language.empty() ? trf("config.language.follow_system", lubancode::cli::CurrentLanguage())
                                           : config.language)
              << "  [" << lubancode::config::ToString(sources.language) << "]\n";
    TermOut() << "  system_prompt_file = "
              << (config.system_prompt_file.empty() ? tr("config.not_set") : config.system_prompt_file) << "  ["
              << lubancode::config::ToString(sources.system_prompt_file) << "]\n";
    TermOut() << "  context_window     = " << config.context_window_tokens << " tokens  ["
              << lubancode::config::ToString(sources.context_window_tokens) << "]\n";
    TermOut() << "  compact_model      = "
              << (config.compact_model.empty() ? tr("config.compact_model.unset") : config.compact_model) << "  ["
              << lubancode::config::ToString(sources.compact_model) << "]\n";
    TermOut() << "  think              = " << (config.think.empty() ? tr("config.think.unset") : config.think)
              << "  [" << lubancode::config::ToString(sources.think) << "]\n";
    TermOut() << "  soul               = " << (config.soul.empty() ? tr("config.soul.unset") : config.soul)
              << "  [" << lubancode::config::ToString(sources.soul) << "]\n";
    TermOut() << "  tool_search_threshold = " << config.tool_search_threshold
              << (config.tool_search_threshold == 0 ? tr("config.threshold.never") : "") << "  ["
              << lubancode::config::ToString(sources.tool_search_threshold) << "]\n";
    // 动态工具 P4:延迟挂载的 token 预算门(0 = 只看枚数)。与 threshold
    // 同级展示,排查"枚数过了怎么没延迟"就看这行。
    TermOut() << "  tool_search_token_floor = " << config.tool_search_token_floor
              << (config.tool_search_token_floor == 0 ? "(只看枚数)" : "") << "  ["
              << lubancode::config::ToString(sources.tool_search_token_floor) << "]\n";
    // 动态工具 P1:延迟工具模式(空 = legacy_expand 现状;proxy_reference
    // 是 P1 新路,disabled 全量常驻;auto 是 P4 能力驱动档——门开走
    // native、门不开落宿主推荐档)。
    TermOut() << "  deferred_tool_mode = "
              << (config.deferred_tool_mode.empty() ? "legacy_expand(默认)" : config.deferred_tool_mode)
              << "  [" << lubancode::config::ToString(sources.deferred_tool_mode) << "]\n";
    TermOut() << "  memory            = " << (config.memory.enabled ? "on" : "off")
              << " (use=" << (config.memory.use ? "on" : "off")
              << ", generate=" << (config.memory.generate ? "on" : "off") << ")  ["
              << lubancode::config::ToString(sources.memory) << "]\n";
    // 分层:项目级、全局各自的配置文件路径分开列(有哪个列哪个,标清是
    // 哪一级),都没有就沿用老的单行 config.label.file(通常也不会走到)。
    if (result.project_config_file_path.has_value() || result.global_config_file_path.has_value()) {
        if (result.project_config_file_path.has_value()) {
            TermOut() << "  项目级配置       = " << *result.project_config_file_path << "\n";
        }
        if (result.global_config_file_path.has_value()) {
            TermOut() << "  全局配置         = " << *result.global_config_file_path << "\n";
        }
    } else if (result.config_file_path.has_value()) {
        TermOut() << trf("config.label.file", *result.config_file_path) << "\n";
    }
    // hooks 摘要:schema 2 按事件名数 handler(=装载后的定义数,与启动横幅、
    // /hooks 的"已装载 N 条"对得上账),分 user/project 两层;旧四类另列。
    // 两边都是空的才说"未配置",省得打一堆 ×0。
    {
        const auto& hooks = config.hooks;
        std::vector<std::string> parts;

        // schema 2 的来源分级与 loader 同一套口径:项目配置文件匹配、或路径
        // 落在当前目录之下 = project,其余 = user(保守取边,见 loader.cpp)。
        const std::string cwd = lubancode::platform::CurrentDirUtf8();
        const auto is_project_source = [&](const std::string& source_path) {
            if (result.project_config_file_path.has_value() && source_path == *result.project_config_file_path) {
                return true;
            }
            if (result.global_config_file_path.has_value() && source_path == *result.global_config_file_path) {
                return false;
            }
            return !cwd.empty() && source_path.rfind(cwd, 0) == 0;
        };

        if (!hooks.events.empty()) {
            int schema_total = 0;
            int schema_user = 0;
            int schema_project = 0;
            std::vector<std::string> event_parts;
            for (const auto& [event, groups] : hooks.events) {
                int event_count = 0;
                for (const auto& group : groups) {
                    event_count += static_cast<int>(group.hooks.size());
                    if (is_project_source(group.source_path)) {
                        schema_project += static_cast<int>(group.hooks.size());
                    } else {
                        schema_user += static_cast<int>(group.hooks.size());
                    }
                }
                schema_total += event_count;
                if (event_count > 0) {
                    event_parts.push_back(std::string(lubancode::hooks::ToString(event)) + "×" +
                                          std::to_string(event_count));
                }
            }
            std::string schema_part = "schema 2 ×" + std::to_string(schema_total) + " (user×" +
                                      std::to_string(schema_user) + ", project×" + std::to_string(schema_project) + ")";
            if (!event_parts.empty()) {
                schema_part += "; ";
                for (std::size_t i = 0; i < event_parts.size(); ++i) {
                    if (i > 0) {
                        schema_part += ", ";
                    }
                    schema_part += event_parts[i];
                }
            }
            parts.push_back(std::move(schema_part));
        }
        if (!hooks.pre_tool.empty()) {
            parts.push_back("legacy pre_tool×" + std::to_string(hooks.pre_tool.size()));
        }
        if (!hooks.post_tool.empty()) {
            parts.push_back("legacy post_tool×" + std::to_string(hooks.post_tool.size()));
        }
        if (!hooks.session_start.empty()) {
            parts.push_back("legacy session_start×" + std::to_string(hooks.session_start.size()));
        }
        if (!hooks.session_end.empty()) {
            parts.push_back("legacy session_end×" + std::to_string(hooks.session_end.size()));
        }
        TermOut() << "  hooks              = ";
        if (parts.empty()) {
            TermOut() << tr("config.hooks.none");
        } else {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) {
                    TermOut() << "; ";
                }
                TermOut() << parts[i];
            }
        }
        TermOut() << "\n";
    }
    // M8:mcpServers 同样只从配置文件来,没有分级来源可打,只打个数——
    // /config 只报"配置了几个",实际存活状态得看 /mcp(那个才知道哪个真的
    // 起来了、握手成没成功)。
    {
        TermOut() << "  mcpServers         = ";
        if (config.mcp_servers.empty()) {
            TermOut() << tr("config.hooks.none");
        } else {
            TermOut() << trf("config.mcp.count", config.mcp_servers.size());
        }
        TermOut() << "\n";
    }
    // websearch:search 段同样只从配置文件来。api_key 照例打码,provider
    // 直接亮出来——配了这一段 web_search 工具才会注册。
    {
        TermOut() << "  search             = ";
        if (!config.search.Configured()) {
            TermOut() << tr("config.search.none");
        } else {
            TermOut() << config.search.provider
                      << " (api_key " << lubancode::config::MaskApiKey(config.search.api_key) << ")";
        }
        TermOut() << "\n";
    }
    // permissions(settings.local.json):项目级本地权限摘要——allow_tools
    // 几个、allow/deny_commands 几条、起手确认档。没有这份文件就说"未配置"。
    {
        TermOut() << "  permissions        = ";
        if (settings == nullptr || settings->Empty()) {
            TermOut() << tr("config.hooks.none");
        } else {
            std::vector<std::string> parts;
            if (!settings->allow_tools.empty()) {
                parts.push_back("allow_tools×" + std::to_string(settings->allow_tools.size()));
            }
            if (!settings->allow_commands.empty()) {
                parts.push_back("allow_commands×" + std::to_string(settings->allow_commands.size()));
            }
            if (!settings->deny_commands.empty()) {
                parts.push_back("deny_commands×" + std::to_string(settings->deny_commands.size()));
            }
            if (settings->default_confirm_mode.has_value()) {
                parts.push_back("default_confirm_mode=" + *settings->default_confirm_mode);
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) {
                    TermOut() << ", ";
                }
                TermOut() << parts[i];
            }
        }
        TermOut() << "\n";
    }
    // 模型目录(models.json):路径 + 条目数,以及当前模型命没命中。
    if (catalog != nullptr) {
        TermOut() << tr("config.label.catalog");
        if (catalog->source_path.empty()) {
            const auto expected = lubancode::config::ModelCatalogPath();
            TermOut() << trf("config.catalog.none",
                              expected.has_value() ? *expected
                                                    : tr("path.no_home") + "/.lubancode/models.json");
        } else {
            TermOut() << trf("config.catalog.entries", catalog->source_path, catalog->models.size());
        }
        TermOut() << "\n";
        const std::string& current = session_model.has_value() ? *session_model : config.model;
        const auto* entry = catalog->FindBySlug(current);
        TermOut() << tr("config.label.catalog_hit");
        if (current.empty()) {
            TermOut() << tr("config.catalog.model_unset");
        } else if (entry != nullptr) {
            TermOut() << trf("config.catalog.hit",
                              entry->slug + (entry->display_name.empty()
                                                  ? std::string()
                                                  : trf("config.catalog.display_name", entry->display_name)));
        } else {
            TermOut() << trf("config.catalog.miss", current);
        }
        TermOut() << "\n";
    }
    if (session_model.has_value()) {
        TermOut() << "\n" << trf("config.session_model", *session_model);
        if (*session_model != config.model) {
            TermOut() << tr("config.session_model.note");
        }
        TermOut() << "\n";
    }
}

// ---- /keymap 与 /copy(终端接线收尾单自大类搬出;输出走 TerminalPort) ----

// /keymap [set 动作 和弦 | reset [动作|all]]:列动作名/当前键/作用域/
// 可否改绑;set 走 keymap 的冲突检查(同域撞车拒绝),落盘用户级
// ~/.lubancode/keymap.json(项目配置不读不写,改键是全局的)。
void HandleKeymapCommand(const std::string& raw_args, const std::optional<std::string>& home_lubancode,
                         const lubancode::cli::Theme& theme) {
    namespace keymap = lubancode::cli::keymap;
    auto& out = lubancode::cli::TermOut();
    std::vector<std::string> words;
    std::string current;
    for (const char c : raw_args) {
        if (c == ' ' || c == '\t') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }

    if (words.empty()) {
        // 列全表:作用域分组,和弦右对齐,固定键/未绑键标明。
        out << tr("keymap.list_header") << "\n";
        for (const auto& record : keymap::ActiveKeymap().AllBindings()) {
            const std::string chord = record.has_default ? keymap::FormatKeyChord(record.chord) : "-";
            out << theme.stats << "  [" << keymap::ScopeName(record.scope) << "] " << chord;
            for (int pad = static_cast<int>(chord.size()); pad < 12; ++pad) {
                out << ' ';
            }
            out << keymap::ActionName(record.action)
                << (!record.bindable ? tr("keymap.fixed_suffix")
                     : !record.has_default ? tr("keymap.unbound_suffix") : "")
                << theme.reset << "\n";
        }
        out << tr("keymap.usage") << "\n";
        return;
    }
    if (words[0] == "set") {
        if (words.size() != 3) {
            out << theme.error << tr("keymap.usage") << theme.reset << "\n";
            return;
        }
        const auto action = keymap::ActionFromName(words[1]);
        if (!action.has_value()) {
            out << theme.error << trf("keymap.unknown_action", words[1]) << theme.reset << "\n";
            return;
        }
        const auto chord = keymap::ParseKeyChord(words[2]);
        if (!chord.has_value()) {
            out << theme.error << trf("keymap.bad_chord", words[2]) << theme.reset << "\n";
            return;
        }
        std::string error;
        if (!keymap::ActiveKeymap().SetBinding(*action, *chord, error)) {
            out << theme.error << trf("keymap.bind_failed", error) << theme.reset << "\n";
            return;
        }
        if (home_lubancode.has_value()) {
            if (const auto save_error = keymap::SaveActiveKeymapOverrides(*home_lubancode);
                save_error.has_value()) {
                out << theme.error << trf("keymap.save_failed", *save_error) << theme.reset << "\n";
            }
        }
        out << theme.stats
            << trf("keymap.bound", keymap::ActionName(*action), keymap::FormatKeyChord(*chord))
            << theme.reset << "\n";
        return;
    }
    if (words[0] == "reset") {
        std::string error;
        if (words.size() == 2 && words[1] == "all") {
            keymap::Keymap fresh;  // 出厂默认整份换血
            for (const auto& record : fresh.AllBindings()) {
                if (record.bindable) {
                    (void)keymap::ActiveKeymap().ResetBinding(record.action, error);
                }
            }
            out << theme.stats << tr("keymap.reset_all") << theme.reset << "\n";
        } else if (words.size() == 2) {
            const auto action = keymap::ActionFromName(words[1]);
            if (!action.has_value() || !keymap::ActiveKeymap().ResetBinding(*action, error)) {
                out << theme.error << trf("keymap.reset_failed", words[1]) << theme.reset << "\n";
                return;
            }
            out << theme.stats << trf("keymap.reset_one", words[1]) << theme.reset << "\n";
        } else {
            out << theme.error << tr("keymap.usage") << theme.reset << "\n";
            return;
        }
        if (home_lubancode.has_value()) {
            if (const auto save_error = keymap::SaveActiveKeymapOverrides(*home_lubancode);
                save_error.has_value()) {
                out << theme.error << trf("keymap.save_failed", *save_error) << theme.reset << "\n";
            }
        }
        return;
    }
    out << theme.error << tr("keymap.usage") << theme.reset << "\n";
}

// /copy [plain]:复制上一段完整答话。"已完成"由结构保证:交互循环单线程,
// 这个命令只在回合收口、提示符回来之后才会被分派——不存在"流式中"的调
// 用点。默认复制原始 Markdown(history 里的 TextBlock 本就无 ANSI、无
// spinner、无 token 统计);plain 走 MarkdownToPlainText。
void HandleCopyCommand(const std::string& raw_args, const std::vector<lubancode::api::Message>& history,
                       const lubancode::cli::Theme& theme) {
    auto& out = lubancode::cli::TermOut();
    std::string args = raw_args;
    while (!args.empty() && (args.front() == ' ' || args.front() == '\t')) {
        args.erase(args.begin());
    }
    for (char& c : args) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (!args.empty() && args != "plain") {
        out << theme.stats << tr("cmd.copy.usage") << theme.reset << "\n";
        return;
    }

    // 倒着找最近一条有正文的 assistant 消息(工具调用中间可能穿插空文本)。
    std::string text;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (it->role != lubancode::api::Role::Assistant) {
            continue;
        }
        std::vector<std::string> parts;
        for (const auto& block : it->content) {
            if (const auto* block_text = std::get_if<lubancode::api::TextBlock>(&block);
                block_text != nullptr && !block_text->text.empty()) {
                parts.push_back(block_text->text);
            }
        }
        if (parts.empty()) {
            continue;
        }
        text = parts.front();
        for (std::size_t i = 1; i < parts.size(); ++i) {
            text += "\n\n";
            text += parts[i];
        }
        break;
    }
    if (text.empty()) {
        out << theme.error << tr("cmd.copy.no_assistant") << theme.reset << "\n";
        return;
    }
    if (args == "plain") {
        text = lubancode::cli::MarkdownToPlainText(text);
    }

    std::string detail;
    switch (lubancode::platform::CopyTextToClipboard(text, detail)) {
        case lubancode::platform::ClipboardResult::Ok:
            out << theme.stats << trf("cmd.copy.done", text.size()) << theme.reset << "\n";
            break;
        case lubancode::platform::ClipboardResult::Unsupported:
            out << theme.error << trf("cmd.copy.unsupported", detail) << theme.reset << "\n";
            break;
        case lubancode::platform::ClipboardResult::Failure:
            // 失败必须报错,不得打印"已复制"后空着。
            out << theme.error << trf("cmd.copy.failed", detail) << theme.reset << "\n";
            break;
    }
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):设置域的分派位。case 体原样自
// interactive_session 的大 switch 搬来,材料经 SlashDispatchContext 递入。
// ---------------------------------------------------------------------------

CommandFlow HandleSlashProvider(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleProviderCommand(parsed.args, *ctx.config, *ctx.active_provider, *ctx.real_backend, *ctx.wire_str,
                          ctx.current_model, ctx.current_think, ctx.current_think_history,
                          *ctx.context_tracker, ctx.current_model_instructions, *ctx.model_catalog,
                          *ctx.prompt_options, ctx.rebuild_loop, ctx.spinner_enabled, *ctx.theme,
                          *ctx.active_provider_write_path, ctx.config_result->sources.active_provider);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashConfig(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintConfigDiagnostics(*ctx.config_result, *ctx.current_model, ctx.model_catalog, ctx.settings_local);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashUpdate(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleUpdateCommand(parsed.args, ctx.config->connect_timeout_ms, ctx.config->request_timeout_secs);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashLanguage(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleLanguageCommand(parsed.args, *ctx.config_file_path);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashThink(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // 目录条目按"此刻的会话模型"现查——/model 切过之后,/think 列的就是
    // 新模型声明的档位。目录没有声明再看当前 provider 配置的声明(Effort
    // 诊断单:未知模型至少列本 provider 配置,不只甩一句"以服务商为准")。
    const lubancode::config::ModelCatalogEntry* entry =
        ctx.model_catalog->FindByProviderAndSlug(*ctx.active_provider, *ctx.current_model);
    // /think history <default|all>(Kimi 保留式思考单 P1):跨轮保留的专用
    // 子命令,不与档位混写。第一个词认出 history 就整段接走。
    const auto [first_word, rest] = SplitSkillCommandArgs(parsed.args);
    if (first_word == "history") {
        HandleThinkHistoryCommand(rest, ctx.current_think_history, ctx.current_think, entry,
                                  ctx.session_runtime);
        ctx.sync_request_policy();
        return CommandFlow::Continue;
    }
    // 关思考与 history all 的冲突拦截(P1 第 3 条):可选保留的模型上,用户
    // 已开 all 又把档位切到 none——明报拒绝,不猜(要关思考先把 history 切
    // 回 default)。其余模型照旧走档位切换。
    if (!parsed.args.empty() && ctx.current_think_history != nullptr &&
        *ctx.current_think_history == lubancode::api::ReasoningHistoryMode::All && entry != nullptr &&
        lubancode::api::ReasoningHistorySupportFor(entry->reasoning) ==
            lubancode::api::ReasoningHistorySupport::RequestControl &&
        lubancode::api::ReasoningEffortIsOff(parsed.args, entry->reasoning)) {
        TermOut() << "冲突: 跨轮保留(history all)建立在思考开启之上,此模型不支持关着思考保留。"
                     "先 /think history default 再关思考,或换一个开思考的档。"
                  << "\n";
        return CommandFlow::Continue;
    }
    HandleThinkCommand(parsed.args, ctx.current_think, entry, ctx.config->provider_think_levels,
                       ctx.config->think_param);
    // 模型协议兼容实录矩阵单 P1:落线方言一并亮出来——档位只是抽象值,
    // 用户该看见"这个模型最终在 wire 上长什么样"。目录没声明(本地自配
    // 端)打兼容形状那条,不冒充已验证。(形状行与 DescribeRequestEffort
    // 同风格:诊断行直接拼字,不走 i18n 键。)
    if (parsed.args.empty()) {
        const auto catalog = lubancode::config::LoadProviderCatalog();
        const config::ProviderPreset* preset = catalog.FindProvider(*ctx.active_provider);
        const config::ProviderCatalogModel* model =
            preset != nullptr ? preset->FindModel(*ctx.current_model) : nullptr;
        if (model != nullptr && !model->reasoning.dialect.empty()) {
            TermOut() << "落线形状: "
                      << lubancode::config::DescribeReasoningDialect(model->reasoning.dialect)
                      << (model->reasoning.dialect.verified ? "  [已验证]" : "  [未验证]")
                      << "\n";
        } else {
            TermOut() << "落线形状: 目录未声明方言,走兼容形状(未验证)。\n";
        }
        // Kimi 保留式思考单 P0:"本轮思考"与"历史回传"是两笔账,分开亮——
        // 档位只管这一轮想多深,历史 assistant 的 reasoning 是否随下一份
        // 请求送回,由 replay 策略说了算(方言优先;本地自定义端回落
        // provider 兼容声明)。诊断行直接拼字,与落线形状同一风格。
        TermOut() << "本轮思考: "
                  << (ctx.current_think->empty() ? std::string("未设档位(不发推理参数)")
                                                 : *ctx.current_think)
                  << "\n";
        const auto replay_line = [](const std::string& policy, const std::string& field,
                                    const char* source) {
            const std::string named = field.empty() ? std::string("reasoning_content") : field;
            return "历史回传: " + policy + " -> " + named + "(" + source + ")";
        };
        if (model != nullptr && !model->reasoning.dialect.empty()) {
            // P1:开了 history all 时,回传形状按升级后的 always 报——用户
            // 该看见下一份请求真正会怎么走,不是方言的静态缺省。
            std::string replay = model->reasoning.dialect.replay;
            if (*ctx.current_think_history == lubancode::api::ReasoningHistoryMode::All &&
                model->reasoning.dialect.history_control == "thinking_keep") {
                replay = "always";
            }
            TermOut() << replay_line(replay, model->reasoning.dialect.replay_field, "模型方言声明")
                      << "\n";
        } else {
            TermOut() << replay_line(ctx.config->reasoning_replay.empty()
                                         ? std::string("never")
                                         : ctx.config->reasoning_replay,
                                     ctx.config->reasoning_replay_field,
                                     "provider 兼容声明,模型无方言")
                      << "\n";
        }
        // P1:跨轮保留选择一行(验收:本轮开关/effort/replay/history mode
        // 分别可见)。模型能力行顺带说明这个选择在这个模型上是什么身份。
        const lubancode::api::ReasoningConfig empty_reasoning;
        TermOut() << "历史保留: "
                  << (*ctx.current_think_history == lubancode::api::ReasoningHistoryMode::All
                          ? std::string("all")
                          : std::string("default"))
                  << "\n";
        TermOut() << ThinkHistorySupportLine(entry != nullptr ? entry->reasoning : empty_reasoning) << "\n";
    }
    // 五层后端退役(批四):effort 的即时生效改走皮上的 request 档案,
    // 下一份请求带上新档位。
    ctx.sync_request_policy();
    return CommandFlow::Continue;
}

CommandFlow HandleSlashSkills(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    PrintSkillsCommand(*ctx.skills, lubancode::platform::CurrentDirUtf8(), *ctx.home_dir);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashSkill(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    if (HandleSkillCommand(parsed.args, *ctx.global_skills_root, *ctx.project_skills_root)) {
        ctx.refresh_skills();
        TermOut() << tr("cmd.skill.refreshed") << "\n";
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSlashKeymap(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    HandleKeymapCommand(parsed.args, *ctx.home_lubancode, *ctx.theme);
    return CommandFlow::Continue;
}

CommandFlow HandleSlashCopy(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // P0-3 轨迹档(§14.5:/copy 一律读 ReplayState):折叠本场 main.jsonl
    // 投影出 history,再走同一只复制口。flag 关照旧路。
    if (ctx.trajectory != nullptr) {
        const auto fold = ctx.trajectory->FoldMainReplay();
        if (fold.ok()) {
            HandleCopyCommand(parsed.args, lubancode::runtime::ProjectHistoryFromReplay(fold.state),
                              *ctx.theme);
            return CommandFlow::Continue;
        }
    }
    HandleCopyCommand(parsed.args, ctx.main_agent->History(), *ctx.theme);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
