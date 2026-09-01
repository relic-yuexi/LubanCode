// SessionInsightSummary 合同(Token 账本单 §6.5 A0 冻结)。
//
// 每间封口 session 产一份摘要;源 terminal hash 或 analyzer version 变了,
// 摘要判 stale,删掉重算。A4 的本地分析器(IntegrityGate/FrictionClassifier
// 等)按这份合同产出;本件先冻结形状与序列化。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "insights/finding.hpp"

namespace lubancode::insights {

inline constexpr const char* kSessionSummarySchema = "lubancode.insights.session_summary";
inline constexpr int kSessionSummarySchemaVersion = 1;

// A0 钉的 usage 汇总块:A2 的五层聚合在其上加层,不另起形状。
struct SummaryUsage {
    std::uint64_t requests_total = 0;
    std::uint64_t requests_with_usage = 0;
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;  // 已含在 output_tokens,拆账用
    // 估算费用(整数 micros);没配价格表恒 0,status=not_priced。
    std::string cost_status = "not_priced";
    std::string cost_currency;
    std::int64_t cost_micros = 0;
    std::string price_table_id;
};

struct SessionInsightSummary {
    // 与 kInsightsAnalyzerVersion 同步(A5 抬到 insights-v1.1:信号 id 改
    // 规则钉死,旧摘要判 stale 重算)。
    std::string analyzer_version = "insights-v1.1";
    struct Source {
        std::string session_id;
        // stream run_id -> terminal event hash;任一变化即 stale。
        std::map<std::string, std::string> stream_terminal_hashes;
        std::string integrity = "verified";  // verified/provisional
    } source;
    struct Coverage {
        std::uint64_t runs_total = 0;
        std::uint64_t runs_analyzed = 0;
        std::uint64_t requests_total = 0;
        std::uint64_t requests_with_usage = 0;
        std::uint64_t outcomes_assessed = 0;
    } coverage;
    struct Work {
        std::uint64_t turns = 0;
        std::uint64_t tool_calls = 0;
        std::uint64_t files_touched = 0;
        std::uint64_t verifications = 0;
        std::string outcome;  // 空 = 无 outcome 评估
    } work;
    SummaryUsage usage;
    std::vector<Finding> prompt_findings;
    std::vector<std::string> friction_events;   // 摩擦类名(§9.3 枚举)
    std::vector<std::string> feature_signals;   // 建议引用的信号 id

    nlohmann::json ToJson() const;
    static std::optional<SessionInsightSummary> FromJsonStrict(const nlohmann::json& json,
                                                               std::string* error);
};

}  // namespace lubancode::insights
