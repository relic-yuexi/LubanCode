// Usage 五层聚合(Token 账本单 §7.1 A2)。层次:sample(request) -> turn ->
// run -> session -> workspace。本件是纯求和:吃 UsageSample 序列,产一份
// 聚合账。每层只求和下一层,不另扫文本估 token。
//
// 口径铁律(§四/§14.3):
//   - provider 没报 usage 的 sample 只进 requests_unknown,不贡献 token——
//     合计是下界,报告须写 coverage,不冒充完整总数;
//   - reasoning 已含在 output 里,汇总不再加一遍;
//   - total_input 只认 sample.total_input_tokens(只调 api::TotalInputTokens
//     的那一只口);
//   - 金额只累 sample.cost.status==estimated 的 micros;not_priced 单列,
//     不拿 0 冒充"免费"。
//
// cache 行为(§7.2)只给观察计数:"没 cache_read"不自动等于 bug——provider
// 可能没支持、没回报、TTL 已过。分栏(capability/reported/observed)里的
// capability 与 reported 由调用方摆在报告别处,本件只出 observed。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/usage_sample.hpp"

namespace lubancode::accounting {

// 一层账的合计。token 各项只累计 usage 有值的 sample(下界)。
struct UsageTotals {
    std::int64_t requests_total = 0;      // 全部 request attempt
    std::int64_t requests_with_usage = 0;  // provider 报了的(明报全零也算)
    std::int64_t requests_unknown = 0;    // 没报的;绝不冒充 0
    std::int64_t requests_retry = 0;      // attempt > 1 的重试笔
    std::int64_t input_tokens = 0;
    std::int64_t cache_read_tokens = 0;
    std::int64_t cache_creation_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t reasoning_tokens = 0;    // output 的子集,拆账用
    std::int64_t total_input_tokens = 0;  // input+read+creation 合计
    std::int64_t total_billed_shape_tokens = 0;  // total_input + output
    // request 终态分布:completed/failed/cancelled/空串(未见终态)。
    std::map<std::string, std::int64_t> by_outcome;
    // 费用(§6.3):只累 status=estimated 的整数 micros。
    std::int64_t cost_micros = 0;
    std::int64_t requests_priced = 0;

    void Add(const UsageSample& sample);

    // cache 命中率(§7.2):cache_read / (input+read+creation),只对有实测的
    // sample 成立。分母为 0(一笔实测都没有)给 nullopt——报告写 unknown,
    // 不写 0%。
    std::optional<int> cache_read_ratio_percent() const;
};

// 分账行:标签 -> 合计。标签按维度定(purpose 线上名、provider/model、
// run_kind、outcome)。
struct UsageBreakdown {
    std::string label;
    UsageTotals totals;
};

// cache 行为的观察计数(§7.2 三种行为,只计数不判罪):
//   expected_rebuild —— 同 run 内 cache_epoch 变化(compact/清理/toolset
//                       改变一类,重建属预期);
//   append_only_breaks —— prefix_append_only=false 的请求(前缀被改写,
//                       cache 丢是自找的);
//   unexpected_miss_candidates —— 同 run、同 epoch、自称 append-only,前
//                       一笔实测有命中、本笔实测零命中。这只是候选:TTL
//                       过期、provider 不稳定都长这模样,报告措辞不越界。
//   epoch_unlabeled —— cache_epoch 缺席的请求(没法归类的,点名不猜)。
struct CacheBehavior {
    std::int64_t expected_rebuild_events = 0;
    std::int64_t append_only_breaks = 0;
    std::int64_t unexpected_miss_candidates = 0;
    std::int64_t epoch_unlabeled = 0;
};

// 一份聚合账(turn/run/session/workspace 哪一层由调用方喂多少决定)。
struct UsageAggregate {
    std::vector<std::string> run_ids;  // 收进来的 stream(coverage 写"几条 run")
    UsageTotals totals;
    CacheBehavior cache;
    std::vector<UsageBreakdown> by_purpose;  // purpose 线上名;缺 = "unknown"
    std::vector<UsageBreakdown> by_model;    // "provider/model";prepared 缺 = "unknown/*"
    std::vector<UsageBreakdown> by_run;      // run_kind 线上名
    std::vector<UsageBreakdown> by_outcome;  // completed/failed/cancelled/"unclosed"
    // 投影缺口点名(§6.2):不混进任何"异常"语气,只说明账的成色。
    std::int64_t legacy_samples = 0;             // v1 completed 顶的旧账
    std::int64_t incomplete_linkage_samples = 0;  // prepared/usage 缺一的笔
    std::vector<std::string> warnings;            // projector warnings 透传

    nlohmann::json ToJson() const;
};

// 聚合(纯函数)。samples 为空给出全零账(报告写"没有请求",不猜)。
// 分账行按 label 字典序稳定排列。
UsageAggregate AggregateUsage(const std::vector<UsageSample>& samples);

}  // namespace lubancode::accounting
