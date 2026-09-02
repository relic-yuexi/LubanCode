// 请求级 UsageSample 合同(Token 账本单 §6.1 A0 冻结)。
//
// 每次本地 request attempt 最多投出一条 sample;provider 没回 usage 也投
// (usage_source=unknown、usage 为空),这样才数得出 coverage——报告写
// "17/20 请求有 provider usage",不给一只貌似完整的总数。
//
// 口径铁律:
//   - total_input_tokens 只调 api::TotalInputTokens(input+cache_read+cache_creation);
//   - reasoning 含在 output 里,汇总不再加一遍;
//   - provider 没报,token 字段不出现,绝不拿 0 顶上;
//   - context 估算、provider usage、价格估算、实际账单分栏(此处只装
//     provider usage 与本地估算费用两栏)。
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "api/types.hpp"

namespace lubancode::accounting {

inline constexpr const char* kUsageSampleSchema = "lubancode.usage.sample";
inline constexpr int kUsageSampleSchemaVersion = 1;

// usage 的来源三态(§6.1)。
enum class UsageSource { ProviderReported, Estimated, Unknown };
const char* UsageSourceName(UsageSource source);
std::optional<UsageSource> UsageSourceFromName(std::string_view name);

// 费用四态(§6.1/§6.3):订阅档可写 not_applicable;没配价格表写 not_priced。
enum class CostStatus { Estimated, ProviderReported, NotPriced, NotApplicable };
const char* CostStatusName(CostStatus status);
std::optional<CostStatus> CostStatusFromName(std::string_view name);

// 金额一律整数 micros,禁 float(§15.3)。
struct CostEstimate {
    CostStatus status = CostStatus::NotPriced;
    std::string currency;         // ISO 码,如 USD;not_priced 时可空
    std::int64_t micros = 0;      // 1e-6 计价单位;not_priced 恒 0
    std::string price_table_id;   // 命中的价格表 id;没配可空

    nlohmann::json ToJson() const;
    static std::optional<CostEstimate> FromJsonStrict(const nlohmann::json& json,
                                                      std::string* error);
};

// usage owner 事件(或 v1 legacy 的 completed 事件)的引用。
struct SourceEventRef {
    std::string stream;     // run_id(main/subagent-…)
    std::string event_id;   // "run-0001:evt-00000042"
    std::string event_hash; // 64 hex

    nlohmann::json ToJson() const;
    static std::optional<SourceEventRef> FromJsonStrict(const nlohmann::json& json,
                                                        std::string* error);
};

// 一次 request attempt 的 usage 投影。
struct UsageSample {
    std::string workspace_key;
    std::string session_id;
    std::string run_id;
    std::string run_kind;                     // trajectory RunKind 线上名
    std::optional<std::string> turn_id;
    std::string request_id;                   // local request id(Journal 关联主键)
    std::optional<std::string> provider_response_id;
    int attempt = 1;
    std::optional<RequestPurpose> purpose;    // prepared 缺失/认不得时 nullopt
    std::string provider;
    std::string wire;
    std::string model;
    UsageSource usage_source = UsageSource::Unknown;
    std::optional<api::Usage> usage;          // nullopt = unknown,字段不出现
    std::int64_t total_input_tokens = 0;      // 只等于 api::TotalInputTokens(usage)
    std::int64_t total_billed_shape_tokens = 0;  // total_input + output,比较规模用
    std::optional<int> cache_epoch;
    std::optional<bool> prefix_append_only;
    // nullopt = 旧账未知；false = usage 有报但 cache 明细字段缺席；true = 明报。
    std::optional<bool> cache_reported_by_provider;
    CostEstimate cost;
    std::optional<SourceEventRef> source_event;
    // request 终态:completed/failed/cancelled;没见到终态留空(unknown)。
    std::string request_outcome;
    // 完备性标记(§6.2):prepared 与 usage 缺一、v1 completed 顶账、
    // reported 位靠旧推断,各自点名,不冒充实测。
    bool incomplete_linkage = false;
    bool legacy_owner = false;
    bool legacy_inferred = false;

    nlohmann::json ToJson() const;
    // 严格解析:未知键拒绝、枚举严格、口径校验(reasoning ⊆ output、
    // total_input 与五项一致、unknown ⇒ usage 缺席)。
    static std::optional<UsageSample> FromJsonStrict(const nlohmann::json& json,
                                                     std::string* error);
};

}  // namespace lubancode::accounting
