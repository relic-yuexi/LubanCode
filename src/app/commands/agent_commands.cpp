// /agents 与 /agent doctor 的实现:Catalog 现扫现列、doctor 静态预检。
// 输出走 cli/terminal_port(散打 std::cout 清零的仓库规矩)。
#include "app/commands/agent_commands.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "agent/prompt_assembler.hpp"  // BuildPromptProfileLedger(阶段 2 来源账本)
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "cli/terminal_port.hpp"
#include "config/config.hpp"                  // HomeLubancodeDir
#include "config/project_instructions.hpp"    // FindProjectRoot(项目层根)
#include "package/mounting.hpp"               // MountAgentEntries/MountProfileRoots(阶段 3)
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

// 用户 prompts 根(~/.lubancode/prompts)的 UTF-8 串;没主目录给空串
//(= 只有嵌入层,与拼装侧"prompts_dir 空只用嵌入版"同语义)。
std::string UserPromptsRootUtf8() {
    const auto home = lubancode::config::HomeLubancodeDir();
    return home.has_value() ? (*home + "/prompts") : std::string();
}

// Profile 是否真有覆盖:账本里只要有任一模块来自三个 Profile 层(内置/
// 用户/项目)就算存在。doctor 的"Profile 是否存在"查的就是这个。
std::size_t ProfileOverlayCount(const lubancode::agent::PromptSourceLedger& ledger) {
    std::size_t count = 0;
    for (const auto& entry : ledger.entries) {
        if (entry.origin == lubancode::agent::PromptModuleOrigin::EmbeddedProfile ||
            entry.origin == lubancode::agent::PromptModuleOrigin::UserProfile ||
            entry.origin == lubancode::agent::PromptModuleOrigin::ProjectProfile) {
            ++count;
        }
    }
    return count;
}

}  // namespace

std::vector<std::string> FormatAgentCatalogListing(const lubancode::agent::AgentCatalog& catalog) {
    std::vector<std::string> lines;
    lines.push_back("Agent Catalog 共 " + std::to_string(catalog.entries.size()) +
                    " 个(优先级 project > user > package > builtin;包层带 canonical 名"
                    " <包id>:<名>;/agent doctor <名字> 看静态预检):");
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
                                                 const std::string& name, const AgentDoctorMaterials& materials,
                                                 const AgentPromptContext& prompts) {
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

    // ---- Profile(阶段 2):名字 + 覆盖是否存在(三层里有没有任何模块) ----
    // 阶段 3:包层根一并递进——canonical 名("<包id>:<名>")的覆盖只在包里。
    if (!def.prompt.profile.has_value() || *def.prompt.profile == "default") {
        lines.push_back("Profile: " + DescribeProfile(def) + "(default 上下文,三层覆盖不参与)");
    } else {
        const lubancode::agent::PromptSourceLedger ledger = lubancode::agent::BuildPromptProfileLedger(
            *def.prompt.profile, prompts.user_prompts_dir, prompts.project_prompts_dir,
            prompts.package_roots);
        const std::size_t overlays = ProfileOverlayCount(ledger);
        if (overlays > 0) {
            lines.push_back("Profile: " + *def.prompt.profile + "(三层共 " + std::to_string(overlays) +
                            " 个模块覆盖;/agent inspect " + entry->name + " 看逐段来源账本)");
        } else {
            lines.push_back("Profile: " + *def.prompt.profile +
                            " ✗(内置/用户/项目三层都没有任何模块覆盖,现全走 default 模块;先建 "
                            "profiles/" + *def.prompt.profile + "/ 下的覆盖文件)");
        }
    }

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
    // 阶段 3 起五枚预算字段一并登(并流口径:YAML 显式 > 父值;steps 另有
    // 入参与配置默认两级,见 AgentProfileResolver)。显式与否如实标——
    // "继承"就是落父值,不猜数。P1-0(turn 预算单 §5.2/§11.3)起 max_turns
    // 一并登,并列明生效的是任务级 turn 预算还是 legacy per-run step 预算。
    std::string runtime = "runtime: max_output_tokens=";
    runtime += def.max_output_tokens.has_value()
                   ? std::to_string(*def.max_output_tokens) + "(YAML 显式,视同 config 级)"
                   : std::string("继承");
    runtime += " · max_steps_per_turn=";
    runtime += def.max_steps_per_turn.has_value()
                   ? std::to_string(*def.max_steps_per_turn) + "(入参 > YAML > 配置默认;legacy,待迁移)"
                   : std::string("继承");
    runtime += " · max_turns=";
    runtime += def.max_turns.has_value()
                   ? std::to_string(*def.max_turns) + "(任务总 turn;override > YAML > subagent.default_max_turns)"
                   : std::string("继承(配置 subagent.default_max_turns,未设 = 0 不限)");
    runtime += " · max_context_chars=";
    runtime += def.max_context_chars.has_value() ? std::to_string(*def.max_context_chars)
                                                 : std::string("继承");
    runtime += " · context_window_tokens=";
    runtime += def.context_window_tokens.has_value() ? std::to_string(*def.context_window_tokens)
                                                     : std::string("继承");
    runtime += " · length_continuations=";
    runtime += def.length_continuations.has_value() ? std::to_string(*def.length_continuations)
                                                    : std::string("继承");
    runtime += " · execution_mode=" + (def.execution_mode.empty() ? std::string("auto") : def.execution_mode);
    runtime += " · isolation=" + (def.isolation.empty() ? std::string("none") : def.isolation);
    lines.push_back(std::move(runtime));
    // ---- 预算合同判读(turn 预算单 §11.3/§5.1,P1-0)--------------------------
    // 列明生效的是哪条路,顺带给迁移建议:老定义不突变,新定义不掉进每轮
    // 重置漏洞,用户一眼看得出自己走哪条。
    if (def.max_turns.has_value()) {
        lines.push_back("预算合同: task turn 预算 " + std::to_string(*def.max_turns) +
                        "(来源: Agent Definition runtime.max_turns;续投、孩子回流、Stop 钩子续跑共这本账)");
    } else if (def.max_steps_per_turn.has_value()) {
        lines.push_back("预算合同: legacy per-run step 预算 " + std::to_string(*def.max_steps_per_turn) +
                        "(每个 input round 各自上限;续投/Stop 钩子会重领额度)——待迁移");
        lines.push_back("迁移建议: 删掉 runtime.max_steps_per_turn,改写 runtime.max_turns: " +
                        std::to_string(*def.max_steps_per_turn) +
                        "(任务总 turn,一道闸管到底;语义从\"每轮各自\"变\"整任务合计\",按需调大数值)");
    } else {
        lines.push_back("预算合同: 未显式声明(task turn 落 subagent.default_max_turns,未设 = 0 不限)");
    }
    lines.push_back("预算归属: TaskLedger 任务记录(attempted/completed 分账;正常收场 reserved=0)");
    lines.push_back("permissions: " + (def.permissions_mode.empty() ? std::string("inherit") : def.permissions_mode) +
                    "(只能比父 Agent 更窄;派发时 AgentProfileResolver 按 agent.permission_widening 明拒)");

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

// /agent inspect <name> 的报告(阶段 2 落地):定义来源与覆盖链、prompt 段
// 三笔开关、Prompt 来源账本(单子 §5.5——哪个模块从哪层哪文件来,出了
// 覆盖问题一眼看见是谁压了谁)。模型/权限的最终合并属阶段 3,这里只登
// 定义里写的值;依赖预检归 /agent doctor,各管一摊。
std::vector<std::string> FormatAgentInspectReport(const lubancode::agent::AgentCatalog& catalog,
                                                  const std::string& name, const AgentPromptContext& prompts) {
    std::vector<std::string> lines;
    const auto* entry = catalog.Find(name);
    if (entry == nullptr) {
        lines.push_back("没有叫 \"" + name + "\" 的 Agent(先 /agents 看清单;名字大小写敏感)。");
        return lines;
    }
    lines.push_back("agent inspect: " + entry->name);
    lines.push_back("定义来源: " + lubancode::agent::ToString(entry->layer) + " " + entry->file);
    if (!entry->shadowed_sources.empty()) {
        lines.push_back("覆盖链(被盖住的来源,优先级从高到低):");
        for (const std::string& shadow : entry->shadowed_sources) {
            lines.push_back("  - " + shadow);
        }
    }
    if (!entry->definition.has_value()) {
        lines.push_back("定义: 没解析成,没有可查的 Prompt 账本 —— 先 /agent doctor " + entry->name +
                        " 看诊断。");
        return lines;
    }
    const auto& def = *entry->definition;

    // prompt 段三笔:profile / project_instructions / soul。
    const std::string project_instructions =
        def.prompt.project_instructions == lubancode::agent::AgentPromptSpec::ProjectInstructions::Omit
            ? "omit"
            : "inherit";
    const std::string soul = def.prompt.soul == lubancode::agent::AgentPromptSpec::Soul::Off ? "off" : "inherit";
    lines.push_back("prompt: profile=" + DescribeProfile(def) + " · project_instructions=" + project_instructions +
                    " · soul=" + soul);

    // runtime 并流账(阶段 3):定义里显式声明的预算字段逐笔点名,没声明的
    // 落父值。来源口径:入参显式 > YAML runtime > 父值/配置默认
    //(AgentProfileResolver 是唯一权威,两条派发路同一份)。
    {
        std::string declared;
        const auto append = [&declared](const char* field, const std::string& value) {
            if (!declared.empty()) {
                declared += "、";
            }
            declared += std::string(field) + "=" + value;
        };
        if (def.max_output_tokens.has_value()) {
            append("max_output_tokens", std::to_string(*def.max_output_tokens) + "(来源档:config 级)");
        }
        if (def.max_steps_per_turn.has_value()) {
            append("max_steps_per_turn", std::to_string(*def.max_steps_per_turn) + "(legacy,待迁移)");
        }
        if (def.max_turns.has_value()) {
            append("max_turns", std::to_string(*def.max_turns) + "(任务总 turn)");
        }
        if (def.max_context_chars.has_value()) {
            append("max_context_chars", std::to_string(*def.max_context_chars));
        }
        if (def.context_window_tokens.has_value()) {
            append("context_window_tokens", std::to_string(*def.context_window_tokens));
        }
        if (def.length_continuations.has_value()) {
            append("length_continuations", std::to_string(*def.length_continuations));
        }
        lines.push_back("runtime 并流: " +
                        (declared.empty() ? std::string("定义未显式声明预算字段,五枚全落父值")
                                          : ("显式声明 " + declared + ";其余落父值")));
    }
    // ---- 迁移片段(turn 预算单 §5.2 阶段 B,P1-0):旧字段还在用的定义给
    // 一段可直接复制的替换 YAML;新字段的定义不补这段。
    if (def.max_steps_per_turn.has_value()) {
        lines.push_back("迁移片段(把 runtime 段的旧键换成下面这行即可):");
        lines.push_back("  runtime:");
        lines.push_back("    max_turns: " + std::to_string(*def.max_steps_per_turn));
        lines.push_back("(语义变化:旧键是\"每个 input round 各自上限\",新键是\"整项任务合计\";"
                        "按任务实际规模调数值,再删旧键——两者同现会按 agent.turn_budget_conflict 拒载)");
    }

    // 来源账本:整张 default 模块树在这个 Profile 上下文下逐段解析。
    const std::string profile = def.prompt.profile.value_or(std::string());
    lines.push_back("Prompt 来源账本(逐模块,谁压了谁):");
    const lubancode::agent::PromptSourceLedger ledger =
        lubancode::agent::BuildPromptProfileLedger(profile, prompts.user_prompts_dir,
                                                   prompts.project_prompts_dir, prompts.package_roots);
    for (const auto& ledger_entry : ledger.entries) {
        lines.push_back("  " + ledger_entry.FormatLine());
    }
    if (!lubancode::agent::IsPromptProfileActive(profile)) {
        lines.push_back("  (default 上下文:三层 Profile 覆盖不参与;改一个用户 Profile 文件只影响"
                        "点名它的 Agent)");
    }
    lines.push_back("依赖预检(Skill/MCP/工具/模型): /agent doctor " + entry->name);
    return lines;
}

lubancode::agent::AgentCatalogScanRoots ComputeAgentScanRoots(
    std::vector<lubancode::agent::PackagedAgentEntry> packaged) {
    lubancode::agent::AgentCatalogScanRoots roots;
    roots.builtin_dir = EmbeddedAgentsDir();
    if (const auto home = lubancode::config::HomeLubancodeDir(); home.has_value()) {
        roots.user_dir = lubancode::platform::Utf8ToPath(*home) / "agents";
    }
    roots.project_dir = lubancode::config::FindProjectRoot(std::filesystem::current_path()) / ".lubancode" /
                        "agents";
    // 统一 Package 封装单阶段 3:包层成品件(调用方从会话钉快照折来)。
    roots.packaged = std::move(packaged);
    return roots;
}

// 会话钉快照折包层成品件(快照缺席 = 空表,行为与从前一致)。
std::vector<lubancode::agent::PackagedAgentEntry> PackagedAgentsFromMount(SlashDispatchContext& ctx) {
    if (ctx.package_mount == nullptr) {
        return {};
    }
    return lubancode::package::MountAgentEntries(*ctx.package_mount);
}

std::vector<lubancode::agent::PackageProfileRoot> PackagedProfileRootsFromMount(SlashDispatchContext& ctx) {
    if (ctx.package_mount == nullptr) {
        return {};
    }
    return lubancode::package::MountProfileRoots(*ctx.package_mount);
}

// Prompt Profile 的项目层根(阶段 2):<项目根>/.lubancode/prompts,UTF-8
// 串。项目根与 Agent 扫描同一条发现规则(FindProjectRoot),不各自猜 cwd。
// 目录不存在照旧返回——拼装侧对缺席层静默跳过。会话装配与 /agent inspect
// 都从这儿拿,一个口径。
std::string ComputeProjectPromptsRoot() {
    return lubancode::platform::PathToUtf8(lubancode::config::FindProjectRoot(std::filesystem::current_path()) /
                                           ".lubancode" / "prompts");
}

CommandFlow HandleSlashAgents(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    (void)parsed;
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
        ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
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
        TermOut() << "用法:/agent doctor <名字>(静态预检)、/agent inspect <名字>(Prompt 来源账本)。\n"
                     "/agents 列清单。\n";
        return CommandFlow::Continue;
    }
    if (sub == "doctor") {
        if (rest.empty()) {
            TermOut() << "用法:/agent doctor <名字>(名字看 /agents;大小写敏感)。\n";
            return CommandFlow::Continue;
        }
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
            ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
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
        AgentPromptContext prompts;
        prompts.user_prompts_dir = UserPromptsRootUtf8();
        prompts.project_prompts_dir = ComputeProjectPromptsRoot();
        prompts.package_roots = PackagedProfileRootsFromMount(ctx);
        for (const std::string& line : FormatAgentDoctorReport(catalog, rest, materials, prompts)) {
            TermOut() << line << "\n";
        }
        return CommandFlow::Continue;
    }
    if (sub == "inspect") {
        if (rest.empty()) {
            TermOut() << "用法:/agent inspect <名字>(名字看 /agents;大小写敏感)。\n";
            return CommandFlow::Continue;
        }
        const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
            ComputeAgentScanRoots(PackagedAgentsFromMount(ctx)));
        AgentPromptContext prompts;
        prompts.user_prompts_dir = UserPromptsRootUtf8();
        prompts.project_prompts_dir = ComputeProjectPromptsRoot();
        prompts.package_roots = PackagedProfileRootsFromMount(ctx);
        for (const std::string& line : FormatAgentInspectReport(catalog, rest, prompts)) {
            TermOut() << line << "\n";
        }
        return CommandFlow::Continue;
    }
    if (sub == "reload") {
        TermOut() << "/agent reload 属后续阶段(阶段 3 统一解析时连同原子替换一起落);现阶段 Catalog "
                     "现扫现建,改了 YAML 下一次派发即生效。\n";
        return CommandFlow::Continue;
    }
    TermOut() << "认不得的子命令 \"" << sub << "\"。用法:/agent doctor <名字>、/agent inspect <名字>。\n";
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
