// prompt_commands.hpp 的实现:魂/法/提示词命令的函数体。
#include "app/commands/prompt_commands.hpp"

#include <iostream>

#include "agent/prompts.hpp"
#include "config/config.hpp"

#include <memory>
#include <optional>
#include <string>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "config/prompt_files.hpp"

namespace lubancode::app {


using lubancode::cli::tr;
using lubancode::cli::trf;

// ---------------------------------------------------------------------------
// 魂法分家(0.16.x):/soul //prompt 的执行逻辑。纯拼接/剥注释在
// agent/prompts.hpp,文件生成/还原/扫描在 config/prompt_files,这里只做
// "接命令、找文件、打印、问一句"这层壳。
// ---------------------------------------------------------------------------

// 数一段 UTF-8 文本有多少个字符(码点)——/prompt 报"字数"用,字节数对
// 中文没意义。
std::size_t CountUtf8Chars(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// 按魂名读内容(原始全文,注释留给注入时剥):"off" -> 空;空串/"default"
// -> SOUL.md;别的名字 -> souls/<名字>.md。默认 SOUL.md 走专用读口:缺失、
// 打不开或 UTF-8 已坏都降成空魂,不拦启动。warn 为真时打一行说明。启动读
// 一次、/soul 切换即时重读,都走这一个函数。
std::string LoadSoulContentByName(const std::string& name, bool warn) {
    if (name == "off") {
        return std::string();
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        return std::string();
    }
    const bool default_soul = name.empty() || name == "default";
    const std::string path = default_soul ? lubancode::config::SoulFilePath(*luban_dir)
                                          : lubancode::config::SoulPathByName(*luban_dir, name);
    const auto content = default_soul ? lubancode::config::ReadSoulFile(*luban_dir)
                                      : lubancode::config::ReadTextFileIfExists(path);
    if (!content.has_value()) {
        if (warn) {
            std::cout << trf("soul.unavailable", path) << "\n";
        }
        return std::string();
    }
    return *content;
}

// /soul 命令:裸敲看当前正文和可选旧魂;/soul clear 把 SOUL.md 还原成
// 默认空魂;/soul <内容> 直接写 SOUL.md、立刻生效,下回启动也会读回来。
// 兼容旧用法:参数恰好命中 souls/<名字>.md 时仍是选魂。off/default/
// <名字> 三条路都当场生效,并在有配置文件时问一句要不要持久化——答 y
// 才落盘,免得下次启动被配置里的旧值悄悄盖过去(或者悄悄留着没改)。
// clear 语义不同,是把 SOUL.md 本身还原成空魂,所以自动把配置里的选魂
// 项归位 default,不用问。
void HandleSoulCommand(const std::string& args, const std::shared_ptr<std::string>& current_soul,
                        std::string& current_soul_name, const std::optional<std::string>& config_file_path) {
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        std::cout << tr("cmd.soul.no_home") << "\n";
        return;
    }

    if (args.empty()) {
        const std::vector<std::string> souls = lubancode::config::ListSouls(*luban_dir);
        std::cout << trf("cmd.soul.current", current_soul_name) << "\n";
        const std::string visible = lubancode::agent::StripPromptComments(*current_soul);
        if (visible.empty()) {
            std::cout << tr("cmd.soul.empty_note") << "\n";
        } else {
            std::cout << visible << "\n";
        }
        std::cout << tr("cmd.soul.available_header") << "\n";
        std::cout << tr("cmd.soul.default_item") << "\n";
        for (const auto& name : souls) {
            std::cout << "  - " << name << "\n";
        }
        std::cout << "\n" << tr("cmd.soul.usage") << "\n";
        return;
    }

    if (args == "off") {
        current_soul->clear();
        current_soul_name = "off";
        std::cout << tr("cmd.soul.off") << "\n" << tr("cmd.soul.switch_hint") << "\n";

        // 跟 /soul <名字> 一路的持久化问法对齐:配置里原先若存着旧魂名,
        // 不问清楚就不动它,免得下次启动又被旧值盖过去。
        if (config_file_path.has_value()) {
            const std::optional<std::string> answer = lubancode::cli::ReadLine(tr("cmd.soul.write_prompt"));
            if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
                const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, "off");
                if (updated.has_value()) {
                    std::cout << trf("cmd.write_config.updated", *config_file_path) << "\n";
                } else {
                    std::cout << trf("cmd.write_config.failed", updated.error()) << "\n";
                }
            }
        } else {
            std::cout << tr("cmd.session_only") << "\n";
        }
        return;
    }

    if (args == "default") {
        *current_soul = LoadSoulContentByName("default", /*warn=*/true);
        current_soul_name = "default";
        std::cout << tr("cmd.soul.back_default");
        if (lubancode::agent::StripPromptComments(*current_soul).empty()) {
            std::cout << tr("cmd.soul.empty_note");
        }
        std::cout << "。\n" << tr("cmd.soul.switch_hint") << "\n";

        // 同上:配置里原先若存着旧魂名,问清楚了才改,不然下次启动照旧
        // 被旧值盖过去(这就是本函数要修的那个 bug)。
        if (config_file_path.has_value()) {
            const std::optional<std::string> answer = lubancode::cli::ReadLine(tr("cmd.soul.write_prompt"));
            if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
                const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, "default");
                if (updated.has_value()) {
                    std::cout << trf("cmd.write_config.updated", *config_file_path) << "\n";
                } else {
                    std::cout << trf("cmd.write_config.failed", updated.error()) << "\n";
                }
            }
        } else {
            std::cout << tr("cmd.session_only") << "\n";
        }
        return;
    }

    if (args == "clear") {
        const auto cleared = lubancode::config::ClearSoulFile(*luban_dir);
        if (!cleared.has_value()) {
            std::cout << trf("cmd.soul.write_failed", cleared.error()) << "\n";
            return;
        }
        *current_soul = lubancode::config::DefaultSoulFileContent();
        current_soul_name = "default";
        if (config_file_path.has_value()) {
            const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, "default");
            if (!updated.has_value()) {
                std::cout << trf("cmd.soul.default_config_failed", updated.error()) << "\n";
            }
        }
        std::cout << tr("cmd.soul.cleared") << "\n" << tr("cmd.soul.switch_hint") << "\n";
        return;
    }

    const std::string path = lubancode::config::SoulPathByName(*luban_dir, args);
    const auto content = lubancode::config::ReadTextFileIfExists(path);
    if (content.has_value()) {
        *current_soul = *content;
        current_soul_name = args;
        std::cout << trf("cmd.soul.switched", args) << "\n" << tr("cmd.soul.switch_hint") << "\n";

        if (config_file_path.has_value()) {
            const std::optional<std::string> answer = lubancode::cli::ReadLine(tr("cmd.soul.write_prompt"));
            if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
                const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, args);
                if (updated.has_value()) {
                    std::cout << trf("cmd.write_config.updated", *config_file_path) << "\n";
                } else {
                    std::cout << trf("cmd.write_config.failed", updated.error()) << "\n";
                }
            }
        } else {
            std::cout << tr("cmd.session_only") << "\n";
        }
        return;
    }

    const auto written = lubancode::config::WriteSoulFile(*luban_dir, args);
    if (!written.has_value()) {
        std::cout << trf("cmd.soul.write_failed", written.error()) << "\n";
        return;
    }
    *current_soul = args;
    current_soul_name = "default";
    if (config_file_path.has_value()) {
        const auto updated = lubancode::config::UpdateSoulInConfigFile(*config_file_path, "default");
        if (!updated.has_value()) {
            std::cout << trf("cmd.soul.default_config_failed", updated.error()) << "\n";
        }
    }
    std::cout << tr("cmd.soul.saved") << "\n" << tr("cmd.soul.switch_hint") << "\n";
}

// /prompt 命令:裸敲显示当前法(人格段)的来源和字数,外加各提示词模块
// 的来源统计(用户文件/内置,0.21.x 运行时化);/prompt reset 带二次确认,
// 把 system_prompt.md 还原成内置默认(旧文件挪成 .bak)。
// persona 是本会话实际在用的人格段(空串 = 内置默认);law_source 是启动时
// 算好的来源说明(CLI 参数/文件/内置);prompts_dir 是用户模块目录
// (~/.lubancode/prompts,找不到主目录时空串)。
void HandlePromptCommand(const std::string& args, const std::string& law_source, const std::string& persona,
                          const std::string& prompts_dir) {
    if (args.empty()) {
        const std::string effective =
            persona.empty() ? lubancode::agent::AssembledCorePersona(prompts_dir) : persona;
        std::cout << trf("cmd.prompt.info", law_source, CountUtf8Chars(effective)) << "\n";
        if (!prompts_dir.empty()) {
            const auto sources = lubancode::agent::PromptModuleSources(prompts_dir);
            std::size_t modified_count = 0;
            for (const auto& source : sources) {
                if (source.from_user_file && source.differs_from_embedded) {
                    ++modified_count;
                }
            }
            std::cout << trf("cmd.prompt.modules_header", prompts_dir, modified_count, sources.size()) << "\n";
            for (const auto& source : sources) {
                const char* tag = !source.from_user_file          ? "cmd.prompt.module_builtin"
                                  : source.differs_from_embedded ? "cmd.prompt.module_user_modified"
                                                                  : "cmd.prompt.module_user_same";
                std::cout << "  - " << source.rel_path << "  [" << tr(tag) << "]\n";
            }
        }
        return;
    }
    if (args != "reset") {
        std::cout << tr("cmd.prompt.usage") << "\n";
        return;
    }

    const std::optional<std::string> answer = lubancode::cli::ReadLine(tr("cmd.prompt.confirm"));
    if (!answer.has_value() || (*answer != "y" && *answer != "Y")) {
        std::cout << tr("cmd.prompt.cancelled") << "\n";
        return;
    }
    const auto luban_dir = lubancode::config::HomeLubancodeDir();
    if (!luban_dir.has_value()) {
        std::cout << tr("cmd.prompt.no_home") << "\n";
        return;
    }
    const auto reset_result =
        lubancode::config::ResetSystemPromptFile(*luban_dir, lubancode::agent::DefaultPersona());
    if (!reset_result.has_value()) {
        std::cout << trf("cmd.prompt.reset_failed", reset_result.error()) << "\n";
        return;
    }
    std::cout << trf("cmd.prompt.reset_done", lubancode::config::SystemPromptFilePath(*luban_dir));
    if (!reset_result->empty()) {
        std::cout << trf("cmd.prompt.old_file", *reset_result);
    }
    std::cout << "。\n" << tr("cmd.prompt.reset_tail") << "\n";
}

}  // namespace lubancode::app
