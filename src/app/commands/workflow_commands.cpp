// /workflow 命令终端薄壳实现(自然语言编排单第 1 批)。

#include "app/commands/workflow_commands.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派注册制)
#include "app/turn_runner.hpp"                  // PromptAskUser(终端交互宿主)
#include "tools/path_utils.hpp"                // Utf8ToPath(catalog 锚点拼路径)
#include "tools/skill_loader.hpp"
#include "cli/console_input.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>

#include "agent/agent.hpp"  // AgentProfile(批四自立门户)
#include "cli/slash_commands.hpp"
#include "config/model_catalog.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/text_encoding.hpp"
#include "workflow/compiler.hpp"
#include "workflow/graph_view.hpp"

namespace lubancode::app {

namespace {

class ReadyWorkflowInteractionFuture final : public lubancode::runtime::InteractionFuture {
public:
    explicit ReadyWorkflowInteractionFuture(std::optional<lubancode::runtime::ApprovalResponse> approval)
        : approval_(std::move(approval)) {}
    explicit ReadyWorkflowInteractionFuture(std::optional<lubancode::runtime::QuestionResponse> question)
        : question_(std::move(question)) {}

    std::optional<lubancode::runtime::ApprovalResponse> WaitApproval() override { return approval_; }
    std::optional<lubancode::runtime::QuestionResponse> WaitQuestion() override { return question_; }

private:
    std::optional<lubancode::runtime::ApprovalResponse> approval_;
    std::optional<lubancode::runtime::QuestionResponse> question_;
};

class TerminalWorkflowInteractionBroker final : public lubancode::runtime::InteractionBroker {
public:
    explicit TerminalWorkflowInteractionBroker(const lubancode::cli::Theme& theme) : theme_(theme) {}

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskApproval(
        const lubancode::runtime::ApprovalRequest& request) override {
        lubancode::tools::AskUserQuestion question;
        question.header = "Workflow 审批";
        question.question = request.reason.empty() ? "要放行这一步吗?" : request.reason;
        question.options = {{"批准", "本次放行"}, {"拒绝", "不跑这一步"}};
        auto answer = PromptAskUser(question, theme_);
        if (!answer.has_value()) {
            return std::make_shared<ReadyWorkflowInteractionFuture>(
                std::optional<lubancode::runtime::ApprovalResponse>{});
        }
        lubancode::runtime::ApprovalResponse response;
        if (answer->kind == lubancode::tools::AskUserResponseKind::Answered &&
            !answer->answers.empty() && answer->answers.front() == "批准") {
            response.decision = lubancode::runtime::InteractionDecision::Accept;
        } else if (answer->kind == lubancode::tools::AskUserResponseKind::Declined) {
            response.decision = lubancode::runtime::InteractionDecision::Cancel;
        } else {
            response.decision = lubancode::runtime::InteractionDecision::Decline;
            response.reason = "用户拒绝";
        }
        return std::make_shared<ReadyWorkflowInteractionFuture>(
            std::optional<lubancode::runtime::ApprovalResponse>{std::move(response)});
    }

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskQuestion(
        const lubancode::runtime::QuestionRequest& request) override {
        lubancode::tools::AskUserQuestion question;
        question.header = request.header;
        question.question = request.question;
        question.multi_select = request.multi_select;
        for (const auto& option : request.options) {
            question.options.push_back({option.label, option.description});
        }
        auto answer = PromptAskUser(question, theme_);
        if (!answer.has_value() || answer->kind == lubancode::tools::AskUserResponseKind::Declined) {
            return std::make_shared<ReadyWorkflowInteractionFuture>(
                std::optional<lubancode::runtime::QuestionResponse>{});
        }
        lubancode::runtime::QuestionResponse response;
        response.answers = answer->answers;
        if (answer->kind == lubancode::tools::AskUserResponseKind::Discuss && !answer->message.empty()) {
            response.answers.push_back(answer->message);
        }
        return std::make_shared<ReadyWorkflowInteractionFuture>(
            std::optional<lubancode::runtime::QuestionResponse>{std::move(response)});
    }

    bool ResolveApproval(const lubancode::runtime::InteractionRequestId&,
                         const lubancode::runtime::ApprovalResponse&) override { return false; }
    bool AnswerQuestion(const lubancode::runtime::InteractionRequestId&,
                        const lubancode::runtime::QuestionResponse&) override { return false; }

private:
    const lubancode::cli::Theme& theme_;
};

// workflow 直呼会同步占住会话线程，不能等主循环回来才重开 composer。
// 执行期借普通 turn 的 footer 与输入监听器：框常驻、消息可排队，Esc 也
// 直连图运行的 cancel token。析构先停读键线程，再擦 footer。
class WorkflowTerminalRunScope final {
public:
    WorkflowTerminalRunScope(const lubancode::cli::Theme& theme, bool interactive)
        : theme_(theme),
          footer_enabled_(interactive && lubancode::platform::SupportsScreenRepaint()),
          started_at_(std::chrono::steady_clock::now()) {
        lubancode::cli::BeginStreamFooter(theme_, footer_enabled_);
        if (footer_enabled_) {
            lubancode::cli::BeginTurnActivity(
                lubancode::cli::tr("spinner.thinking"),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            heartbeat_ = std::make_unique<lubancode::cli::StreamFooterHeartbeat>(
                true, started_at_, &cancel_);
        }
        if (interactive) {
            listener_ = std::make_unique<lubancode::cli::TurnInputListener>(cancel_, theme_);
        }
    }

    ~WorkflowTerminalRunScope() {
        if (listener_) listener_->Stop();
        if (heartbeat_) heartbeat_->Stop();
        lubancode::cli::EndTurnActivity();
        lubancode::cli::EndStreamFooter();
    }

    WorkflowTerminalRunScope(const WorkflowTerminalRunScope&) = delete;
    WorkflowTerminalRunScope& operator=(const WorkflowTerminalRunScope&) = delete;

    const std::atomic<bool>* cancel_token() const { return &cancel_; }

private:
    const lubancode::cli::Theme& theme_;
    bool footer_enabled_ = false;
    std::chrono::steady_clock::time_point started_at_;
    std::atomic<bool> cancel_{false};
    std::unique_ptr<lubancode::cli::TurnInputListener> listener_;
    std::unique_ptr<lubancode::cli::StreamFooterHeartbeat> heartbeat_;
};

std::vector<std::string> BuiltinSlashWords() {
    std::vector<std::string> words;
    words.reserve(64);
    for (const auto& info : lubancode::cli::AllSlashCommands()) {
        words.push_back(info.name);
    }
    return words;
}

lubancode::workflow::Catalog LoadCheckedCatalog(const WorkflowCommandContext& context) {
    lubancode::workflow::Catalog catalog =
        lubancode::workflow::LoadCatalog(context.project_root, context.user_root);
    lubancode::workflow::DetectAliasConflicts(catalog, context.skill_names, BuiltinSlashWords());
    return catalog;
}

std::string TrimWord(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    const std::size_t start = pos;
    while (pos < s.size() && s[pos] != ' ' && s[pos] != '\t') ++pos;
    return s.substr(start, pos - start);
}

std::string TrimWorkflowRunArgs(const std::string& raw) {
    std::size_t begin = 0;
    while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    // 终端里常有人把提示符/视觉分隔写成 `/alias > 需求`。`>` 不是一份
    // 有用的需求，旧解析器却拿它填唯一必填项，后文全丢。这里宽容这一种
    // 明确形状；真正以 `>` 起头且后面不跟空白的正文原样保留。
    if (begin < raw.size() && raw[begin] == '>' && begin + 1 < raw.size() &&
        (raw[begin + 1] == ' ' || raw[begin + 1] == '\t')) {
        ++begin;
        while (begin < raw.size() && (raw[begin] == ' ' || raw[begin] == '\t')) ++begin;
    }
    std::size_t end = raw.size();
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t')) --end;
    return raw.substr(begin, end - begin);
}

nlohmann::json CoerceWorkflowInput(const nlohmann::json& properties, const std::string& name,
                                   const std::string& raw) {
    if (!properties.is_object()) return raw;
    const auto property = properties.find(name);
    if (property == properties.end() || !property->is_object()) return raw;
    const auto type = property->find("type");
    if (type == property->end() || !type->is_string() || *type == "string") return raw;

    const auto parsed = nlohmann::json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) return raw;
    if (*type == "integer" && parsed.is_number_integer()) return parsed;
    if (*type == "number" && parsed.is_number()) return parsed;
    if (*type == "boolean" && parsed.is_boolean()) return parsed;
    if (*type == "array" && parsed.is_array()) return parsed;
    if (*type == "object" && parsed.is_object()) return parsed;
    return raw;
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
    TermOut() << "用法: /workflow list [project|home|all] | show <id> | graph <id> [ascii|mermaid|json]"
              << " | validate <id> | doctor\n";
    TermOut() << "  运行与恢复: /workflow run <id> [参数...] | resume <run_id> | cancel <run_id>"
              << " | history <id>\n";
    (void)theme;
}

void PrintIssues(const std::string& title, const std::vector<lubancode::workflow::ParseIssue>& issues,
                 const lubancode::cli::Theme& theme) {
    TermOut() << theme.error << title << theme.reset << "\n";
    for (const auto& issue : issues) {
        TermOut() << "  " << theme.error << (issue.location.empty() ? "" : issue.location + ": ") << issue.message
                  << theme.reset << "\n";
    }
}

void PrintValidation(const std::string& id, const lubancode::workflow::ValidationResult& result,
                     const lubancode::cli::Theme& theme) {
    if (result.ok()) {
        TermOut() << theme.stats << "workflow " << id << ": 校验通过" << theme.reset << "\n";
        return;
    }
    TermOut() << theme.error << "workflow " << id << ": " << result.issues.size() << " 处问题" << theme.reset
              << "\n";
    for (const auto& issue : result.issues) {
        TermOut() << "  " << theme.error << (issue.path.empty() ? "" : issue.path + ": ") << issue.message
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
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    const lubancode::workflow::CatalogEntry* entry = catalog.FindByAlias(alias);
    return entry != nullptr ? entry->definition.id : std::string();
}

std::vector<lubancode::cli::CompletionCandidate> BuildWorkflowSlashCompletionCandidates(
    const WorkflowCommandContext& context) {
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    std::vector<lubancode::cli::CompletionCandidate> candidates;
    for (const auto& entry : catalog.entries) {
        const auto& def = entry.definition;
        if (entry.broken || !def.enabled || def.alias.empty() ||
            catalog.disabled_aliases.count(def.alias) > 0) {
            continue;
        }
        std::string description = "Workflow · " + def.name;
        if (!def.description.empty()) description += " · " + def.description;
        candidates.push_back({"/" + def.alias, std::move(description)});
    }
    return candidates;
}

bool HandleWorkflowCommand(const std::string& args, const WorkflowCommandContext& context) {
    const ParsedWorkflowCommand parsed = ParseWorkflowCommand(args);
    const lubancode::cli::Theme& theme = *context.theme;

    if (parsed.action == WorkflowCommandAction::Invalid) {
        PrintUsage(theme);
        return true;
    }

    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);

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
                    TermOut() << theme.error << "  " << entry.definition.id << " [损坏] (" << source << ")"
                              << theme.reset << "\n";
                    for (const auto& issue : entry.issues) {
                        TermOut() << "      " << issue.location << ": " << issue.message << "\n";
                    }
                    continue;
                }
                TermOut() << "  " << entry.definition.name << "  [" << entry.definition.id << " v"
                          << entry.definition.version << "] (" << source << ")";
                if (!entry.definition.alias.empty()) {
                    TermOut() << "  /" << entry.definition.alias;
                    if (catalog.disabled_aliases.count(entry.definition.alias) > 0) {
                        TermOut() << theme.error << "(禁用:" << catalog.disabled_aliases.at(entry.definition.alias)
                                  << ")" << theme.reset;
                    }
                }
                if (!entry.definition.enabled) TermOut() << "  [已停用]";
                TermOut() << "\n      " << entry.definition.description << "\n";
            }
            if (shown == 0) {
                TermOut() << theme.stats << "(没有" << (scope == "all" ? "" : " " + scope)
                          << " workflow;.lubancode/workflows/ 下装一份就有)" << theme.reset << "\n";
            }
            for (const auto& conflict : catalog.conflicts) {
                TermOut() << theme.stats << "[冲突] " << conflict.alias << ": " << conflict.owner << " ("
                          << conflict.kind << ")" << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Show: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            const auto& def = entry->definition;
            TermOut() << def.name << " [" << def.id << " v" << def.version << "] 来源:"
                      << lubancode::workflow::ToString(entry->scope) << "\n";
            TermOut() << "  " << def.description << "\n";
            if (!def.alias.empty()) {
                TermOut() << "  alias: /" << def.alias
                          << (catalog.disabled_aliases.count(def.alias) > 0 ? "(禁用)" : "") << "\n";
            }
            TermOut() << "  hash: " << entry->content_hash.substr(0, 12) << "  目录: "
                      << lubancode::platform::PathToUtf8(entry->dir) << "\n";
            TermOut() << "  limits: 并发 " << def.limits.max_concurrency << " · 节点 " << def.nodes.size() << "/"
                      << def.limits.max_nodes << " · 步数 " << def.limits.max_steps << " · 时限 "
                      << def.limits.timeout_secs << "s\n";
            TermOut() << "  节点:\n";
            for (const auto& node : def.nodes) {
                TermOut() << "    " << lubancode::workflow::NodeSummaryLine(node) << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Graph: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                return true;
            }
            if (parsed.format == "mermaid") {
                TermOut() << lubancode::workflow::RenderMermaidGraph(entry->definition);
            } else if (parsed.format == "json") {
                TermOut() << entry->definition.normalized.dump(2) << "\n";
            } else {
                TermOut() << lubancode::workflow::RenderAsciiGraph(entry->definition);
            }
            break;
        }
        case WorkflowCommandAction::Validate: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
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
            TermOut() << theme.stats << "run <id> 的执行器装配由会话层注入;本路径给测试与 "
                      << "app-server 用" << theme.reset << "\n";
            break;
        }
        case WorkflowCommandAction::Resume: {
            if (context.home_lubancode.has_value()) {
                const std::filesystem::path run_dir =
                    *context.home_lubancode / "workflow-runs" / parsed.id;
                std::error_code ec;
                if (!std::filesystem::exists(run_dir, ec)) {
                    TermOut() << theme.error << "run 不存在: " << parsed.id << theme.reset << "\n";
                    break;
                }
                // 恢复也走会话层执行器装配;这里先给账面。
                TermOut() << theme.stats << "run " << parsed.id << " 可恢复;执行入口与 run 同一道装配"
                          << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Cancel: {
            TermOut() << theme.error << "cancel:跑动中的 run 经 ESC/打断通道取消(首版同步 run,"
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
                    TermOut() << "  " << run.run_id << "  " << run.workflow_id << " v" << run.workflow_version
                              << "  " << (run.final_state.empty() ? "(未完成)" : run.final_state) << "  "
                              << run.started_at << "\n";
                }
                if (shown == 0) {
                    TermOut() << theme.stats << "(没有运行账)" << theme.reset << "\n";
                }
            }
            break;
        }
        case WorkflowCommandAction::Enable: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            const bool enable = parsed.rest == "enable";
            auto result = lubancode::workflow::SetWorkflowEnabled(entry->dir, enable);
            if (result.has_value()) {
                TermOut() << theme.stats << (enable ? "已启用" : "已停用") << "(直呼 alias "
                          << (enable ? "恢复" : "不再响应") << ")" << theme.reset << "\n";
            } else {
                TermOut() << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Remove: {
            const lubancode::workflow::CatalogEntry* entry = catalog.Find(parsed.id);
            if (entry == nullptr) {
                TermOut() << theme.error << "找不到 workflow: " << parsed.id << theme.reset << "\n";
                break;
            }
            if (!parsed.confirm) {
                TermOut() << theme.error << "remove 要确认(只删定义,不动运行账): /workflow remove "
                          << parsed.id << " yes" << theme.reset << "\n";
                break;
            }
            auto result = lubancode::workflow::RemoveWorkflow(entry->dir);
            if (result.has_value()) {
                TermOut() << theme.stats << "已移除定义: " << parsed.id << "(运行账另走 /workflow history)"
                          << theme.reset << "\n";
            } else {
                TermOut() << theme.error << result.error() << theme.reset << "\n";
            }
            break;
        }
        case WorkflowCommandAction::Create: {
            // 第 5 批:向导走会话层的模型装配(意图提取经 IntentCompiler
            // 注入);这里先给用法骨架,第 6 批接模型。
            TermOut() << "用法: /workflow create <自然语言描述>\n"
                      << "  例: /workflow create 论文检索:四路并行查 arXiv/DBLP/Scholar/AnySearch,"
                      << "去重排序写成 Markdown\n"
                      << "  向导会追问缺口,预览图与将写文件,确认后落进项目或用户目录。\n";
            break;
        }
        case WorkflowCommandAction::Alias: {
            for (const auto& entry : catalog.entries) {
                if (entry.broken || entry.definition.alias.empty()) continue;
                const bool disabled = catalog.disabled_aliases.count(entry.definition.alias) > 0;
                TermOut() << "  /" << entry.definition.alias << " -> " << entry.definition.id << " ("
                          << lubancode::workflow::ToString(entry.scope) << ")"
                          << (disabled ? theme.error + " [禁用:跨类撞名]" + theme.reset : "") << "\n";
            }
            if (catalog.disabled_aliases.empty() && catalog.entries.empty()) {
                TermOut() << theme.stats << "(没有可直呼的 workflow alias)" << theme.reset << "\n";
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
            TermOut() << theme.stats << "workflow doctor: " << ok << " 份可用, " << broken << " 份损坏, "
                      << catalog.conflicts.size() << " 处冲突, " << catalog.disabled_aliases.size()
                      << " 个 alias 被禁用" << theme.reset << "\n";
            for (const auto& [alias, reason] : catalog.disabled_aliases) {
                TermOut() << "  /" << alias << ": " << reason << "\n";
            }
            break;
        }
    }
    return true;
}

std::string RunWorkflowById(const WorkflowCommandContext& context, const std::string& id,
                            const std::string& raw_args,
                            const std::map<lubancode::workflow::NodeKind,
                                           std::shared_ptr<lubancode::workflow::NodeExecutor>>& executors,
                            const std::atomic<bool>* cancel_token) {
    const lubancode::cli::Theme& theme = *context.theme;
    const lubancode::workflow::Catalog catalog = LoadCheckedCatalog(context);
    const lubancode::workflow::CatalogEntry* entry = catalog.Find(id);
    if (entry == nullptr) {
        return theme.error + "找不到 workflow: " + id + theme.reset;
    }

    // 参数解析:按 input_schema 的 properties 逐个 --name value / 位置参。
    // 直呼别名没带必填项时，终端按字段 description 补问；无交互宿主
    // 才把缺项留给 runtime，照旧返回结构化错。
    nlohmann::json inputs = nlohmann::json::object();
    const nlohmann::json props = entry->definition.inputs.contains("properties")
                                     ? entry->definition.inputs["properties"]
                                     : nlohmann::json::object();
    const std::string clean_args = TrimWorkflowRunArgs(raw_args);
    // 只有一项必填 string 时，alias 后面通常就是整段旨意。只要不是以
    // `--` 起头的显式参数写法，便一字不拆地交给它；需求正文里的
    // `--dry-run`、引号与连续空格都不该被命令行解析器吃掉。
    std::optional<std::string> natural_language_field;
    if (entry->definition.inputs.contains("required") &&
        entry->definition.inputs["required"].is_array() &&
        entry->definition.inputs["required"].size() == 1 &&
        entry->definition.inputs["required"][0].is_string()) {
        const std::string candidate = entry->definition.inputs["required"][0].get<std::string>();
        const auto property = props.is_object() ? props.find(candidate) : props.end();
        if (property == props.end() || !property->is_object() ||
            !property->contains("type") || (*property)["type"] == "string") {
            natural_language_field = candidate;
        }
    }
    const bool explicit_named_args = clean_args.rfind("--", 0) == 0;
    const bool natural_language_bound =
        natural_language_field.has_value() && !clean_args.empty() && !explicit_named_args;
    if (natural_language_bound) {
        inputs[*natural_language_field] = clean_args;
    }
    std::vector<std::string> positional;
    std::size_t pos = 0;
    while (!natural_language_bound && pos < clean_args.size()) {
        while (pos < clean_args.size() && (clean_args[pos] == ' ' || clean_args[pos] == '\t')) ++pos;
        if (pos >= clean_args.size()) break;
        if (clean_args.compare(pos, 2, "--") == 0) {
            pos += 2;
            const std::size_t name_start = pos;
            while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t' &&
                   clean_args[pos] != '=') {
                ++pos;
            }
            std::string name = clean_args.substr(name_start, pos - name_start);
            if (pos < clean_args.size() && clean_args[pos] == '=') ++pos;
            while (pos < clean_args.size() && (clean_args[pos] == ' ' || clean_args[pos] == '\t')) ++pos;
            const std::size_t value_start = pos;
            const bool quoted = pos < clean_args.size() && clean_args[pos] == '"';
            if (quoted) {
                ++pos;
                while (pos < clean_args.size() && clean_args[pos] != '"') ++pos;
                if (pos < clean_args.size()) ++pos;
            } else {
                while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t') ++pos;
            }
            std::string value = clean_args.substr(value_start, pos - value_start);
            if (quoted && value.size() >= 2) value = value.substr(1, value.size() - 2);
            if (!name.empty()) inputs[name] = CoerceWorkflowInput(props, name, value);
            continue;
        }
        const std::size_t start = pos;
        const bool quoted = clean_args[pos] == '"';
        if (quoted) {
            ++pos;
            while (pos < clean_args.size() && clean_args[pos] != '"') ++pos;
            if (pos < clean_args.size()) ++pos;
        } else {
            while (pos < clean_args.size() && clean_args[pos] != ' ' && clean_args[pos] != '\t') ++pos;
        }
        std::string token = clean_args.substr(start, pos - start);
        if (quoted && token.size() >= 2) token = token.substr(1, token.size() - 2);
        positional.push_back(token);
    }
    // 位置参依序填 required。只有一个 string 必填项时，它通常就是自然
    // 语言任务；把所有位置词重新连成整句，不能只取第一个词。
    if (entry->definition.inputs.contains("required") && entry->definition.inputs["required"].is_array()) {
        std::vector<std::string> missing_required;
        for (const auto& field : entry->definition.inputs["required"]) {
            if (field.is_string() && !inputs.contains(field.get<std::string>())) {
                missing_required.push_back(field.get<std::string>());
            }
        }
        if (missing_required.size() == 1 && !positional.empty()) {
            const std::string& name = missing_required.front();
            const auto property = props.is_object() ? props.find(name) : props.end();
            const bool string_field = property == props.end() || !property->is_object() ||
                                      !property->contains("type") || (*property)["type"] == "string";
            if (string_field) {
                std::string joined;
                for (const auto& token : positional) {
                    if (!joined.empty()) joined += ' ';
                    joined += token;
                }
                inputs[name] = joined;
                positional.clear();
            }
        }
        std::size_t arg_index = 0;
        for (const auto& field : entry->definition.inputs["required"]) {
            if (!field.is_string()) continue;
            const std::string name = field.get<std::string>();
            if (inputs.contains(name) || arg_index >= positional.size()) continue;
            inputs[name] = CoerceWorkflowInput(props, name, positional[arg_index++]);
        }
        if (context.request_input) {
            for (const auto& field : entry->definition.inputs["required"]) {
                if (!field.is_string()) continue;
                const std::string name = field.get<std::string>();
                if (inputs.contains(name)) continue;
                const nlohmann::json schema = props.is_object() && props.contains(name) && props[name].is_object()
                                                  ? props[name]
                                                  : nlohmann::json::object();
                const auto answer = context.request_input(name, schema);
                if (!answer.has_value() || answer->empty()) {
                    return theme.stats + "workflow 已取消：没有收到 " + name + theme.reset + "\n";
                }
                inputs[name] = CoerceWorkflowInput(props, name, *answer);
            }
        }
    }

    lubancode::workflow::RuntimeOptions options;
    options.executors = executors;
    if (context.home_lubancode.has_value()) {
        options.runs_root = *context.home_lubancode / "workflow-runs";
    }
    lubancode::workflow::WorkflowRuntime runtime(std::move(options));
    const lubancode::workflow::WorkflowRunSummary summary =
        runtime.Run(entry->definition, lubancode::workflow::RunInputs(inputs), cancel_token);

    std::ostringstream out;
    const char* run_mark = summary.state == lubancode::workflow::RunState::Succeeded ? "✓" : "×";
    out << run_mark << " " << entry->definition.name << " [" << entry->definition.id << "] " << theme.stats
        << lubancode::workflow::ToString(summary.state) << theme.reset << " · "
        << summary.duration_ms / 1000 << "." << (summary.duration_ms % 1000) / 100 << "s · tokens "
        << summary.tokens_used << "\n";
    for (const auto& [node_id, record] : summary.nodes) {
        const char* mark = record.state == lubancode::workflow::NodeState::Succeeded  ? "✓"
                           : record.state == lubancode::workflow::NodeState::Failed    ? "×"
                           : record.state == lubancode::workflow::NodeState::Skipped   ? "-"
                                                                                       : "…";
        const auto node = entry->definition.node_map.find(node_id);
        const std::string label = node != entry->definition.node_map.end() && !node->second.label.empty()
                                      ? node->second.label
                                      : node_id;
        out << "  " << mark << " " << label;
        if (label != node_id) out << theme.stats << "  " << node_id << theme.reset;
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
        std::string rendered = summary.result.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        constexpr std::size_t kResultPreviewBytes = 8000;
        if (rendered.size() > kResultPreviewBytes) {
            rendered.resize(lubancode::platform::Utf8PrefixBoundary(rendered, kResultPreviewBytes));
            rendered += "\n…(结果过长，已截断；完整账见 /workflow history)";
        }
        out << "\n结果\n" << rendered << "\n";
    } else if (!entry->definition.result.empty() &&
               summary.state == lubancode::workflow::RunState::Succeeded) {
        out << theme.error << "  结果为空：请检查 workflow 的 result 映射" << theme.reset << "\n";
    }
    if (!summary.error_message.empty() && summary.state != lubancode::workflow::RunState::Succeeded) {
        out << theme.error << "  " << summary.error_code << ": " << summary.error_message << theme.reset << "\n";
    }
    return out.str();
}

// ---- 执行器装配(终端接线收尾单自大类两段重复装配收口;原文随行) -------

std::map<lubancode::workflow::NodeKind, std::shared_ptr<lubancode::workflow::NodeExecutor>>
BuildWorkflowExecutors(const WorkflowCommandContext& wf_ctx, const WorkflowExecutorContext& exec_ctx,
                       const std::string& workflow_id) {
    std::map<lubancode::workflow::NodeKind, std::shared_ptr<lubancode::workflow::NodeExecutor>> executors;
    auto transform = std::make_shared<lubancode::workflow::TransformExecutor>();
    transform->Register("json_merge", [](const nlohmann::json& in) { return in; });
    executors[lubancode::workflow::NodeKind::Transform] = transform;
    executors[lubancode::workflow::NodeKind::Template] =
        std::make_shared<lubancode::workflow::TemplateExecutor>();
    executors[lubancode::workflow::NodeKind::Tool] =
        std::make_shared<lubancode::workflow::ToolExecutor>(exec_ctx.build_tool_options());
    std::shared_ptr<lubancode::workflow::LlmExecutor> llm_executor;
    {
        // prompt 从 workflow 目录读(包内相对路径;越界已被 validator
        // 拦,这里只管读)。
        const lubancode::workflow::Catalog wf_catalog =
            lubancode::workflow::LoadCatalog(wf_ctx.project_root, wf_ctx.user_root);
        const lubancode::workflow::CatalogEntry* wf_entry = wf_catalog.Find(workflow_id);
        const std::filesystem::path prompt_dir =
            wf_entry != nullptr ? wf_entry->dir : std::filesystem::path();
        const lubancode::workflow::PromptLoader workflow_prompt_loader = [prompt_dir](const std::string& relative) {
            if (prompt_dir.empty()) return std::string();
            std::error_code ec;
            const std::filesystem::path file = prompt_dir / relative;
            if (!std::filesystem::exists(file, ec)) return std::string();
            std::ifstream in(file, std::ios::binary);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        };
        lubancode::workflow::AgentExecutor::Options agent_options;
        agent_options.default_binding.backend = exec_ctx.backend;
        agent_options.default_binding.profile.provider = exec_ctx.provider;
        agent_options.default_binding.profile.request.model = exec_ctx.model;
        agent_options.default_binding.profile.request.reasoning_effort = exec_ctx.effort;
        if (exec_ctx.model_catalog != nullptr) {
            if (const auto* entry = exec_ctx.model_catalog->FindByProviderAndSlug(exec_ctx.provider, exec_ctx.model);
                entry != nullptr) {
                agent_options.default_binding.profile.request.reasoning = entry->reasoning;
            }
        }
        agent_options.default_binding.profile.runtime = exec_ctx.agent_profile;
        agent_options.registry = exec_ctx.registry;
        agent_options.task_loader = workflow_prompt_loader;
        // 批二:agent 节点上事件流(会话 sink,seq 与主回合同源)。
        agent_options.event_sink = exec_ctx.event_sink;
        agent_options.thread_id = exec_ctx.thread_id;
        agent_options.ids = exec_ctx.id_authority;
        executors[lubancode::workflow::NodeKind::Agent] =
            std::make_shared<lubancode::workflow::AgentExecutor>(std::move(agent_options));
        lubancode::workflow::LlmExecutor::Options llm_options;
        llm_options.backend = exec_ctx.backend;
        llm_options.model = exec_ctx.model;
        llm_options.prompt_loader = workflow_prompt_loader;
        llm_executor = std::make_shared<lubancode::workflow::LlmExecutor>(llm_options);
        executors[lubancode::workflow::NodeKind::Llm] = llm_executor;
    }

    executors[lubancode::workflow::NodeKind::Approval] =
        std::make_shared<lubancode::workflow::ApprovalExecutor>(exec_ctx.interaction_broker.get());
    executors[lubancode::workflow::NodeKind::AskUser] =
        std::make_shared<lubancode::workflow::AskUserExecutor>(exec_ctx.interaction_broker.get());

    std::map<std::string, std::string> skill_bodies;
    if (exec_ctx.skills != nullptr) {
        for (const auto& skill : *exec_ctx.skills) {
            const std::filesystem::path file = lubancode::tools::Utf8ToPath(skill.dir_path) / "SKILL.md";
            std::ifstream in(file, std::ios::binary);
            if (!in) continue;
            std::ostringstream content;
            content << in.rdbuf();
            const auto parsed = lubancode::tools::ParseSkillMarkdown(content.str());
            if (parsed.has_value()) skill_bodies.emplace(skill.name, parsed->body);
        }
    }
    executors[lubancode::workflow::NodeKind::Skill] =
        std::make_shared<lubancode::workflow::SkillExecutor>(llm_executor, std::move(skill_bodies));

    // nesting 首版只开一层。子流程另起自己的 Store 与预算账，只拿父节点
    // 明写的 input；事件、发号局与交互门仍沿用本场会话。
    if (exec_ctx.subflow_depth < 1) {
        const auto catalog = std::make_shared<lubancode::workflow::Catalog>(
            lubancode::workflow::LoadCatalog(wf_ctx.project_root, wf_ctx.user_root));
        lubancode::workflow::SubflowExecutor::DefinitionResolver resolver =
            [catalog](const std::string& id) -> std::optional<lubancode::workflow::WorkflowDefinition> {
                const auto* entry = catalog->Find(id);
                if (entry == nullptr || entry->broken) return std::nullopt;
                return entry->definition;
            };
        lubancode::workflow::SubflowExecutor::RuntimeRunner runner =
            [wf_ctx, exec_ctx](const lubancode::workflow::WorkflowDefinition& definition,
                               const nlohmann::json& inputs) {
                WorkflowExecutorContext child_ctx = exec_ctx;
                ++child_ctx.subflow_depth;
                lubancode::workflow::RuntimeOptions options;
                options.executors = BuildWorkflowExecutors(wf_ctx, child_ctx, definition.id);
                options.event_sink = exec_ctx.event_sink;
                options.broker = exec_ctx.interaction_broker.get();
                options.thread_id = exec_ctx.thread_id;
                options.id_authority = exec_ctx.id_authority;
                lubancode::workflow::WorkflowRuntime runtime(std::move(options));
                return runtime.Run(definition, lubancode::workflow::RunInputs(inputs));
            };
        executors[lubancode::workflow::NodeKind::Subflow] =
            std::make_shared<lubancode::workflow::SubflowExecutor>(std::move(resolver), std::move(runner));
    }
    return executors;
}

// ---------------------------------------------------------------------------
// 命令分派注册制(会话终章):workflow 域的分派位。/workflow 正门与
// /<alias> 直呼(Unknown 兜底)共用同一份 catalog/执行器装配。
// ---------------------------------------------------------------------------
namespace {

// catalog 上下文的现场装配(两案共用):project_root 现取 cwd,user_root/
// home_lubancode 按主目录,能力表取当前主表,技能名做撞名检查。
lubancode::app::WorkflowCommandContext BuildWorkflowCatalogContext(SlashDispatchContext& ctx) {
    lubancode::app::WorkflowCommandContext wf_ctx;
    wf_ctx.project_root = std::filesystem::current_path();
    wf_ctx.user_root = ctx.home_dir->has_value()
                           ? std::optional<std::filesystem::path>(
                                 lubancode::tools::Utf8ToPath(**ctx.home_dir))
                           : std::nullopt;
    wf_ctx.home_lubancode = ctx.home_lubancode->has_value()
                                ? std::optional<std::filesystem::path>(
                                      lubancode::tools::Utf8ToPath(**ctx.home_lubancode))
                                : std::nullopt;
    wf_ctx.registry = ctx.registry;
    for (const auto& skill : *ctx.skills) {
        wf_ctx.skill_names.push_back(skill.name);
    }
    wf_ctx.theme = ctx.theme;
    wf_ctx.request_input = [theme = ctx.theme](const std::string& field,
                                               const nlohmann::json& schema)
        -> std::optional<std::string> {
        // workflow 的忙时 footer 已开着；补问必填项时暂让整块屏面，答完
        // 再由作用域画回，提交后的 composer 不会只剩一枚裸光标。
        const lubancode::cli::StreamFooterSuspendScope footer_suspend;
        const std::string question = schema.value("description", "请补充 " + field + "：");
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << "\n" << question << "\n";
            TermOut().flush();
        }
        lubancode::cli::ReadExitReason reason = lubancode::cli::ReadExitReason::Cancel;
        return lubancode::cli::ReadLine(theme->prompt + "> " + theme->reset, *theme,
                                        /*esc_rejects=*/true, /*composer=*/true, &reason);
    };
    return wf_ctx;
}

// run/alias 直呼共用的执行器装配(终端接线收尾单收口,与正门同源)。
lubancode::app::WorkflowExecutorContext BuildWorkflowExecutorContext(SlashDispatchContext& ctx) {
    lubancode::app::WorkflowExecutorContext wf_exec;
    wf_exec.registry = ctx.registry;
    wf_exec.backend = ctx.real_backend;
    wf_exec.build_tool_options = ctx.build_workflow_tool_options;
    wf_exec.provider = *ctx.active_provider;
    wf_exec.model = *ctx.current_model;
    wf_exec.effort = *ctx.current_think;
    wf_exec.model_catalog = ctx.model_catalog;
    wf_exec.agent_profile = ctx.main_agent->runtime_profile();
    wf_exec.event_sink = ctx.session_events;
    wf_exec.thread_id = ctx.session_runtime->thread_id();
    wf_exec.id_authority = &ctx.session_runtime->ids();
    wf_exec.interaction_broker = std::make_shared<TerminalWorkflowInteractionBroker>(*ctx.theme);
    wf_exec.skills = ctx.skills;
    return wf_exec;
}

std::string RunWorkflowFromTerminal(SlashDispatchContext& ctx,
                                    const lubancode::app::WorkflowCommandContext& wf_ctx,
                                    const std::string& workflow_id, const std::string& raw_args) {
    lubancode::app::WorkflowExecutorContext wf_exec = BuildWorkflowExecutorContext(ctx);
    WorkflowTerminalRunScope terminal_run(*ctx.theme, ctx.spinner_enabled);
    return lubancode::app::RunWorkflowById(
        wf_ctx, workflow_id, raw_args,
        lubancode::app::BuildWorkflowExecutors(wf_ctx, wf_exec, workflow_id),
        terminal_run.cancel_token());
}

}  // namespace

CommandFlow HandleSlashWorkflow(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Workflows 自然语言编排单:正门 /workflow。catalog 现扫现用,不占会话
    // 状态;能力表取自当前主表(此刻挂着的工具)。
    lubancode::app::WorkflowCommandContext wf_ctx = BuildWorkflowCatalogContext(ctx);
    const lubancode::app::ParsedWorkflowCommand wf_parsed = lubancode::app::ParseWorkflowCommand(parsed.args);
    if (wf_parsed.action == lubancode::app::WorkflowCommandAction::Run) {
        // run:执行器装配与 alias 直呼共用一份；footer 收妥后摘要再落屏。
        TermOut() << RunWorkflowFromTerminal(ctx, wf_ctx, wf_parsed.id, wf_parsed.rest);
        return CommandFlow::Continue;
    }
    HandleWorkflowCommand(parsed.args, wf_ctx);
    if (ctx.refresh_workflow_completions) ctx.refresh_workflow_completions();
    return CommandFlow::Continue;
}

CommandFlow HandleSlashUnknown(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed) {
    // Workflows 单:不认得的 / 词先查 WorkflowCatalog——查着了是 /<alias>
    // 直呼(整行参数按 input_schema 解析,不当一坨 prompt),查不着才打
    // "不认得"。内建词永远居首,撞名禁用的 alias 也不接(只留 /workflow
    // run 正门)。
    if (!parsed.alias_word.empty()) {
        lubancode::app::WorkflowCommandContext wf_ctx = BuildWorkflowCatalogContext(ctx);
        const std::string wf_id = ResolveWorkflowAlias(wf_ctx, parsed.alias_word);
        if (!wf_id.empty()) {
            // 与 /workflow run 同一道终端外壳和执行器装配。
            TermOut() << RunWorkflowFromTerminal(ctx, wf_ctx, wf_id, parsed.args);
            return CommandFlow::Continue;
        }
    }
    TermOut() << trf("error.unknown_command", parsed.raw_word) << "\n";
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
