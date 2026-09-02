#include "insights/session_analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <set>

#include "accounting/usage_aggregate.hpp"
#include "accounting/usage_projector.hpp"
#include "insights/derived_store.hpp"

namespace lubancode::insights {
namespace {

// "YYYYMMDD" -> days 计;形状不合给 nullopt(不猜日期)。
std::optional<std::int64_t> DaysOf(const std::string& yyyymmdd) {
    if (yyyymmdd.size() != 8) {
        return std::nullopt;
    }
    for (const char c : yyyymmdd) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }
    const int year = std::stoi(yyyymmdd.substr(0, 4));
    const unsigned month = static_cast<unsigned>(std::stoi(yyyymmdd.substr(4, 2)));
    const unsigned day = static_cast<unsigned>(std::stoi(yyyymmdd.substr(6, 2)));
    const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                          std::chrono::day{day}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(
        std::chrono::sys_days{ymd}.time_since_epoch().count());
}

}  // namespace

SessionAnalyzeResult AnalyzeSession(const std::filesystem::path& session_dir,
                                    const SessionAnalyzeOptions& options) {
    SessionAnalyzeResult result;
    result.gate = GateSession(session_dir);
    const bool active_ok =
        result.gate.status == SessionGateStatus::Active && options.include_active;
    if (result.gate.status != SessionGateStatus::Analyzed && !active_ok) {
        return result;
    }
    result.analyzed = true;
    result.provisional = result.gate.status != SessionGateStatus::Analyzed;

    SessionInsightSummary& summary = result.summary;
    summary.source.session_id = result.gate.session_id;
    summary.source.stream_terminal_hashes = result.gate.stream_terminal_hashes;
    summary.source.integrity = result.provisional ? "provisional" : "verified";

    // usage 投影 + work/coverage 的底账。
    std::vector<accounting::UsageSample> samples;
    FeatureSignalInput signal_input;
    signal_input.session_id = result.gate.session_id;
    std::set<std::string> files_touched;
    std::map<std::string, std::int64_t> verification_kinds;
    std::string last_outcome;

    for (const auto& [run_id, envelopes] : result.gate.streams) {
        summary.coverage.runs_total += 1;
        const accounting::UsageProjection projection = accounting::ProjectUsage(envelopes);
        if (!projection.ok) {
            result.warnings.push_back("usage.stream_rejected: " + run_id + ": " +
                                      projection.error_code);
        } else {
            for (const auto& warning : projection.warnings) {
                result.warnings.push_back(warning);
            }
            for (const auto& sample : projection.samples) {
                samples.push_back(sample);
                summary.coverage.requests_total += 1;
                if (sample.usage.has_value()) {
                    summary.coverage.requests_with_usage += 1;
                    summary.usage.requests_with_usage += 1;
                    summary.usage.input_tokens += sample.usage->input_tokens;
                    summary.usage.cache_read_tokens += sample.usage->cache_read_tokens;
                    summary.usage.cache_creation_tokens +=
                        sample.usage->cache_creation_tokens;
                    summary.usage.output_tokens += sample.usage->output_tokens;
                    summary.usage.reasoning_tokens += sample.usage->output_reasoning_tokens;
                }
                const std::int64_t shape = sample.total_billed_shape_tokens;
                // one_shot 的 main run 与交互 main 同类(单发轨迹断档单):
                // 单发一场就是该进程的主会话,token 计入 main 侧,不冒充
                // 子代理(subagent 收窄建议的分母别被单发重会话灌歪)。
                if (sample.run_kind == "main_session" || sample.run_kind == "one_shot") {
                    signal_input.main_tokens += shape;
                } else {
                    signal_input.subagent_tokens += shape;
                }
            }
        }
        summary.coverage.runs_analyzed += 1;
        for (const auto& envelope : envelopes) {
            switch (envelope.kind) {
                case trajectory::EventKind::TurnStarted:
                    summary.work.turns += 1;
                    break;
                case trajectory::EventKind::ToolResultCommitted:
                    summary.work.tool_calls += 1;
                    break;
                case trajectory::EventKind::ToolInputEffective: {
                    // files_touched:effective_arguments 里 path/file_path 字段
                    // 的去重计数(只数路径,不存路径)。
                    const auto args = envelope.payload.find("effective_arguments");
                    if (args != envelope.payload.end() && args->is_object()) {
                        for (const char* key : {"path", "file_path"}) {
                            const auto it = args->find(key);
                            if (it != args->end() && it->is_string()) {
                                files_touched.insert(it->get<std::string>());
                            }
                        }
                    }
                    break;
                }
                case trajectory::EventKind::VerificationRecorded: {
                    summary.work.verifications += 1;
                    verification_kinds[envelope.payload.value("kind", "")] += 1;
                    break;
                }
                case trajectory::EventKind::OutcomeAssessed:
                    summary.coverage.outcomes_assessed += 1;
                    last_outcome = envelope.payload.value("outcome", "");
                    break;
                default:
                    break;
            }
        }
        for (auto& occurrence : ClassifyFriction(envelopes)) {
            result.frictions.push_back(std::move(occurrence));
        }
    }
    summary.usage.requests_total = summary.coverage.requests_total;
    summary.work.files_touched = files_touched.size();
    summary.work.outcome = last_outcome;
    summary.usage.cost_status = "not_priced";  // A4 不贴价;A5 聚合层带价格表

    // 摩擦折叠:类名去重排序进 summary(§6.5);occurrence 留给调用方。
    {
        std::set<std::string> categories;
        for (const auto& occurrence : result.frictions) {
            categories.insert(occurrence.category);
            signal_input.friction_counts[occurrence.category] += 1;
        }
        summary.friction_events.assign(categories.begin(), categories.end());
    }
    signal_input.verification_kinds = verification_kinds;

    // runtime 层变化(prompt audit 的 runtime 半场)。
    {
        RuntimeRequestsRead runtime =
            CollectRuntimeRequestsFromStreams(result.gate.streams);
        for (const auto& warning : runtime.warnings) {
            result.warnings.push_back(warning);
        }
        RuntimeAuditInput audit_input;
        audit_input.session_id = result.gate.session_id;
        audit_input.requests = std::move(runtime.requests);
        result.prompt_findings = AuditPromptRuntime(audit_input);
        summary.prompt_findings = result.prompt_findings;
        const RuntimeChangeSummary changes =
            SummarizeRuntimeChanges(audit_input.requests);
        signal_input.prefix_breaks_same_epoch = changes.prefix_breaks_same_epoch;
        // 工具定义占比:按请求均值比请求均值(同量纲才可比)。
        std::int64_t tool_tokens = 0;
        std::int64_t tool_count = 0;
        std::int64_t input_sum = 0;
        std::int64_t input_count = 0;
        for (const auto& view : audit_input.requests) {
            if (view.snapshot.has_value()) {
                tool_tokens += view.snapshot->request_shape.tool_definition_tokens_estimated;
                tool_count += 1;
            }
            if (view.usage_reported && view.total_input_tokens > 0) {
                input_sum += view.total_input_tokens;
                input_count += 1;
            }
        }
        if (tool_count > 0 && input_count > 0) {
            signal_input.tool_definition_tokens = tool_tokens / tool_count;
            signal_input.total_input_tokens = input_sum / input_count;
        }
    }

    // cache 行为(A2 的账)进功能信号。
    {
        const accounting::UsageAggregate aggregate = accounting::AggregateUsage(samples);
        signal_input.unexpected_miss_candidates =
            aggregate.cache.unexpected_miss_candidates;
        signal_input.cache_read_tokens = aggregate.totals.cache_read_tokens;
    }

    result.signals = DetectFeatureSignals(signal_input);
    summary.feature_signals.clear();
    for (const auto& signal : result.signals) {
        summary.feature_signals.push_back(signal.signal_id);
    }

    // 摘要落盘:active 不写长期摘要(§14.2,免与 writer 争)。
    if (options.write_summary && !result.provisional) {
        const DerivedReadResult existing = ReadExistingSessionSummary(session_dir);
        if (existing.exists && existing.parse_ok &&
            !IsSummaryStale(existing, result.gate.stream_terminal_hashes)) {
            result.summary_reused = true;
            result.summary = existing.summary;
        } else {
            const DerivedWriteResult written =
                WriteSessionSummaryAtomic(session_dir, summary);
            if (written.ok) {
                result.summary_written = true;
            } else {
                result.derived_error = written.message;
            }
        }
    }
    return result;
}

WorkspaceScanReport ScanWorkspaceSessions(const std::filesystem::path& sessions_root,
                                          const std::string& now_yyyymmdd, int since_days) {    WorkspaceScanReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(sessions_root, ec)) {
        return report;
    }
    const auto now_days = DaysOf(now_yyyymmdd);
    std::optional<std::int64_t> since_bound;
    if (now_days.has_value()) {
        since_bound = *now_days - since_days;
    }
    std::vector<std::filesystem::path> dirs;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root, ec)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        const std::string name = dir.filename().string();
        const auto days = DaysOf(name.substr(0, std::min<std::size_t>(name.size(), 8)));
        // 日期形状不合的场不猜:一律收进扫描(shape 靠 gate 说话),时间
        // 窗过滤只对读得出日期的生效。
        if (since_bound.has_value() && days.has_value() && *days < *since_bound) {
            continue;
        }
        WorkspaceScanEntry scan;
        scan.session_id = name;
        SessionGateReport gate = GateSession(dir);
        scan.status = gate.status;
        if (!gate.error_code.empty()) {
            scan.reason = gate.error_code + (gate.message.empty() ? "" : ": " + gate.message);
        }
        report.status_counts[SessionGateStatusName(gate.status)] += 1;
        report.entries.push_back(std::move(scan));
    }
    report.sessions_found = static_cast<std::int64_t>(report.entries.size());
    return report;
}

std::vector<Finding> AuditPromptOutcome(const std::vector<SessionAnalyzeResult>& sessions) {
    std::vector<Finding> out;
    // 类 -> (命中数, 场次, 首个事件引用)。
    struct Tally {
        std::int64_t count = 0;
        std::vector<std::string> session_ids;
        std::vector<std::string> event_ids;
    };
    std::map<std::string, Tally> tally;
    for (const auto& session : sessions) {
        if (!session.analyzed) {
            continue;
        }
        for (const auto& occurrence : session.frictions) {
            Tally& entry = tally[occurrence.category];
            entry.count += 1;
            if (entry.session_ids.empty() || entry.session_ids.back() != session.gate.session_id) {
                entry.session_ids.push_back(session.gate.session_id);
            }
            if (occurrence.evidence.event_id.has_value() && entry.event_ids.size() < 5) {
                entry.event_ids.push_back(*occurrence.evidence.event_id);
            }
        }
    }
    const auto make = [](std::string category, FindingSeverity severity,
                         FindingConfidence confidence, std::string summary,
                         std::string recommendation, const char* id) {
        Finding finding;
        finding.finding_id = std::string("P-AUD-") + id;
        finding.category = std::move(category);
        finding.severity = severity;
        finding.confidence = confidence;
        finding.scope = "workspace";
        finding.summary = std::move(summary);
        finding.recommendation = std::move(recommendation);
        finding.origin = FindingOrigin::DeterministicRule;
        finding.rule_version = std::string(kPromptAuditRuleVersion) + ":" + id;
        return finding;
    };
    const auto evidence_of = [](const Tally& entry) {
        std::vector<EvidenceItem> items;
        EvidenceItem count_item;
        count_item.metric = "occurrences";
        count_item.value = entry.count;
        items.push_back(count_item);
        EvidenceItem sessions_item;
        sessions_item.metric = "sessions_affected";
        nlohmann::json ids = nlohmann::json::array();
        for (std::size_t i = 0; i < entry.session_ids.size() && i < 5; ++i) {
            ids.push_back(entry.session_ids[i]);
        }
        sessions_item.value = ids;
        items.push_back(sessions_item);
        for (const auto& event_id : entry.event_ids) {
            EvidenceItem item;
            item.metric = "event_ref";
            item.value = event_id;
            item.event_id = event_id;
            items.push_back(item);
        }
        return items;
    };

    if (const auto it = tally.find("verification.missing"); it != tally.end()) {
        Finding finding = make(
            "outcome.verification_missing", FindingSeverity::Warning, FindingConfidence::Medium,
            std::to_string(it->second.count) +
                " 个 turn 报了 outcome=passed 却没有 verification(只能说明证据缺失,不能断言假完成)",
            "收工前把验证落进 verification(测试/构建跑一遍并留账)", "O01");
        finding.evidence = evidence_of(it->second);
        out.push_back(std::move(finding));
    }
    if (const auto it = tally.find("tool.repeated_retry"); it != tally.end()) {
        Finding finding = make(
            "outcome.tool_retry", FindingSeverity::Info, FindingConfidence::High,
            "同工具反复重试 " + std::to_string(it->second.count) +
                " 次(可能是外部故障,不一定是 prompt 的事)",
            "看对应事件的失败原因;参数反复填错才回头查工具描述", "O02");
        finding.evidence = evidence_of(it->second);
        out.push_back(std::move(finding));
    }
    if (const auto it = tally.find("verification.failure"); it != tally.end()) {
        Finding finding = make(
            "outcome.verification_failure", FindingSeverity::Info, FindingConfidence::High,
            "验证失败 " + std::to_string(it->second.count) + " 次(事实账;失败后修好的也算)",
            "失败密集处看任务拆分;这不是 prompt 诊断,只是信号", "O03");
        finding.evidence = evidence_of(it->second);
        out.push_back(std::move(finding));
    }
    if (const auto it = tally.find("provider.failure"); it != tally.end()) {
        Finding finding = make(
            "outcome.provider_failure", FindingSeverity::Info, FindingConfidence::High,
            "模型请求失败 " + std::to_string(it->second.count) + " 次",
            "失败与重试的 token 各记各账(/usage);这里只报发生", "O04");
        finding.evidence = evidence_of(it->second);
        out.push_back(std::move(finding));
    }
    if (const auto it = tally.find("cancelled"); it != tally.end()) {
        Finding finding = make(
            "outcome.cancelled", FindingSeverity::Info, FindingConfidence::High,
            "取消 " + std::to_string(it->second.count) + " 次(用户打断/运行中止)",
            "取消密集的时段看任务粒度;不下人格判断", "O05");
        finding.evidence = evidence_of(it->second);
        out.push_back(std::move(finding));
    }
    return out;
}

}  // namespace lubancode::insights
