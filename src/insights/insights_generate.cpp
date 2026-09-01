// /insights 生成管线的实现。发现/验账/增量判定走 A4 的原班引擎
// (GateSession/ReadExistingSessionSummary/IsSummaryStale/AnalyzeSession),
// 这里只掌次序、窗口、上限与汇总——不另写一套验证。
#include "insights/insights_generate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

#include "insights/derived_store.hpp"
#include "insights/session_analyzer.hpp"
#include "trajectory/directory.hpp"  // ReadSessionJson(排序用的 created_at)

namespace lubancode::insights {
namespace {

// days(距纪元)-> "YYYY-MM-DD";坏值给空串。
std::string IsoDateFromDays(std::int64_t days) {
    const std::chrono::sys_days day{std::chrono::days{days}};
    const std::chrono::year_month_day ymd{day};
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                  static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                  static_cast<unsigned>(ymd.day()));
    return buffer;
}

// 一间候选场的发现账。
struct Candidate {
    std::filesystem::path dir;
    std::string workspace_key;
    std::string session_id;      // 目录名(gate 未跑前先顶名)
    SessionGateReport gate;      // 验过的账
    std::int64_t created_at_ms = 0;
    bool gate_ok = false;        // status == Analyzed
    bool active = false;
    bool summary_fresh = false;  // gate_ok 且派生摘要 fresh
    SessionInsightSummary summary;
};

std::optional<std::int64_t> DaysOfDirName(const std::string& name) {
    return YyyymmddToDays(name.substr(0, std::min<std::size_t>(name.size(), 8)));
}

// 摩擦类 -> INS 编号(封闭表次序,01 起)。表外类给空(不出 finding,
// 只进摩擦节的事实表——封闭表之外的类不硬编号)。
std::string FrictionFindingId(const std::string& category) {
    const auto& table = FrictionClosedTableOrder();
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i] == category) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "INS-F%02d",
                          static_cast<int>(i + 1));
            return buffer;
        }
    }
    return std::string();
}

// INS-F 建议文案:镜像 A3 O01–O05 的措辞戒律,只说事实与动作;封闭表
// 其余类给中性事实行,不编因果。
std::string FrictionRecommendation(const std::string& category) {
    if (category == "verification.missing") {
        return "收工前把验证落进 verification(测试/构建跑一遍并留账)";
    }
    if (category == "tool.repeated_retry") {
        return "看对应事件的失败原因;参数反复填错才回头查工具描述";
    }
    if (category == "verification.failure") {
        return "失败密集处看任务拆分;这是信号账,不是诊断";
    }
    if (category == "provider.failure") {
        return "失败与重试的 token 各记各账(/usage);这里只报发生";
    }
    if (category == "cancelled") {
        return "取消密集的时段看任务粒度;不下人格判断";
    }
    return "逐场明细与事件引用看 /prompt audit outcome";
}

// 工作区级的摩擦 finding(按场次计,§9.5 第四节的 finding 面)。
Finding MakeFrictionFinding(const FrictionRollup& rollup) {
    Finding finding;
    finding.finding_id = FrictionFindingId(rollup.category);
    finding.category = "friction." + rollup.category;
    finding.severity = rollup.category == "verification.missing"
                           ? FindingSeverity::Warning
                           : FindingSeverity::Info;
    finding.confidence = FindingConfidence::High;
    finding.scope = "workspace";
    finding.summary = std::to_string(rollup.sessions) + " 场 session 出现 " +
                      rollup.category + "(按场次计;micro 场不作样本)";
    finding.recommendation = FrictionRecommendation(rollup.category);
    finding.origin = FindingOrigin::DeterministicRule;
    finding.rule_version = std::string(kInsightsAnalyzerVersion) + ":" +
                           FrictionFindingId(rollup.category);
    EvidenceItem affected;
    affected.metric = "sessions_affected";
    affected.value = rollup.sessions;
    finding.evidence.push_back(affected);
    EvidenceItem sample;
    sample.metric = "sample_sessions";
    nlohmann::json ids = nlohmann::json::array();
    for (const auto& id : rollup.sample_session_ids) {
        ids.push_back(id);
    }
    sample.value = ids;
    finding.evidence.push_back(std::move(sample));
    return finding;
}

}  // namespace

std::optional<std::int64_t> YyyymmddToDays(const std::string& yyyymmdd) {
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
    const std::chrono::year_month_day ymd{std::chrono::year{year},
                                          std::chrono::month{month},
                                          std::chrono::day{day}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(
        std::chrono::sys_days{ymd}.time_since_epoch().count());
}

InsightsGenerateResult GenerateInsightsReport(
    const std::vector<InsightsWorkspaceRef>& workspaces,
    const InsightsGenerateOptions& options) {
    InsightsGenerateResult result;
    if (workspaces.empty()) {
        result.error_code = "insights.no_workspaces";
        result.message = "没有可扫的 workspace(轨迹根下没有 sessions 目录?)";
        return result;
    }

    InsightsReport& report = result.report;
    report.generated_at = options.generated_at;
    report.analysis_mode = "local_deterministic";
    const bool all = workspaces.size() > 1;
    report.scope.all_workspaces = all;
    report.scope.workspace_key = all ? "*" : workspaces.front().workspace_key;
    {
        const auto now_days = YyyymmddToDays(options.now_yyyymmdd);
        if (now_days.has_value()) {
            report.scope.until = IsoDateFromDays(*now_days);
            report.scope.since = IsoDateFromDays(*now_days - options.since_days);
        }
    }

    // workspace 按字典序(汇总与 sessions 归属账的稳定次序)。
    std::vector<const InsightsWorkspaceRef*> ordered;
    ordered.reserve(workspaces.size());
    for (const auto& workspace : workspaces) {
        ordered.push_back(&workspace);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const InsightsWorkspaceRef* a, const InsightsWorkspaceRef* b) {
                  return a->workspace_key < b->workspace_key;
              });
    for (const auto* workspace : ordered) {
        result.extras.workspace_names[workspace->workspace_key] =
            workspace->readable_name;
    }

    const auto now_days = YyyymmddToDays(options.now_yyyymmdd);
    std::optional<std::int64_t> since_bound;
    if (now_days.has_value()) {
        since_bound = *now_days - options.since_days;
    }

    // ---- discover + verify(逐 workspace;候选按开始时间新->旧) ----
    std::vector<Candidate> candidates;  // gate_ok(active 视开关)的场
    for (const auto* workspace : ordered) {
        std::error_code ec;
        if (!std::filesystem::is_directory(workspace->sessions_root, ec)) {
            continue;
        }
        std::vector<std::filesystem::path> dirs;
        for (const auto& entry :
             std::filesystem::directory_iterator(workspace->sessions_root, ec)) {
            std::error_code dir_ec;
            if (entry.is_directory(dir_ec) && !dir_ec) {
                dirs.push_back(entry.path());
            }
        }
        std::sort(dirs.begin(), dirs.end());
        for (const auto& dir : dirs) {
            const std::string name = dir.filename().string();
            // 日期窗:读得出日期且在窗外的跳过;形状不合不猜日期,收进来
            // 让 gate 说话(与 A4 workspace 扫描同口径)。
            const auto days = DaysOfDirName(name);
            if (since_bound.has_value() && days.has_value() && *days < *since_bound) {
                continue;
            }
            result.counts.found += 1;

            Candidate candidate;
            candidate.dir = dir;
            candidate.workspace_key = workspace->workspace_key;
            candidate.session_id = name;
            candidate.gate = GateSession(dir);
            if (const auto manifest = trajectory::ReadSessionJson(dir)) {
                candidate.created_at_ms = manifest->created_at_ms;
            }
            switch (candidate.gate.status) {
                case SessionGateStatus::Analyzed:
                    candidate.gate_ok = true;
                    result.counts.verified += 1;
                    break;
                case SessionGateStatus::Active:
                    candidate.active = true;
                    break;
                default:
                    break;
            }
            if (candidate.gate_ok) {
                // fresh 判定(§9.2):terminal hash + analyzer 版本对得上
                // 就跳过重算,直接吃摘要。
                const DerivedReadResult existing = ReadExistingSessionSummary(dir);
                if (existing.exists && existing.parse_ok &&
                    !IsSummaryStale(existing, candidate.gate.stream_terminal_hashes)) {
                    candidate.summary_fresh = true;
                    candidate.summary = existing.summary;
                }
                candidates.push_back(std::move(candidate));
            } else if (candidate.active && options.include_active) {
                candidates.push_back(std::move(candidate));
            } else {
                InsightsExcludedEntry excluded;
                excluded.workspace_key = workspace->workspace_key;
                excluded.session_id = candidate.gate.session_id.empty()
                                          ? name
                                          : candidate.gate.session_id;
                excluded.status = SessionGateStatusName(candidate.gate.status);
                if (!candidate.gate.error_code.empty()) {
                    excluded.reason =
                        candidate.gate.error_code +
                        (candidate.gate.message.empty()
                             ? std::string()
                             : ": " + candidate.gate.message);
                }
                result.extras.excluded.push_back(std::move(excluded));
                result.counts.excluded += 1;
            }
        }
    }

    // 选号次序:开始时间新->旧,同时间 session id 降序(§9.1"按结束时间
    // 从新到旧"——结束时间不在 manifest,按开始时间排,口径写进限制节)。
    std::vector<std::size_t> order(candidates.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Candidate& ca = candidates[a];
        const Candidate& cb = candidates[b];
        if (ca.created_at_ms != cb.created_at_ms) {
            return ca.created_at_ms > cb.created_at_ms;
        }
        return ca.session_id > cb.session_id;
    });

    // ---- 增量:先吃 fresh,再按上限重算 stale/missing(active 不占
    // 重算名额——它本来就不写摘要,逐场现分析) ----
    std::vector<SessionInsightSummary> summaries;
    std::map<std::string, std::string> session_workspace;
    std::int64_t to_analyze_budget = options.max_sessions;
    for (const std::size_t index : order) {
        Candidate& candidate = candidates[index];
        if (candidate.summary_fresh) {
            summaries.push_back(candidate.summary);
            session_workspace[candidate.summary.source.session_id] =
                candidate.workspace_key;
            result.counts.reused += 1;
            continue;
        }
        if (candidate.active) {
            // --include-active:高水位现分析,摘要 provisional,不落盘。
            SessionAnalyzeOptions analyze;
            analyze.include_active = true;
            const SessionAnalyzeResult analyzed = AnalyzeSession(candidate.dir, analyze);
            if (analyzed.analyzed) {
                summaries.push_back(analyzed.summary);
                session_workspace[analyzed.summary.source.session_id] =
                    candidate.workspace_key;
            } else {
                InsightsExcludedEntry excluded;
                excluded.workspace_key = candidate.workspace_key;
                excluded.session_id = candidate.session_id;
                excluded.status = SessionGateStatusName(SessionGateStatus::Active);
                excluded.reason = "active 高水位读不出来";
                result.extras.excluded.push_back(std::move(excluded));
                result.counts.excluded += 1;
            }
            continue;
        }
        if (to_analyze_budget <= 0) {
            result.counts.pending += 1;  // 页眉 pending 数(§9.1 尾段)
            continue;
        }
        to_analyze_budget -= 1;
        const SessionAnalyzeResult analyzed =
            AnalyzeSession(candidate.dir, SessionAnalyzeOptions{});
        if (!analyzed.analyzed) {
            InsightsExcludedEntry excluded;
            excluded.workspace_key = candidate.workspace_key;
            excluded.session_id = candidate.session_id;
            excluded.status = SessionGateStatusName(candidate.gate.status);
            excluded.reason = "验账后分析未过";
            result.extras.excluded.push_back(std::move(excluded));
            result.counts.excluded += 1;
            continue;
        }
        summaries.push_back(analyzed.summary);
        session_workspace[analyzed.summary.source.session_id] = candidate.workspace_key;
        if (analyzed.summary_written) {
            result.counts.written += 1;
        } else if (analyzed.summary_reused) {
            // 验账与分析之间被人抢先写了 fresh 摘要:按复用记账,不冒充
            // 本轮重算。
            result.counts.reused += 1;
        }
        if (!analyzed.derived_error.empty()) {
            result.extras.derived_errors.push_back(analyzed.summary.source.session_id +
                                                   ": " + analyzed.derived_error);
        }
    }
    result.extras.sessions_pending = result.counts.pending;
    result.extras.session_workspace = std::move(session_workspace);

    // ---- aggregate:摘要按 session_id 字典序排稳,report 字节才稳;
    // workspace 归属账在 extras(渲染筛 workspace 用)。 ----
    std::sort(summaries.begin(), summaries.end(),
              [](const SessionInsightSummary& a, const SessionInsightSummary& b) {
                  return a.source.session_id < b.source.session_id;
              });
    report.sessions = std::move(summaries);
    // analyzed = 进汇总的摘要份数(fresh 复用 + 本轮重算 + 高水位场),
    // 与 report.coverage.sessions_analyzed 同一只口径。
    result.counts.analyzed = static_cast<std::int64_t>(report.sessions.size());
    report.coverage.sessions_found = static_cast<std::uint64_t>(result.counts.found);
    report.coverage.sessions_verified =
        static_cast<std::uint64_t>(result.counts.verified);
    report.coverage.sessions_analyzed =
        static_cast<std::uint64_t>(report.sessions.size());
    report.coverage.sessions_pending =
        static_cast<std::uint64_t>(result.counts.pending);
    report.coverage.sessions_excluded =
        static_cast<std::uint64_t>(result.counts.excluded);

    result.aggregate = AggregateInsights(report);
    const WorkspaceAggregate& agg = result.aggregate;
    report.usage.requests_total = static_cast<std::uint64_t>(agg.requests_total);
    report.usage.requests_with_usage =
        static_cast<std::uint64_t>(agg.requests_with_usage);
    report.usage.requests_unknown = static_cast<std::uint64_t>(agg.requests_unknown);
    report.usage.input_tokens = agg.input_tokens;
    report.usage.cache_read_tokens = agg.cache_read_tokens;
    report.usage.cache_creation_tokens = agg.cache_creation_tokens;
    report.usage.output_tokens = agg.output_tokens;
    report.usage.reasoning_tokens = agg.reasoning_tokens;
    report.usage.cost_status = "not_priced";  // 摘要级 schema 不带逐模型拆账,
                                              // 费用逐笔看 /usage;边界进限制节

    // 工作区级 findings:prompt 规则汇总 + 摩擦类(INS-F##)。
    report.findings = result.aggregate.prompt_rollups;
    for (const auto& rollup : result.aggregate.frictions) {
        if (!FrictionFindingId(rollup.category).empty()) {
            report.findings.push_back(MakeFrictionFinding(rollup));
        }
    }

    result.ok = true;
    return result;
}

}  // namespace lubancode::insights
