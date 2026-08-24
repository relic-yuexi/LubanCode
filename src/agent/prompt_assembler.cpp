#include "agent/prompt_assembler.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

#include "embedded_prompts.hpp"  // 构建期生成:<build>/generated/embedded_prompts.hpp

namespace lubancode::agent {

namespace {

// 本机今天的日期,YYYY-MM-DD。系统提示在会话构造时拼一次,跨天长会话里
// 这行会陈旧——跟 cwd 一个待遇,够用,不为它做逐轮刷新。
std::string TodayLocalDate() {
    const std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char buf[16] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return buf;
}

constexpr const char* kOsLabel =
#ifdef _WIN32
    "Windows";
#elif defined(__APPLE__)
    "macOS";
#else
    "Linux";
#endif

// UTF-8 字符串 -> fs::path,不走系统 ANSI 代码页(跟 config/tools 各处同一
// 套写法;agent 层不依赖 config 层,这里自备一份小的)。
std::filesystem::path Utf8Path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// CRLF 归一 + 剥两端空白——跟 embed_prompts.cmake 嵌入时做的同一套归一,
// 用户文件"没改内容只换了行尾"也能跟嵌入版逐字节对上。
std::string NormalizeModuleText(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        if (c != '\r') {
            out += c;
        }
    }
    std::size_t begin = 0;
    while (begin < out.size() && (out[begin] == ' ' || out[begin] == '\t' || out[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = out.size();
    while (end > begin && (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\n')) {
        --end;
    }
    return out.substr(begin, end - begin);
}

// 读用户目录里的一个模块文件:存在、可读、归一后非空才算数,否则 nullopt
// (= 回退嵌入版)。读盘失败不吭声——运行期永远有嵌入回退,不拦人。
std::optional<std::string> ReadUserModule(const std::string& prompts_dir, const char* rel_path) {
    if (prompts_dir.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path path = Utf8Path(prompts_dir) / Utf8Path(rel_path);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec || std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = NormalizeModuleText(buffer.str());
    if (content.empty()) {
        return std::nullopt;
    }
    return content;
}

// 按相对路径取模块正文:用户文件优先,嵌入版回退。
std::string ModuleText(const std::string& prompts_dir, const embedded::EmbeddedModule& module) {
    if (auto user = ReadUserModule(prompts_dir, module.rel_path); user.has_value()) {
        return std::move(*user);
    }
    return module.content;
}

// kAllModules 里按相对路径找一条(找不到回退空——总表跟拼装代码同一次
// 构建生成,正常永远找得到,防御而已)。
const embedded::EmbeddedModule* FindModule(std::string_view rel_path) {
    for (const auto& module : embedded::kAllModules) {
        if (rel_path == module.rel_path) {
            return &module;
        }
    }
    return nullptr;
}

std::string ModuleByPath(const std::string& prompts_dir, std::string_view rel_path) {
    const embedded::EmbeddedModule* module = FindModule(rel_path);
    if (module == nullptr) {
        return std::string();
    }
    return ModuleText(prompts_dir, *module);
}

}  // namespace

std::string ModuleTextByPath(const std::string& prompts_dir, const std::string& rel_path) {
    return ModuleByPath(prompts_dir, rel_path);
}

std::string AssembledCorePersona(const std::string& prompts_dir) {
    std::string out;
    for (const auto& module : embedded::kAllModules) {
        if (std::string_view(module.rel_path).substr(0, 5) != "core/") {
            continue;
        }
        if (!out.empty()) {
            out += "\n\n";
        }
        out += ModuleText(prompts_dir, module);
    }
    return out;
}

std::string AssembledDefaultPersona() {
    return AssembledCorePersona(std::string());
}

std::vector<std::pair<std::string, std::string>> PromptModuleSeeds() {
    std::vector<std::pair<std::string, std::string>> seeds;
    for (const auto& module : embedded::kAllModules) {
        seeds.emplace_back(module.rel_path, module.content);
    }
    return seeds;
}

std::vector<PromptModuleSource> PromptModuleSources(const std::string& prompts_dir) {
    std::vector<PromptModuleSource> sources;
    for (const auto& module : embedded::kAllModules) {
        PromptModuleSource source;
        source.rel_path = module.rel_path;
        if (const auto user = ReadUserModule(prompts_dir, module.rel_path); user.has_value()) {
            source.from_user_file = true;
            source.differs_from_embedded = *user != module.content;
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

std::string BuildEnvironmentSegment(const std::string& cwd, const std::string& current_date) {
    return "# 运行环境\n\n- 工作目录: " + cwd + "\n- 今天日期: " +
           (current_date.empty() ? TodayLocalDate() : current_date) + "\n- 操作系统: " + kOsLabel +
           "\n\n凡是能动手做的事(读文件、跑命令、改文件……),优先调用工具去做,不要凭空猜测或编造结果——"
           "这条不受上面人格设定的影响,该用工具时就用。";
}

std::string AssembleSystemPrompt(const PromptOptions& options) {
    // 人格:法/CLI 非空整段替换 core;空串走 core 模块(用户文件优先,
    // 嵌入回退)。
    std::string prompt = options.persona.empty() ? AssembledCorePersona(options.prompts_dir) : options.persona;
    const auto append = [&prompt](const std::string& segment) {
        prompt += "\n\n";
        prompt += segment;
    };

    append(BuildEnvironmentSegment(options.cwd, options.current_date));

    if (!options.project_instructions.empty()) {
        append(options.project_instructions);
    }

    // 恒在的四件套:基础工具的方针跟着工具走,不跟人格走。
    append(ModuleByPath(options.prompts_dir, "features/files.md"));
    append(ModuleByPath(options.prompts_dir, "features/shell.md"));
    append(ModuleByPath(options.prompts_dir, "features/delegation.md"));
    append(ModuleByPath(options.prompts_dir, "features/todo.md"));

    // 条件注入:没启用的能力一个字不占。
    if (!options.skills_segment.empty()) {
        append(ModuleByPath(options.prompts_dir, "features/skills.md"));
        append(options.skills_segment);  // 模块讲规矩,清单紧随其后
    }
    if (options.web) {
        append(ModuleByPath(options.prompts_dir, "features/web.md"));
    }
    if (options.mcp) {
        append(ModuleByPath(options.prompts_dir, "features/mcp.md"));
    }
    if (options.lsp) {
        append(ModuleByPath(options.prompts_dir, "features/lsp.md"));
    }

    // 平台段按 wire 注一个;认不出的 wire 不注,不瞎猜。wire 名以
    // ProviderWireName 的规范名(wire 更名单,2026-08)为准,旧名一并认——
    // 这里吃的是字符串,调用方(喂 ProviderWireName 的那三处)与规范名
    // 之间的口径差不许把平台段悄悄漏掉。
    if (options.wire == "anthropic-messages" || options.wire == "anthropic") {
        append(ModuleByPath(options.prompts_dir, "platforms/anthropic.md"));
    } else if (options.wire == "openai-responses" || options.wire == "responses") {
        append(ModuleByPath(options.prompts_dir, "platforms/responses.md"));
    } else if (options.wire == "openai-chat-completions" || options.wire == "chat_completions" ||
               options.wire == "chat") {
        append(ModuleByPath(options.prompts_dir, "platforms/chat_completions.md"));
    }

    // 模式段殿后(Plan 模式单):宿主内置,不看用户目录。Default 也注——
    // 模板里明说"旧 Plan 指令已结束",防模型带着上一档的规矩跑(单子:
    // Codex 公开 issue 出过这类残留)。
    append(ModeInstructionSegment(options.plan_mode));
    return prompt;
}

std::string ModeInstructionSegment(bool plan_mode) {
    // modes/ 组不走 kAllModules 的"用户文件优先"回路——嵌入版即真值
    //(单子:mode instructions 宿主内置,不给项目覆盖)。embed 脚本按文件名
    // 生成 kMode_<stem>(plan/default),这里按名字直取;构建期 GLOB 保证
    // 在,防御分支返回空串,拼装侧空段不 append。
    return std::string(plan_mode ? embedded::kMode_plan : embedded::kMode_default);
}

}  // namespace lubancode::agent
