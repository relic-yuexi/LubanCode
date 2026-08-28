// /agents 与 /agent doctor 的实现:Catalog 现扫现列、doctor 静态预检。
// 输出走 cli/terminal_port(散打 std::cout 清零的仓库规矩)。
#include "app/commands/agent_commands.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"
#include "config/config.hpp"                  // HomeLubancodeDir
#include "config/project_instructions.hpp"    // FindProjectRoot(项目层根)
#include "platform/paths.hpp"

using lubancode::cli::TermOut;

namespace lubancode::app {

namespace {

// 嵌入式资源里的 builtin Agent 目录:<exe 目录>/agents(与官方 skills 同一
// 相对布局)。开发构建与发行包都没有这目录 = builtin 层只剩码内两条,静默。
std::optional<std::filesystem::path> EmbeddedAgentsDir() {
    const auto executable = lubancode::platform::ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path candidate = executable->parent_path() / "agents";
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        return std::nullopt;
    }
    return candidate;
}

// 模型/Profile/工具三行的展示值:省了就写"继承",不写空串让人猜。
std::string DescribeModelRole(const lubancode::agent::AgentDefinition& def) {
    return def.model.role.empty() ? std::string("inherit") : def.model.role;
}

std::string DescribeEffort(const lubancode::agent::AgentDefinition& def) {
    return def.model.effort.empty() ? std::string("inherit") : def.model.effort;
}

std::string DescribeProfile(const lubancode::agent::AgentDefinition& def) {
    return def.prompt.profile.has_value() ? *def.prompt.profile : std::string("继承(落回 default)");
}

std::string DescribeTools(const lubancode::agent::AgentDefinition& def) {
    if (def.tools.allow.empty() && def.tools.deny.empty()) {
        return "全量(未裁)";
    }
    std::string out = std::to_string(def.tools.allow.size()) + " allow";
    if (!def.tools.deny.empty()) {
        out += " / " + std::to_string(def.tools.deny.size()) + " deny";
    }
    return out;
}

// 交集(保 allow 原序):allow 与 deny 撞名时 deny 胜出(单子测试账),
// doctor 把撞上的名字摆出来。
std::vector<std::string> AllowDenyOverlap(const lubancode::agent::AgentDefinition& def) {
    std::set<std::string> deny(def.tools.deny.begin(), def.tools.deny.end());
    std::vector<std::string> out;
    for (const std::string& name : def.tools.allow) {
        if (deny.count(name) != 0) {
            out.push_back(name);
        }
    }
    return out;
}

bool HasError(const lubancode::agent::AgentCatalogEntry& entry) {
    for (const auto& issue : entry.issues) {
        if (!issue.warning) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::string> FormatAgentCatalogListing(const lubancode::agent::AgentCatalog& catalog) {
    std::vector<std::string> lines;
    lines.push_back("Agent Catalog 共 " + std::to_string(catalog.entries.size()) +
                    " 个(优先级 project > user > builtin;/agent doctor <名字> 看静态预检):");
    for (const auto& entry : catalog.entries) {
        if (entry.available) {
            lines.push_back("  - " + entry.name + "  [" + lubancode::agent::ToString(entry.layer) + "]  可用");
        } else {
            lines.push_back("  - " + entry.name + "  [" + lubancode::agent::ToString(entry.layer) +
                            "]  不可用:" + entry.FirstError());
        }
        if (entry.definition.has_value()) {
            lines.push_back("      " + entry.definition->description);
            lines.push_back("      模型 " + DescribeModelRole(*entry.definition) + " · effort " +
                            DescribeEffort(*entry.definition) + " · Profile " +
                            DescribeProfile(*entry.definition) + " · 工具 " +
                            DescribeTools(*entry.definition) + " · 预装 Skill " +
                            std::to_string(entry.definition->skills_preload.size()));
        }
        for (const std::string& shadow : entry.shadowed_sources) {
            lines.push_back("      (盖住: " + shadow + ")");
        }
    }
    if (!catalog.load_errors.empty()) {
        lines.push_back("加载警告:");
        for (const std::string& error : catalog.load_errors) {
            lines.push_back("  - " + error);
        }
    }
    return lines;
}

std::vector<std::string> FormatAgentDoctorReport(const lubancode::agent::AgentCatalog& catalog,
                                                 const std::string& name, const AgentDoctorMaterials& materials) {
    std::vector<std::string> lines;
    const auto* entry = catalog.Find(name);
    if (entry == nullptr) {
        lines.push_back("没有叫 \"" + name + "\" 的 Agent(先 /agents 看清单;名字大小写敏感)。");
        return lines;
    }
    lines.push_back("agent doctor: " + entry->name);
    lines.push_back("来源: " + lubancode::agent::ToString(entry->layer) + " " + entry->file);
    if (!entry->shadowed_sources.empty()) {
        lines.push_back("覆盖链(被盖住的来源,优先级从高到低):");
        for (const std::string& shadow : entry->shadowed_sources) {
            lines.push_back("  - " + shadow);
        }
    }

    // ---- 定义本体:解析诊断逐条摆(错在前、警告在后,保持解析次序) ----
    if (entry->definition.has_value() && !HasError(*entry)) {
        lines.push_back("定义: 解析通过");
    } else {
        lines.push_back("定义: 不可用,诊断 " + std::to_string(entry->issues.size()) + " 条:");
    }
    for (const auto& issue : entry->issues) {
        lines.push_back(std::string("  [") + (issue.warning ? "警告" : "错误") + "] " + issue.Format(entry->file));
    }
    if (!entry->definition.has_value()) {
        lines.push_back("结论: 不可用 —— 定义没解析成,先把上面的错改了再查依赖。");
        return lines;
    }
    const auto& def = *entry->definition;

    // ---- 模型与 Profile:role 写法在此定死三档;能力校验属阶段 3 ----
    lines.push_back("模型: role=" + DescribeModelRole(def) + " · effort=" + DescribeEffort(def) +
                    "(档位是否越过 provider 能力,阶段 3 的 resolver 查)");

    // ---- Profile:名字登记;存在性与来源账本(PromptSourceLedger)属阶段 2 ----
    lines.push_back("Profile: " + DescribeProfile(def) +
                        "(模块存在性与来源账本属阶段 2 Prompt Profile,此处只登记名字)");

    // ---- Skill 预装 ----
    if (def.skills_preload.empty()) {
        lines.push_back("Skill 预装: 无");
    } else {
        std::string text = "Skill 预装: ";
        for (std::size_t i = 0; i < def.skills_preload.size(); ++i) {
            if (i != 0) {
                text += "; ";
            }
            text += def.skills_preload[i];
            if (materials.skills != nullptr) {
                bool found = false;
                for (const auto& skill : *materials.skills) {
                    if (skill.name == def.skills_preload[i]) {
                        found = true;
                        break;
                    }
                }
                text += found ? " ✓" : " ✗(不在已扫描技能清单)";
            }
        }
        lines.push_back(std::move(text));
    }

    // ---- 工具引用:allow/deny/requires 对注册表;交叠点名列出(deny 胜出) ----
    if (materials.registry != nullptr) {
        const auto check_list = [&](const std::vector<std::string>& names, const std::string& label) {
            if (names.empty()) {
                return std::string();
            }
            std::string text = label + ": ";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    text += "; ";
                }
                text += names[i];
                text += materials.registry->Find(names[i]) != nullptr ? " ✓" : " ✗(当前会话注册表里没有)";
            }
            return text;
        };
        if (std::string text = check_list(def.tools.allow, "tools.allow"); !text.empty()) {
            lines.push_back(std::move(text));
        }
        if (std::string text = check_list(def.tools.deny, "tools.deny"); !text.empty()) {
            lines.push_back(std::move(text));
        }
        if (std::string text = check_list(def.requires_tools, "requires.tools"); !text.empty()) {
            lines.push_back(std::move(text));
        }
    } else {
        lines.push_back("工具引用: 会话工具表不可用,跳过比对");
    }
    if (const std::vector<std::string> overlap = AllowDenyOverlap(def); !overlap.empty()) {
        std::string text = "allow 与 deny 交叠: ";
        for (std::size_t i = 0; i < overlap.size(); ++i) {
            text += (i == 0 ? "" : "; ") + overlap[i];
        }
        text += "(deny 胜出)";
        lines.push_back(std::move(text));
    }

    // ---- MCP:只许引用已挂载的服务名 ----
    if (def.mcp_servers.empty()) {
        lines.push_back("MCP: 无");
    } else {
        std::string text = "MCP: ";
        for (std::size_t i = 0; i < def.mcp_servers.size(); ++i) {
            if (i != 0) {
                text += "; ";
            }
            text += def.mcp_servers[i];
            if (materials.mcp_server_names != nullptr) {
                const bool mounted = std::find(materials.mcp_server_names->begin(),
                                               materials.mcp_server_names->end(),
                                               def.mcp_servers[i]) != materials.mcp_server_names->end();
                text += mounted ? " ✓ 已挂载" : " ✗ 未挂载";
            }
        }
        lines.push_back(std::move(text));
    }

    // ---- runtime 与 permissions:登账;权限越界比对属阶段 3 ----
    std::string runtime = "runtime: max_steps_per_turn=";
    runtime += def.max_steps_per_turn.has_value() ? std::to_string(*def.max_steps_per_turn) : std::string("继承");
    runtime += " · execution_mode=" + (def.execution_mode.empty() ? std::string("auto") : def.execution_mode);
    runtime += " · isolation=" + (def.isolation.empty() ? std::string("none") : def.isolation);
    lines.push_back(std::move(runtime));
    lines.push_back("permissions: " + (def.permissions_mode.empty() ? std::string("inherit") : def.permissions_mode) +
                    "(只能比父 Agent 更窄;越界比对属阶段 3)");

    // ---- 结论:定义解析过 ≠ 依赖齐;缺项如实数出来 ----
    std::size_t problems = 0;
    for (const std::string& line : lines) {
        if (line.find(" ✗") != std::string::npos) {
            ++problems;
        }
    }
    if (entry->available && problems == 0) {
        lines.push_back("结论: 静态预检通过(没发现缺项;运行期合并父上下文是阶段 3 的事)。");
    } else if (entry->available) {
        lines.push_back("结论: 定义可用,但静态预检发现 " + std::to_string(problems) +
                        " 处缺项(派活时 resolver 会按 requires 报缺,不会悄悄放宽)。");
    } else {
        lines.push_back("结论: 不可用 —— " + entry->FirstError());
    }
    return lines;
}

lubancode::agent::AgentCatalogScanRoots ComputeAgentScanRoots() {
    lubancode::agent::AgentCatalogScanRoots roots;
    roots.builtin_dir = EmbeddedAgentsDir();
    if (const auto home = lubancode::config::HomeLubancodeDir(); home.has_value()) {
        roots.user_dir = lubancode::platform::Utf8ToPath(*home) / "agents";
    }
    roots.project_dir = lubancode::config::FindProjectRoot(std::filesystem::current_path()) / ".lubancode" /
                        "agents";
    return roots;
}

CommandFlow HandleSlashAgents(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)ctx;
    (void)parsed;
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(ComputeAgentScanRoots());
    for (const std::string& line : FormatAgentCatalogListing(catalog)) {
        TermOut() << line << "\n";
    }
    return CommandFlow::Continue;
}

CommandFlow HandleSlashAgent(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // 拆子命令与名字(名可含连字符,不能按词数硬拆,取第一个词后全部当名字)。
    std::string sub = parsed.args;
    std::string rest;
    const std::size_t space = parsed.args.find_first_of(" \t");
    if (space != std::string::npos) {
        sub = parsed.args.substr(0, space);
        rest = parsed.args.substr(space + 1);
    }
    const auto trim = [](std::string value) {
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos) {
            return std::string();
        }
        const auto end = value.find_last_not_of(" \t");
        return value.substr(begin, end - begin + 1);
    };
    rest = trim(rest);

    if (sub.empty()) {
        TermOut() << "用法:/agent doctor <名字>(静态预检,只查不改)。/agents 列清单。\n";
        return CommandFlow::Continue;
    }
    if (sub == "doctor") {
        if (rest.empty()) {
            TermOut() << "用法:/agent doctor <名字>(名字看 /agents;大小写敏感)。\n";
            return CommandFlow::Continue;
        }
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(ComputeAgentScanRoots());
        AgentDoctorMaterials materials;
        materials.skills = ctx.skills;
        materials.registry = ctx.registry;
        std::vector<std::string> mcp_names;
        if (ctx.mcp_servers != nullptr) {
            for (const auto& runtime : *ctx.mcp_servers) {
                mcp_names.push_back(runtime.name);
            }
            materials.mcp_server_names = &mcp_names;
        }
        for (const std::string& line : FormatAgentDoctorReport(catalog, rest, materials)) {
            TermOut() << line << "\n";
        }
        return CommandFlow::Continue;
    }
    if (sub == "inspect" || sub == "reload") {
        TermOut() << "/agent " << sub << " 属后续阶段(阶段 2/3 接 Prompt Profile 与统一解析后落);现阶段用 "
                     "/agents 列清单、/agent doctor <名字> 做静态预检。\n";
        return CommandFlow::Continue;
    }
    TermOut() << "认不得的子命令 \"" << sub << "\"。用法:/agent doctor <名字>。\n";
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
