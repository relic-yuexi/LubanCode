#include "agent/prompt_assembler.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "embedded_prompts.hpp"  // 构建期生成:<build>/generated/embedded_prompts.hpp
#include "platform/paths.hpp"    // PathToUtf8:账本里的文件路径不走 ACP 窄口

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

// 读模块目录里的一个模块文件:存在、可读、归一后非空才算数,返回正文与
// UTF-8 全路径(账本要记哪层哪文件),否则 nullopt(= 回退下一层)。读盘
// 失败不吭声——运行期永远有嵌入回退,不拦人。
struct ModuleFile {
    std::string content;
    std::string path_utf8;
};
std::optional<ModuleFile> ReadModuleFile(const std::string& dir, const std::string& rel_path) {
    if (dir.empty()) {
        return std::nullopt;
    }
    const std::filesystem::path path = Utf8Path(dir) / Utf8Path(rel_path);
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
    ModuleFile out;
    out.content = std::move(content);
    out.path_utf8 = platform::PathToUtf8(path);
    return out;
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

// kProfileModules 里按带前缀的相对路径(profiles/<名>/<相对路径>)找一条。
const embedded::EmbeddedModule* FindProfileModule(std::string_view profile_rel_path) {
    for (const auto& module : embedded::kProfileModules) {
        if (profile_rel_path == module.rel_path) {
            return &module;
        }
    }
    return nullptr;
}

// 一个模块经五层解析后的结果:正文 + 来源账(契约 §6.2)。
//   内置 default -> 用户全局 default -> 内置选中 Profile -> 用户选中
//   Profile -> 项目选中 Profile
// Profile 不生效(profile 空或 "default")时只走前两层,与 0.21.x 的
// "用户文件优先、嵌入回退"逐字节同路——default 上下文的黄金基线钉这
// 一点。下层缺席(文件不存在/空白/目录没有)就停在该层,稳稳退回下一层。
//
// 包层(统一 Package 封装单阶段 3,契约 §6.1 列了"Package 覆盖"这个来源、
// §6.2 的五层次序没给它定位)在此定死:**canonical 名("<包id>:<名>")
// 的 Profile 只在包根里解析,裸名 Profile 只在内置/用户/项目三层里解析,
// 两套命名空间不相交**。这样定的一句理:包内 Profile 是包给自家 Agent
// 配的套件,用户/项目要盖它,不是去包目录里改文件,而是给自己的 Agent
// 点自家裸名 Profile——本地主人自家的话,永远走自家的层。裸名各层的
// 五层次序一个字符不动(黄金基线续钉)。
struct ResolvedModule {
    std::string content;
    PromptSourceLedgerEntry entry;
};

// canonical 名的包层解析:profile = "<包id>:<名>",在 package_roots 里找
// 同包 id 的根,读 <根>/<名>/<相对路径>。找不到包或缺文件 = nullopt
//(停在该层,回退已解析的正文)。
std::optional<ModuleFile> ReadPackagedProfileModule(const std::vector<PackageProfileRoot>& roots,
                                                    const std::string& profile, std::string_view rel_path) {
    const std::size_t colon = profile.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= profile.size()) {
        return std::nullopt;
    }
    const std::string package_id = profile.substr(0, colon);
    const std::string local = profile.substr(colon + 1);
    for (const PackageProfileRoot& root : roots) {
        if (root.package_id != package_id) continue;
        return ReadModuleFile(root.profiles_dir_utf8, local + "/" + std::string(rel_path));
    }
    return std::nullopt;
}

// Profile 名是否 canonical("<包id>:<名>" 形状):只在有名有冒号时认,
// 不再深究包 id 合法性——包层挂载材料只会给出真包 id,这里宽松些只为
// 分流(非 canonical 的带冒号名找不到包根,自然全程缺席)。
bool IsCanonicalProfileName(const std::string& profile) {
    return profile.find(':') != std::string::npos;
}

ResolvedModule ResolveModule(const std::string& profile, const std::string& prompts_dir,
                             const std::string& project_prompts_dir,
                             const std::vector<PackageProfileRoot>& package_roots,
                             std::string_view rel_path) {
    ResolvedModule out;
    out.entry.rel_path = std::string(rel_path);
    if (const embedded::EmbeddedModule* module = FindModule(rel_path); module != nullptr) {
        out.content = module->content;
        out.entry.origin = PromptModuleOrigin::EmbeddedDefault;
    }
    if (auto user = ReadModuleFile(prompts_dir, out.entry.rel_path); user.has_value()) {
        out.content = std::move(user->content);
        out.entry.origin = PromptModuleOrigin::UserDefault;
        out.entry.file = std::move(user->path_utf8);
    }
    if (IsPromptProfileActive(profile)) {
        if (IsCanonicalProfileName(profile)) {
            // canonical 名:只走包层(命名空间不相交,见上注)。
            if (auto packaged = ReadPackagedProfileModule(package_roots, profile, rel_path);
                packaged.has_value()) {
                out.content = std::move(packaged->content);
                out.entry.origin = PromptModuleOrigin::PackageProfile;
                out.entry.profile = profile;
                out.entry.file = std::move(packaged->path_utf8);
            }
            return out;
        }
        const std::string profile_rel = "profiles/" + profile + "/" + out.entry.rel_path;
        if (const embedded::EmbeddedModule* module = FindProfileModule(profile_rel); module != nullptr) {
            out.content = module->content;
            out.entry.origin = PromptModuleOrigin::EmbeddedProfile;
            out.entry.profile = profile;
            out.entry.file.clear();
        }
        if (auto user = ReadModuleFile(prompts_dir, profile_rel); user.has_value()) {
            out.content = std::move(user->content);
            out.entry.origin = PromptModuleOrigin::UserProfile;
            out.entry.profile = profile;
            out.entry.file = std::move(user->path_utf8);
        }
        if (auto project = ReadModuleFile(project_prompts_dir, profile_rel); project.has_value()) {
            out.content = std::move(project->content);
            out.entry.origin = PromptModuleOrigin::ProjectProfile;
            out.entry.profile = profile;
            out.entry.file = std::move(project->path_utf8);
        }
    }
    return out;
}

// 老回路(0.21.x):用户文件优先、嵌入回退,不走 Profile 层。
// ModuleTextByPath(系统提示之外的消费方)与 AssembledCorePersona(default
// 上下文的人格段)沿用。
std::string ModuleText(const std::string& prompts_dir, const embedded::EmbeddedModule& module) {
    if (auto user = ReadModuleFile(prompts_dir, module.rel_path); user.has_value()) {
        return std::move(user->content);
    }
    return module.content;
}

std::string ModuleByPath(const std::string& prompts_dir, std::string_view rel_path) {
    const embedded::EmbeddedModule* module = FindModule(rel_path);
    if (module == nullptr) {
        return std::string();
    }
    return ModuleText(prompts_dir, *module);
}

// 账本里塞一条的顺手封装。
void LedgerAdd(PromptSourceLedger* ledger, PromptSourceLedgerEntry entry) {
    if (ledger != nullptr) {
        ledger->entries.push_back(std::move(entry));
    }
}

}  // namespace

// Profile 名是否生效(契约 §4.2):空 = 没选;显式 "default" = 强制用内置
// 默认。两者都不走 Profile 覆盖层。
bool IsPromptProfileActive(const std::string& profile) {
    return !profile.empty() && profile != "default";
}

PromptCapabilities DerivePromptCapabilities(const std::vector<std::string>& tool_names) {
    PromptCapabilities caps;
    for (const std::string& name : tool_names) {
        if (name == "read_file" || name == "write_file" || name == "edit_file" || name == "undo_file_edit") {
            caps.files = true;
        } else if (name == "run_command") {
            caps.shell = true;
        } else if (name == "agent") {
            caps.delegation = true;
        } else if (name == "todo_write") {
            caps.todo = true;
        } else if (name == "web_search" || name == "web_fetch") {
            caps.web = true;
        } else if (name.rfind("mcp__", 0) == 0) {
            caps.mcp = true;
        } else if (name == "lsp") {
            caps.lsp = true;
        }
    }
    return caps;
}

std::string ToString(PromptModuleOrigin origin) {
    switch (origin) {
        case PromptModuleOrigin::EmbeddedDefault:
            return "embedded default";
        case PromptModuleOrigin::UserDefault:
            return "user global default";
        case PromptModuleOrigin::EmbeddedProfile:
            return "embedded profile";
        case PromptModuleOrigin::UserProfile:
            return "user profile";
        case PromptModuleOrigin::ProjectProfile:
            return "project profile";
        case PromptModuleOrigin::PackageProfile:
            return "package profile";
        case PromptModuleOrigin::Persona:
            return "persona";
        case PromptModuleOrigin::RuntimeEnvironment:
            return "runtime environment";
        case PromptModuleOrigin::ProjectInstructions:
            return "project instructions";
        case PromptModuleOrigin::EmbeddedHostPolicy:
            return "embedded host policy";
    }
    return "unknown";
}

std::string PromptSourceLedgerEntry::FormatLine() const {
    std::string label = ToString(origin);
    if (!profile.empty()) {
        label += " " + profile;
    }
    if (!file.empty()) {
        label += " (" + file + ")";
    }
    return rel_path + " <- " + label;
}

const PromptSourceLedgerEntry* PromptSourceLedger::Find(const std::string& rel_path) const {
    for (const auto& entry : entries) {
        if (entry.rel_path == rel_path) {
            return &entry;
        }
    }
    return nullptr;
}

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
        if (const auto user = ReadModuleFile(prompts_dir, module.rel_path); user.has_value()) {
            source.from_user_file = true;
            source.differs_from_embedded = user->content != module.content;
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

std::string AssembleSystemPrompt(const PromptOptions& options, PromptSourceLedger* ledger) {
    // 人格:法/CLI 非空整段替换 core(账本记一条 persona,替掉的是整个
    // core 组);空串走 core 模块逐个过五层回路(default 上下文 = 用户文件
    // 优先、嵌入回退,与 0.21.x 同路)。
    std::string prompt;
    if (!options.persona.empty()) {
        prompt = options.persona;
        PromptSourceLedgerEntry entry;
        entry.rel_path = "core/*";
        entry.origin = PromptModuleOrigin::Persona;
        LedgerAdd(ledger, std::move(entry));
    } else {
        for (const auto& module : embedded::kAllModules) {
            if (std::string_view(module.rel_path).substr(0, 5) != "core/") {
                continue;
            }
            ResolvedModule resolved = ResolveModule(options.profile, options.prompts_dir,
                                                    options.project_prompts_dir,
                                                    options.package_profile_roots, module.rel_path);
            if (!prompt.empty()) {
                prompt += "\n\n";
            }
            prompt += resolved.content;
            LedgerAdd(ledger, std::move(resolved.entry));
        }
    }
    const auto append = [&prompt](const std::string& segment) {
        prompt += "\n\n";
        prompt += segment;
    };
    const auto append_module = [&](std::string_view rel_path) {
        ResolvedModule resolved = ResolveModule(options.profile, options.prompts_dir,
                                                options.project_prompts_dir,
                                                options.package_profile_roots, rel_path);
        append(resolved.content);
        LedgerAdd(ledger, std::move(resolved.entry));
    };

    append(BuildEnvironmentSegment(options.cwd, options.current_date));
    {
        PromptSourceLedgerEntry entry;
        entry.rel_path = "(runtime environment)";
        entry.origin = PromptModuleOrigin::RuntimeEnvironment;
        LedgerAdd(ledger, std::move(entry));
    }

    if (!options.project_instructions.empty()) {
        append(options.project_instructions);
        PromptSourceLedgerEntry entry;
        entry.rel_path = "(project instructions)";
        entry.origin = PromptModuleOrigin::ProjectInstructions;
        LedgerAdd(ledger, std::move(entry));
    }

    // 基础工具的方针跟着工具走,不跟人格走。自定义 Agent 带了能力推导
    // (PromptCapabilities)就按有效工具开合——裁掉的工具一个字不描述
    //(单子 §5.4);主 Agent 不带,恒在四件套照旧。
    const PromptCapabilities* caps = options.capabilities.has_value() ? &*options.capabilities : nullptr;
    const bool files_on = caps == nullptr || caps->files;
    const bool shell_on = caps == nullptr || caps->shell;
    const bool delegation_on = caps == nullptr || caps->delegation;
    const bool todo_on = caps == nullptr || caps->todo;
    if (files_on) {
        append_module("features/files.md");
    }
    if (shell_on) {
        append_module("features/shell.md");
    }
    if (delegation_on) {
        append_module("features/delegation.md");
    }
    if (todo_on) {
        append_module("features/todo.md");
    }

    // 条件注入:没启用的能力一个字不占。自定义 Agent 的 web/mcp/lsp 只认
    // 能力推导(父会话的配置开关不越界代言——工具都不在表里,文案不装);
    // 主 Agent 照旧吃配置开关。
    if (!options.skills_segment.empty()) {
        append_module("features/skills.md");
        append(options.skills_segment);  // 模块讲规矩,清单紧随其后
    }
    const bool web_on = caps != nullptr ? caps->web : options.web;
    const bool mcp_on = caps != nullptr ? caps->mcp : options.mcp;
    const bool lsp_on = caps != nullptr ? caps->lsp : options.lsp;
    if (web_on) {
        append_module("features/web.md");
    }
    if (mcp_on) {
        append_module("features/mcp.md");
    }
    if (lsp_on) {
        append_module("features/lsp.md");
    }

    // 平台段按 wire 注一个;认不出的 wire 不注,不瞎猜。wire 名以
    // ProviderWireName 的规范名(wire 更名单,2026-08)为准,旧名一并认——
    // 这里吃的是字符串,调用方(喂 ProviderWireName 的那三处)与规范名
    // 之间的口径差不许把平台段悄悄漏掉。
    if (options.wire == "anthropic-messages" || options.wire == "anthropic") {
        append_module("platforms/anthropic.md");
    } else if (options.wire == "openai-responses" || options.wire == "responses") {
        append_module("platforms/responses.md");
    } else if (options.wire == "openai-chat-completions" || options.wire == "chat_completions" ||
               options.wire == "chat") {
        append_module("platforms/chat_completions.md");
    }

    // 模式段殿后(Plan 模式单):宿主内置,不看用户目录。Default 也注——
    // 模板里明说"旧 Plan 指令已结束",防模型带着上一档的规矩跑(单子:
    // Codex 公开 issue 出过这类残留)。
    append(ModeInstructionSegment(options.plan_mode));
    {
        PromptSourceLedgerEntry entry;
        entry.rel_path = options.plan_mode ? "modes/plan.md" : "modes/default.md";
        entry.origin = PromptModuleOrigin::EmbeddedHostPolicy;
        LedgerAdd(ledger, std::move(entry));
    }
    return prompt;
}

PromptSourceLedger BuildPromptProfileLedger(const std::string& profile, const std::string& prompts_dir,
                                            const std::string& project_prompts_dir,
                                            const std::vector<PackageProfileRoot>& package_roots) {
    PromptSourceLedger ledger;
    for (const auto& module : embedded::kAllModules) {
        ResolvedModule resolved =
            ResolveModule(profile, prompts_dir, project_prompts_dir, package_roots, module.rel_path);
        ledger.entries.push_back(std::move(resolved.entry));
    }
    PromptSourceLedgerEntry mode_entry;
    mode_entry.rel_path = "modes/default.md";
    mode_entry.origin = PromptModuleOrigin::EmbeddedHostPolicy;
    ledger.entries.push_back(std::move(mode_entry));
    return ledger;
}

std::string ModeInstructionSegment(bool plan_mode) {
    // modes/ 组不走 kAllModules 的"用户文件优先"回路——嵌入版即真值
    //(单子:mode instructions 宿主内置,不给项目覆盖)。embed 脚本按文件名
    // 生成 kMode_<stem>(plan/default),这里按名字直取;构建期 GLOB 保证
    // 在,防御分支返回空串,拼装侧空段不 append。
    return std::string(plan_mode ? embedded::kMode_plan : embedded::kMode_default);
}

}  // namespace lubancode::agent
