// /workflow 命令终端薄壳实现(自然语言编排单第 1 批)。

#include "app/commands/workflow_commands.hpp"

#include <iostream>

#include "cli/slash_commands.hpp"
#include "platform/paths.hpp"
#include "workflow/graph_view.hpp"

namespace lubancode::app {

namespace {

std::vector<std::string> BuiltinSlashWords() {
    std::vector<std::string> words;
    words.reserve(64);
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        words.push_back(info.name);
    }
    return words;
}

std::string TrimWord(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    const std::size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') ++pos;
    return s.substr(start, pos - start);
}

lubancode::workflow::CapabilityTable BuildCapabilities(const WorkflowCommandContext& context) {
    lubancode::workflow::CapabilityTable caps;
    if (context.registry != nullptr) {
        for (const auto& tool : context.registry->All()) {
            caps.tools.push_back(tool->name());
        }
    }
    caps.skills = context.skill_names;
    return caps;
}

void PrintUsage(const lubancode::cli::Theme& theme) {
    std::cout << "用法: /workflow list [project|home|all] | show <id> | graph <id> [ascii|mermaid|json]"
              << " | validate <id> | doctor\n";
    std::cout << "  运行与恢复: /workflow run <id> [参数...] | resume <run_id> | cancel <run_id>"
              << " | history <id>\n";
    (void)theme;
}

void PrintIssues(const std::string& title, const std::vector<lubancode::workflow::ParseIssue>& issues,
                 const lubancode::cli::Theme& theme) {
    std::cout << theme.error << title << theme.reset << "\n";
    for (const auto& issue : issues) {
        std::cout << "  " << theme.error << (issue.location.empty() ? "" : issue.location + ": ") << issue.message
                  << theme.reset << "\n";
    }
}

void PrintValidation(const std::string& id, const lubancode::workflow::ValidationResult& result,
                     const lubancode::cli::Theme& theme) {
    if (result.ok()) {
        std::cout << theme.stats << "workflow " << id << ": 校验通过" << theme.reset << "\n";
        return;
    }
    std::cout << theme.error << "workflow " << id << ": " << result.issues.size() << " 处问题" << theme.reset
              << "\n";
    for (const auto& issue : result.issues) {
        std::cout << "  " << theme.error << (issue.path.empty() ? "" : issue.path + ": ") << issue.message
                  << theme.reset << "\n";
    }
}

}  // namespace

ParsedWorkflowCommand ParseWorkflowCommand(const std::string& args) {
    ParsedWorkflowCommand parsed;
    std::size_t pos = 0;
    const std::string verb = TrimWord(args, pos);
    if (verb == "list") {
        parsed.action = WorkflowCommandAction::List;
        parsed.scope = TrimWord(args, pos);
        if (parsed.scope != "project" && parsed.scope != "home" && parsed.scope != "all" && !parsed.scope.empty()) {
            parsed.action = WorkflowCommandAction::Invalid;
        }
        return parsed;
    }
    if (verb == "show" || verb == "graph" || verb == "validate") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) {
            return parsed;  // Invalid,usage 兜底
        }
        parsed.action = verb == "show"
                            ? WorkflowCommandAction::Show
                            : (verb == "graph" ? WorkflowCommandAction::Graph : WorkflowCommandAction::Validate);
        if (verb == "graph") {
            parsed.format = TrimWord(args, pos);
            if (parsed.format.empty()) parsed.format = "ascii";
            if (parsed.format != "ascii" && parsed.format != "mermaid" && parsed.format != "json") {
                parsed.action = WorkflowCommandAction::Invalid;
            }
        } else if (!TrimWord(args, pos).empty()) {
            parsed.action = WorkflowCommandAction::Invalid;
        }
        return parsed;
    }
    if (verb == "doctor") {
        parsed.action = WorkflowCommandAction::Doctor;
        return parsed;
    }
    // run/resume/cancel/history/edit/... 后续批次接线;先报"本版还没有"。
    if (!verb.empty()) {
        parsed.id = verb;
    }
    return parsed;
}

std::string ResolveWorkflowAlias(const WorkflowCommandContext& context, const std::string& alias) {
    const lubancode::workflow::Catalog catalog =
        lubancode::workflow::LoadCatalog(context.project_root, context.user_root);
    const lubancode::workflow::CatalogEntry* entry = catalog.FindByAlias(alias);
    return entry != nullptr ? entry->definition.id : std::string();
}

bool HandleWorkflowCommand(const std::string& args, const WorkflowCommandContext& context) {
    const ParsedWorkflowCommand parsed = ParseWorkflowCommand(args);
    const lubancode::cli::Theme& theme = *context.theme;

    if (parsed.action == WorkflowCommandAction::Invalid) {
        PrintUsage(theme);
        return true;
    }

    const lubancode::workflow::Catalog catalog =
        lubancode::workflow::LoadCatalog(context.project_root, context.user_root);

    switch (parsed.action) {
        case WorkflowCommandAction::List: {
            const std::string& scope = parsed.scope.empty() ? "all" : parsed.scope;
            std::size_t shown = 0;
            for (const auto& entry : catalog.entries) {
                if (scope != "all" &&
                    (scope == "project") != (entry.scope == lubancode::workflow::WorkflowScope::Project)) {
                    continue;
                }
                ++shown;
                const std::string source = lubancode::workflow::ToString(entry.scope);
                if (entry.broken) {
                    std::cout << theme.error << "  " << entry.definition.id << " [损坏] (" << source << ")"
                              << theme.reset << "\n";
                    for (const auto& issue : entry.issues) {
                        std::cout << "      " << issue.location << ": " << issue.message << "\n";
                    }
                    continue;
                }
                std::cout << "  " << entry.definition.name << "  [" << entry.definition.id << " v"
                          << entry.definition.version << "] (" << source << ")";
                if (!entry.definition.alias.empty()) {
                    std::cout << "  /" << entry.definition.alias;
                    if (catalog.disabled_aliases.count(entry.definition.alias) > 0) {
                        std::cout << theme.error << "(禁用:" << catalog.disabled_aliases.at(entry.definition.alias)
                                  << ")" << theme.reset;
                    }
                }
                if (!entry.definition.enabled) std::cout << "  [已停用]";
                std::cout << "\n      " << entry.definition.description << "\n";
            }
            if (shown == 0) {
                std::cout << theme.stats << "(没有" << (scope == "all" ? "" : " " + scope)
                          << " workflow;.lubancode/workflows/ 下装一份就有)" << theme.reset << "\n";
            }
            for (const auto& conflict : catalog.conflicts) {
                std::cout << theme.stats << "[冲突] " << conflict.alias << ": " << conflict.owner << " ("
                          << conflict.kind << ")" << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Show: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                std::cout << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            const auto& def = entry->definition;
            std::cout << def.name << " [" << def.id << " v" << def.version << "] 来源:"
                      << lubancode::workflow::ToString(entry->scope) << "\n";
            std::cout << "  " << def.description << "\n";
            if (!def.alias.empty()) {
                std::cout << "  alias: /" << def.alias
                          << (catalog.disabled_aliases.count(def.alias) > 0 ? "(禁用)" : "") << "\n";
            }
            std::cout << "  hash: " << entry->content_hash.substr(0, 12) << "  目录: "
                      << lubancode::platform::PathToUtf8(entry->dir) << "\n";
            std::cout << "  limits: 并发 " << def.limits.max_concurrency << " · 节点 " << def.nodes.size() << "/"
                      << def.limits.max_nodes << " · 步数 " << def.limits.max_steps << " · 时限 "
                      << def.limits.timeout_secs << "s\n";
            std::cout << "  节点:\n";
            for (const auto& node : def.nodes) {
                std::cout << "    " << lubancode::workflow::NodeSummaryLine(node) << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Graph: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                std::cout << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            if (parsed.format == "mermaid") {
                std::cout << lubancode::workflow::RenderMermaidGraph(entry->definition);
            } else if (parsed.format == "json") {
                std::cout << entry->definition.normalized.dump(2) << "\n";
            } else {
                std::cout << lubancode::workflow::RenderAsciiGraph(entry->definition);
            }
            break;
        }
        case WorkflowCommandAction::Validate: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                std::cout << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            if (entry->broken) {
                PrintIssues("workflow " + parsed.id + " 定义解析失败", entry->issues, theme);
                return true;
            }
            const lubancode::workflow::CapabilityTable caps = BuildCapabilities(context);
            PrintValidation(parsed.id,
                            lubancode::workflow::ValidateDefinition(entry->definition, caps), theme);
            break;
        }
        case WorkflowCommandAction::Doctor: {
            std::size_t broken = 0;
            std::size_t ok = 0;
            for (const auto& entry : catalog.entries) {
                if (entry.broken) {
                    ++broken;
                    PrintIssues(entry.definition.id + " 解析失败", entry.issues, theme);
                    continue;
                }
                ++ok;
                PrintValidation(entry.definition.id,
                                lubancode::workflow::ValidateDefinition(entry.definition, BuildCapabilities(context)),
                                theme);
            }
            std::cout << theme.stats << "workflow doctor: " << ok << " 份可用, " << broken << " 份损坏, "
                      << catalog.conflicts.size() << " 处冲突, " << catalog.disabled_aliases.size()
                      << " 个 alias 被禁用" << theme.reset << "\n";
            for (const auto& [alias, reason] : catalog.disabled_aliases) {
                std::cout << "  /" << alias << ": " << reason << "\n";
            }
            break;
        }
    }
    return true;
}

}  // namespace lubancode::app
