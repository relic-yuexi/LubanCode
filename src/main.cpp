// lubancode - C++ AI 编程 CLI
// M5:去掉硬编码的 MiniMax 默认值(lubancode 是通用工具,不绑死哪一家模型
// 服务)——base_url/api_key/model 不再有内置默认值。交互模式启动时这几项
// 缺了就先走一遍初次配置向导,配完直接进入会话,不用重启;单发模式/管道
// 模式缺配置则直接报可读的错。交互循环里加了 /help /model /config /clear
// /exit 几个 slash 命令,/model 能让模型切换真正在下一次请求生效。

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "agent/prompts.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "cli/console_input.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/spinner.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "tools/edit_file.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/write_file.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr std::string_view kVersion = "0.5.0";

void PrintVersion() {
    std::cout << "lubancode " << kVersion << "\n";
}

void PrintHelp() {
    std::cout
        << "lubancode " << kVersion << " - C++ AI 编程 CLI\n\n"
        << "用法:\n"
        << "  lubancode [选项]\n"
        << "  lubancode \"问题\"          一次问答,能用工具就用工具\n"
        << "  lubancode                  不带参数则进入交互循环;首次运行缺配置会先走一遍初次配置\n"
        << "                              向导,配完直接进入会话,不用重启。exit/quit 或 EOF(Ctrl+Z /\n"
        << "                              管道读尽)退出;空行只是重新给提示符,不退出\n\n"
        << "选项:\n"
        << "  --version              打印版本号\n"
        << "  --help                 打印本帮助\n"
        << "  --yes                  自动确认所有需要确认的工具调用(比如 run_command),不再逐条询问\n"
        << "  --config               打印最终生效的配置(api_key 打码)和每个字段来自哪一级,排查配置问题用\n"
        << "  --system-prompt <文件> 用这个文件(.md/.txt,UTF-8)替换默认系统提示的人格段,工作目录、\n"
        << "                         工具调用这些运行必需的上下文照样追加,不受影响。压过配置文件里的\n"
        << "                         system_prompt_file 字段\n\n"
        << "交互模式里,输入以 / 开头的一行走命令,不发给模型:\n"
        << "  /help           列出所有命令\n"
        << "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
        << "  /model 名字     直接切到指定模型名,不用拉列表\n"
        << "  /config         打印当前生效配置(复用 --config 的逻辑),外加本会话实际在用的 model\n"
        << "  /clear          清空对话历史\n"
        << "  /exit           退出(裸词 exit/quit 也认)\n\n"
        << "配置优先级(从高到低,按字段逐个决,不是整套配置一刀切):\n"
        << "  1) LUBANCODE_ 专属环境变量\n"
        << "       LUBANCODE_WIRE          协议选择,anthropic 或 responses\n"
        << "       LUBANCODE_BASE_URL      API 地址\n"
        << "       LUBANCODE_API_KEY       认证令牌\n"
        << "       LUBANCODE_MODEL         模型名\n"
        << "       LUBANCODE_MAX_CONTEXT   history 裁剪阈值(字符数)\n"
        << "       LUBANCODE_THEME         终端配色主题,dark / light / plain\n"
        << "       LUBANCODE_SYSTEM_PROMPT_FILE  人格文件路径,同 --system-prompt(命令行参数压过这个)\n"
        << "  2) 配置文件(第一个找到的生效,查找顺序:cwd 的 .lubancode/config.json → 主目录的\n"
        << "     .lubancode/config.json → cwd 的旧位置 .lubancode.json → 主目录的旧位置\n"
        << "     .lubancode.json;读到旧位置会自动挪到新位置)。字段:wire / base_url / api_key / model /\n"
        << "     max_context_chars / theme / system_prompt_file,全部可选。\n"
        << "  3) 通用环境变量(向后兼容旧用法,跟 Claude Code 等工具共用同名变量时容易撞车,\n"
        << "     建议改用第 1 级的 LUBANCODE_* 专属变量):\n"
        << "       wire=anthropic 时读 ANTHROPIC_BASE_URL / ANTHROPIC_AUTH_TOKEN / ANTHROPIC_MODEL\n"
        << "       wire=responses 时读 OPENAI_BASE_URL / OPENAI_API_KEY / OPENAI_MODEL\n"
        << "  4) 内置默认值:wire=anthropic、max_context_chars=" << lubancode::config::kDefaultMaxContextChars
        << "、theme=" << lubancode::config::kDefaultTheme << "。\n"
        << "     base_url/api_key/model/system_prompt_file 不绑死任何一家模型服务,没有内置默认值——\n"
        << "     四级都没配到,交互模式会自动走初次配置向导;单发模式/管道模式会直接报错,提示三条\n"
        << "     配置途径。用 --config 能看到当前实际生效的配置和每个字段的来源。\n";
}

// 按 wire 造对应的后端实现。agent 层只认 Backend 这个抽象接口,不关心
// 背后具体是哪个协议在干活。
std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(config.base_url, config.auth_token);
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(config.base_url, config.auth_token);
}

// 包一层 Backend:真正发请求前,把 Request.model 换成"当前会话实际在用的
// model"。AgentLoop 的 model 是构造时定死的私有成员,没有 setter(agent 层
// 现有文件不让动,详见任务规矩),这层包装是唯一能让 /model 切换在下一次
// 请求"真正生效"、又不用碰 agent/loop.hpp/.cpp 的办法。current_model 用
// shared_ptr,是因为 /model 命令改的和这里读的得是同一块内存,AgentLoop
// 只认引用、包装器要跨多轮 Run() 存活。
class ModelOverrideBackend : public lubancode::api::Backend {
public:
    ModelOverrideBackend(lubancode::api::Backend& inner, std::shared_ptr<std::string> current_model)
        : inner_(inner), current_model_(std::move(current_model)) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event) override {
        lubancode::api::Request patched = request;
        patched.model = *current_model_;
        return inner_.send_stream(patched, on_event);
    }

private:
    lubancode::api::Backend& inner_;
    std::shared_ptr<std::string> current_model_;
};

// --config、/config 共用:打印最终生效的配置和每个字段的来源。session_model
// 有值时(/config 场景)额外打一行"本会话实际在用的 model"——/model 切换
// 只影响会话内存,不一定跟 config.model(四级合并出来的那份)一致。
void PrintConfigDiagnostics(const lubancode::config::ConfigResult& result,
                             const std::optional<std::string>& session_model = std::nullopt) {
    const auto& config = result.config;
    const auto& sources = result.sources;
    const std::string wire_str = config.wire == lubancode::config::Wire::Responses ? "responses" : "anthropic";

    std::cout << "lubancode 最终生效的配置:\n\n";
    std::cout << "  wire               = " << wire_str << "  [" << lubancode::config::ToString(sources.wire) << "]\n";
    std::cout << "  base_url           = " << (config.base_url.empty() ? "(未设置)" : config.base_url) << "  ["
              << lubancode::config::ToString(sources.base_url) << "]\n";
    std::cout << "  api_key            = " << lubancode::config::MaskApiKey(config.auth_token) << "  ["
              << lubancode::config::ToString(sources.auth_token) << "]\n";
    std::cout << "  model              = " << (config.model.empty() ? "(未设置)" : config.model) << "  ["
              << lubancode::config::ToString(sources.model) << "]\n";
    std::cout << "  max_context_chars  = " << config.max_context_chars << "  ["
              << lubancode::config::ToString(sources.max_context_chars) << "]\n";
    std::cout << "  theme              = " << config.theme << "  [" << lubancode::config::ToString(sources.theme)
              << "]\n";
    std::cout << "  system_prompt_file = " << (config.system_prompt_file.empty() ? "(未设置)" : config.system_prompt_file)
              << "  [" << lubancode::config::ToString(sources.system_prompt_file) << "]\n";
    if (result.config_file_path.has_value()) {
        std::cout << "  配置文件           = " << *result.config_file_path << "\n";
    }
    if (session_model.has_value()) {
        std::cout << "\n  本会话实际在用的 model = " << *session_model;
        if (*session_model != config.model) {
            std::cout << "  (只在本会话生效,尚未写入配置文件)";
        }
        std::cout << "\n";
    }
}

// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string CurrentDirUtf8() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::u8string u8 = cwd.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 包一层 Backend:发起真正的网络请求前起一个"思考中"转轮(cli::Spinner),
// 收到第一个流事件就停。转轮跟着 send_stream 这一次调用走——AgentLoop 一次
// Run() 里可能因为工具调用来回好几趟,每趟各自单独调一次 send_stream,
// 工具执行发生在两次 send_stream 之间(loop.cpp 里,不在这层包装范围内),
// 天然满足"工具执行期间不转,发下一轮请求再转"这条要求,不用改
// agent/loop.cpp 一个字。spinner_enabled 由调用方按"stdout 是不是真控制台"
// 算好传进来——管道模式下这层直接透传,不起线程、不输出任何转轮字符。
class SpinnerBackend : public lubancode::api::Backend {
public:
    SpinnerBackend(lubancode::api::Backend& inner, const lubancode::cli::Theme& theme, bool spinner_enabled)
        : inner_(inner), theme_(theme), spinner_enabled_(spinner_enabled) {}

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event) override {
        lubancode::cli::Spinner spinner(theme_, spinner_enabled_);
        bool stopped = false;
        const auto wrapped = [&](const lubancode::api::StreamEvent& event) {
            if (!stopped) {
                spinner.Stop();
                stopped = true;
            }
            on_event(event);
        };
        return inner_.send_stream(request, wrapped);
        // spinner 在这里析构,Stop() 兜底再调一次也是安全的(空操作)——
        // 万一 send_stream 直接失败、一个事件都没吐(比如连都没连上),
        // 转轮不会一直转着。
    }

private:
    lubancode::api::Backend& inner_;
    const lubancode::cli::Theme& theme_;
    bool spinner_enabled_;
};

// 交互模式启动横幅:一眼看全版本、wire、当前模型、工作目录,两行,不啰嗦。
void PrintBanner(const lubancode::config::Config& config, const lubancode::cli::Theme& theme) {
    const std::string wire_str = config.wire == lubancode::config::Wire::Responses ? "responses" : "anthropic";
    std::cout << theme.banner << "lubancode " << kVersion << "  [" << wire_str << "] " << config.model << theme.reset
              << "\n";
    std::cout << theme.stats << "cwd: " << CurrentDirUtf8() << "  ·  输入问题回车发送,exit 退出,/help 看命令"
              << theme.reset << "\n";
}

lubancode::tools::ToolRegistry BuildToolRegistry() {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    return registry;
}

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
        std::cout << "      ...(共 " << total << " 行,已省略其余)\n";
    }
}

// 确认前把工具的入参打印清楚,好让人一眼看明白将要发生什么:
// write_file/edit_file 显示路径和内容/改动的前几行摘要,run_command 显示
// 完整命令,别的按通用 JSON 打印兜底。
void PrintConfirmDetails(const std::string& name, const nlohmann::json& input) {
    if (name == "write_file") {
        const std::string path = input.value("path", std::string());
        const std::string content = input.value("content", std::string());
        std::cout << "    路径: " << path << "\n";
        std::cout << "    内容(" << content.size() << " 字节),前几行:\n";
        PrintFirstLines(content, 5);
    } else if (name == "edit_file") {
        const std::string path = input.value("path", std::string());
        const std::string old_s = input.value("old_string", std::string());
        const std::string new_s = input.value("new_string", std::string());
        const bool replace_all = input.value("replace_all", false);
        std::cout << "    路径: " << path << (replace_all ? "  (replace_all=true,全部替换)" : "") << "\n";
        std::cout << "    - 旧文本:\n";
        PrintFirstLines(old_s, 3);
        std::cout << "    + 新文本:\n";
        PrintFirstLines(new_s, 3);
    } else if (name == "run_command") {
        const std::string command = input.value("command", std::string());
        const std::string shell = input.value("shell", std::string("powershell"));
        std::cout << "    命令(" << shell << "): " << command << "\n";
    } else {
        std::cout << "    参数: " << input.dump() << "\n";
    }
    std::cout.flush();
}

// 一次 Run() 内(可能因为工具调用来回好几趟)的 token 用量累计:输入、
// 输出 tokens 各自求和,再数一下总共发了几次独立请求。RunTurn() 结束后
// 打一行,不跨多次用户提问累计——一问一答算一次统计。
struct UsageStats {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    int request_count = 0;
};

// 交互循环、单发模式共用的回调:文本打字机打印(正文保持原色,不着色),
// 工具调用打一行提示,needs_confirm 的工具按 auto_confirm 决定是自动放行
// 还是问用户一句(三选:y 本次允许 / a 本会话总是允许该工具 / N 拒绝)。
// always_allowed_tools 由调用方持有,跨多轮 Run() 保留,选过 a 的工具本
// 会话内不会再问。usage_stats 由调用方持有,只在这一次 Run() 范围内累计
// (RunTurn() 每次都会传一份新的进来)。
lubancode::agent::Callbacks BuildCallbacks(bool auto_confirm, std::set<std::string>& always_allowed_tools,
                                            const lubancode::cli::Theme& theme, UsageStats& usage_stats) {
    lubancode::agent::Callbacks callbacks;

    callbacks.on_text_delta = [](const std::string& text) {
        std::cout << text;
        std::cout.flush();
    };

    callbacks.on_tool_start = [&theme](const std::string& name, const nlohmann::json& input) {
        std::cout << "\n" << theme.tool_line << "[工具] " << name << " " << input.dump() << theme.reset << "\n";
        std::cout.flush();
    };

    callbacks.on_tool_confirm = [auto_confirm, &always_allowed_tools, &theme](const std::string& name,
                                                                               const nlohmann::json& input) -> bool {
        // 会话级确认模式(Shift+Tab 循环切),跟 --yes/auto_confirm 叠加:
        //   yolo  —— 全自动放行(auto_confirm 本来就是这个语义,这里再查一遍
        //             CurrentConfirmMode() 是为了让 Shift+Tab 中途切到 yolo
        //             也立刻生效,不用等下一轮 --yes)
        //   auto  —— write_file/edit_file 自动放行,run_command 之类仍然要问
        //   confirm(默认)—— 老规矩,needs_confirm 的工具逐个问
        const lubancode::cli::ConfirmMode mode = lubancode::cli::CurrentConfirmMode();
        if (auto_confirm || mode == lubancode::cli::ConfirmMode::Yolo) {
            return true;
        }
        if (mode == lubancode::cli::ConfirmMode::Auto && (name == "write_file" || name == "edit_file")) {
            return true;
        }
        if (always_allowed_tools.count(name) != 0) {
            return true;
        }
        PrintConfirmDetails(name, input);
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + "[y] 本次允许  [a] 本会话总是允许(该工具)  [N] 拒绝: " + theme.reset, theme);
        if (!answer.has_value()) {
            return false;  // 读到 EOF,按拒绝处理,不要在这儿卡住
        }
        if (*answer == "a" || *answer == "A") {
            always_allowed_tools.insert(name);
            return true;
        }
        return *answer == "y" || *answer == "Y";
    };

    callbacks.on_tool_done = [&theme](const std::string& name, const lubancode::tools::Tool::Result& result) {
        if (result.is_error) {
            std::cout << theme.error << "[工具出错] " << name << ": " << result.content << theme.reset << "\n";
            std::cout.flush();
        }
    };

    callbacks.on_usage = [&usage_stats](const lubancode::api::Usage& usage) {
        usage_stats.input_tokens += usage.input_tokens;
        usage_stats.output_tokens += usage.output_tokens;
        usage_stats.cache_read_tokens += usage.cache_read_tokens;
        usage_stats.request_count += 1;
    };

    return callbacks;
}

// 发一轮用户输入,走 agent loop(可能会有若干次工具调用来回),流式打字机
// 打印回复,结束后打一行 token 用量统计(暗色/淡色,plain 主题下就是空
// 前后缀)。返回 0 表示成功,非 0 表示出错。always_allowed_tools 由调用方
// 持有,记录本会话内选过"总是允许"的工具。
int RunTurn(lubancode::agent::AgentLoop& loop, const std::string& user_input, bool auto_confirm,
            std::set<std::string>& always_allowed_tools, const lubancode::cli::Theme& theme) {
    UsageStats usage_stats;
    const lubancode::agent::Callbacks callbacks = BuildCallbacks(auto_confirm, always_allowed_tools, theme, usage_stats);

    const auto result = loop.Run(user_input, callbacks);
    std::cout << "\n";

    if (!result.has_value()) {
        std::cerr << theme.error << "[错误] " << result.error() << theme.reset << "\n";
        return 1;
    }

    if (usage_stats.request_count > 0) {
        std::cout << theme.stats << "[tokens] 输入 " << usage_stats.input_tokens;
        if (usage_stats.cache_read_tokens > 0) {
            std::cout << "(缓存命中 " << usage_stats.cache_read_tokens << ")";
        }
        std::cout << " · 输出 " << usage_stats.output_tokens << " · 请求 " << usage_stats.request_count << " 次"
                   << theme.reset << "\n";
    }
    return 0;
}

// 初次配置向导:接 cli::ReadLine 做输入、std::cout 做输出、api::ListModels
// 做模型列表拉取。用户中途 EOF(Ctrl+Z / 管道读尽)放弃时返回 std::nullopt。
// 用户选择保存时,把保存后的路径写进 out_config_file_path,好让接下来的
// /model 命令知道"有配置文件"。
std::optional<lubancode::config::Config> RunInitialSetupWizard(std::optional<std::string>& out_config_file_path) {
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

    const auto home_lubancode_dir = lubancode::config::HomeLubancodeDir();
    io.home_config_display_path =
        (home_lubancode_dir.has_value() ? *home_lubancode_dir : std::string("<找不到主目录>/.lubancode")) +
        "/config.json";

    const auto outcome = lubancode::cli::RunSetupWizard(io);
    if (!outcome.has_value()) {
        return std::nullopt;
    }

    if (outcome->save_requested) {
        const auto saved = lubancode::config::SaveConfigFile(outcome->config);
        if (saved.has_value()) {
            std::cout << "已保存到 " << *saved << "\n";
            out_config_file_path = *saved;
        } else {
            std::cout << "保存失败: " << saved.error() << "(不影响本次继续用,只是这份配置这次没记住)\n";
        }
    }
    return outcome->config;
}

void PrintSlashHelp() {
    std::cout << "可用命令:\n"
              << "  /help           列出所有命令\n"
              << "  /model          拉取模型列表,编号选择切换(默认第一个)\n"
              << "  /model 名字     直接切到指定模型名,不用拉列表\n"
              << "  /config         打印当前生效配置(api_key 打码),外加本会话实际在用的 model\n"
              << "  /clear          清空对话历史\n"
              << "  /exit           退出(裸词 exit/quit 也认)\n";
}

// /model 命令的执行逻辑:带参数直接切;不带参数拉列表编号选。切完了,
// 有配置文件才问"写进配置文件?",没有就只提示本会话生效。
void HandleModelCommand(const std::string& args, const lubancode::config::Config& config,
                         const std::shared_ptr<std::string>& current_model,
                         std::optional<std::string>& config_file_path) {
    std::string chosen;

    if (!args.empty()) {
        chosen = args;
    } else {
        const auto list_result = lubancode::api::ListModels(config.wire, config.base_url, config.auth_token);
        if (!list_result.has_value()) {
            std::cout << "拉取模型列表失败: " << list_result.error().message << "\n";
            return;
        }
        if (list_result->empty()) {
            std::cout << "接口返回的模型列表是空的。\n";
            return;
        }
        for (std::size_t i = 0; i < list_result->size(); ++i) {
            const auto& m = (*list_result)[i];
            const std::string& label = m.display_name.empty() ? m.id : m.display_name;
            std::cout << "  " << (i + 1) << ") " << label << "\n";
        }
        const std::optional<std::string> selection = lubancode::cli::ReadLine("选择模型编号 [1]: ");
        if (!selection.has_value()) {
            return;
        }
        std::size_t idx = 0;
        if (!selection->empty()) {
            try {
                std::size_t consumed = 0;
                const int n = std::stoi(*selection, &consumed);
                if (consumed != selection->size() || n < 1 || static_cast<std::size_t>(n) > list_result->size()) {
                    std::cout << "编号不对,取消切换。\n";
                    return;
                }
                idx = static_cast<std::size_t>(n - 1);
            } catch (...) {
                std::cout << "没听懂,取消切换。\n";
                return;
            }
        }
        chosen = (*list_result)[idx].id;
    }

    *current_model = chosen;
    std::cout << "已切换到模型: " << chosen << "(本会话生效)\n";

    if (config_file_path.has_value()) {
        const std::optional<std::string> answer =
            lubancode::cli::ReadLine("写进配置文件 " + *config_file_path + "? [y/N]: ");
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            const auto updated = lubancode::config::UpdateModelInConfigFile(*config_file_path, chosen);
            if (updated.has_value()) {
                std::cout << "已更新 " << *config_file_path << "\n";
            } else {
                std::cout << "更新失败: " << updated.error() << "\n";
            }
        }
    } else {
        std::cout << "当前没有生效的配置文件,只在本会话生效。\n";
    }
}

// 没带参数时的交互循环:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符,继续等下一行(老规则"空行退出"跟
// Windows 控制台偶发读空串的老毛病撞在一块,会把读空串误当成用户要退出,
// 改成只认 exit/quit/EOF 才靠谱)。
// AgentLoop 用 std::optional 包着,存一份在循环外面,历史跨轮保留;
// /clear 需要清空历史,而 AgentLoop 的历史是私有成员、没有 Clear()(agent
// 层现有文件不让动),唯一的办法是就地重新构造一份全新的 AgentLoop——
// optional::emplace 走的是构造而不是赋值,AgentLoop 引用成员导致的
// "不可赋值"不影响这条路。
// always_allowed_tools 同样在循环外面建一次,"本会话总是允许"才能真的跨
// 多轮用户输入生效,/clear 不清空它(清没清对话历史跟工具授权是两码事)。
// config_result.config.model 是四级合并出来的初始 model,current_model 是
// 真正"这一刻发请求用哪个 model"的会话级状态,两者可能因为 /model 切换而
// 不一致——ModelOverrideBackend 在真正发请求前把 Request.model 换成
// *current_model,这样 /model 切换才能不碰 agent/loop.hpp/.cpp 就真正生效。
void InteractiveLoop(lubancode::config::ConfigResult config_result, bool auto_confirm,
                      const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled) {
    const lubancode::config::Config& config = config_result.config;

    std::unique_ptr<lubancode::api::Backend> real_backend = BuildBackend(config);
    auto current_model = std::make_shared<std::string>(config.model);
    ModelOverrideBackend model_backend(*real_backend, current_model);
    // SpinnerBackend 包最外层:每次 send_stream 起转轮,收到第一个流事件就
    // 停,ModelOverrideBackend 负责 /model 切换生效,两层包装顺序不影响
    // 语义(model 补丁在更内层做,转轮只关心"发出去了没有第一个字节回来")。
    SpinnerBackend wrapped_backend(model_backend, theme, spinner_enabled);

    PrintBanner(config, theme);

    lubancode::tools::ToolRegistry registry = BuildToolRegistry();

    std::optional<lubancode::agent::AgentLoop> loop;
    const auto rebuild_loop = [&]() {
        // max_tokens=4096、max_turns=25 是 AgentLoop 自己的默认值,这里显式
        // 传出来是为了能把 config.max_context_chars 一起传进去。
        loop.emplace(wrapped_backend, registry, config.model,
                     lubancode::agent::BuildSystemPrompt(CurrentDirUtf8(), persona),
                     /*max_tokens=*/4096, /*max_turns=*/25, config.max_context_chars);
    };
    rebuild_loop();

    std::set<std::string> always_allowed_tools;
    std::optional<std::string> config_file_path = config_result.config_file_path;

    while (true) {
        const std::optional<std::string> line =
            lubancode::cli::ReadLine(theme.prompt + "> " + theme.reset, theme);
        if (!line.has_value()) {
            break;  // EOF:Ctrl+Z 或管道读尽
        }
        if (line->empty()) {
            continue;  // 空行不退出,重新给提示符
        }
        if (*line == "exit" || *line == "quit") {
            break;
        }

        const lubancode::cli::ParsedSlashCommand parsed = lubancode::cli::ParseSlashCommand(*line);
        if (parsed.command != lubancode::cli::SlashCommand::NotSlash) {
            switch (parsed.command) {
                case lubancode::cli::SlashCommand::Help:
                    PrintSlashHelp();
                    break;
                case lubancode::cli::SlashCommand::Model:
                    HandleModelCommand(parsed.args, config, current_model, config_file_path);
                    break;
                case lubancode::cli::SlashCommand::Config:
                    PrintConfigDiagnostics(config_result, *current_model);
                    break;
                case lubancode::cli::SlashCommand::Clear:
                    rebuild_loop();
                    std::cout << "已清空对话历史。\n";
                    break;
                case lubancode::cli::SlashCommand::Exit:
                    return;
                case lubancode::cli::SlashCommand::Unknown:
                    std::cout << "不认得命令 " << parsed.raw_word << ",试试 /help\n";
                    break;
                case lubancode::cli::SlashCommand::NotSlash:
                    break;  // 走不到这里,switch 外层已经排除了
            }
            continue;
        }

        RunTurn(*loop, *line, auto_confirm, always_allowed_tools, theme);
    }
}

// 单发模式(位置参数):也走 agent loop,同样支持工具,只是只问这一句。
// 管道/单发场景下 spinner_enabled 传进来的必然是 false(RunCli 里按
// DetectConsoleCapability().is_console 算好的),这里不用再判断一次。
int AskOnce(const lubancode::config::Config& config, const std::string& question, bool auto_confirm,
            const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled) {
    std::unique_ptr<lubancode::api::Backend> backend = BuildBackend(config);
    SpinnerBackend wrapped_backend(*backend, theme, spinner_enabled);
    lubancode::tools::ToolRegistry registry = BuildToolRegistry();
    lubancode::agent::AgentLoop loop(wrapped_backend, registry, config.model,
                                      lubancode::agent::BuildSystemPrompt(CurrentDirUtf8(), persona),
                                      /*max_tokens=*/4096, /*max_turns=*/25, config.max_context_chars);
    std::set<std::string> always_allowed_tools;

    return RunTurn(loop, question, auto_confirm, always_allowed_tools, theme);
}

// 真正的入口逻辑,跟平台无关:args[0] 是程序名,args[1..] 是实参。
// Windows 下 argv 单独处理(见文件末尾的 wmain),是为了绕开 Windows
// 那套"窄字符 argv 按系统 ANSI 代码页解码"的老规矩——命令行里的中文字符
// 一旦经这条路转一圈,就会被拆成不合法的 UTF-8 字节,喂给 nlohmann::json
// 的 dump() 时直接抛 type_error(316: invalid UTF-8 byte)崩掉。
int RunCli(const std::vector<std::string>& args) {
    std::string positional;
    bool auto_confirm = false;
    bool print_config = false;
    std::string system_prompt_file_arg;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--version") {
            PrintVersion();
            return 0;
        }
        if (arg == "--help") {
            PrintHelp();
            return 0;
        }
        if (arg == "--yes") {
            auto_confirm = true;
            continue;
        }
        if (arg == "--config") {
            print_config = true;
            continue;
        }
        if (arg == "--system-prompt") {
            if (i + 1 >= args.size()) {
                std::cerr << "--system-prompt 后面要跟一个文件路径\n";
                return 1;
            }
            system_prompt_file_arg = args[++i];
            continue;
        }
        if (!positional.empty()) {
            positional += " ";
        }
        positional += arg;
    }

    const auto config_result = lubancode::config::LoadFromEnv();
    if (!config_result.has_value()) {
        std::cerr << config_result.error() << "\n";
        return 1;
    }
    if (config_result->migration_notice.has_value()) {
        std::cout << *config_result->migration_notice << "\n";
    }

    if (print_config) {
        PrintConfigDiagnostics(*config_result);
        return 0;
    }

    // --system-prompt 命令行参数压过配置文件里的 system_prompt_file 字段
    // (四级合并已经把 config.system_prompt_file 算好了,这里只是命令行
    // 再压一级)。只替换人格段,工作目录、工具调用这些运行必需的上下文
    // (prompts.hpp 的 EnvironmentSegment)照样由 BuildSystemPrompt 追加,
    // 不受这里影响。
    const std::string effective_prompt_file =
        !system_prompt_file_arg.empty() ? system_prompt_file_arg : config_result->config.system_prompt_file;
    std::string persona;
    if (!effective_prompt_file.empty()) {
        const auto persona_result = lubancode::config::ReadSystemPromptFile(effective_prompt_file);
        if (!persona_result.has_value()) {
            std::cerr << persona_result.error() << "\n";
            return 1;
        }
        persona = *persona_result;
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
    lubancode::cli::SetConfirmMode(auto_confirm ? lubancode::cli::ConfirmMode::Yolo
                                                 : lubancode::cli::ConfirmMode::Confirm);

    // 兜底:JSON 编码、网络库内部等地方万一抛出没接住的异常,也不能让
    // 整个进程崩掉(崩掉的话用户只会看到一个莫名其妙的退出码)。
    try {
        if (!positional.empty()) {
            // 单发模式/管道模式:不进向导(没有交互终端可问),缺配置直接
            // 报可读的错,指路三条配置途径。
            const auto configured_check = lubancode::config::RequireConfigured(*config_result);
            if (!configured_check.has_value()) {
                std::cerr << configured_check.error() << "\n";
                return 1;
            }
            return AskOnce(config_result->config, positional, auto_confirm, theme, persona, spinner_enabled);
        }

        // 交互模式:base_url/api_key/model 有一个解不出来,就先走一遍初次
        // 配置向导——三个字段都没有内置默认值,任何一个空着都没法真的
        // 跟模型对话,不如趁手就问清楚(即便本次规矩里描述的触发条件只提了
        // base_url/api_key,这里多加一条 model 判断更稳妥,免得 env 只配了
        // base_url/api_key 漏了 model,走进会话却发不出请求)。
        lubancode::config::ConfigResult effective = *config_result;
        if (effective.config.base_url.empty() || effective.config.auth_token.empty() ||
            effective.config.model.empty()) {
            const auto wizard_config = RunInitialSetupWizard(effective.config_file_path);
            if (!wizard_config.has_value()) {
                std::cerr << "配置向导未完成,退出。\n";
                return 1;
            }
            effective.config = *wizard_config;
            // 向导给出的这份配置,来源标记简化成两种:保存了就算"配置文件"
            // 来源,没保存就算"内置默认值"(最接近"临时值,没有更合适的
            // 持久来源"这个语义)——/config 展示用,不影响实际发请求。
            const lubancode::config::Source marked = effective.config_file_path.has_value()
                                                          ? lubancode::config::Source::ConfigFile
                                                          : lubancode::config::Source::Default;
            effective.sources.wire = marked;
            effective.sources.base_url = marked;
            effective.sources.auth_token = marked;
            effective.sources.model = marked;
        }
        InteractiveLoop(effective, auto_confirm, theme, persona, spinner_enabled);
    } catch (const std::exception& e) {
        std::cerr << "[错误] 未预料的异常: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace

#ifdef _WIN32

// Windows 下用宽字符入口:窄字符 main(argc, char**) 的 argv 是 CRT 按
// "系统 ANSI 代码页"(不是 UTF-8)解码来的,中文命令行参数会被解码错。
// wmain 拿到的是原始的 UTF-16 参数,自己用 WideCharToMultiByte 转成
// UTF-8,才能跟程序内部统一按 UTF-8 处理的字符串对上。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string utf8;
        if (utf8_len > 0) {
            utf8.resize(static_cast<std::size_t>(utf8_len - 1));  // 去掉结尾的 \0
            if (utf8_len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, utf8.data(), utf8_len, nullptr, nullptr);
            }
        }
        args.push_back(std::move(utf8));
    }
    return RunCli(args);
}

#else

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    return RunCli(args);
}

#endif
