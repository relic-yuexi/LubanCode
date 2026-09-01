// /insights 的生成管线(Token 账本单 §9.1/§9.2/A5)。
//
//   discover sessions(时间窗 + 目录名形状)
//     -> verify journal and parent/child edges(IntegrityGate)
//     -> compare terminal hashes + analyzer version(fresh 跳过,§9.2)
//     -> analyze stale/missing(AnalyzeSession,原子写摘要)
//     -> stream aggregate selected summaries(WorkspaceAggregator)
//     -> InsightsReport(typed,report.json 的唯一来源)
//
// 口径(§9.1):
//   - 默认当前 workspace;跨仓要显式递多个 workspace ref(命令层管开关);
//   - 只纳入已封口且 verify 通过的场;--include-active 才读高水位(摘要
//     provisional,不写长期摘要);
//   - 一轮最多分析 max_sessions 间"尚未由当前 analyzer 版本分析"的场,
//     按 session 开始时间从新到旧(结束时间不在 manifest,不猜;同时间按
//     session id 降序),目录遍历次序不配当选号依据;
//   - fresh 摘要一律进汇总(200 间上限只限"要重算的",§9.1 尾段);
//   - 不调模型;报告路径的落盘在 report_store,这里只出 typed report。
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "insights/report_model.hpp"
#include "insights/workspace_aggregator.hpp"

namespace lubancode::insights {

// 一个待汇总的 workspace(命令层解析:默认当前仓;--all-workspaces 时
// trajectories_root 下逐仓一枚)。
struct InsightsWorkspaceRef {
    std::string workspace_key;
    std::string readable_name;
    std::filesystem::path sessions_root;
};

struct InsightsGenerateOptions {
    int since_days = 30;     // 时间窗(§9.1 默认 30 天)
    int max_sessions = 200;  // 一轮重算上限(§9.1 默认 200)
    bool include_active = false;
    std::string now_yyyymmdd;  // "YYYYMMDD",时钟注入(空 = 不按日期过滤)
    std::string generated_at;  // "YYYY-MM-DDTHH:MM:SSZ",调用方注入
};

// 生成侧的运行账(报告页眉的"样本"行 / 限制节)。
struct InsightsRunCounts {
    std::int64_t found = 0;
    std::int64_t verified = 0;
    std::int64_t analyzed = 0;
    std::int64_t reused = 0;   // fresh 摘要直接复用(增量,§9.2)
    std::int64_t written = 0;  // 本轮写了/重算了摘要
    std::int64_t pending = 0;  // 待分析但本轮没轮上(页眉 pending 数)
    std::int64_t excluded = 0;
};

struct InsightsGenerateResult {
    bool ok = false;
    std::string error_code;
    std::string message;
    InsightsReport report;         // typed report(report.json 的唯一来源)
    WorkspaceAggregate aggregate;  // 七节材料(渲染共用)
    InsightsRenderExtras extras;
    InsightsRunCounts counts;
};

InsightsGenerateResult GenerateInsightsReport(const std::vector<InsightsWorkspaceRef>& workspaces,
                                              const InsightsGenerateOptions& options);

// "YYYYMMDD" -> 距纪元天数;形状不合/非真实日期给 nullopt(不猜日期,
// 与 A4 workspace 扫描同一口径)。
std::optional<std::int64_t> YyyymmddToDays(const std::string& yyyymmdd);

}  // namespace lubancode::insights
