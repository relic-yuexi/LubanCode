// WorkspaceAggregator(Token 账本单 §9.3 步骤 6/A5):把 N 份 session
// 摘要折成一份工作区(或跨工作区)总账。
//
// 输入只有 SessionInsightSummary(A4 落的派生摘要,含增量复用的旧账)——
// 聚合是摘要的纯函数,同输入同输出,这是 report.json 字节稳定的根。
// Journal 级的逐笔分账(模型/用途/重试/compact)不进摘要(A0 冻结的
// schema 没带),聚合层不猜不补,边界写进限制节:逐笔账看 /usage。
//
// 口径:
//   - micro session(§9.1:turn<2 且无工具/验证/outcome)usage 照收,
//     摩擦频次与交互形状不拿它作样本;
//   - 摩擦按"场次"计(摘要只存类名集合,事件级次数在 /prompt audit
//     outcome),样本上限 5 场;
//   - cache 命中比例分母为 0 给 unknown,不给 0%。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "insights/finding.hpp"
#include "insights/report_model.hpp"

namespace lubancode::insights {

// ---- 生成侧的运行账(渲染要用、冻结的 report JSON 不带的部分) ----

// 一间被排除的 session(active/incomplete/corrupt),理由原样给(§9.5
// 第七节"排除理由")。
struct InsightsExcludedEntry {
    std::string workspace_key;
    std::string session_id;
    std::string status;  // active/incomplete/corrupt(gate 名)
    std::string reason;  // error_code(+message 摘要)
};

// workspace_key -> readable name(§9.1:默认只显 readable name 与 key
// 短码,绝对路径须 --show-paths)。
using InsightsWorkspaceNames = std::map<std::string, std::string>;

struct InsightsRenderExtras {
    InsightsWorkspaceNames workspace_names;
    // session_id -> workspace_key(摘要 schema 不带 key;跨工作区筛选靠
    // 生成侧的这份归属账,单工作区时全指同一个 key)。
    std::map<std::string, std::string> session_workspace;
    std::vector<InsightsExcludedEntry> excluded;
    std::vector<std::string> derived_errors;  // 摘要落盘失败(§14.5 透传)
    std::string pricing_note;                 // 价格表口径(未配/表 id/坏表)
    std::int64_t sessions_pending = 0;        // 本轮没轮上的待分析场
};

// ---- 聚合账(七节的材料;纯函数从 typed report 折出) ----

struct FrictionRollup {
    std::string category;
    std::int64_t sessions = 0;  // 非 micro 样本里出现的场数
    std::vector<std::string> sample_session_ids;  // 最多 5 场,字典序
};

struct SignalRollup {
    std::string signal_id;      // FS-01..FS-04(规则钉死,跨场可比)
    std::string feature;        // 静态目录文案(现成能力)
    std::string precondition;   // §12.2 先决条件
    std::string action;         // 一句话动作(建议节用)
    std::int64_t sessions = 0;
    std::vector<std::string> sample_session_ids;
};

struct OutcomeCount {
    std::string outcome;
    std::int64_t sessions = 0;
};

struct WorkspaceAggregate {
    // 一 工作概览。
    std::int64_t sessions = 0;
    std::int64_t provisional_sessions = 0;  // include_active 的高水位场
    std::int64_t micro_sessions = 0;
    std::int64_t turns = 0;
    std::int64_t tool_calls = 0;
    std::int64_t files_touched_sum = 0;  // 各场去重后求和(跨场不去重,口径注明)
    std::int64_t verifications = 0;
    std::vector<OutcomeCount> outcome_counts;  // outcome 名字典序
    // 二 Token 与 Cache(逐笔分账的边界在限制节)。
    std::int64_t requests_total = 0;
    std::int64_t requests_with_usage = 0;
    std::int64_t requests_unknown = 0;
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;  // 已含在 output,拆账用
    std::optional<int> cache_read_ratio_percent;  // nullopt = unknown(分母 0)
    // 三 Prompt 构成:runtime finding 按规则 id 汇总(每规则一条)。
    std::vector<Finding> prompt_rollups;
    // 四 摩擦点(按场次计,micro 除外)。
    std::vector<FrictionRollup> frictions;
    // 五 交互形状(不做人身评价,只报习惯账)。
    std::int64_t sample_sessions = 0;                // 非 micro 场数
    std::int64_t sessions_with_verification = 0;
    std::int64_t sessions_with_verification_failure = 0;
    std::int64_t sessions_cancelled = 0;
    std::int64_t sessions_repeated_retry = 0;
    std::int64_t sessions_outcome_assessed = 0;
    // 六 建议:功能信号按规则汇总。
    std::vector<SignalRollup> signals;
};

// micro 判定(§9.1):摘要字段口径——turn<2 且无工具/验证/outcome。
bool IsMicroSession(const SessionInsightSummary& summary);

// 摩擦封闭表(§9.3 十六类)的规范次序。聚合排序与 INS-F<nn> 编号共用
// 这份表,不另抄一份。
const std::vector<std::string>& FrictionClosedTableOrder();

// 聚合本体(纯函数)。frictions 按摩擦封闭表(§9.3)次序排,表外的类名
// 字典序垫后;outcome_counts 字典序;prompt_rollups 按规则 id 字典序。
WorkspaceAggregate AggregateInsights(const InsightsReport& report);

}  // namespace lubancode::insights
