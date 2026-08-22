// /workflow 命令终端薄壳实现(自然语言编排单第 1 批)。

#include "app/commands/workflow_commands.hpp"

#include <iostream>
#include <sstream>

#include "cli/slash_commands.hpp"
#include "platform/paths.hpp"
#include "workflow/compiler.hpp"
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
    if (verb == "run") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = WorkflowCommandAction::Run;
        while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) ++pos;
        parsed.rest = args.substr(pos);
        return parsed;
    }
    if (verb == "resume" || verb == "cancel") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = verb == "resume" ? WorkflowCommandAction::Resume : WorkflowCommandAction::Cancel;
        return parsed;
    }
    if (verb == "history") {
        parsed.action = WorkflowCommandAction::History;
        const std::string sub = TrimWord(args, pos);
        if (sub == "delete") {
            parsed.id = TrimWord(args, pos);
            const std::string yes = TrimWord(args, pos);
            parsed.confirm = yes == "yes";
            if (parsed.id.empty()) parsed.action = WorkflowCommandAction::Invalid;
        } else if (!sub.empty()) {
            parsed.id = sub;
        }
        return parsed;
    }
    if (verb == "enable" || verb == "disable") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        parsed.action = WorkflowCommandAction::Enable;
        parsed.rest = verb;  // 复用:enable/disable 词
        return parsed;
    }
    if (verb == "remove") {
        parsed.id = TrimWord(args, pos);
        if (parsed.id.empty()) return parsed;
        const std::string yes = TrimWord(args, pos);
        parsed.confirm = yes == "yes";
        parsed.action = WorkflowCommandAction::Remove;
        return parsed;
    }
    if (verb == "create") {
        parsed.action = WorkflowCommandAction::Create;
        while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) ++pos;
        parsed.rest = args.substr(pos);
        return parsed;
    }
    if (verb == "alias") {
        parsed.action = WorkflowCommandAction::Alias;
        return parsed;
    }
    // edit/export/import 后续接线(第 6 批/app-server 合同)。
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
        case WorkflowCommandAction::Run: {
            // 执行器由会话层经 ResolveWorkflowRunContext 注入(第 4 批的
            // 宿主执行器);这里只做编排。没注入(如测试)给一句明话。
            std::cout << theme.stats << "run <id> 的执行器装配由会话层注入;本路径给测试与 "
                      << "app-server 用" << theme.reset << "\n";
            break;
        }
        case WorkflowCommandAction::Resume: {
            if (context.home_lubancode.has_value()) {
                const std::filesystem::path run_dir =
                    *context.home_lubancode / "workflow-runs" / parsed.id;
                std::error_code ec;
                if (!std::filesystem::exists(run_dir, ec)) {
                    std::cout << theme.error << "run 不存在: " << parsed.id << theme.reset << "\n";
                    break;
                }
                // 恢复也走会话层执行器装配;这里先给账面。
                std::cout << theme.stats << "run " << parsed.id << " 可恢复;执行入口与 run 同一道装配"
                          << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Cancel: {
            std::cout << theme.error << "cancel:跑动中的 run 经 ESC/打断通道取消(首版同步 run,"
                      << "命令返回时 run 已终态)" << theme.reset << "\n";
            break;
        }
        case WorkflowCommandAction::History: {
            if (context.home_lubancode.has_value()) {
                const std::filesystem::path runs_root = *context.home_lubancode / "workflow-runs";
                const auto runs = lubancode::workflow::ListRuns(runs_root);
                std::size_t shown = 0;
                for (const auto& run : runs) {
                    if (!parsed.id.empty() && run.workflow_id != parsed.id) continue;
                    ++shown;
                    std::cout << "  " << run.run_id << "  " << run.workflow_id << " v" << run.workflow_version
                              << "  " << (run.final_state.empty() ? "(未完成)" : run.final_state) << "  "
                              << run.started_at << "\n";
                }
                if (shown == 0) {
                    std::cout << theme.stats << "(没有运行账)" << theme.reset << "\n";
                }
            }
            break;
        }
        case WorkflowCommandAction::Enable: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                std::cout << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            const bool enable = parsed.rest == "enable";
            auto result = lubancode::workflow::SetWorkflowEnabled(entry->dir, enable);
            if (result.has_value()) {
                std::cout << theme.stats << (enable ? "已启用" : "已停用") << "(直呼 alias "
                          << (enable ? "恢复" : "不再响应") << ")" << theme.reset << "\n";
            } else {
                std::cout << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Remove: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                std::cout << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            if (!parsed.confirm) {
                std::cout << theme.error << "remove 要确认(只删定义,不动运行账): /workflow remove "
                          << parsed.id << " yes" << theme.reset << "\n";
                break;
            }
            auto result = lubancode::workflow::RemoveWorkflow(entry->dir);
            if (result.has_value()) {
                std::cout << theme.stats << "已移除定义: " << parsed.id << "(运行账另走 /workflow history)"
                          << theme.reset << "\n";
            } else {
                std::cout << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Create: {
            // 第 5 批:向导走会话层的模型装配(意图提取经 IntentCompiler
            // 注入);这里先给用法骨架,第 6 批接模型。
            std::cout << "用法: /workflow create <自然语言描述>\n"
                      << "  例: /workflow create 论文检索:四路并行查 arXiv/DBLP/Scholar/AnySearch,"
                      << "去重排序写成 Markdown\n"
                      << "  向导会追问缺口,预览图与将写文件,确认后落进项目或用户目录。\n";
            break;
        }
        case WorkflowCommandAction::Alias: {
            for (const auto& entry : catalog.entries) {
                if (entry.broken || entry.definition.alias.empty()) continue;
                const bool disabled = catalog.disabled_aliases.count(entry.definition.alias) > 0;
                std::cout << "  /" << entry.definition.alias << " -> " << entry.definition.id << " ("
                          << lubancode::workflow::ToString(entry.scope) << ")"
                          << (disabled ? theme.error + " [禁用:跨类撞名]" + theme.reset : "") << "\n";
            }
            if (catalog.disabled_aliases.empty() && catalog.entries.empty()) {
                std::cout << theme.stats << "(没有可直呼的 workflow alias)" << theme.reset << "\n";
            }
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

std::string RunWorkflowById(const WorkflowCommandContext& context, const std::string& id,
                            const std::string& raw_args,
                            const std::map<lubancode::workflow::NodeKind,
                                           std::shared_ptr<lubancode::workflow::NodeExecutor>>& executors) {
    const lubancode::cli::Theme& theme = *context.theme;
    const lubancode::workflow::Catalog catalog =
        lubancode::workflow::LoadCatalog(context.project_root, context.user_root);
    const lubancode::workflow::CatalogEntry* entry = catalog.Find(id);
    if (entry == nullptr) {
        return theme.error + "找不到 workflow: " + id + theme.reset;
    }

    // 参数解析:按 input_schema 的 properties 逐个 --name value / 位置参
    // (首个位置参给第一个 required 字段)。非交互缺必填给结构化错。
    nlohmann::json inputs = nlohmann::json::object();
    const auto& props = entry->definition.inputs.contains("properties")
                            ? entry->definition.inputs["properties"]
                            : nlohmann::json::object();
    std::vector<std::string> positional;
    std::size_t pos = 0;
    while (pos < raw_args.size()) {
        while (pos < raw_args.size() && (raw_args[pos] == ' ' || raw_args[pos] == '\t')) ++pos;
        if (pos >= raw_args.size()) break;
        if (raw_args.compare(pos, 2, "--") == 0) {
            pos += 2;
            const std::size_t name_start = pos;
            while (pos < raw_args.size() && raw_args[pos] != ' ' && raw_args[pos] != '\t' &&
                   raw_args[pos] != '=') {
                ++pos;
            }
            std::string name = raw_args.substr(name_start, pos - name_start);
            if (pos < raw_args.size() && raw_args[pos] == '=') ++pos;
            while (pos < raw_args.size() && (raw_args[pos] == ' ' || raw_args[pos] == '\t')) ++pos;
            const std::size_t value_start = pos;
            const bool quoted = pos < raw_args.size() && raw_args[pos] == '"';
            if (quoted) {
                ++pos;
                while (pos < raw_args.size() && raw_args[pos] != '"') ++pos;
                if (pos < raw_args.size()) ++pos;
            } else {
                while (pos < raw_args.size() && raw_args[pos] != ' ' && raw_args[pos] != '\t') ++pos;
            }
            std::string value = raw_args.substr(value_start, pos - value_start);
            if (quoted && value.size() >= 2) value = value.substr(1, value.size() - 2);
            if (!name.empty()) inputs[name] = value;
            continue;
        }
        const std::size_t start = pos;
        const bool quoted = raw_args[pos] == '"';
        if (quoted) {
            ++pos;
            while (pos < raw_args.size() && raw_args[pos] != '"') ++pos;
            if (pos < raw_args.size()) ++pos;
        } else {
            while (pos < raw_args.size() && raw_args[pos] != ' ' && raw_args[pos] != '\t') ++pos;
        }
        std::string token = raw_args.substr(start, pos - start);
        if (quoted && token.size() >= 2) token = token.substr(1, token.size() - 2);
        positional.push_back(token);
    }
    // 位置参依序填 required 里还没值的字段。
    if (entry->definition.inputs.contains("required") && entry->definition.inputs["required"].is_array()) {
        std::size_t arg_index = 0;
        for (const auto& field : entry->definition.inputs["required"]) {
            if (!field.is_string()) continue;
            const std::string name = field.get<std::string>();
            if (inputs.contains(name) || arg_index >= positional.size()) continue;
            inputs[name] = positional[arg_index++];
        }
    }
    (void)props;

    lubancode::workflow::RuntimeOptions options;
    options.executors = executors;
    if (context.home_lubancode.has_value()) {
        options.runs_root = *context.home_lubancode / "workflow-runs";
    }
    lubancode::workflow::WorkflowRuntime runtime(std::move(options));
    const lubancode::workflow::WorkflowRunSummary summary =
        runtime.Run(entry->definition, lubancode::workflow::RunInputs(inputs));

    std::ostringstream out;
    out << entry->definition.name << " [" << entry->definition.id << "] " << theme.stats
        << lubancode::workflow::ToString(summary.state) << theme.reset << " · "
        << summary.duration_ms / 1000 << "." << (summary.duration_ms % 1000) / 100 << "s · tokens "
        << summary.tokens_used << "\n";
    for (const auto& [node_id, record] : summary.nodes) {
        const char* mark = record.state == lubancode::workflow::NodeState::Succeeded  ? "[ok] "
                           : record.state == lubancode::workflow::NodeState::Failed    ? "[!!] "
                           : record.state == lubancode::workflow::NodeState::Skipped   ? "[skip] "
                                                                                       : "[..] ";
        out << "  " << mark << node_id;
        if (!record.error_code.empty()) {
            out << "  " << record.error_code << " " << record.error_message.substr(0, 120);
        }
        out << "\n";
    }
    if (!summary.unavailable_sources.empty()) {
        out << theme.error << "  缺失来源: ";
        for (const auto& source : summary.unavailable_sources) out << source << " ";
        out << theme.reset << "\n";
    }
    if (!summary.result.empty()) {
        out << theme.stats << "  结果: " << summary.result.dump().substr(0, 400) << theme.reset << "\n";
    }
    if (!summary.error_message.empty() && summary.state != lubancode::workflow::RunState::Succeeded) {
        out << theme.error << "  " << summary.error_code << ": " << summary.error_message << theme.reset << "\n";
    }
    return out.str();
}

}  // namespace lubancode::app
