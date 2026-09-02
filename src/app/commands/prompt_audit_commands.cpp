// /prompt audit 的执行体(Token 账本单 A3)。分派位在文件尾,纯函数在
// 前——解析、渲染、JSON 都不碰 IO;IO 只有读 Journal、走拼装现场、打印。
#include "app/commands/prompt_audit_commands.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <sstream>

#include "agent/prompts.hpp"  // StripPromptComments
#include "agent/prompt_assembler.hpp"
#include "agent/resolved_prompt_builder.hpp"
#include "app/commands/command_registry.hpp"  // SlashDispatchContext(分派位用)
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "insights/redaction.hpp"
#include "tools/registry.hpp"
#include "trajectory/directory.hpp"  // ReadSessionJson(runtime 的封口口径)

namespace lubancode::app {
namespace {

using lubancode::cli::TermOut;
using lubancode::insights::EvidenceItem;
using lubancode::insights::Finding;
using lubancode::insights::FindingConfidence;
using lubancode::insights::FindingSeverity;

const char* SeverityLabel(FindingSeverity severity) {
    switch (severity) {
        case FindingSeverity::Info: return "info";
        case FindingSeverity::Warning: return "warning";
        case FindingSeverity::High: return "high";
    }
    return "?";
}

const char* ConfidenceLabel(FindingConfidence confidence) {
    switch (confidence) {
        case FindingConfidence::Low: return "low";
        case FindingConfidence::Medium: return "medium";
        case FindingConfidence::High: return "high";
    }
    return "?";
}

// 证据值的一行短账(JSON 折平,限长;正文不进来,这里是防御性截断)。
std::string EvidenceLine(const EvidenceItem& item) {
    std::string text = item.value.dump();
    if (text.size() > 100) {
        text = text.substr(0, 100) + "…";
    }
    std::string line = "      " + item.metric + "=" + text;
    if (item.event_id.has_value()) {
        line += " @event " + *item.event_id;
    }
    return line;
}

// tools 层来源枚举 -> 稳定名(与 agent::ToString(ToolSourceKind) 同表;
// 不引 agent 层,防 insights 面反向耦合)。
std::string ToolSourceName(lubancode::tools::ToolSourceKind kind) {
    switch (kind) {
        case lubancode::tools::ToolSourceKind::Builtin: return "builtin";
        case lubancode::tools::ToolSourceKind::Mcp: return "mcp";
        case lubancode::tools::ToolSourceKind::Lsp: return "lsp";
        case lubancode::tools::ToolSourceKind::PluginLua: return "plugin-lua";
        case lubancode::tools::ToolSourceKind::PluginNative: return "plugin-native";
        case lubancode::tools::ToolSourceKind::Agent: return "agent";
        case lubancode::tools::ToolSourceKind::Ptc: return "ptc";
        case lubancode::tools::ToolSourceKind::Deferred: return "deferred";
    }
    return "builtin";
}

}  // namespace

// ---------------- 纯函数 ----------------

ParsedPromptAuditCommand ParsePromptAuditCommand(const std::string& args) {
    ParsedPromptAuditCommand parsed;
    std::istringstream stream(args);
    std::string word;
    if (!(stream >> word)) {
        parsed.bad_word = "(空)";
        return parsed;  // 裸 /prompt audit:usage 由 handler 打
    }
    if (word == "static") {
        parsed.mode = ParsedPromptAuditCommand::Mode::Static;
    } else if (word == "runtime") {
        parsed.mode = ParsedPromptAuditCommand::Mode::Runtime;
        std::string session;
        if (stream >> session) {
            if (session == "--json") {
                parsed.json = true;
            } else if (session.rfind("--", 0) == 0) {
                parsed.invalid = true;
                parsed.bad_word = session;
                return parsed;
            } else {
                parsed.session_id = session;
            }
        }
    } else if (word == "outcome") {
        parsed.mode = ParsedPromptAuditCommand::Mode::Outcome;
    } else if (word == "all") {
        parsed.mode = ParsedPromptAuditCommand::Mode::All;
    } else if (word == "explain") {
        parsed.mode = ParsedPromptAuditCommand::Mode::Explain;
        if (!(stream >> parsed.finding_id)) {
            parsed.invalid = true;
            parsed.bad_word = "explain";
            return parsed;
        }
    } else {
        parsed.invalid = true;
        parsed.bad_word = word;
        return parsed;
    }
    while (stream >> word) {
        if (word == "--json") {
            parsed.json = true;
        } else if (word == "--model-review") {
            parsed.later_model_review = true;
        } else if (word == "--since") {
            std::string value;
            if (!(stream >> value)) {
                parsed.invalid = true;
                parsed.bad_word = "--since";
                continue;
            }
            std::string digits = value;
            if (!digits.empty() && (digits.back() == 'd' || digits.back() == 'D')) {
                digits.pop_back();
            }
            bool ok = !digits.empty();
            for (const char c : digits) {
                ok = ok && c >= '0' && c <= '9';
            }
            if (!ok) {
                parsed.invalid = true;
                parsed.bad_word = value;
                continue;
            }
            parsed.since_days = std::stoi(digits);
        } else {
            parsed.invalid = true;
            parsed.bad_word = word;
        }
    }
    return parsed;
}

std::vector<std::string> FormatPromptAuditReport(const PromptAuditReportModel& model) {
    std::vector<std::string> lines;
    std::string title = "Prompt audit · " + model.mode;
    if (model.mode == "runtime") {
        title += " · " + model.session_id;
        if (model.provisional) {
            title += "(未封口 provisional)";
        }
    }
    lines.push_back(std::move(title));

    // static 事实账:system/魂/模型指令/工具定义四栏 + 段级表。
    if (model.has_static) {
        const auto& facts = model.facts;
        std::ostringstream out;
        out << "  构成        system " << lubancode::cli::FormatTokenCount(facts.system_tokens)
            << " · 魂 " << lubancode::cli::FormatTokenCount(facts.soul_tokens)
            << " · 模型指令 "
            << lubancode::cli::FormatTokenCount(facts.model_instructions_tokens)
            << " · 工具定义 "
            << lubancode::cli::FormatTokenCount(facts.tool_definition_tokens) << "("
            << facts.tool_count << " 枚)";
        if (facts.budget_tokens > 0) {
            out << " · 合计约占预算 "
                << static_cast<int>((facts.total_context_tokens * 200 +
                                     facts.budget_tokens) /
                                    (facts.budget_tokens * 2))
                << "%";
        } else {
            out << " · 预算未知,占比不判";
        }
        lines.push_back(out.str());
        if (!facts.segments.empty()) {
            // 段级 Top 5(token 降序,同份字典序):常驻的大头一眼可见。
            std::vector<const insights::PromptAuditFacts::SegmentFact*> sorted;
            for (const auto& segment : facts.segments) {
                sorted.push_back(&segment);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto* a, const auto* b) {
                          if (a->tokens != b->tokens) {
                              return a->tokens > b->tokens;
                          }
                          return a->segment_id < b->segment_id;
                      });
            std::ostringstream seg_line;
            seg_line << "  段级 Top    ";
            for (std::size_t i = 0; i < sorted.size() && i < 5; ++i) {
                if (i > 0) {
                    seg_line << " · ";
                }
                seg_line << sorted[i]->segment_id << " "
                         << lubancode::cli::FormatTokenCount(sorted[i]->tokens);
                if (sorted[i]->volatile_segment) {
                    seg_line << "(动态)";
                }
            }
            if (sorted.size() > 5) {
                seg_line << " · …共 " << sorted.size() << " 段";
            }
            lines.push_back(seg_line.str());
        }
    }

    // runtime 逐请求表(最多 6 行,超了计数;只摆事实)。
    if (model.mode == "runtime" || model.mode == "all") {
        if (model.requests.empty()) {
            lines.push_back("  请求        这场 session 没有模型请求账——不猜");
        } else {
            std::size_t shown = std::min<std::size_t>(model.requests.size(), 6);
            for (std::size_t i = 0; i < shown; ++i) {
                const auto& view = model.requests[i];
                std::ostringstream out;
                out << "  请求 " << view.request_id << "  purpose=" << view.purpose;
                if (view.snapshot.has_value()) {
                    out << " · tools "
                        << view.snapshot->request_shape.tool_count << " 枚/"
                        << lubancode::cli::FormatTokenCount(
                               view.snapshot->request_shape.tool_definition_tokens_estimated)
                        << " · messages "
                        << view.snapshot->request_shape.message_count;
                } else {
                    out << " · 无 snapshot";
                }
                if (view.usage_reported) {
                    const int ratio = view.total_input_tokens > 0
                                          ? static_cast<int>((view.cache_read_tokens * 200 +
                                                              view.total_input_tokens) /
                                                             (view.total_input_tokens * 2))
                                          : 0;
                    out << " · cache 读 " << ratio << "%";
                } else {
                    out << " · usage 未报";
                }
                lines.push_back(out.str());
            }
            if (model.requests.size() > shown) {
                std::ostringstream out;
                out << "    …另有 " << (model.requests.size() - shown) << " 笔";
                lines.push_back(out.str());
            }
        }
    }

    // outcome coverage:active/incomplete/corrupt 单列,不混分母。
    if (model.has_outcome) {
        std::ostringstream out;
        out << "  场次        found " << model.sessions_found;
        for (const auto& [status, count] : model.status_counts) {
            out << " · " << status << " " << count;
        }
        lines.push_back(out.str());
        for (const auto& entry : model.scan) {
            if (entry.status == lubancode::insights::SessionGateStatus::Analyzed ||
                entry.reason.empty()) {
                continue;
            }
            std::string reason = entry.reason;
            if (reason.size() > 90) {
                reason = reason.substr(0, 90) + "…";
            }
            lines.push_back("    排除 " + entry.session_id + ": " + reason);
        }
    }

    // 发现:每条四行起(头/摘要/建议/证据);explain 模式换全账渲染。
    if (!model.findings.empty()) {
        std::ostringstream head;
        head << "  发现 " << model.findings.size() << " 条";
        lines.push_back(head.str());
        for (const auto& finding : model.findings) {
            lines.push_back("    " + finding.finding_id + " · " +
                            SeverityLabel(finding.severity) + " · 证据置信 " +
                            ConfidenceLabel(finding.confidence) + " · " + finding.category);
            lines.push_back("      " + finding.summary);
            lines.push_back("      建议: " + finding.recommendation);
            for (const auto& item : finding.evidence) {
                lines.push_back(EvidenceLine(item));
            }
            for (const auto& item : finding.counter_evidence) {
                lines.push_back("      反证 " + EvidenceLine(item).substr(6));
            }
        }
    } else {
        lines.push_back("  发现 0 条(规则没命中不硬凑;语义复核属 --model-review,A6)");
    }

    // 功能信号(A4 的建议面:只指现成功能)。
    if (!model.signals.empty()) {
        lines.push_back("  可少走弯路  " + std::to_string(model.signals.size()) + " 条");
        for (const auto& signal : model.signals) {
            lines.push_back("    " + signal.signal_id + " · " + signal.feature);
            lines.push_back("      " + signal.summary);
            lines.push_back("      先决: " + signal.precondition);
        }
    }

    if (!model.warnings.empty()) {
        lines.push_back("  缺口点名");
        const std::size_t shown = std::min<std::size_t>(model.warnings.size(), 5);
        for (std::size_t i = 0; i < shown; ++i) {
            lines.push_back("    " + model.warnings[i]);
        }
        if (model.warnings.size() > shown) {
            std::ostringstream out;
            out << "    …另有 " << (model.warnings.size() - shown) << " 条";
            lines.push_back(out.str());
        }
    }
    lines.push_back("  口径        只摆事实;prompt 正文与绝对路径不进报告;语义类仅 suspected");
    return lines;
}

nlohmann::json BuildPromptAuditJson(const PromptAuditReportModel& model) {
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& finding : model.findings) {
        findings.push_back(finding.ToJson());
    }
    nlohmann::json requests = nlohmann::json::array();
    for (const auto& view : model.requests) {
        nlohmann::json row{{"run_id", view.run_id},
                           {"request_id", view.request_id},
                           {"purpose", view.purpose},
                           {"event_id", view.event_id},
                           {"usage_reported", view.usage_reported}};
        if (view.snapshot.has_value()) {
            row["request_shape"] = view.snapshot->request_shape.ToJson();
        } else {
            row["request_shape"] = nullptr;
        }
        requests.push_back(std::move(row));
    }
    nlohmann::json scan = nlohmann::json::array();
    for (const auto& entry : model.scan) {
        scan.push_back(nlohmann::json{{"session_id", entry.session_id},
                                      {"status", lubancode::insights::SessionGateStatusName(
                                                     entry.status)},
                                      {"reason", entry.reason}});
    }
    nlohmann::json signals = nlohmann::json::array();
    for (const auto& signal : model.signals) {
        nlohmann::json evidence = nlohmann::json::array();
        for (const auto& item : signal.evidence) {
            evidence.push_back(item.ToJson());
        }
        signals.push_back(nlohmann::json{{"signal_id", signal.signal_id},
                                         {"feature", signal.feature},
                                         {"summary", signal.summary},
                                         {"precondition", signal.precondition},
                                         {"evidence", evidence}});
    }
    return nlohmann::json{
        {"schema", lubancode::insights::kPromptAuditReportSchema},
        {"schema_version", lubancode::insights::kPromptAuditReportSchemaVersion},
        {"rule_version", lubancode::insights::kPromptAuditRuleVersion},
        {"mode", model.mode},
        {"session_id", model.session_id},
        {"provisional", model.provisional},
        {"static_facts", model.has_static ? model.facts.ToJson() : nlohmann::json()},
        {"findings", findings},
        {"runtime_requests", requests},
        {"outcome_scan", scan},
        {"feature_signals", signals},
        {"warnings", model.warnings},
        {"privacy",
         nlohmann::json{{"content_policy", "metadata_only"}, {"prompt_text_included", false}}}};
}

// ---------------- 执行(IO) ----------------

namespace {

// static 输入装配:走真实拼装现场(ResolvedPromptBuilder),manifest 与
// 段级正文账出自同一次拼装,不是事后拆字符串。只装材料,规则在领域层。
bool BuildStaticInput(const PromptAuditContext& context,
                      lubancode::insights::StaticAuditInput& input) {
    if (context.prompt_options == nullptr) {
        return false;
    }
    const lubancode::agent::ResolvedPromptBase base =
        lubancode::agent::BuildResolvedPromptBase(*context.prompt_options);
    const lubancode::agent::AssembledPrompt assembled = lubancode::agent::ResolveFinalPrompt(
        base, std::string(), context.model_instructions, context.soul_text, context.soul_name);
    input.manifest = assembled.manifest;
    for (const auto& entry : base.ledger.entries) {
        if (!entry.content_text.empty()) {
            input.segment_texts[entry.rel_path] = entry.content_text;
        }
    }
    input.soul_text = context.soul_text;
    input.model_instructions_text = context.model_instructions;
    if (!context.prompts_dir.empty()) {
        input.module_sources = lubancode::agent::PromptModuleSources(context.prompts_dir);
    }
    input.context_budget_tokens = context.context_budget_tokens;
    if (context.registry != nullptr) {
        for (const auto& tool : context.registry->All()) {
            lubancode::insights::AuditToolDefinition definition;
            definition.name = tool->name();
            definition.description = tool->description();
            definition.input_schema = tool->input_schema();
            const auto* registration = context.registry->RegistrationOf(tool->name());
            if (registration != nullptr) {
                definition.source_kind = ToolSourceName(registration->source_kind);
                definition.source_instance = registration->source_instance;
            } else {
                definition.source_kind = "builtin";
            }
            input.tools.push_back(std::move(definition));
        }
    }
    return true;
}

std::string NowYyyymmdd(const PromptAuditContext& context) {
    if (!context.now_yyyymmdd.empty()) {
        return context.now_yyyymmdd;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buffer;
}

}  // namespace

void HandlePromptAuditCommand(const std::string& args, const PromptAuditContext& context) {
    const ParsedPromptAuditCommand parsed = ParsePromptAuditCommand(args);
    if (parsed.mode == ParsedPromptAuditCommand::Mode::Invalid) {
        TermOut() << context.theme.error
                  << lubancode::cli::trf("cmd.prompt.audit.unknown_arg", parsed.bad_word) << "\n"
                  << lubancode::cli::tr("cmd.prompt.audit.usage_line") << "\n"
                  << context.theme.reset << "\n";
        TermOut().flush();
        return;
    }
    if (parsed.later_model_review) {
        TermOut() << lubancode::cli::tr("cmd.prompt.audit.later_model_review") << "\n";
    }

    PromptAuditReportModel model;
    const bool wants_static = parsed.mode == ParsedPromptAuditCommand::Mode::Static ||
                              parsed.mode == ParsedPromptAuditCommand::Mode::All ||
                              parsed.mode == ParsedPromptAuditCommand::Mode::Explain;
    const bool wants_runtime = parsed.mode == ParsedPromptAuditCommand::Mode::Runtime ||
                               parsed.mode == ParsedPromptAuditCommand::Mode::All ||
                               parsed.mode == ParsedPromptAuditCommand::Mode::Explain;
    const bool wants_outcome = parsed.mode == ParsedPromptAuditCommand::Mode::Outcome ||
                               parsed.mode == ParsedPromptAuditCommand::Mode::All ||
                               parsed.mode == ParsedPromptAuditCommand::Mode::Explain;
    model.mode = parsed.mode == ParsedPromptAuditCommand::Mode::Static     ? "static"
                 : parsed.mode == ParsedPromptAuditCommand::Mode::Runtime  ? "runtime"
                 : parsed.mode == ParsedPromptAuditCommand::Mode::Outcome  ? "outcome"
                 : parsed.mode == ParsedPromptAuditCommand::Mode::Explain  ? "all(explain)"
                                                                           : "all";

    // ---- static:不碰 Journal,账没开也照跑 ----
    if (wants_static) {
        lubancode::insights::StaticAuditInput input;
        lubancode::insights::PromptAuditFacts facts;
        if (BuildStaticInput(context, input)) {
            std::vector<Finding> static_findings =
                lubancode::insights::AuditPromptStatic(input, &facts);
            model.has_static = true;
            model.facts = facts;
            model.findings.insert(model.findings.end(), static_findings.begin(),
                                  static_findings.end());
        } else {
            model.warnings.push_back("prompt.static_unavailable: 拼装现场没接(单测/旧装配),static 不猜");
        }
    }

    // ---- runtime:要 Journal ----
    if (wants_runtime) {
        if (context.trajectory == nullptr) {
            model.warnings.push_back(
                "prompt.runtime_ledger_off: Token 账本不可得,runtime 层没有事实可审");
        } else {
            std::filesystem::path session_dir;
            if (!parsed.session_id.empty()) {
                session_dir = context.sessions_root / parsed.session_id;
                if (!std::filesystem::is_directory(session_dir)) {
                    TermOut() << context.theme.error
                              << lubancode::cli::trf("cmd.prompt.audit.session_not_found",
                                                     parsed.session_id)
                              << "\n"
                              << context.theme.reset << "\n";
                    TermOut().flush();
                    return;
                }
            } else {
                session_dir = context.trajectory->session_dir();
            }
            lubancode::insights::RuntimeRequestsRead read =
                lubancode::insights::CollectRuntimeRequests(session_dir);
            if (!read.ok) {
                TermOut() << context.theme.error << read.message << "\n"
                          << context.theme.reset << "\n";
                TermOut().flush();
                return;
            }
            model.session_id = read.session_id;
            const auto session_manifest = lubancode::trajectory::ReadSessionJson(session_dir);
            model.provisional =
                !session_manifest.has_value() || session_manifest->status != "closed";
            model.requests = std::move(read.requests);
            for (const auto& warning : read.warnings) {
                model.warnings.push_back(warning);
            }
            lubancode::insights::RuntimeAuditInput audit_input;
            audit_input.session_id = read.session_id;
            audit_input.requests = model.requests;
            std::vector<Finding> runtime_findings =
                lubancode::insights::AuditPromptRuntime(audit_input);
            model.findings.insert(model.findings.end(), runtime_findings.begin(),
                                  runtime_findings.end());
        }
    }

    // ---- outcome:多场扫描 + A4 分析器(只读,不写派生) ----
    if (wants_outcome) {
        if (context.trajectory == nullptr) {
            model.warnings.push_back(
                "prompt.outcome_ledger_off: Token 账本不可得,outcome 层没有事实可审");
        } else {
            const std::string now = NowYyyymmdd(context);
            const lubancode::insights::WorkspaceScanReport scan =
                lubancode::insights::ScanWorkspaceSessions(context.sessions_root, now,
                                                           parsed.since_days);
            model.has_outcome = true;
            model.scan = scan.entries;
            model.status_counts = scan.status_counts;
            model.sessions_found = scan.sessions_found;
            std::vector<lubancode::insights::SessionAnalyzeResult> analyzed;
            for (const auto& entry : scan.entries) {
                if (entry.status != lubancode::insights::SessionGateStatus::Analyzed) {
                    continue;
                }
                lubancode::insights::SessionAnalyzeOptions options;
                options.write_summary = false;  // /prompt audit 是 read_only 面
                analyzed.push_back(
                    lubancode::insights::AnalyzeSession(context.sessions_root / entry.session_id,
                                                        options));
            }
            std::vector<Finding> outcome_findings =
                lubancode::insights::AuditPromptOutcome(analyzed);
            model.findings.insert(model.findings.end(), outcome_findings.begin(),
                                  outcome_findings.end());
            for (const auto& session : analyzed) {
                for (const auto& signal : session.signals) {
                    model.signals.push_back(signal);
                }
                for (const auto& warning : session.warnings) {
                    model.warnings.push_back(warning);
                }
            }
        }
    }

    // ---- explain:同输入重跑三层,按稳定 finding_id 挑一条打全账 ----
    if (parsed.mode == ParsedPromptAuditCommand::Mode::Explain) {
        const auto it = std::find_if(model.findings.begin(), model.findings.end(),
                                     [&](const Finding& finding) {
                                         return finding.finding_id == parsed.finding_id;
                                     });
        if (it == model.findings.end()) {
            TermOut() << context.theme.error
                      << lubancode::cli::trf("cmd.prompt.audit.finding_not_found",
                                             parsed.finding_id)
                      << "\n"
                      << context.theme.reset << "\n";
            TermOut().flush();
            return;
        }
        if (parsed.json) {
            TermOut() << it->ToJson().dump(2) << "\n";
            TermOut().flush();
            return;
        }
        const Finding& finding = *it;
        TermOut() << finding.finding_id << " · " << finding.category << "\n"
                  << "  严重度 " << SeverityLabel(finding.severity) << "(讲影响) · 置信 "
                  << ConfidenceLabel(finding.confidence) << "(讲证据;两者不换算)\n"
                  << "  " << finding.summary << "\n"
                  << "  证据\n";
        for (const auto& item : finding.evidence) {
            TermOut() << "    " << EvidenceLine(item).substr(6) << "\n";
        }
        if (!finding.counter_evidence.empty()) {
            TermOut() << "  反证\n";
            for (const auto& item : finding.counter_evidence) {
                TermOut() << "    " << EvidenceLine(item).substr(6) << "\n";
            }
        }
        TermOut() << "  建议  " << finding.recommendation << "\n"
                  << "  规则  " << finding.rule_version << "(deterministic_rule)\n";
        TermOut().flush();
        return;
    }

    if (parsed.json) {
        TermOut() << lubancode::insights::RedactSecrets(BuildPromptAuditJson(model).dump(2))
                  << "\n";
        TermOut().flush();
        return;
    }
    for (const auto& line : FormatPromptAuditReport(model)) {
        TermOut() << line << "\n";
    }
    TermOut().flush();
}

// 命令分派注册制:/prompt audit 的分派位(prompt_commands 的壳递到这)。
CommandFlow HandleSlashPromptAudit(SlashDispatchContext& ctx, const std::string& audit_args) {
    PromptAuditContext context{*ctx.theme};
    context.prompt_options = ctx.prompt_options;
    context.prompts_dir = ctx.prompts_dir != nullptr ? *ctx.prompts_dir : std::string();
    context.model_instructions =
        ctx.current_model_instructions != nullptr ? *ctx.current_model_instructions : std::string();
    if (ctx.current_soul != nullptr) {
        context.soul_text = lubancode::agent::StripPromptComments(*ctx.current_soul);
    }
    context.soul_name = ctx.current_soul_name != nullptr ? *ctx.current_soul_name : "default";
    if (ctx.context_tracker != nullptr) {
        context.context_budget_tokens =
            static_cast<std::int64_t>(ctx.context_tracker->window_tokens());
    }
    context.registry = ctx.registry;
    context.trajectory = ctx.trajectory;
    if (ctx.trajectory != nullptr) {
        context.sessions_root = ctx.trajectory->session_dir().parent_path();
    }
    HandlePromptAuditCommand(audit_args, context);
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
