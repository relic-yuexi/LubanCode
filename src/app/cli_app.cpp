// cli_app.hpp 的实现:PrintVersion/PrintHelp/向导/SessionHookScope/RunCli
// 的函数体,原样搬自原头文件,行为一字未改。

#include "app/cli_options.hpp"
#include "app/interactive_session.hpp"
#include "app/one_shot.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/peer_session.hpp"
#include "agent/prompts.hpp"
#include "agent/session_store.hpp"
#include "agent/workflow_recorder.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/version.hpp"
#include "cli/agent_status.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"
#include "cli/live_transcript.hpp"
#include "cli/worktree.hpp"
#include "cli/markdown.hpp"
#include "cli/provider_wizard.hpp"
#include "cli/record_command.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/spinner.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/todo_render.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/prompt_files.hpp"
#include "config/project_instructions.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/command_safety.hpp"
#include "tools/edit_file.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::app::kVersion;
using lubancode::platform::CurrentDirUtf8;
using lubancode::app::PromptAskUser;
using lubancode::app::TrimAscii;
using lubancode::app::PrintSessionsCommand;
using lubancode::app::PromptResumeTarget;
using lubancode::app::ResumeSession;
using lubancode::app::HandleExportCommand;
using lubancode::app::EstimateHistoryChars;
using lubancode::app::EstimateTokens;
using lubancode::app::HandleContextCommand;
using lubancode::app::HandleCompactCommand;
using lubancode::app::LoadSoulContentByName;
using lubancode::app::HandleSoulCommand;
using lubancode::app::HandlePromptCommand;
using lubancode::app::PrintConfigDiagnostics;
using lubancode::app::HandleUpdateCommand;
using lubancode::app::PrintSkillsCommand;
using lubancode::app::JoinSkillNames;
using lubancode::app::HandleSkillCommand;
using lubancode::app::HandleThinkCommand;
using lubancode::app::ApplyModelCatalog;
using lubancode::app::HandleModelCommand;
using lubancode::app::PrintProviderList;
using lubancode::app::RunProviderAddWizardInteractive;
using lubancode::app::HandleProviderCommand;
using lubancode::app::HandleLanguageCommand;
using lubancode::app::MakeInteractiveWizardIO;
using lubancode::app::PrintBanner;
using lubancode::app::PrintLubanIcon;
using lubancode::app::PrintToolsCommand;
using lubancode::app::PrintWorktreeResult;
using lubancode::app::PrintPluginsCommand;
using lubancode::app::PrintMcpCommand;
using lubancode::app::PrintLspCommand;
using lubancode::app::PathToUtf8;
using lubancode::app::SameFilesystemPath;
using lubancode::app::ClearAndPrintBanner;
using lubancode::app::RunTurn;
using lubancode::app::RunTurnResult;
using lubancode::app::MemoryOptionsFromConfig;
using lubancode::cli::tr;
using lubancode::cli::trf;
using lubancode::app::BuildBaseToolRegistry;
using lubancode::app::BuildExploreToolRegistry;
using lubancode::app::McpServerRuntime;
using lubancode::app::StartMcpServers;
using lubancode::app::RegisterMcpTools;
using lubancode::app::PluginMountInfo;
using lubancode::app::MountPlugins;
using lubancode::app::BuildBackend;
using lubancode::app::RebuildableBackend;
using lubancode::app::ModelOverrideBackend;
using lubancode::app::ThinkOverrideBackend;
using lubancode::app::ModelInstructionsBackend;
using lubancode::app::SoulOverlayBackend;
using lubancode::app::DeferredIndexBackend;
using lubancode::app::SpinnerBackend;
using lubancode::cli::AgentStatusPainter;
using lubancode::cli::StreamBodyTracker;
using lubancode::cli::ToolDisplay;

using lubancode::cli::StreamBodyTracker;
using lubancode::cli::ToolDisplay;

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

// i18n:帮助文本按节进表(help.title/usage/options/scaffold/slash/config),
// 版本号、三个内置默认值走占位符。zh-CN 表的值与旧字面文案一致。
void PrintHelp() {
    std::cout << trf("help.title", kVersion) << "\n\n"
              << tr("help.usage") << "\n"
              << tr("help.options") << "\n"
              << tr("help.scaffold") << "\n"
              << tr("help.slash") << "\n"
              << trf("help.config", lubancode::config::kDefaultMaxContextChars,
                      lubancode::config::kDefaultTheme, lubancode::config::kDefaultContextWindowTokens);
}


// /tools 命令:列工具三态——核心(恒在)/已加载的延迟工具/延迟未加载,





// 初次配置向导:接 cli::ReadLine 做输入、std::cout 做输出、api::ListModels
// 做模型列表拉取。用户中途 EOF(Ctrl+Z / 管道读尽)放弃时返回 std::nullopt。
// 用户选择保存时,把保存后的路径写进 out_config_file_path,好让接下来的
// /model 命令知道"有配置文件"。

std::optional<lubancode::config::Config> RunInitialSetupWizard(std::optional<std::string>& out_config_file_path,
                                                                const lubancode::cli::Theme& theme) {
    lubancode::cli::WizardIO io = MakeInteractiveWizardIO(theme);

    const auto home_lubancode_dir = lubancode::config::HomeLubancodeDir();
    io.home_config_display_path =
        (home_lubancode_dir.has_value() ? *home_lubancode_dir : tr("path.no_home") + "/.lubancode") +
        "/config.json";

    const auto outcome = lubancode::cli::RunSetupWizard(io);
    if (!outcome.has_value()) {
        return std::nullopt;
    }

    if (outcome->save_requested) {
        const auto saved = lubancode::config::SaveConfigFile(outcome->config);
        if (saved.has_value()) {
            std::cout << trf("wizard.saved", *saved) << "\n";
            out_config_file_path = *saved;
        } else {
            std::cout << trf("wizard.save_failed", saved.error()) << "\n";
        }
    }
    return outcome->config;
}

// M9:session_start/session_end 钩子的生命周期跟"这一次 CLI 进程真正进入了
// 一次会话"绑在一起——构造时跑 session_start,析构时跑 session_end。只在
// RunCli 真正要进 AskOnce/InteractiveLoop 那条路时构造(--config/--version/
// --help 这些提前 return 的路径不会走到这里,不该触发)。用的是最初
// LoadFromEnv() 读出来的那份 hooks 配置,不是初次配置向导之后的
// "effective"副本——向导只关心 wire/base_url/api_key/model 四个字段,压根
// 不知道 hooks 这回事,拿它的副本反而会把用户配置文件里写的 hooks 弄丢。
class SessionHookScope {
public:
    explicit SessionHookScope(const lubancode::config::HooksConfig& hooks) : hooks_(hooks) {
        lubancode::tools::RunSessionStartHooks(hooks_);
    }
    ~SessionHookScope() { lubancode::tools::RunSessionEndHooks(hooks_); }

    SessionHookScope(const SessionHookScope&) = delete;
    SessionHookScope& operator=(const SessionHookScope&) = delete;

private:
    lubancode::config::HooksConfig hooks_;
};

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见文件末尾的 wmain),是为了绕开 Windows
// 那套"窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args) {
    if (args.size() == 3 && args[1] == "--memory-worker") {
        const auto result = lubancode::memory::RunPendingMemoryJobs(
            lubancode::tools::Utf8ToPath(args[2]));
        if (!result.has_value()) {
            std::cerr << "memory worker: " << result.error() << "\n";
            return 1;
        }
        return 0;
    }

    // i18n 早初始化:--help/--version 在读配置之前就要打印,先扫语言包、按
    // LUBANCODE_LANG(空 = 系统探测)定一版语言;配置加载成功后按四级合并的
    // language 字段再定一次(env 仍是最高级,两次结果一致;差别只在"语言写
    // 在配置文件里、又用 --help"这一种组合——那时 --help 按 env/系统走,
    // 属于诚实的取舍,不为它提前解析整份配置)。坏语言包的警告攒着,等语言
    // 定下来再打(警告本身也要按所选语言出)。
    std::vector<std::string> language_pack_warnings;
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        language_pack_warnings = lubancode::cli::LoadLanguagePacksFromDir(*luban_dir + "/languages");
    }
    {
        const std::string early_lang = lubancode::platform::GetEnvVar("LUBANCODE_LANG").value_or(std::string());
        lubancode::cli::SetLanguage(early_lang.empty() ? lubancode::cli::DetectSystemLanguage() : early_lang);
    }

    // 参数解析走纯函数(cli_options):这里只兑现早退动作,再按解析结果
    // 继续启动。次序与旧的内联扫描一致——动作在 i18n 早初始化之后、配置
    // 加载之前当场退出。
    const ParsedCliArgs parsed_cli = ParseCliArgs(args);
    switch (parsed_cli.action) {
        case CliAction::PrintVersion:
            PrintVersion();
            return 0;
        case CliAction::PrintHelp:
            PrintHelp();
            return 0;
        case CliAction::CheckUpdate:
            return HandleUpdateCommand(std::string(), lubancode::config::kDefaultConnectTimeoutMs,
                                       lubancode::config::kDefaultRequestTimeoutSecs)
                       ? 0
                       : 1;
        case CliAction::MissingSystemPromptValue:
            std::cerr << tr("error.system_prompt_arg") << "\n";
            return 1;
        case CliAction::ResetSystemPrompt: {
            // 跟 /prompt reset 同效,只是不进交互、不二次确认(命令行参数
            // 本身就是明确意图),打结果就退。
            const auto luban_dir = lubancode::config::HomeLubancodeDir();
            if (!luban_dir.has_value()) {
                std::cerr << tr("resetprompt.no_home") << "\n";
                return 1;
            }
            const auto reset_result =
                lubancode::config::ResetSystemPromptFile(*luban_dir, lubancode::agent::DefaultPersona());
            if (!reset_result.has_value()) {
                std::cerr << trf("cmd.prompt.reset_failed", reset_result.error()) << "\n";
                return 1;
            }
            std::cout << trf("cmd.prompt.reset_done", lubancode::config::SystemPromptFilePath(*luban_dir));
            if (!reset_result->empty()) {
                std::cout << trf("cmd.prompt.old_file", *reset_result);
            }
            std::cout << "。\n";
            return 0;
        }
        case CliAction::Proceed:
            break;
    }
    const CliOptions& cli_options = parsed_cli.options;

    const auto config_result = lubancode::config::LoadFromEnv();
    if (!config_result.has_value()) {
        std::cerr << config_result.error() << "\n";
        return 1;
    }
    if (config_result->migration_notice.has_value()) {
        std::cout << *config_result->migration_notice << "\n";
    }

    // i18n:配置读出来了,按四级合并的 language 字段定稿(空 = 跟系统)。
    // 语言包早在函数开头扫过,这里只是切码;坏包警告攒到现在,按定稿语言打。
    lubancode::cli::SetLanguage(config_result->config.language.empty()
                                     ? lubancode::cli::DetectSystemLanguage()
                                     : config_result->config.language);
    for (const auto& warning : language_pack_warnings) {
        std::cout << trf("i18n.pack_warning", warning) << "\n";
    }

    // 魂法分家(0.16.x)+ 提示词运行时化(0.21.x):每次启动查漏补缺——
    // ~/.lubancode/ 下的 system_prompt.md(法)/ SOUL.md(魂)/ souls/
    // wenyan.md(示例)/ prompts/{core,features,platforms}/*.md(运行时
    // 模块,内容播种自嵌入版)缺哪样补哪样,已存在的绝不覆盖。官方 skills
    // 从发行包资源目录直接读取,不再往主目录播种。静默做,不打
    // 输出(单发/管道模式的输出常被重定向,不该混进这些话)。
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        lubancode::config::EnsurePromptScaffold(*luban_dir, lubancode::agent::DefaultPersona(),
                                                 lubancode::agent::PromptModuleSeeds());
    }

    // 模型目录(models.json):启动时读一次,坏 JSON/坏条目只打警告跳过,
    // 文件不存在就是空目录,一切回退现状,绝不拦人。
    const lubancode::config::ModelCatalog model_catalog = lubancode::config::LoadModelCatalog();
    for (const auto& warning : model_catalog.warnings) {
        std::cout << trf("catalog.warning", warning) << "\n";
    }

    // settings.local.json(项目级本地权限):启动读一次,坏 JSON 只告警跳过
    // (当没配置),不拦人。allow_tools/allow_commands/deny_commands/
    // default_confirm_mode 之后各处应用。cwd 基准。
    lubancode::config::SettingsLocal settings_local;
    if (auto loaded = lubancode::config::LoadSettingsLocal(CurrentDirUtf8()); loaded.has_value()) {
        if (loaded->has_value()) {
            settings_local = **loaded;
        }
    } else {
        std::cout << trf("settings.local.warning", loaded.error()) << "\n";
    }

    if (cli_options.print_config) {
        PrintConfigDiagnostics(*config_result, std::nullopt, &model_catalog, &settings_local);
        return 0;
    }

    // --system-prompt 命令行参数压过配置文件里的 system_prompt_file 字段
    // (四级合并已经把 config.system_prompt_file 算好了,这里只是命令行
    // 再压一级)。只替换人格段,工作目录、工具调用这些运行必需的上下文
    // (prompts.hpp 的 EnvironmentSegment)照样由 BuildSystemPrompt 追加,
    // 不受这里影响。
    const std::string effective_prompt_file =
        !cli_options.system_prompt_file_arg.empty() ? cli_options.system_prompt_file_arg
                                                             : config_result->config.system_prompt_file;
    std::string persona;
    std::string law_source = tr("law.builtin");  // /prompt 裸敲展示用
    if (!effective_prompt_file.empty()) {
        const auto persona_result = lubancode::config::ReadSystemPromptFile(effective_prompt_file);
        if (!persona_result.has_value()) {
            std::cerr << persona_result.error() << "\n";
            return 1;
        }
        persona = *persona_result;
        law_source = !cli_options.system_prompt_file_arg.empty() ? trf("law.cli_arg", effective_prompt_file)
                                                      : trf("law.config_file", effective_prompt_file);
    } else if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        // 魂法分家:没有 CLI/配置指定的人格文件时,法从 ~/.lubancode/
        // system_prompt.md 来(顶部注释剥掉;文件缺失或剥完全空白,persona
        // 留空串,BuildSystemPrompt 自回退内置默认)。
        const std::string law_path = lubancode::config::SystemPromptFilePath(*luban_dir);
        const auto law_content = lubancode::config::ReadTextFileIfExists(law_path);
        persona = lubancode::agent::ResolvePersona(std::string(), law_content.value_or(std::string()));
        if (!persona.empty()) {
            // 提示词运行时化(0.21.x):播种的法文件本来就是内置默认人格的
            // 原样副本——CRLF 归一后跟嵌入默认逐字节相同,就当"用户没改过
            // 法",人格留空,core 走运行时模块(prompts/core/*.md 用户文件
            // 优先);真被用户改出内容差异,才整段替换 core,语义不变。
            std::string normalized;
            normalized.reserve(persona.size());
            for (const char c : persona) {
                if (c != '\r') {
                    normalized += c;
                }
            }
            if (normalized == lubancode::agent::DefaultPersona()) {
                persona.clear();
            } else {
                law_source = trf("law.file", law_path);
            }
        }
    }

    // 主题、转轮开关这两样跟"配没配好模型"无关,不管走哪条路都先算好。
    // DetectConsoleCapability() 内部已经处理了 LUBANCODE_FORCE_COLOR 强制
    // 着色的情况(管道模式下也能测出 ANSI 序列),但转轮不受这个开关影响——
    // is_console 为假(管道)时无论如何都不转,免得转轮字符污染管道输出。
    const lubancode::cli::ConsoleCapability console_cap = lubancode::cli::DetectConsoleCapability();
    const lubancode::cli::Theme theme =
        lubancode::cli::ResolveTheme(config_result->config.theme, console_cap.colors_enabled);
    const bool spinner_enabled = console_cap.is_console;

    // --yes 等价于起手就把会话级确认模式切到 yolo(全自动,needs_confirm
    // 的工具一概放行)——单发模式(AskOnce)也一起设,虽然单发模式走不到
    // Shift+Tab 那条路,但 on_tool_confirm 统一查 CurrentConfirmMode(),
    // 这里设了才对得上。
    // LUBANCODE_CONFIRM_MODE 环境变量(auto/yolo/confirm)可指定起手档位——
    // 管道模式敲不了 Shift+Tab,自动化验证 auto 档全靠它;--yes 优先级更高,
    // 认不出的值一律按默认 confirm 档走,不报错不拦人。
    // 起手档位优先级(高到低):--yes/LUBANCODE_CONFIRM_MODE >
    // settings.local.json 的 default_confirm_mode > 内置默认 confirm。
    lubancode::cli::ConfirmMode initial_mode =
        cli_options.auto_confirm ? lubancode::cli::ConfirmMode::Yolo : lubancode::cli::ConfirmMode::Confirm;
    bool mode_from_explicit = cli_options.auto_confirm;  // --yes 或 env 显式指定过,settings 不再插手
    if (!cli_options.auto_confirm) {
        if (const auto env_mode = lubancode::platform::GetEnvVar("LUBANCODE_CONFIRM_MODE"); env_mode.has_value()) {
            const std::string& mode_str = *env_mode;
            if (mode_str == "auto") {
                initial_mode = lubancode::cli::ConfirmMode::Auto;
                mode_from_explicit = true;
            } else if (mode_str == "yolo") {
                initial_mode = lubancode::cli::ConfirmMode::Yolo;
                mode_from_explicit = true;
            } else if (mode_str == "confirm") {
                initial_mode = lubancode::cli::ConfirmMode::Confirm;
                mode_from_explicit = true;
            }
        }
    }
    // settings.local.json 的 default_confirm_mode:只在没被 --yes/env 显式压过
    // 时才生效(认不出的值一律忽略,不拦人)。
    if (!mode_from_explicit && settings_local.default_confirm_mode.has_value()) {
        const std::string& mode_str = *settings_local.default_confirm_mode;
        if (mode_str == "auto") {
            initial_mode = lubancode::cli::ConfirmMode::Auto;
        } else if (mode_str == "yolo") {
            initial_mode = lubancode::cli::ConfirmMode::Yolo;
        } else if (mode_str == "confirm") {
            initial_mode = lubancode::cli::ConfirmMode::Confirm;
        }
    }
    lubancode::cli::SetConfirmMode(initial_mode);

    // M9:真正要进一次会话了(单发问答也算一次会话)——session_start 在这里
    // 跑,session_end 在这个作用域结束(RunCli 返回、或者中途抛异常被下面
    // catch 住之后自然析构)时跑。--config/--version/--help 提前 return,
    // 走不到这里,不会触发。
    const SessionHookScope session_hook_scope(config_result->config.hooks);

    // 兜底:JSON 编码、网络库内部等地方万一抛出没接住的异常,也不能让
    // 整个进程崩掉(崩掉的话用户只会看到一个莫名其妙的退出码)。
    try {
        if (!cli_options.positional.empty()) {
            // 单发模式/管道模式:不进向导(没有交互终端可问),缺配置直接
            // 报可读的错,指路三条配置途径。
            const auto configured_check = lubancode::config::RequireConfigured(*config_result);
            if (!configured_check.has_value()) {
                std::cerr << configured_check.error() << "\n";
                return 1;
            }
            // 模型目录应用(单发模式):think/context_window 直接并进这份
            // 一次性的配置副本(用户显式配过的不动,跟交互模式同一条规矩),
            // base_instructions 单独传给 AskOnce 拼进系统提示。不打提示行——
            // 单发/管道模式的输出常被重定向,不该混进这些会话性的话。
            lubancode::config::Config once_config = config_result->config;
            const auto catalog_apply = lubancode::config::ComputeCatalogApplication(
                model_catalog, once_config.model,
                config_result->sources.think != lubancode::config::Source::Default,
                config_result->sources.context_window_tokens != lubancode::config::Source::Default);
            if (catalog_apply.think.has_value()) {
                once_config.think = *catalog_apply.think;
            }
            if (catalog_apply.context_window_tokens.has_value()) {
                once_config.context_window_tokens = *catalog_apply.context_window_tokens;
            }
            // 魂:按配置的 soul 名读一次(缺文件不警告——管道输出保持干净),
            // 拼在系统提示最后。
            const std::string soul_content =
                LoadSoulContentByName(once_config.soul.empty() ? "default" : once_config.soul, /*warn=*/false);
            return AskOnce(once_config, cli_options.positional, cli_options.auto_confirm, theme, persona,
                            spinner_enabled,
                            settings_local, catalog_apply.base_instructions, soul_content);
        }

        // 交互模式:base_url/api_key/model 有一个解不出来,就先走一遍初次
        // 配置向导——三个字段都没有内置默认值,任何一个空着都没法真的
        // 跟模型对话,不如趁手就问清楚(即便本次规矩里描述的触发条件只提了
        // base_url/api_key,这里多加一条 model 判断更稳妥,免得 env 只配了
        // base_url/api_key 漏了 model,走进会话却发不出请求)。
        lubancode::config::ConfigResult effective = *config_result;
        if (effective.config.base_url.empty() || effective.config.auth_token.empty() ||
            effective.config.model.empty()) {
            const auto wizard_config = RunInitialSetupWizard(effective.config_file_path, theme);
            if (!wizard_config.has_value()) {
                std::cerr << tr("error.wizard_incomplete") << "\n";
                return 1;
            }
            const auto memory_config = effective.config.memory;
            effective.config = *wizard_config;
            effective.config.memory = memory_config;
            // 向导给出的这份配置,来源标记简化成两种:保存了就算"全局配置
            // 文件"来源(向导写的是主目录 ~/.lubancode/config.json),没保存
            // 就算"内置默认值"(最接近"临时值,没有更合适的持久来源"这个
            // 语义)——/config 展示用,不影响实际发请求。
            const lubancode::config::Source marked = effective.config_file_path.has_value()
                                                          ? lubancode::config::Source::GlobalConfigFile
                                                          : lubancode::config::Source::Default;
            effective.sources.wire = marked;
            effective.sources.base_url = marked;
            effective.sources.auth_token = marked;
            effective.sources.model = marked;
        }
        std::string executable;
        if (!args.empty()) {
            std::error_code ec;
            const auto absolute = std::filesystem::absolute(lubancode::tools::Utf8ToPath(args[0]), ec);
            executable = PathToUtf8(ec ? lubancode::tools::Utf8ToPath(args[0]) : absolute);
        }
        const lubancode::app::InteractiveSessionOptions session_options{
            effective, theme, model_catalog, settings_local,
            cli_options.auto_confirm, persona, spinner_enabled, cli_options.continue_last, law_source,
            executable};
        RunInteractiveSession(session_options);
    } catch (const std::exception& e) {
        std::cerr << tr("error.prefix") << trf("error.unexpected", e.what()) << "\n";
        return 1;
    }
    return 0;
}

}  // namespace lubancode::app