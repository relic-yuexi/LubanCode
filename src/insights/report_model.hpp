// Insights report 的 report.json 合同(Token 账本单 §6.5/§9.2 A0 冻结)。
//
// A5 的 HTML renderer 读同一份 typed report;terminal 与 GUI 不各算一套。
// generated_at 由调用方注入(时钟不进领域层),测试注固定值即得字节稳定
// 的 golden。正文七节在 A5 齐;A0 先钉范围/coverage/usage/finding 四节。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "insights/finding.hpp"
#include "insights/session_summary.hpp"

namespace lubancode::insights {

inline constexpr const char* kInsightsReportSchema = "lubancode.insights.report";
inline constexpr int kInsightsReportSchemaVersion = 1;
// v1.2:在 v1.1 规则钉死基础上，把 cache_epoch 分段写进 session 摘要；
// analyzer version 抬升使旧摘要重算，report schema 顶层形状不变。
inline constexpr const char* kInsightsAnalyzerVersion = "insights-v1.2";

struct InsightsReport {
    std::string generated_at;  // "YYYY-MM-DDTHH:MM:SSZ",调用方注入
    struct Scope {
        std::string workspace_key;  // readable name 由 presenter 配,此处放 key
        std::string since;          // "YYYY-MM-DD"
        std::string until;          // "YYYY-MM-DD"
        bool all_workspaces = false;
    } scope;
    struct Coverage {
        std::uint64_t sessions_found = 0;
        std::uint64_t sessions_verified = 0;
        std::uint64_t sessions_analyzed = 0;
        std::uint64_t sessions_pending = 0;   // 本轮没轮上的未分析 session
        std::uint64_t sessions_excluded = 0;  // active/corrupt/incomplete 排除数
    } coverage;
    // usage 总账:只把 requests_with_usage 的 token 计入;unknown 单列,
    // 不估数补进"实测总计"(§14.3)。
    struct UsageTotals {
        std::uint64_t requests_total = 0;
        std::uint64_t requests_with_usage = 0;
        std::uint64_t requests_unknown = 0;
        std::int64_t input_tokens = 0;
        std::int64_t cache_read_tokens = 0;
        std::int64_t cache_creation_tokens = 0;
        std::int64_t output_tokens = 0;
        std::int64_t reasoning_tokens = 0;
        std::string cost_status = "not_priced";
        std::string cost_currency;
        std::int64_t cost_micros = 0;
        std::string price_table_id;
    } usage;
    std::string analysis_mode = "local_deterministic";  // 页眉"分析方式"
    std::vector<SessionInsightSummary> sessions;
    std::vector<Finding> findings;  // 跨 session 的 workspace 级发现

    nlohmann::json ToJson() const;
    static std::optional<InsightsReport> FromJsonStrict(const nlohmann::json& json,
                                                        std::string* error);
};

}  // namespace lubancode::insights
