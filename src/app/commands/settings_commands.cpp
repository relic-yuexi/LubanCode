// settings_commands.hpp 的实现:模型/供应商/配置/语言/技能/更新命令的函数体。
#include "app/commands/settings_commands.hpp"

#include <iostream>

#include "app/version.hpp"
#include "app/turn_runner.hpp"
#include "platform/paths.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/models.hpp"
#include "cli/console_input.hpp"
#include "platform/console.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/provider_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "tools/skill_loader.hpp"

namespace lubancode::app {


using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::tr;
using lubancode::cli::trf;
void PrintLubanIcon(const lubancode::cli::Theme& theme) {
    std::cout << theme.banner << "╭───────────────────────╮" << theme.reset << "\n";
    std::cout << theme.banner << "│  鲁 班 code           │" << theme.reset << "\n";
    std::cout << theme.stats << "│  匠心运斤 · 代码成器  │" << theme.reset << "\n";
    std::cout << theme.banner << "╰───────────────────────╯" << theme.reset << "\n";
}

// 交互模式启动横幅:一眼看全版本、wire、当前模型、工作目录,两行,不啰嗦。
void PrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme) {
    const std::string wire_str = lubancode::config::ProviderWireName(config.wire);
    std::cout << theme.banner << "lubancode " << kVersion << "  [" << wire_str << "] " << config.model << theme.reset
              << "\n";
    std::cout << theme.stats << "cwd: " << CurrentDirUtf8() << "  ·  " << tr("banner.hint") << theme.reset << "\n";
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
// interactive 让 ReadChoice 在管道/重定向时回落编号。两处向导共用,免得注入逻辑漂移。
lubancode::cli::WizardIO MakeInteractiveWizardIO(const lubancode::cli::Theme& theme) {
    lubancode::cli::WizardIO io;
    io.print = [](const std::string& line) {
        std::cout << line << "\n";
        std::cout.flush();
    };
    // prompt 已经由向导自己通过 print 打出来了,这里传空串,别让 ReadLine 再打一遍。
    io.read_line = []() -> std::optional<std::string> { return lubancode::cli::ReadLine(""); };
    io.fetch_models = [](lubancode::config::Wire wire, const std::string& base_url, const std::string& api_key) {
        return lubancode::api::ListModels(wire, base_url, api_key);
    };
    io.interactive = lubancode::platform::StdinIsInteractive() &&
                     lubancode::platform::ProbeStdoutConsole().is_console;
    io.choose = [&theme](const std::vector<lubancode::cli::WizardChoiceItem>& items,
                         std::size_t default_index, const std::string& hint) -> std::optional<std::size_t> {
        std::vector<lubancode::cli::ChoiceMenuItem> menu_items;
        menu_items.reserve(items.size());
        for (const auto& it : items) {
            menu_items.push_back({it.label, it.description});
        }
        lubancode::cli::ChoiceMenuOptions opts;
        opts.hint = hint;
        opts.initial_cursor = default_index;  // 初始高亮落在默认项,回车即选中默认
        const auto selected = lubancode::cli::ReadChoiceMenu(menu_items, opts, theme);
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
        std::cout << tr("cmd.update.usage") << "\n";
        return false;
    }

    std::cout << tr("cmd.update.checking") << "\n";
    std::cout.flush();
    const auto checked = lubancode::config::CheckForUpdate(
        std::string(kVersion), connect_timeout_ms, request_timeout_secs);
    if (!checked.has_value()) {
        std::cout << trf("cmd.update.failed", checked.error()) << "\n";
        return false;
    }
    if (!checked->update_available) {
        std::cout << trf("cmd.update.current", checked->current_version, checked->latest_version) << "\n";
        return true;
    }

    std::cout << trf("cmd.update.available", checked->current_version, checked->latest_version) << "\n"
              << trf("cmd.update.release", checked->release_url) << "\n"
              << tr("cmd.update.install_hint") << "\n";
    return true;
}

// /skills 命令:列出扫描到的技能;一个都没有时打印两处目录路径,顺带说明
// 怎么造一份(SKILL.md 起手 frontmatter 的最小样例)。
void PrintSkillsCommand(const std::vector<lubancode::tools::SkillMeta>& skills, const std::string& project_dir,
                         const std::optional<std::string>& home_dir) {
    if (skills.empty()) {
        std::cout << trf("cmd.skills.empty", project_dir,
                          home_dir.has_value() ? *home_dir : tr("path.no_home"))
                   << "\n";
        return;
    }
    std::cout << trf("cmd.skills.header", skills.size()) << "\n";
    for (const auto& skill : skills) {
        std::cout << "  - " << skill.name << " [" << skill.source_level << "]: "
                   << (skill.description.empty() ? tr("cmd.skills.no_desc") : skill.description) << "\n";
        std::cout << "      " << skill.dir_path << "\n";
    }
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
        std::cout << tr("cmd.skill.no_home") << "\n";
        return false;
    }
    const auto [verb, value] = SplitSkillCommandArgs(args);
    if (verb == "list") {
        const auto global = lubancode::config::ListStoredSkills(global_skills_root);
        const auto project = lubancode::config::ListStoredSkills(project_skills_root);
        if (!global.has_value()) {
            std::cout << trf("cmd.skill.error", "/skill list", global.error()) << "\n";
            return false;
        }
        if (!project.has_value()) {
            std::cout << trf("cmd.skill.error", "/skill list", project.error()) << "\n";
            return false;
        }
        if (global->empty() && project->empty()) {
            std::cout << tr("cmd.skill.list_empty") << "\n";
            return false;
        }

        std::cout << tr("cmd.skill.list_header") << "\n";
        const auto print_entries = [](const std::vector<lubancode::config::StoredSkill>& entries,
                                      const std::string& scope) {
            for (const auto& skill : entries) {
                const std::string source =
                    skill.source_url.has_value() ? trf("cmd.skill.remote", *skill.source_url,
                                                       skill.installed_at.value_or(std::string()))
                                                 : tr("cmd.skill.local");
                std::cout << "  - " << skill.name << " [" << scope << "; " << source << "]\n"
                          << "      " << skill.dir_path << "\n";
            }
        };
        print_entries(*project, tr("cmd.skill.scope_project"));
        print_entries(*global, tr("cmd.skill.scope_global"));
        return false;
    }
    if (verb == "install") {
        if (value.empty()) {
            std::cout << tr("cmd.skill.usage") << "\n";
            return false;
        }
        const auto installed = lubancode::config::InstallSkillSource(
            global_skills_root, value, lubancode::config::FetchRemoteSkillUrl);
        if (!installed.has_value()) {
            std::cout << trf("cmd.skill.error", "/skill install", installed.error()) << "\n";
            return false;
        }
        std::cout << trf("cmd.skill.install_done", JoinSkillNames(installed->installed_names)) << "\n";
        return true;
    }
    if (verb == "update") {
        const auto records = lubancode::config::LoadRemoteSkillRecords(global_skills_root);
        if (!records.has_value()) {
            std::cout << trf("cmd.skill.error", "/skill update", records.error()) << "\n";
            return false;
        }
        std::vector<lubancode::config::RemoteSkillRecord> chosen;
        for (const auto& record : *records) {
            if (value.empty() || record.name == value) {
                chosen.push_back(record);
            }
        }
        if (chosen.empty()) {
            std::cout << tr("cmd.skill.update_none") << "\n";
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
                std::cout << trf("cmd.skill.error", "/skill update " + record.name, updated.error()) << "\n";
                continue;
            }
            std::cout << trf("cmd.skill.update_done", JoinSkillNames(updated->installed_names)) << "\n";
            changed = true;
        }
        return changed;
    }
    if (verb == "remove") {
        if (value.empty()) {
            std::cout << tr("cmd.skill.usage") << "\n";
            return false;
        }
        const auto removed = lubancode::config::RemoveStoredSkill(global_skills_root, value);
        if (!removed.has_value()) {
            std::cout << trf("cmd.skill.error", "/skill remove", removed.error()) << "\n";
            return false;
        }
        std::cout << trf("cmd.skill.remove_done", value) << "\n";
        return true;
    }
    std::cout << tr("cmd.skill.usage") << "\n";
    return false;
}

// /think(/effort 同义)命令:不带参数看当前档位,带参数切档位(本会话
// 生效)。M10 把档位放开成任意字符串——不在这儿拦,认不认得留给发请求
// 那一刻(responses 原样递,anthropic 查映射表、映射不上打警告)去判断,
// 原样存,不强制转小写(anthropic 那张映射表自己做大小写不敏感匹配,
// responses 要"原样递",这里转了小写反而破坏这条承诺)。
// entry:当前模型在模型目录(models.json)里的条目,没有就是 nullptr。
// 有条目且声明了 supported_think_levels → 裸敲列真实档位带描述,设了表外
// 档位只提示"目录未声明,仍会发送",不拦;没有条目 → 维持现状提示。
void HandleThinkCommand(const std::string& args, const std::shared_ptr<std::string>& current_think,
                         const lubancode::config::ModelCatalogEntry* entry) {
    const std::vector<std::string> hint_lines = lubancode::config::ThinkLevelHintLines(entry);
    if (args.empty()) {
        std::cout << trf("cmd.think.current", current_think->empty() ? tr("config.think.unset") : *current_think)
                  << "\n";
        if (!hint_lines.empty()) {
            std::cout << trf("cmd.think.catalog_header", entry->slug) << "\n";
            for (const auto& line : hint_lines) {
                std::cout << line << "\n";
            }
        } else {
            std::cout << tr("cmd.think.provider") << "\n";
        }
        return;
    }
    *current_think = args;
    std::cout << trf("cmd.think.switched", args);
    if (!hint_lines.empty()) {
        if (!lubancode::config::ThinkLevelDeclared(*entry, args)) {
            std::cout << tr("cmd.think.undeclared");
        }
        std::cout << "\n";
    } else {
        std::cout << tr("cmd.think.provider") << "\n";
    }
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
        std::cout << trf("catalog.apply_think", *apply.think) << "\n";
    }
    if (apply.context_window_tokens.has_value()) {
        context_tracker.set_window_tokens(*apply.context_window_tokens);
        std::cout << trf("catalog.apply_window", *apply.context_window_tokens) << "\n";
    }
    if (*current_model_instructions != apply.base_instructions) {
        *current_model_instructions = apply.base_instructions;
        if (!apply.base_instructions.empty()) {
            std::cout << trf("catalog.apply_instructions", slug) << "\n";
        }
    }
}

// /model 命令的执行逻辑:带参数直接切;不带参数拉列表编号选。切完了,
// 有配置文件才问"写进配置文件?",没有就只提示本会话生效。
// catalog:模型目录——列表里优先显示目录条目的 display_name(其次接口
// 给的 display_name,最后 id 兜底);切换成功后按目录条目应用
// default_think / context_window / base_instructions(见 ApplyModelCatalog)。
void HandleModelCommand(const std::string& args, lubancode::config::Config& config,
                         const std::shared_ptr<std::string>& current_model,
                         std::optional<std::string>& config_file_path,
                         const lubancode::config::ModelCatalog& catalog,
                         const std::shared_ptr<std::string>& current_think,
                         lubancode::cli::ContextTracker& context_tracker,
                         const std::shared_ptr<std::string>& current_model_instructions,
                         bool offer_config_write) {
    std::string chosen;

    if (!args.empty()) {
        chosen = args;
    } else {
        const auto headers = lubancode::config::ResolveProviderHeaderTemplates(config.extra_headers,
                                                                                config.auth_token);
        const auto list_result = lubancode::api::ListModels(
            config.wire, config.base_url, config.auth_token, config.connect_timeout_ms,
            config.request_timeout_secs, headers);
        if (!list_result.has_value()) {
            std::cout << trf("cmd.model.fetch_failed", list_result.error().message) << "\n";
            return;
        }
        if (list_result->empty()) {
            std::cout << tr("cmd.model.list_empty") << "\n";
            return;
        }
        std::size_t default_idx = 0;
        std::vector<lubancode::cli::ChoiceMenuItem> items;
        items.reserve(list_result->size());
        for (std::size_t i = 0; i < list_result->size(); ++i) {
            const auto& m = (*list_result)[i];
            const bool current = m.id == *current_model;
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
                std::cout << tr("cmd.model.cancelled") << "\n";
                return;
            }
            idx = sel->selected_indices.empty() ? default_idx : sel->selected_indices.front();
        } else {
            for (std::size_t i = 0; i < items.size(); ++i) {
                std::cout << "  " << (i + 1) << ") " << items[i].label
                          << (items[i].description.empty() ? "" : "  " + items[i].description) << "\n";
            }
            const std::optional<std::string> selection = lubancode::cli::ReadLine(
                trf("cmd.model.choose", default_idx + 1), {}, /*esc_rejects=*/true);
            if (!selection.has_value()) {
                std::cout << tr("cmd.model.cancelled") << "\n";
                return;
            }
            if (!selection->empty()) {
                try {
                    std::size_t consumed = 0;
                    const int n = std::stoi(*selection, &consumed);
                    if (consumed != selection->size() || n < 1 || static_cast<std::size_t>(n) > list_result->size()) {
                        std::cout << tr("cmd.model.bad_number") << "\n";
                        return;
                    }
                    idx = static_cast<std::size_t>(n - 1);
                } catch (...) {
                    std::cout << tr("cmd.model.not_number") << "\n";
                    return;
                }
            }
        }
        chosen = (*list_result)[idx].id;
    }

    *current_model = chosen;
    config.model = chosen;
    std::cout << trf("cmd.model.switched", chosen) << "\n";

    // 模型目录应用:主动切换,目录声明了就用(两个 explicit 都传 false);
    // 切到目录外的名字时这一步什么都不动(base_instructions 清空),回退现状。
    ApplyModelCatalog(catalog, chosen, /*think_explicit=*/false, /*window_explicit=*/false, current_think,
                       context_tracker, current_model_instructions);

    if (offer_config_write && config_file_path.has_value()) {
        const std::optional<std::string> answer =
            lubancode::cli::ReadLine(trf("cmd.write_config_prompt", *config_file_path));
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateModelInConfigFile(*config_file_path, chosen);
            if (updated.has_value()) {
                std::cout << trf("cmd.write_config.updated", *config_file_path) << "\n";
            } else {
                std::cout << trf("cmd.write_config.failed", updated.error()) << "\n";
            }
        }
    } else if (offer_config_write) {
        std::cout << tr("cmd.session_only") << "\n";
    }
}
void PrintProviderList(const std::vector<lubancode::config::ProviderConfig>& providers,
                       const lubancode::config::Config& current_config,
                       const std::string& active_provider) {
    if (providers.empty()) {
        std::cout << tr("cmd.provider.empty") << "\n";
        return;
    }
    std::cout << tr("cmd.provider.header") << "\n";
    for (const auto& provider : providers) {
        const std::string model = provider.model.empty() ? tr("cmd.provider.model_unset") : provider.model;
        const bool is_current = provider.name == active_provider ||
                                (active_provider.empty() && provider.wire == current_config.wire &&
                                 provider.base_url == current_config.base_url && provider.model == current_config.model);
        const std::string current = is_current ? tr("cmd.provider.current") : "";
        // api_key/model_reasoning_effort 都是可选字段;api_key 展示一律走
        // MaskApiKey 打码,绝不明文——跟 /config、初次配置向导汇总同一个规矩。
        std::string extra;
        if (!provider.api_key.empty()) {
            extra += trf("cmd.provider.extra_api_key", lubancode::config::MaskApiKey(provider.api_key));
        }
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
        std::cout << trf("cmd.provider.line", provider.name, lubancode::config::ProviderWireName(provider.wire),
                          provider.base_url, model, provider.context_window_tokens, provider.key_env, extra, current)
                  << "\n";
    }
}

// /provider add 向导:跟 RunInitialSetupWizard(初次配置向导)同一套 WizardIO
// 建法——接 std::cout / cli::ReadLine / api::ListModels,不碰真实 IO 之外的
// 任何东西。问出来的是一条 ProviderConfig,写盘复用一行式旧用法同一条路径
// (AddProviderToGlobalConfig)。用户中途 EOF、或者最后一问回答 n,都当"整个
// 添加动作被取消"处理:不改 config.providers、不写盘。
void RunProviderAddWizardInteractive(const std::string& name_prefill, lubancode::config::Config& config,
                                     const lubancode::cli::Theme& theme) {
    lubancode::cli::WizardIO io = MakeInteractiveWizardIO(theme);

    if (lubancode::config::ProviderCatalogCacheIsStale()) {
        std::cout << tr("provider_catalog.refreshing") << "\n";
        const auto refreshed = lubancode::config::RefreshProviderCatalog();
        if (!refreshed.has_value()) {
            std::cout << trf("provider_catalog.refresh_failed", refreshed.error()) << "\n";
        }
    }
    const lubancode::config::ProviderCatalog provider_catalog = lubancode::config::LoadProviderCatalog();
    for (const auto& warning : provider_catalog.warnings) {
        std::cout << trf("provider_catalog.warning", warning) << "\n";
    }
    const auto outcome =
        lubancode::cli::RunProviderPresetWizard(io, provider_catalog, name_prefill, config.providers);
    if (!outcome.has_value() || !outcome->save_requested) {
        std::cout << tr("cmd.provider.add_cancelled") << "\n";
        return;
    }

    const auto saved = lubancode::config::AddProviderToGlobalConfig(outcome->provider);
    if (!saved.has_value()) {
        std::cout << trf("cmd.provider.add_failed", saved.error()) << "\n";
        return;
    }
    config.providers.push_back(outcome->provider);
    std::cout << trf("cmd.provider.added", outcome->provider.name, *saved) << "\n";
}

// /provider:添端只写全局配置；项目级若自行写了 providers，加载时仍按既有
// "整段压过"规则优先。切端时换 client、提示词平台段与模型连接，旧历史
// 保留不动；成功后把端名写回配置，下次启动照旧选中。
void HandleProviderCommand(const std::string& args, lubancode::config::Config& config,
                           std::string& active_provider, RebuildableBackend& real_backend,
                           std::string& session_wire,
                           const std::shared_ptr<std::string>& current_model,
                           const std::shared_ptr<std::string>& current_think,
                           lubancode::cli::ContextTracker& context_tracker,
                           const std::shared_ptr<std::string>& current_model_instructions,
                           const lubancode::config::ModelCatalog& catalog,
                           lubancode::agent::PromptOptions& prompt_options,
                           const std::function<void(bool)>& rebuild_loop, bool is_console,
                           const lubancode::cli::Theme& theme,
                           const std::optional<std::string>& active_provider_write_path,
                           lubancode::config::Source& active_provider_source) {
    const lubancode::cli::ParsedProviderCommand command = lubancode::cli::ParseProviderCommand(args);
    switch (command.action) {
        case lubancode::cli::ProviderCommandAction::List:
            PrintProviderList(config.providers, config, active_provider);
            return;
        case lubancode::cli::ProviderCommandAction::Refresh: {
            std::cout << tr("provider_catalog.refreshing") << "\n";
            const auto refreshed = lubancode::config::RefreshProviderCatalog();
            if (!refreshed.has_value()) {
                std::cout << trf("provider_catalog.refresh_failed", refreshed.error()) << "\n";
            } else if (refreshed->not_modified) {
                std::cout << tr("provider_catalog.refresh_current") << "\n";
            } else {
                std::cout << trf("provider_catalog.refresh_ok", refreshed->revision, refreshed->cache_path) << "\n";
            }
            return;
        }
        case lubancode::cli::ProviderCommandAction::Add: {
            if (command.wizard) {
                // 裸敲 /provider add,或者 /provider add <名字>(名字先给上,
                // 跳过向导第一问)——走分步向导,不进下面的一行式解析路径。
                RunProviderAddWizardInteractive(command.name, config, theme);
                return;
            }
            const auto wire = lubancode::config::ParseProviderWire(command.wire);
            if (!wire.has_value()) {
                std::cout << trf("cmd.provider.add_failed", wire.error()) << "\n";
                return;
            }
            std::size_t window = lubancode::config::kDefaultContextWindowTokens;
            if (!command.window.empty()) {
                const auto parsed_window = lubancode::config::ParseContextWindowTokens(command.window);
                if (!parsed_window.has_value()) {
                    std::cout << trf("cmd.provider.add_failed", parsed_window.error()) << "\n";
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
                std::cout << trf("cmd.provider.add_failed", valid.error()) << "\n";
                return;
            }
            if (lubancode::config::FindProvider(config.providers, provider.name) != nullptr) {
                std::cout << trf("cmd.provider.add_failed", trf("cmd.provider.exists", provider.name)) << "\n";
                return;
            }
            const auto saved = lubancode::config::AddProviderToGlobalConfig(provider);
            if (!saved.has_value()) {
                std::cout << trf("cmd.provider.add_failed", saved.error()) << "\n";
                return;
            }
            config.providers.push_back(std::move(provider));
            std::cout << trf("cmd.provider.added", command.name, *saved) << "\n";
            return;
        }
        case lubancode::cli::ProviderCommandAction::Switch: {
            const lubancode::config::ProviderConfig* provider =
                lubancode::config::FindProvider(config.providers, command.name);
            if (provider == nullptr) {
                std::cout << trf("cmd.provider.not_found", command.name) << "\n";
                return;
            }
            const auto api_key = lubancode::config::ProviderApiKey(*provider);
            if (!api_key.has_value()) {
                std::cout << trf("cmd.provider.key_missing", provider->name, provider->key_env) << "\n";
                return;
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

            config.wire = provider->wire;
            config.base_url = provider->base_url;
            config.auth_token = *api_key;
            config.model = command.model.empty() ? provider->model : command.model;
            config.context_window_tokens = provider->context_window_tokens;
            config.native_web_search = provider->native_web_search;
            config.stream_usage = provider->stream_usage;
            config.extra_body = provider->extra_body;
            config.extra_headers = provider->extra_headers;
            *current_model = config.model;
            config.active_provider = provider->name;
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
                std::cout << trf("cmd.provider.effort_applied", provider->name, provider->model_reasoning_effort)
                          << "\n";
            }
            rebuild_loop(/*preserve_history=*/true);
            std::cout << trf("cmd.provider.switched", provider->name, provider->base_url) << "\n";
            if (remembered.has_value()) {
                active_provider_source = active_provider_write_path.has_value()
                                             ? lubancode::config::Source::ProjectConfigFile
                                             : lubancode::config::Source::GlobalConfigFile;
                std::cout << trf("cmd.provider.remembered", provider->name) << "\n";
            } else {
                std::cout << trf("cmd.provider.remember_failed", remembered.error()) << "\n";
            }
            return;
        }
        case lubancode::cli::ProviderCommandAction::Set: {
            // 认三个可设字段:native_web_search(开关)、extra_body(整段
            // JSON,替换语义)、extra_header(单条 HTTP 头,替换/删除)。
            // 字段名不对、值不合法,都跟 Add 分支同一个套路——套 set_failed
            // 报个更具体的原因,不写盘、不改内存。
            if (command.field == "native_web_search") {
                const auto enabled = lubancode::config::ParseBoolToggle(command.value);
                if (!enabled.has_value()) {
                    std::cout << trf("cmd.provider.set_failed", enabled.error()) << "\n";
                    return;
                }
                // 先改内存里这份:SetProviderNativeWebSearch 顺带当"名字存不存在"
                // 的判断——找不到就原样不动、返回 false,不往下走落盘那一步。
                if (!lubancode::config::SetProviderNativeWebSearch(config.providers, command.name, *enabled)) {
                    std::cout << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved =
                    lubancode::config::SetProviderNativeWebSearchInGlobalConfig(command.name, *enabled);
                if (!saved.has_value()) {
                    std::cout << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                std::cout << trf("cmd.provider.set_ok", command.name, command.field, *enabled ? "on" : "off", *saved)
                          << "\n";
                // 改的正好是当前活跃端:顶层镜像字段跟着同步、重建 backend,别让
                // "刚改完当前端却要等下次 /provider switch 才生效"这种反直觉
                // 体验发生——跟 Switch 分支改完就 Rebuild 是同一个道理。
                if (active_provider == command.name) {
                    config.native_web_search = *enabled;
                    real_backend.Rebuild(config);
                    std::cout << trf("cmd.provider.set_active_applied", command.name) << "\n";
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
                        std::cout << trf("cmd.provider.set_failed",
                                          trf("cmd.provider.extra_body_invalid_json", e.what()))
                                  << "\n";
                        return;
                    }
                    if (!candidate.is_object()) {
                        std::cout << trf("cmd.provider.set_failed", tr("cmd.provider.extra_body_not_object")) << "\n";
                        return;
                    }
                    parsed = std::move(candidate);
                }
                if (!lubancode::config::SetProviderExtraBody(config.providers, command.name, parsed)) {
                    std::cout << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved = lubancode::config::SetProviderExtraBodyInGlobalConfig(command.name, parsed);
                if (!saved.has_value()) {
                    std::cout << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                std::cout << trf("cmd.provider.set_ok", command.name, command.field,
                                  parsed.empty() ? tr("provider_wizard.extra_body.unset")
                                                  : trf("provider_wizard.extra_body.summary", parsed.size()),
                                  *saved)
                          << "\n";
                if (active_provider == command.name) {
                    config.extra_body = parsed;
                    real_backend.Rebuild(config);
                    std::cout << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            if (command.field == "extra_header") {
                if (command.header_name.empty()) {
                    std::cout << trf("cmd.provider.set_failed", tr("cmd.provider.extra_header_name_missing")) << "\n";
                    return;
                }
                if (!lubancode::config::SetProviderExtraHeader(config.providers, command.name, command.header_name,
                                                                command.value)) {
                    std::cout << trf("cmd.provider.not_found", command.name) << "\n";
                    return;
                }
                const auto saved = lubancode::config::SetProviderExtraHeaderInGlobalConfig(
                    command.name, command.header_name, command.value);
                if (!saved.has_value()) {
                    std::cout << trf("cmd.provider.set_failed", saved.error()) << "\n";
                    return;
                }
                std::cout << trf("cmd.provider.set_ok", command.name, command.header_name,
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
                    std::cout << trf("cmd.provider.set_active_applied", command.name) << "\n";
                }
                return;
            }
            std::cout << trf("cmd.provider.set_failed", trf("cmd.provider.set_unknown_field", command.field))
                      << "\n";
            return;
        }
        case lubancode::cli::ProviderCommandAction::Remove:
            if (!lubancode::cli::CanRemoveProvider(active_provider, command.name)) {
                std::cout << trf("cmd.provider.remove_active", command.name) << "\n";
                return;
            }
            if (lubancode::config::FindProvider(config.providers, command.name) == nullptr) {
                std::cout << trf("cmd.provider.not_found", command.name) << "\n";
                return;
            }
            if (const auto removed = lubancode::config::RemoveProviderFromGlobalConfig(command.name);
                removed.has_value()) {
                config.providers.erase(std::remove_if(config.providers.begin(), config.providers.end(),
                                                      [&](const lubancode::config::ProviderConfig& provider) {
                                                          return provider.name == command.name;
                                                      }),
                                       config.providers.end());
                std::cout << trf("cmd.provider.removed", command.name, *removed) << "\n";
            } else {
                std::cout << trf("cmd.provider.remove_failed", removed.error()) << "\n";
            }
            return;
        case lubancode::cli::ProviderCommandAction::Invalid:
            std::cout << tr("cmd.provider.usage") << "\n";
            return;
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
            std::cout << trf("cmd.language.unknown", args) << "\n";
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
        std::cout << tr("cmd.language.list_header") << "\n";
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
                std::cout << "  " << (i + 1) << ") " << items[i].label
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
                        std::cout << tr("cmd.language.bad_number") << "\n";
                        return;
                    }
                    idx = static_cast<std::size_t>(n - 1);
                } catch (...) {
                    std::cout << tr("cmd.language.bad_number") << "\n";
                    return;
                }
            }
        }
        chosen = langs[idx];
    }

    cli::SetLanguage(chosen);
    std::cout << trf("cmd.language.switched", cli::LanguageDisplayName(chosen)) << "\n";

    if (config_file_path.has_value()) {
        const std::optional<std::string> answer =
            cli::ReadLine(trf("cmd.write_config_prompt", *config_file_path));
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateLanguageInConfigFile(*config_file_path, chosen);
            if (updated.has_value()) {
                std::cout << trf("cmd.write_config.updated", *config_file_path) << "\n";
            } else {
                std::cout << trf("cmd.write_config.failed", updated.error()) << "\n";
            }
        }
    } else {
        std::cout << tr("cmd.session_only") << "\n";
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

    std::cout << tr("config.header") << "\n\n";
    std::cout << "  wire               = " << wire_str << "  [" << lubancode::config::ToString(sources.wire) << "]\n";
    std::cout << "  base_url           = " << (config.base_url.empty() ? tr("config.not_set") : config.base_url)
              << "  [" << lubancode::config::ToString(sources.base_url) << "]\n";
    std::cout << "  api_key            = " << lubancode::config::MaskApiKey(config.auth_token) << "  ["
              << lubancode::config::ToString(sources.auth_token) << "]\n";
    std::cout << "  model              = " << (config.model.empty() ? tr("config.not_set") : config.model) << "  ["
              << lubancode::config::ToString(sources.model) << "]\n";
    std::cout << "  active_provider    = "
              << (config.active_provider.empty() ? tr("config.not_set") : config.active_provider) << "  ["
              << lubancode::config::ToString(sources.active_provider) << "]\n";
    std::cout << "  max_context_chars  = " << config.max_context_chars << "  ["
              << lubancode::config::ToString(sources.max_context_chars) << "]\n";
    std::cout << "  max_steps_per_turn = " << config.max_steps_per_turn
              << (config.max_steps_per_turn == 0 ? tr("config.steps.unlimited") : "") << "  ["
              << lubancode::config::ToString(sources.max_steps_per_turn) << "]\n";
    std::cout << "  theme              = " << config.theme << "  [" << lubancode::config::ToString(sources.theme)
              << "]\n";
    std::cout << "  status_panel       = ";
    for (std::size_t i = 0; i < config.status_panel.items.size(); ++i) {
        if (i > 0) {
            std::cout << ",";
        }
        std::cout << config.status_panel.items[i];
    }
    std::cout << "  [" << lubancode::config::ToString(sources.status_panel) << "]\n";
    // i18n:language 空 = 跟系统,顺带亮出此刻实际生效的语言码。
    std::cout << "  language           = "
              << (config.language.empty() ? trf("config.language.follow_system", lubancode::cli::CurrentLanguage())
                                           : config.language)
              << "  [" << lubancode::config::ToString(sources.language) << "]\n";
    std::cout << "  system_prompt_file = "
              << (config.system_prompt_file.empty() ? tr("config.not_set") : config.system_prompt_file) << "  ["
              << lubancode::config::ToString(sources.system_prompt_file) << "]\n";
    std::cout << "  context_window     = " << config.context_window_tokens << " tokens  ["
              << lubancode::config::ToString(sources.context_window_tokens) << "]\n";
    std::cout << "  compact_model      = "
              << (config.compact_model.empty() ? tr("config.compact_model.unset") : config.compact_model) << "  ["
              << lubancode::config::ToString(sources.compact_model) << "]\n";
    std::cout << "  think              = " << (config.think.empty() ? tr("config.think.unset") : config.think)
              << "  [" << lubancode::config::ToString(sources.think) << "]\n";
    std::cout << "  soul               = " << (config.soul.empty() ? tr("config.soul.unset") : config.soul)
              << "  [" << lubancode::config::ToString(sources.soul) << "]\n";
    std::cout << "  tool_search_threshold = " << config.tool_search_threshold
              << (config.tool_search_threshold == 0 ? tr("config.threshold.never") : "") << "  ["
              << lubancode::config::ToString(sources.tool_search_threshold) << "]\n";
    std::cout << "  memory            = " << (config.memory.enabled ? "on" : "off")
              << " (use=" << (config.memory.use ? "on" : "off")
              << ", generate=" << (config.memory.generate ? "on" : "off") << ")  ["
              << lubancode::config::ToString(sources.memory) << "]\n";
    // 分层:项目级、全局各自的配置文件路径分开列(有哪个列哪个,标清是
    // 哪一级),都没有就沿用老的单行 config.label.file(通常也不会走到)。
    if (result.project_config_file_path.has_value() || result.global_config_file_path.has_value()) {
        if (result.project_config_file_path.has_value()) {
            std::cout << "  项目级配置       = " << *result.project_config_file_path << "\n";
        }
        if (result.global_config_file_path.has_value()) {
            std::cout << "  全局配置         = " << *result.global_config_file_path << "\n";
        }
    } else if (result.config_file_path.has_value()) {
        std::cout << trf("config.label.file", *result.config_file_path) << "\n";
    }
    // M9:hooks 只从配置文件来,没有来源分级可打,只打个数——四类都是空的
    // 就直接说"未配置",省得打一堆 ×0。
    {
        const auto& hooks = config.hooks;
        std::vector<std::string> parts;
        if (!hooks.pre_tool.empty()) {
            parts.push_back("pre_tool×" + std::to_string(hooks.pre_tool.size()));
        }
        if (!hooks.post_tool.empty()) {
            parts.push_back("post_tool×" + std::to_string(hooks.post_tool.size()));
        }
        if (!hooks.session_start.empty()) {
            parts.push_back("session_start×" + std::to_string(hooks.session_start.size()));
        }
        if (!hooks.session_end.empty()) {
            parts.push_back("session_end×" + std::to_string(hooks.session_end.size()));
        }
        std::cout << "  hooks              = ";
        if (parts.empty()) {
            std::cout << tr("config.hooks.none");
        } else {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) {
                    std::cout << ", ";
                }
                std::cout << parts[i];
            }
        }
        std::cout << "\n";
    }
    // M8:mcpServers 同样只从配置文件来,没有分级来源可打,只打个数——
    // /config 只报"配置了几个",实际存活状态得看 /mcp(那个才知道哪个真的
    // 起来了、握手成没成功)。
    {
        std::cout << "  mcpServers         = ";
        if (config.mcp_servers.empty()) {
            std::cout << tr("config.hooks.none");
        } else {
            std::cout << trf("config.mcp.count", config.mcp_servers.size());
        }
        std::cout << "\n";
    }
    // websearch:search 段同样只从配置文件来。api_key 照例打码,provider
    // 直接亮出来——配了这一段 web_search 工具才会注册。
    {
        std::cout << "  search             = ";
        if (!config.search.Configured()) {
            std::cout << tr("config.search.none");
        } else {
            std::cout << config.search.provider
                      << " (api_key " << lubancode::config::MaskApiKey(config.search.api_key) << ")";
        }
        std::cout << "\n";
    }
    // permissions(settings.local.json):项目级本地权限摘要——allow_tools
    // 几个、allow/deny_commands 几条、起手确认档。没有这份文件就说"未配置"。
    {
        std::cout << "  permissions        = ";
        if (settings == nullptr || settings->Empty()) {
            std::cout << tr("config.hooks.none");
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
                    std::cout << ", ";
                }
                std::cout << parts[i];
            }
        }
        std::cout << "\n";
    }
    // 模型目录(models.json):路径 + 条目数,以及当前模型命没命中。
    if (catalog != nullptr) {
        std::cout << tr("config.label.catalog");
        if (catalog->source_path.empty()) {
            const auto expected = lubancode::config::ModelCatalogPath();
            std::cout << trf("config.catalog.none",
                              expected.has_value() ? *expected
                                                    : tr("path.no_home") + "/.lubancode/models.json");
        } else {
            std::cout << trf("config.catalog.entries", catalog->source_path, catalog->models.size());
        }
        std::cout << "\n";
        const std::string& current = session_model.has_value() ? *session_model : config.model;
        const auto* entry = catalog->FindBySlug(current);
        std::cout << tr("config.label.catalog_hit");
        if (current.empty()) {
            std::cout << tr("config.catalog.model_unset");
        } else if (entry != nullptr) {
            std::cout << trf("config.catalog.hit",
                              entry->slug + (entry->display_name.empty()
                                                  ? std::string()
                                                  : trf("config.catalog.display_name", entry->display_name)));
        } else {
            std::cout << trf("config.catalog.miss", current);
        }
        std::cout << "\n";
    }
    if (session_model.has_value()) {
        std::cout << "\n" << trf("config.session_model", *session_model);
        if (*session_model != config.model) {
            std::cout << tr("config.session_model.note");
        }
        std::cout << "\n";
    }
}

}  // namespace lubancode::app
