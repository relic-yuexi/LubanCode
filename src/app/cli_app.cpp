// cli_app.hpp 的实现:PrintVersion/PrintHelp/向导/SessionHookScope/RunCli
// 的函数体,原样搬自原头文件,行为一字未改。

#include "app/cli_options.hpp"
#include "app/interactive_session.hpp"
#include "app/session_stack.hpp"  // 组合根装配件(会话终章)
#include "app/one_shot.hpp"
#include "app/plugin_scaffold.hpp"
#include "app_server/server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
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
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "peers/peer_session.hpp"
#include "agent/prompts.hpp"
#include "skills/workflow_recorder.hpp"
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
#include "app/commands/evolve_commands.hpp"  // RunEvolveTestCommand(自进化阶段 3 的 CI 口)
#include "app/version.hpp"
#include "cli/console_input.hpp"
#include "cli/gateway_command.hpp"  // 总装单 G1:gateway run/status/stop 子命令
#include "cli/migrate_storage_command.hpp"  // 存储 v2 P0-5:migrate-storage plan/run/status
#include "cli/trajectory_command.hpp"  // P0-3:trajectory verify/replay/harness-replay 子命令
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"
#include "cli/keymap.hpp"
#include "cli/live_transcript.hpp"
#include "runtime/worktree.hpp"
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
#include "config/settings_local.hpp"
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
#include "app/hook_runtime.hpp"
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

using lubancode::platform::CurrentDirUtf8;
using lubancode::app::LoadSoulContentByName;
using lubancode::app::HandleUpdateCommand;
using lubancode::app::PathToUtf8;
using lubancode::cli::tr;
using lubancode::cli::trf;

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

// Plan 模式单:起手协作档的优先级(高到低):--mode > LUBANCODE_COLLABORATION_
// MODE > settings.local.json 的 default_collaboration_mode > Default。
// CLI 的非法值已在解析层退 BadMode;env/settings 的非法值明报到 stderr
// 并按 Default 走(单子:"认不得的值报错,不能安静落回 Default"——CLI
// 能退出,env/settings 是持久配置,硬退会把人挡在门外,只报不退)。
bool ResolveStartupPlanMode(const CliOptions& cli_options, const config::SettingsLocal& settings_local) {
    if (cli_options.mode_given) {
        return cli_options.mode == "plan";
    }
    if (const auto env_mode = lubancode::platform::GetEnvVar("LUBANCODE_COLLABORATION_MODE");
        env_mode.has_value() && !env_mode->empty()) {
        if (*env_mode == "plan") {
            return true;
        }
        if (*env_mode == "default") {
            return false;
        }
        std::cerr << trf("plan.env.bad_mode", *env_mode) << "\n";
        return false;
    }
    if (settings_local.default_collaboration_mode.has_value()) {
        const std::string& value = *settings_local.default_collaboration_mode;
        if (value == "plan") {
            return true;
        }
        if (value == "default") {
            return false;
        }
        std::cerr << trf("plan.settings.bad_mode", value) << "\n";
    }
    return false;
}

// i18n:帮助文本按节进表(help.title/usage/options/scaffold/slash/config),
// 版本号、三个内置默认值走占位符。zh-CN 表的值与旧字面文案一致。
// 斜杠命令清单(P3-2)不再手抄进表——intro 之后打
// cli::FormatSlashCommandListLines() 生成的行,与 /help、Tab 补全同一份
// AllSlashCommands,三份名单永不各列各的。
void PrintHelp() {
    std::cout << trf("help.title", kVersion) << "\n\n"
              << tr("help.usage") << "\n"
              << tr("help.options") << "\n"
              << tr("help.scaffold") << "\n"
              << tr("help.slash");
    for (const std::string& line : lubancode::cli::FormatSlashCommandListLines()) {
        std::cout << line << "\n";
    }
    std::cout << tr("help.keys")
              << trf("help.config", lubancode::config::kDefaultMaxContextChars,
                     lubancode::config::kDefaultTheme, lubancode::config::kDefaultContextWindowTokens);
}

// /tools 命令:列工具三态——核心(恒在)/已加载的延迟工具/延迟未加载,

// 初次启动不再强填 base_url/api_key/model。欢迎页只分两路：直接接
// /provider add 同款目录向导，或先进入主界面。面板活着时不往它脚下插
// 普通输出；告警与保存回执攒到 WizardIO 析构清场后再打。
std::optional<lubancode::config::ConfigResult> RunInitialSetupWizard(
    const lubancode::config::ConfigResult& current, const lubancode::cli::Theme& theme) {
    lubancode::config::ConfigResult result = current;
    std::optional<lubancode::cli::ProviderWizardOutcome> provider_outcome;
    std::vector<std::string> notices;

    {
        lubancode::cli::WizardIO io = MakeInteractiveWizardIO(theme);
        const auto home_lubancode_dir = lubancode::config::HomeLubancodeDir();
        io.home_config_display_path =
            (home_lubancode_dir.has_value() ? *home_lubancode_dir : tr("path.no_home") + "/.lubancode") +
            "/config.json";

        while (true) {
            const auto entry = lubancode::cli::RunSetupEntryWizard(io);
            if (!entry.has_value()) {
                return std::nullopt;
            }
            result.config.language = entry->language;
            if (entry->action == lubancode::cli::SetupEntryAction::Skip) {
                break;
            }

            // 开局只读本地缓存/内置快照。目录刷新留给 /provider refresh；
            // 不能为一张欢迎页先赌十秒网络超时。
            const lubancode::config::ProviderCatalog catalog = lubancode::config::LoadProviderCatalog();
            for (const std::string& warning : catalog.warnings) {
                notices.push_back(trf("provider_catalog.warning", warning));
            }
            const auto picked = lubancode::cli::RunProviderPresetWizard(
                io, catalog, std::string(), result.config.providers);
            if (!picked.has_value() || !picked->save_requested) {
                continue;  // 从 provider 向导退回欢迎页，仍有“暂时跳过”可走
            }
            provider_outcome = *picked;
            break;
        }
    }

    for (const std::string& notice : notices) {
        std::cout << notice << "\n";
    }
    if (!provider_outcome.has_value()) {
        return result;  // 明选跳过：主界面照开，配置一个字也不写
    }

    const lubancode::config::ProviderConfig& provider = provider_outcome->provider;
    const auto saved = lubancode::config::AddProviderToGlobalConfig(provider);
    if (saved.has_value()) {
        std::cout << trf("cmd.provider.added", provider.name, *saved) << "\n";
        result.global_config_file_path = *saved;
        if (!result.project_config_file_path.has_value()) {
            result.config_file_path = *saved;
        }
        result.sources.providers = lubancode::config::Source::GlobalConfigFile;
        const auto remembered = lubancode::config::SetActiveProviderInGlobalConfig(provider.name);
        if (remembered.has_value()) {
            result.sources.active_provider = lubancode::config::Source::GlobalConfigFile;
        } else {
            std::cout << trf("cmd.provider.remember_failed", remembered.error()) << "\n";
        }
        if (const auto language_saved =
                lubancode::config::UpdateLanguageInConfigFile(*saved, result.config.language);
            !language_saved.has_value()) {
            std::cout << trf("wizard.save_failed", language_saved.error()) << "\n";
        } else {
            result.sources.language = lubancode::config::Source::GlobalConfigFile;
        }
    } else {
        std::cout << trf("wizard.save_failed", saved.error()) << "\n";
    }

    // 写盘失败也不糟蹋刚填完的结果：本次会话先能用。用户刚刚明选了
    // provider，这个会话动作须整套压过启动时残留的半套环境变量。
    result.config.providers.push_back(provider);
    lubancode::config::ApplyProviderToRuntimeConfig(result.config, provider);
    if (!provider.model_reasoning_effort.empty()) {
        result.config.think = provider.model_reasoning_effort;
        result.sources.think = saved.has_value() ? lubancode::config::Source::GlobalConfigFile
                                                 : lubancode::config::Source::Default;
    }
    return result;
}

// M9 -> hooks 框架:session_start/session_end 钩子的生命周期跟"这一次 CLI
// 进程真正进入了一次会话"绑在一起——构造时发 SessionStart(source=startup),
// 析构时发 SessionEnd(reason=exit)。只在 RunCli 真正要进 AskOnce/
// InteractiveLoop 那条路时构造(--config/--version/--help 这些提前 return 的
// 路径不会走到这里,不该触发)。走进程级 dispatcher:来源相加、信任审查、
// legacy adapter 都在那一层。SessionEnd 是 advisory 收尾——进程被硬杀
// (taskkill/断电)时这个析构不会跑,协议文档里如实写明,不承诺必达。
class SessionHookScope {
public:
    explicit SessionHookScope(lubancode::hooks::HookDispatcher* dispatcher) : dispatcher_(dispatcher) {
        if (dispatcher_ == nullptr || dispatcher_->Empty() ||
            !dispatcher_->HasHandlersFor(lubancode::hooks::HookEvent::SessionStart)) {
            return;
        }
        lubancode::hooks::HookPayload payload;
        payload.event = lubancode::hooks::HookEvent::SessionStart;
        payload.fields["source"] = "startup";
        payload.match_value = "startup";
        dispatcher_->Emit(lubancode::hooks::HookEvent::SessionStart, payload);
    }
    ~SessionHookScope() {
        if (dispatcher_ == nullptr || dispatcher_->Empty() ||
            !dispatcher_->HasHandlersFor(lubancode::hooks::HookEvent::SessionEnd)) {
            return;
        }
        lubancode::hooks::HookPayload payload;
        payload.event = lubancode::hooks::HookEvent::SessionEnd;
        payload.fields["reason"] = "exit";
        payload.match_value = "exit";
        dispatcher_->Emit(lubancode::hooks::HookEvent::SessionEnd, payload);
    }

    SessionHookScope(const SessionHookScope&) = delete;
    SessionHookScope& operator=(const SessionHookScope&) = delete;

private:
    lubancode::hooks::HookDispatcher* dispatcher_;
};

// 致命退出的诊断尾行(P1-2):所有非零退出路径都要写足错误类别与会话
// 存档去处——已 flush 的流水丢不了,--continue 接得回来。0.26.76 的静默
// code 1(首次自动压缩后)没有任何回执,用户连"该去哪找现场"都不知道。
void PrintFatalExitDiagnostics(const char* error_category) {
    std::string sessions_hint;
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        sessions_hint = *luban_dir + "/sessions";
    }
    std::cerr << tr("error.fatal_category") << error_category << "\n";
    if (!sessions_hint.empty()) {
        std::cerr << trf("error.fatal_session_hint", sessions_hint) << "\n";
    }
    std::cerr << std::flush;
}

// `lubancode plugin init` 子命令(plugins 单第 3 步):生成插件脚手架后
// 退出。不读配置、不进会话——纯落盘 + 环境诊断。i18n 早初始化在这之前
// 跑过,tr() 可用;配置不加载(脚手架不该因为模型没配好而拒绝干活)。
int HandlePluginInitCommand(const PluginInitArgs& init) {
    const auto home_dir = lubancode::config::HomeLubancodeDir();
    if (!home_dir.has_value()) {
        std::cerr << tr("plugininit.no_home") << "\n";
        return 1;
    }
    const std::string plugins_root = *home_dir + "/plugins";
    if (init.template_name == "python") {
        const auto result = lubancode::app::ScaffoldPythonPlugin(plugins_root, init.plugin_name, std::string());
        if (!result.has_value()) {
            std::cerr << trf("plugininit.failed", result.error()) << "\n";
            return 1;
        }
        std::cout << trf("plugininit.done", result->plugin_name, result->target_dir_utf8) << "\n";
        for (const std::string& file : result->files) {
            std::cout << "  - " << file << "\n";
        }
        for (const std::string& note : result->doctor_notes) {
            std::cout << trf("plugininit.doctor_note", note) << "\n";
        }
        std::cout << tr("plugininit.next") << "\n";
        return 0;
    }
    // lua 模板:legacy .lua 一文件一工具本来就是零配置,生成也只是一份示例
    // 脚本;v1 先指路不代写,免得两套模板各养一份文案。
    if (init.template_name == "lua") {
        std::cout << trf("plugininit.lua_hint", plugins_root) << "\n";
        return 0;
    }
    std::cerr << trf("plugininit.unknown_template", init.template_name) << "\n";
    return 1;
}

// app-server 子模式:无界面后台协议,stdio 上逐行 JSON。装配前奏(配置
// 加载、i18n、hooks 装载)在 RunCli 里已经跑完——这里只把服务立起来进
// 主循环。stdout 从这一刻起是协议专线,任何 std::cout 都不许再出现
// (诊断走 stderr,app_server 模块自己守规矩,这层也一样)。
// backend 走真装配(BuildBackend)——假 backend 只在单测里注入。
int RunAppServerMode(const lubancode::config::ConfigResult& config_result,
                     const CliOptions& cli_options) {
    lubancode::app_server::ServerOptions options;
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        // P0-2:会话账走唯一持久化根 workspaces/。
        options.workspaces_dir = *luban_dir + "/workspaces";
        // wf 线的 run 账根(workflow/query 的快照与增量事件从这里读)。
        options.workflow_runs_dir = *luban_dir + "/workflow-runs";
    }
    options.cwd = CurrentDirUtf8();
    // 会话档 meta 真值(阶段 3 冻结项):wire/model 用配置四级合并的
    // 结果,与 CLI 会话档同一张表;不写占位话。
    switch (config_result.config.wire) {
        case lubancode::config::Wire::Anthropic:
            options.session_wire = "anthropic";
            break;
        case lubancode::config::Wire::Responses:
            options.session_wire = "responses";
            break;
        case lubancode::config::Wire::ChatCompletions:
            options.session_wire = "chat";
            break;
        case lubancode::config::Wire::GoogleGenerateContent:
            options.session_wire = "google-generate-content";
            break;
    }
    options.session_model = config_result.config.model;
    // P0-2(Trajectory 升为唯一 Session):feature/env 开关已删,thread 恒走
    // Trajectory 账(P0-6:features.trajectory 开关与 server 侧字段随之退役)。
    // 浏览器面(可见调试阶段 3):sidecar 命令解析——环境变量
    // LUBAN_BROWSER_SIDECAR 指到 browser/sidecar.js 优先;没指则按可执行
    // 文件旁边与当前目录找 browser/sidecar.js。找不到就不配(browser/*
    // 方法回 browser.not_configured,不冒充)。截图 artifact 落
    // <HomeLubancodeDir>/browser-artifacts(内容寻址)。
    if (const char* env_sidecar = std::getenv("LUBAN_BROWSER_SIDECAR");
        env_sidecar != nullptr && *env_sidecar != '\0') {
        options.browser_sidecar_command = "node";
        options.browser_sidecar_args = {std::string(env_sidecar)};
    } else {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> candidates;
        if (const auto executable = lubancode::platform::ExecutablePath(); executable.has_value()) {
            const fs::path exe_dir = executable->parent_path();
            candidates.push_back(exe_dir / "browser" / "sidecar.js");
            candidates.push_back(exe_dir.parent_path() / "browser" / "sidecar.js");
        }
        candidates.push_back(fs::path("browser") / "sidecar.js");
        for (const fs::path& candidate : candidates) {
            if (fs::exists(candidate, ec)) {
                options.browser_sidecar_command = "node";
                options.browser_sidecar_args = {lubancode::platform::PathToUtf8(candidate)};
                break;
            }
        }
    }
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        options.browser_artifact_dir = *luban_dir + "/browser-artifacts";
    }
    // WS 承载(多前端外壳单阶段 A):--app-server-ws <port | host:port>。
    // 裸端口绑回环;显式 host 须是点分 IPv4 或 localhost(ws_sockets 的
    // 绑定面)。非回环绑定强制要 token:旗标给的优先,其次环境变量
    // LUBANCODE_APPSERVER_TOKEN;两处都没有就拒启——token 不落日志,拒启
    // 的话里也不带它。
    if (!cli_options.app_server_ws_bind.empty()) {
        std::string spec = cli_options.app_server_ws_bind;
        std::string host = "127.0.0.1";
        const std::size_t colon = spec.rfind(':');
        if (colon != std::string::npos) {
            host = spec.substr(0, colon);
            spec = spec.substr(colon + 1);
        }
        std::string token = cli_options.app_server_ws_token;
        if (token.empty()) {
            token = lubancode::platform::GetEnvVar("LUBANCODE_APPSERVER_TOKEN").value_or(std::string());
        }
        const bool loopback = host == "127.0.0.1" || host == "localhost";
        if (!loopback && token.empty()) {
            std::fprintf(stderr,
                         "[app-server] 非回环绑定(%s)须配 token:--app-server-ws-token 或环境变量"
                         " LUBANCODE_APPSERVER_TOKEN\n",
                         host.c_str());
            return 1;
        }
        app_server::WsOptions ws;
        ws.bind_host = host;
        ws.port = std::stoi(spec);
        ws.token = token; // 空串 = 回环免鉴权;配了就启用首帧门
        options.ws = ws;
        // WS 模式下 stdout 不再是协议口,但仍守 stdio 纪律:诊断一律 stderr。
    }
    lubancode::app_server::Server server(
        std::move(options),
        [&config_result]() { return lubancode::app::BuildBackend(config_result.config); },
        nullptr);
    return server.Run();
}

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
        case CliAction::RunPluginInit:
            return HandlePluginInitCommand(parsed_cli.plugin_init);
        case CliAction::BadPluginInit:
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
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
        case CliAction::BadMode:
            // Plan 模式单:--mode 认不得,明报退出——不静默落回 Default(单
            // 子:不能让用户误以为只读保护已经开了)。
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::BadPackageDir:
            // 统一 Package 封装单:--package-dir 缺值,明报退出。
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::BadEvolveTest:
            // 自进化阶段 3:evolve test 参数不对,明报退出。
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::RunTrajectory: {
            // P0-3/P0-4 轨迹子命令:只读诊断(verify/replay/usage/doctor)
            // 与 derived-only GC,跑完就退。
            cli::TrajectoryCommandArgs trajectory_args;
            trajectory_args.verb = parsed_cli.trajectory.verb;
            trajectory_args.session_id = parsed_cli.trajectory.session_id;
            trajectory_args.gc_derived_only = parsed_cli.trajectory.gc_derived_only;
            trajectory_args.format = parsed_cli.trajectory.format;
            return cli::RunTrajectoryCommand(trajectory_args);
        }
        case CliAction::BadTrajectory:
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::RunGateway: {
            // 总装单 G1:gateway run/status/stop。run 是前台真进程;status
            // 是只读 probe(零写盘零建目录,不暗起 Gateway);stop 投本地
            // 控制命令。退出码合同见 docs/architecture/gateway/README.md §5。
            cli::GatewayCommandArgs gateway_args;
            gateway_args.verb = parsed_cli.gateway.verb;
            gateway_args.profile = parsed_cli.gateway.profile;
            gateway_args.json = parsed_cli.gateway.json;
            return cli::RunGatewayCommand(gateway_args);
        }
        case CliAction::BadGateway:
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::RunMigrateStorage: {
            // 存储 v2 P0-5:一次性迁移命令面(plan/run/status),跑完就退。
            cli::MigrateStorageCommandArgs migrate_args;
            migrate_args.verb = parsed_cli.migrate_storage.verb;
            migrate_args.operation_id = parsed_cli.migrate_storage.operation_id;
            migrate_args.project_roots = parsed_cli.migrate_storage.project_roots;
            migrate_args.delete_source = parsed_cli.migrate_storage.delete_source;
            migrate_args.confirm_delete = parsed_cli.migrate_storage.confirm_delete;
            return cli::RunMigrateStorageCommand(migrate_args);
        }
        case CliAction::BadMigrateStorage:
            std::cerr << parsed_cli.error_text << "\n";
            return 1;
        case CliAction::RunEvolveTest:
            // 自进化阶段 3:CI 非交互评测入口(与 /evolve test 同一枚
            // EvolutionCoordinator::TestDir;退出码按结果)。
            return RunEvolveTestCommand(parsed_cli.evolve_test);
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
        case CliAction::ManageSession: {
            // 会话管理子命令(archive/unarchive/delete):不进会话,打完
            // 结果就退。i18n 已在函数头初始化;确认屏在 handler 里。
            const auto luban_dir = lubancode::config::HomeLubancodeDir();
            if (!luban_dir.has_value()) {
                std::cerr << tr("session.no_home") << "\n";
                return 1;
            }
            const lubancode::cli::Theme manage_theme = lubancode::cli::ResolveTheme(
                std::string(), lubancode::cli::DetectConsoleCapability().colors_enabled);
            // P0-2:搬删走 workspace 新账(不进会话;索引定位 + 管理面)。
            return HandleSessionManagementCommand(
                lubancode::tools::Utf8ToPath(*luban_dir) / "workspaces",
                static_cast<int>(parsed_cli.session_command.kind), parsed_cli.session_command.session_ref,
                parsed_cli.session_command.force, manage_theme, nullptr);
        }
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
    // 兼容期提示(命名规范第二批):旧名 max_turns / LUBANCODE_MAX_TURNS 被
    // 读入、或新旧同现冲突时逐条打给用户看。走 stderr,不污染管道输出。
    for (const std::string& notice : config_result->deprecation_notices) {
        std::cerr << notice << "\n";
    }

    // i18n:配置读出来了,按四级合并的 language 字段定稿(空 = 跟系统)。
    // 语言包早在函数开头扫过,这里只是切码;坏包警告攒到现在,按定稿语言打。
    lubancode::cli::SetLanguage(config_result->config.language.empty()
                                     ? lubancode::cli::DetectSystemLanguage()
                                     : config_result->config.language);
    for (const auto& warning : language_pack_warnings) {
        std::cerr << trf("i18n.pack_warning", warning) << "\n";
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
    // 文件不存在就是空目录,一切回退现状,绝不拦人。警告走 stderr:
    // app-server/管道模式下 stdout 是协议口/数据口,一行不糟蹋。
    const lubancode::config::ModelCatalog model_catalog = lubancode::config::LoadModelCatalog();
    for (const auto& warning : model_catalog.warnings) {
        std::cerr << trf("catalog.warning", warning) << "\n";
    }

    // 用户键位覆盖(~/.lubancode/keymap.json,交互抛光总账的 keymap 层):
    // 启动读一次,坏条目只告警跳过(该项回默认),不拦人。刻意不读项目
    // 目录——键位是用户全局的,项目配置不许暗改(规格第 10 条)。
    if (const auto luban_dir = lubancode::config::HomeLubancodeDir(); luban_dir.has_value()) {
        for (const auto& warning : lubancode::cli::keymap::LoadActiveKeymapOverrides(*luban_dir)) {
            std::cerr << trf("keymap.override_warning", warning) << "\n";
        }
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

    // 钥匙撞车单:active provider 的 key_env 变量与 inline api_key 两把都有
    // 且不一致——变量赢,但得叫一声,不然 401 成了无头案(实战:Claude Code
    // 往 ANTHROPIC_AUTH_TOKEN 塞别家钥匙,压过配置里贴好的 key)。stderr 与
    // hook 提示同规矩:交互看得见,单发/管道不污染 stdout 重定向产物。
    if (!config_result->config.active_provider.empty()) {
        if (const lubancode::config::ProviderConfig* active_provider = lubancode::config::FindProvider(
                config_result->config.providers, config_result->config.active_provider);
            active_provider != nullptr) {
            if (const std::optional<std::string> key_warning =
                    lubancode::config::ProviderAuthConflictWarning(*active_provider)) {
                std::cerr << *key_warning << "\n";
            }
        }
    }

    // hooks 运行时装载:来源分级 + definition hash 信任审查在这里完成。
    // 未信任的项目 hook 从这一刻起就绝不起进程;提示打到 stderr(交互模式
    // 用户看得见;单发/管道模式 stderr 也不污染 stdout 的重定向产物)。
    const std::vector<std::string> hook_notices = lubancode::app::SetupHookRuntime(*config_result);
    for (const std::string& notice : hook_notices) {
        std::cerr << notice << "\n";
    }

    // M9 -> hooks 框架:真正要进一次会话了(单发问答也算一次会话)——
    // SessionStart 在这里发,SessionEnd 在这个作用域结束(RunCli 返回、或者
    // 中途抛异常被下面 catch 住之后自然析构)时发。--config/--version/--help
    // 提前 return,走不到这里,不会触发。
    const SessionHookScope session_hook_scope(lubancode::app::HookRuntime());

    // 兜底:JSON 编码、网络库内部等地方万一抛出没接住的异常,也不能让
    // 整个进程崩掉(崩掉的话用户只会看到一个莫名其妙的退出码)。
    try {
        if (cli_options.app_server) {
            // app-server 是独占模式:不进单发,不进交互。旗标在解析层就
            // 拦下了,这里再守一道(one_shot 防守的单子原文),双保险。
            return RunAppServerMode(*config_result, cli_options);
        }
        if (!cli_options.app_server_ws_bind.empty() || !cli_options.app_server_ws_token.empty()) {
            // --app-server-ws 系旗标只在 app-server 子命令下有意义;别处
            // 给了就是用错地方,明说,不当普通位置参数吞。
            std::cerr << "--app-server-ws/--app-server-ws-token 只在 app-server 子命令下有效: "
                         "lubancode app-server --app-server-ws <端口|主机:端口>\n";
            return 1;
        }
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
                            settings_local, catalog_apply.base_instructions, soul_content,
                            cli_options.package_dirs);
        }

        // 交互模式缺连接时开欢迎页。用户可以直接添 provider，也可以明选
        // “暂时跳过”进主界面；只有真发普通消息时才拦，/provider 等命令照走。
        lubancode::config::ConfigResult effective = *config_result;
        if (!lubancode::config::RequireConfigured(effective).has_value()) {
            const auto setup = RunInitialSetupWizard(effective, theme);
            if (!setup.has_value()) {
                std::cerr << tr("error.wizard_incomplete") << "\n";
                return 1;
            }
            effective = *setup;
        }
        std::string executable;
        if (!args.empty()) {
            std::error_code ec;
            const auto absolute = std::filesystem::absolute(lubancode::tools::Utf8ToPath(args[0]), ec);
            executable = PathToUtf8(ec ? lubancode::tools::Utf8ToPath(args[0]) : absolute);
        }
        // 组合根外迁(会话终章):会话装配件(材料/后端栈/工具全栈)在这
        // 装好递给会话控制器,控制器只收装好的件。
        lubancode::app::InteractiveSessionOptions session_options{
            effective, theme, model_catalog, settings_local,
            cli_options.auto_confirm, persona, spinner_enabled, cli_options.continue_last, law_source,
            executable, ResolveStartupPlanMode(cli_options, settings_local)};
        // 统一 Package 封装单:--package-dir 递给会话钉快照(阶段 3 挂载的
        // dev 层)与 /package 命令面。
        session_options.package_dirs = cli_options.package_dirs;
        std::unique_ptr<lubancode::app::SessionStack> session_stack =
            lubancode::app::BuildSessionStack(session_options);
        session_options.stack = session_stack.get();
        RunInteractiveSession(session_options);
    } catch (const std::exception& e) {
        // 最后防线:到这里的是启动期/会话外层的真 fatal,退进程;交互会话
        // 内部的回合异常已在 RunTurn 与 ProcessLine 两道兜底收口,走不到这
        // 儿。异常类型一并打出,真机出事好定位(system_error 的 1113 文案
        // 就是宽窄转换异常那单的原文)。P1-2 的教训:非零退出必须带错误
        // 类别、文本与会话存档去处——0.26.76 首次自动压缩后进程 code 1 静默
        // 退出,终端画面被 TUI 收拾过,一行 stderr 都没留下,只能靠猜。
        std::cerr << tr("error.prefix") << trf("error.unexpected", e.what()) << " (" << typeid(e).name() << ")\n";
        PrintFatalExitDiagnostics("std::exception");
        return 1;
    } catch (...) {
        // 同一道防线的无名分支:非 std::exception(自定义异常体系、跨边界
        // 的 SEH 翻译等)原先直接穿透到 terminate,退出码既非 1 也无话。
        // 收住,报"未知异常类别",存档去处照给。
        std::cerr << tr("error.prefix") << trf("error.unexpected", "(unknown exception: not derived from std::exception)")
                  << "\n";
        PrintFatalExitDiagnostics("unknown-exception");
        return 1;
    }
    return 0;
}

}  // namespace lubancode::app
