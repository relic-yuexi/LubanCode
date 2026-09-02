// WorkspaceAggregator 的实现(纯函数,零 IO)。
#include "insights/workspace_aggregator.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "insights/feature_signal.hpp"

namespace lubancode::insights {

// 摩擦封闭表(§9.3 的十六类;A4 的 FrictionClassifier 在册同一张表)。
// 汇总排序与 INS-F<nn> 编号共用这一份,不另抄。
const std::vector<std::string>& FrictionClosedTableOrder() {
    static const std::vector<std::string> table = {
        "request.ambiguity", "instruction.conflict",   "context.loss",
        "context.churn",     "tool.invalid_input",     "tool.execution_failure",
        "tool.repeated_retry", "verification.failure", "verification.missing",
        "user.correction",   "permission.denied",      "approval.wait",
        "provider.failure",  "budget.limit",           "cancelled",
        "unknown",
    };
    return table;
}

namespace {

// 四舍五入整数百分比;分母 <= 0 给 nullopt(unknown,不写 0%)。
std::optional<int> SharePercent(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) {
        return std::nullopt;
    }
    return static_cast<int>((part * 200 + whole) / (whole * 2));
}

void AppendSample(std::vector<std::string>* samples, const std::string& session_id) {
    if (samples->size() < 5) {
        samples->push_back(session_id);
    }
}

// severity/confidence 取更强的一档(汇总行讲最重的影响与最硬的证据)。
FindingSeverity MaxSeverity(FindingSeverity a, FindingSeverity b) {
    const int ia = static_cast<int>(a);
    const int ib = static_cast<int>(b);
    return ia >= ib ? a : b;
}

FindingConfidence MaxConfidence(FindingConfidence a, FindingConfidence b) {
    const int ia = static_cast<int>(a);
    const int ib = static_cast<int>(b);
    return ia >= ib ? a : b;
}

}  // namespace

bool IsMicroSession(const SessionInsightSummary& summary) {
    return summary.work.turns < 2 && summary.work.tool_calls == 0 &&
           summary.work.verifications == 0 && summary.coverage.outcomes_assessed == 0;
}

WorkspaceAggregate AggregateInsights(const InsightsReport& report) {
    WorkspaceAggregate agg;
    agg.sessions = static_cast<std::int64_t>(report.sessions.size());

    // 摩擦类 -> 场数(非 micro 样本);信号 id -> 场数;prompt 规则 id 账。
    std::map<std::string, std::int64_t> friction_sessions;
    std::map<std::string, std::vector<std::string>> friction_samples;
    std::map<std::string, std::int64_t> signal_sessions;
    std::map<std::string, std::vector<std::string>> signal_samples;
    std::map<std::string, std::set<std::string>> prompt_rule_sessions;
    std::map<std::string, Finding> prompt_rollup_by_id;

    for (const auto& session : report.sessions) {
        // 一 工作概览:全量(micro 也算 usage 样本)。
        agg.turns += static_cast<std::int64_t>(session.work.turns);
        agg.tool_calls += static_cast<std::int64_t>(session.work.tool_calls);
        agg.files_touched_sum += static_cast<std::int64_t>(session.work.files_touched);
        agg.verifications += static_cast<std::int64_t>(session.work.verifications);
        if (session.source.integrity != "verified") {
            agg.provisional_sessions += 1;
        }
        // 二 usage 总账:只把 requests_with_usage 的 token 计入(§14.3
        // unknown 单列,不估数补进实测总计)。
        agg.requests_total += static_cast<std::int64_t>(session.usage.requests_total);
        agg.requests_with_usage +=
            static_cast<std::int64_t>(session.usage.requests_with_usage);
        agg.input_tokens += session.usage.input_tokens;
        agg.cache_read_tokens += session.usage.cache_read_tokens;
        agg.cache_creation_tokens += session.usage.cache_creation_tokens;
        agg.output_tokens += session.usage.output_tokens;
        agg.reasoning_tokens += session.usage.reasoning_tokens;
        for (const auto& epoch : session.cache_epochs) {
            WorkspaceAggregate::CacheEpochRow row;
            row.session_id = session.source.session_id;
            row.run_id = epoch.run_id;
            row.cache_epoch = epoch.cache_epoch;
            row.requests_total = static_cast<std::int64_t>(epoch.requests_total);
            row.requests_cache_reported =
                static_cast<std::int64_t>(epoch.requests_cache_reported);
            row.requests_cache_unknown =
                static_cast<std::int64_t>(epoch.requests_cache_unknown);
            row.input_tokens = epoch.input_tokens;
            row.cache_read_tokens = epoch.cache_read_tokens;
            row.cache_creation_tokens = epoch.cache_creation_tokens;
            const std::int64_t base = row.input_tokens + row.cache_read_tokens +
                                      row.cache_creation_tokens;
            if (row.requests_cache_reported > 0 && row.requests_cache_unknown == 0) {
                row.cache_read_ratio_percent = SharePercent(row.cache_read_tokens, base);
            }
            agg.cache_epochs.push_back(std::move(row));
        }

        const bool micro = IsMicroSession(session);
        if (micro) {
            agg.micro_sessions += 1;
            continue;  // 摩擦频次与交互形状不拿 micro 作样本(§9.1)
        }
        agg.sample_sessions += 1;

        // 五 交互形状(只报习惯账,不做人身评价)。
        if (session.work.verifications > 0) {
            agg.sessions_with_verification += 1;
        }
        if (!session.work.outcome.empty()) {
            agg.sessions_outcome_assessed += 1;
        }
        for (const auto& category : session.friction_events) {
            friction_sessions[category] += 1;
            AppendSample(&friction_samples[category], session.source.session_id);
            if (category == "verification.failure") {
                agg.sessions_with_verification_failure += 1;
            } else if (category == "cancelled") {
                agg.sessions_cancelled += 1;
            } else if (category == "tool.repeated_retry") {
                agg.sessions_repeated_retry += 1;
            }
        }
        // 六 建议面:信号 id 按规则汇总(id 规则钉死,跨场可比)。
        for (const auto& signal_id : session.feature_signals) {
            signal_sessions[signal_id] += 1;
            AppendSample(&signal_samples[signal_id], session.source.session_id);
        }
        // 三 prompt 汇总:runtime finding 按规则 id 折,证据换算场数。
        for (const auto& finding : session.prompt_findings) {
            prompt_rule_sessions[finding.finding_id].insert(session.source.session_id);
            const auto it = prompt_rollup_by_id.find(finding.finding_id);
            if (it == prompt_rollup_by_id.end()) {
                Finding rollup = finding;
                rollup.scope = "workspace";
                rollup.evidence.clear();
                rollup.counter_evidence.clear();
                prompt_rollup_by_id.emplace(finding.finding_id, std::move(rollup));
            } else {
                it->second.severity = MaxSeverity(it->second.severity, finding.severity);
                it->second.confidence = MaxConfidence(it->second.confidence, finding.confidence);
            }
        }
    }

    agg.requests_unknown = agg.requests_total - agg.requests_with_usage;
    const std::int64_t cache_base =
        agg.input_tokens + agg.cache_read_tokens + agg.cache_creation_tokens;
    agg.cache_read_ratio_percent = SharePercent(agg.cache_read_tokens, cache_base);

    // outcome 分布(字典序;空 outcome 不进表,限制节报"无 outcome 评估"数)。
    {
        std::map<std::string, std::int64_t> outcomes;
        for (const auto& session : report.sessions) {
            if (IsMicroSession(session)) {
                continue;  // 交互形状不拿 micro 作样本,outcome 分布同口径
            }
            if (!session.work.outcome.empty()) {
                outcomes[session.work.outcome] += 1;
            }
        }
        for (const auto& [outcome, count] : outcomes) {
            agg.outcome_counts.push_back(OutcomeCount{outcome, count});
        }
    }

    // prompt 汇总行的证据:sessions_affected + 样本场。
    agg.prompt_rollups.reserve(prompt_rollup_by_id.size());
    for (auto& [id, rollup] : prompt_rollup_by_id) {
        EvidenceItem affected;
        affected.metric = "sessions_affected";
        affected.value = static_cast<std::int64_t>(prompt_rule_sessions[id].size());
        rollup.evidence.push_back(affected);
        EvidenceItem sample;
        sample.metric = "sample_sessions";
        nlohmann::json ids = nlohmann::json::array();
        for (const auto& session_id : prompt_rule_sessions[id]) {
            if (ids.size() >= 5) {
                break;
            }
            ids.push_back(session_id);
        }
        sample.value = ids;
        rollup.evidence.push_back(std::move(sample));
        // summary 保留规则自身的人话(首场的措辞),场数在证据栏——规则讲
        // 什么、命中多少,各归各栏。
        agg.prompt_rollups.push_back(std::move(rollup));
    }

    // 摩擦 rollup:封闭表次序,表外字典序垫后。
    {
        const auto& closed = FrictionClosedTableOrder();
        const auto emit = [&](const std::string& category) {
            const auto it = friction_sessions.find(category);
            if (it == friction_sessions.end()) {
                return;
            }
            FrictionRollup rollup;
            rollup.category = category;
            rollup.sessions = it->second;
            rollup.sample_session_ids = friction_samples[category];
            agg.frictions.push_back(std::move(rollup));
        };
        for (const auto& category : closed) {
            emit(category);
        }
        for (const auto& [category, count] : friction_sessions) {
            if (std::find(closed.begin(), closed.end(), category) == closed.end()) {
                (void)count;
                emit(category);
            }
        }
    }

    // 信号 rollup:目录次序(FS-01..04)在前,表外 id 字典序垫后。
    {
        std::set<std::string> emitted;
        for (const auto& entry : FeatureSignalCatalog()) {
            emitted.insert(entry.signal_id);
            const auto it = signal_sessions.find(entry.signal_id);
            if (it == signal_sessions.end()) {
                continue;
            }
            SignalRollup rollup;
            rollup.signal_id = entry.signal_id;
            rollup.feature = entry.feature;
            rollup.precondition = entry.precondition;
            rollup.action = entry.action;
            rollup.sessions = it->second;
            rollup.sample_session_ids = signal_samples[entry.signal_id];
            agg.signals.push_back(std::move(rollup));
        }
        for (const auto& [id, count] : signal_sessions) {
            if (emitted.count(id) != 0) {
                continue;
            }
            SignalRollup rollup;
            rollup.signal_id = id;  // 目录外的旧/新 id:照实列,feature 留空
            rollup.sessions = count;
            rollup.sample_session_ids = signal_samples[id];
            agg.signals.push_back(std::move(rollup));
        }
    }
    return agg;
}

}  // namespace lubancode::insights
