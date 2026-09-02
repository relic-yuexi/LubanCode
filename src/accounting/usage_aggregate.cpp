#include "accounting/usage_aggregate.hpp"

#include <algorithm>
#include <utility>

namespace lubancode::accounting {
namespace {

// 四舍五入到整数的百分比;分母 <= 0 由调用方先拦(unknown 不进这里)。
int RatioPercent(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) {
        return 0;
    }
    // (part*200 + whole) / (whole*2):int64 内的四舍五入,token 量级不溢出。
    const std::int64_t scaled = part * 200 + whole;
    return static_cast<int>(scaled / (whole * 2));
}

// map -> 稳定字典序的 breakdown 行。
std::vector<UsageBreakdown> Flatten(const std::map<std::string, UsageTotals>& groups) {
    std::vector<UsageBreakdown> rows;
    rows.reserve(groups.size());
    for (const auto& [label, totals] : groups) {
        rows.push_back(UsageBreakdown{label, totals});
    }
    return rows;
}

}  // namespace

void UsageTotals::Add(const UsageSample& sample) {
    requests_total += 1;
    if (sample.attempt > 1) {
        requests_retry += 1;
    }
    // 终态:没见到的按"未收口"计,不猜成败。
    ++by_outcome[sample.request_outcome.empty() ? std::string("unclosed") : sample.request_outcome];
    if (sample.cost.status == CostStatus::Estimated) {
        cost_micros += sample.cost.micros;
        requests_priced += 1;
    }
    if (!sample.usage.has_value()) {
        requests_unknown += 1;
        return;
    }
    requests_with_usage += 1;
    const api::Usage& usage = *sample.usage;
    input_tokens += usage.input_tokens;
    cache_read_tokens += usage.cache_read_tokens;
    cache_creation_tokens += usage.cache_creation_tokens;
    output_tokens += usage.output_tokens;
    reasoning_tokens += usage.output_reasoning_tokens;
    total_input_tokens += sample.total_input_tokens;
    total_billed_shape_tokens += sample.total_billed_shape_tokens;
}

std::optional<int> UsageTotals::cache_read_ratio_percent() const {
    const std::int64_t base = input_tokens + cache_read_tokens + cache_creation_tokens;
    if (requests_with_usage <= 0 || base <= 0) {
        return std::nullopt;
    }
    return RatioPercent(cache_read_tokens, base);
}

std::optional<int> CacheEpochBreakdown::cache_read_ratio_percent() const {
    const std::int64_t base = cache_reported_input_tokens + cache_reported_read_tokens +
                              cache_reported_creation_tokens;
    if (requests_cache_reported <= 0 || base <= 0) {
        return std::nullopt;
    }
    return RatioPercent(cache_reported_read_tokens, base);
}

UsageAggregate AggregateUsage(const std::vector<UsageSample>& samples) {
    UsageAggregate aggregate;
    std::map<std::string, UsageTotals> by_purpose;
    std::map<std::string, UsageTotals> by_model;
    std::map<std::string, UsageTotals> by_run;
    std::map<std::string, UsageTotals> by_outcome;
    std::map<std::pair<std::string, int>, CacheEpochBreakdown> by_cache_epoch;
    std::vector<std::string> run_ids;
    // cache 行为观察按 run 分段:epoch 与 append-only 只在同一条 stream 内
    // 比较才有意义(§7.2)。
    struct RunCacheState {
        std::optional<int> last_epoch;
        std::int64_t last_cache_read = -1;  // -1 = 本 epoch 还没有可比的前一笔
        bool last_reported = false;
    };
    std::map<std::string, RunCacheState> run_cache;

    for (const UsageSample& sample : samples) {
        aggregate.totals.Add(sample);
        if (sample.legacy_owner) {
            aggregate.legacy_samples += 1;
        }
        if (sample.incomplete_linkage) {
            aggregate.incomplete_linkage_samples += 1;
        }

        const std::string purpose_label =
            sample.purpose.has_value() ? std::string(PurposeName(*sample.purpose)) : std::string("unknown");
        const std::string model_label =
            sample.provider.empty() ? std::string("unknown/") + sample.model
                                    : sample.provider + "/" + sample.model;
        const std::string outcome_label =
            sample.request_outcome.empty() ? std::string("unclosed") : sample.request_outcome;
        by_purpose[purpose_label].Add(sample);
        by_model[model_label].Add(sample);
        by_run[sample.run_kind].Add(sample);
        by_outcome[outcome_label].Add(sample);
        const int epoch_label = sample.cache_epoch.value_or(0);
        CacheEpochBreakdown& epoch = by_cache_epoch[{sample.run_id, epoch_label}];
        epoch.run_id = sample.run_id;
        epoch.cache_epoch = epoch_label;
        epoch.totals.Add(sample);
        if (sample.cache_reported_by_provider.value_or(false)) {
            epoch.requests_cache_reported += 1;
            if (sample.usage.has_value()) {
                epoch.cache_reported_input_tokens += sample.usage->input_tokens;
                epoch.cache_reported_read_tokens += sample.usage->cache_read_tokens;
                epoch.cache_reported_creation_tokens += sample.usage->cache_creation_tokens;
            }
        } else {
            epoch.requests_cache_unknown += 1;
        }

        if (std::find(run_ids.begin(), run_ids.end(), sample.run_id) == run_ids.end()) {
            run_ids.push_back(sample.run_id);
        }

        // ---- cache 观察(只看有实测的笔;epoch 缺席点名) ----
        RunCacheState& state = run_cache[sample.run_id];
        if (!sample.cache_epoch.has_value()) {
            aggregate.cache.epoch_unlabeled += 1;
        } else {
            if (state.last_epoch.has_value() && *state.last_epoch != *sample.cache_epoch) {
                aggregate.cache.expected_rebuild_events += 1;
            }
            // epoch 换了,前一笔不可比,重新起头。
            if (!state.last_epoch.has_value() || *state.last_epoch != *sample.cache_epoch) {
                state.last_cache_read = -1;
                state.last_reported = false;
            }
            state.last_epoch = sample.cache_epoch;
        }
        if (sample.prefix_append_only.has_value() && !*sample.prefix_append_only) {
            aggregate.cache.append_only_breaks += 1;
        }
        if (sample.usage.has_value()) {
            const std::int64_t read = sample.usage->cache_read_tokens;
            // 候选判据:同 run 同 epoch,自称 append-only(没明说 false),前一笔
            // 实测有命中,本笔实测零命中。TTL/不支持也长这模样——只计数。
            const bool append_only_claimed =
                !sample.prefix_append_only.has_value() || *sample.prefix_append_only;
            const bool epoch_known = sample.cache_epoch.has_value() && state.last_epoch.has_value() &&
                                     state.last_reported;
            if (epoch_known && append_only_claimed && state.last_cache_read > 0 && read == 0 &&
                sample.usage->input_tokens > 0) {
                aggregate.cache.unexpected_miss_candidates += 1;
            }
            state.last_cache_read = read;
            state.last_reported = true;
        }
    }

    aggregate.run_ids = std::move(run_ids);
    aggregate.by_purpose = Flatten(by_purpose);
    aggregate.by_model = Flatten(by_model);
    aggregate.by_run = Flatten(by_run);
    aggregate.by_outcome = Flatten(by_outcome);
    for (auto& [key, row] : by_cache_epoch) {
        (void)key;
        aggregate.by_cache_epoch.push_back(std::move(row));
    }
    return aggregate;
}

nlohmann::json UsageAggregate::ToJson() const {
    const auto breakdown_json = [](const std::vector<UsageBreakdown>& rows) {
        nlohmann::json array = nlohmann::json::array();
        for (const auto& row : rows) {
            array.push_back(nlohmann::json{{"label", row.label},
                                           {"requests", row.totals.requests_total},
                                           {"requests_with_usage", row.totals.requests_with_usage},
                                           {"input_tokens", row.totals.input_tokens},
                                           {"cache_read_tokens", row.totals.cache_read_tokens},
                                           {"cache_creation_tokens", row.totals.cache_creation_tokens},
                                           {"output_tokens", row.totals.output_tokens},
                                           {"reasoning_tokens", row.totals.reasoning_tokens},
                                           {"total_input_tokens", row.totals.total_input_tokens},
                                           {"total_billed_shape_tokens",
                                            row.totals.total_billed_shape_tokens},
                                           {"cost_micros", row.totals.cost_micros}});
        }
        return array;
    };
    nlohmann::json cache_json = nlohmann::json{
        {"expected_rebuild_events", cache.expected_rebuild_events},
        {"append_only_breaks", cache.append_only_breaks},
        {"unexpected_miss_candidates", cache.unexpected_miss_candidates},
        {"epoch_unlabeled", cache.epoch_unlabeled}};
    if (const auto ratio = totals.cache_read_ratio_percent()) {
        cache_json["read_ratio_percent"] = *ratio;
    } else {
        cache_json["read_ratio_percent"] = nullptr;  // unknown:没有实测分母
    }

    nlohmann::json outcomes = nlohmann::json::object();
    for (const auto& [outcome, count] : totals.by_outcome) {
        outcomes[outcome] = count;
    }
    nlohmann::json warnings_json = nlohmann::json::array();
    for (const auto& warning : warnings) {
        warnings_json.push_back(warning);
    }
    nlohmann::json run_ids_json = nlohmann::json::array();
    for (const auto& run_id : run_ids) {
        run_ids_json.push_back(run_id);
    }
    nlohmann::json epoch_json = nlohmann::json::array();
    for (const auto& row : by_cache_epoch) {
        nlohmann::json item{{"run_id", row.run_id},
                            {"cache_epoch", row.cache_epoch == 0 ? nlohmann::json(nullptr)
                                                                  : nlohmann::json(row.cache_epoch)},
                            {"requests", row.totals.requests_total},
                            {"requests_cache_reported", row.requests_cache_reported},
                            {"requests_cache_unknown", row.requests_cache_unknown},
                            {"input_tokens", row.totals.input_tokens},
                            {"cache_read_tokens", row.totals.cache_read_tokens},
                            {"cache_creation_tokens", row.totals.cache_creation_tokens},
                            {"total_input_tokens", row.totals.total_input_tokens}};
        if (const auto ratio = row.cache_read_ratio_percent()) {
            item["read_ratio_percent"] = *ratio;
        } else {
            item["read_ratio_percent"] = nullptr;
        }
        epoch_json.push_back(std::move(item));
    }
    return nlohmann::json{
        {"run_ids", run_ids_json},
        {"totals",
         nlohmann::json{{"requests_total", totals.requests_total},
                        {"requests_with_usage", totals.requests_with_usage},
                        {"requests_unknown", totals.requests_unknown},
                        {"requests_retry", totals.requests_retry},
                        {"input_tokens", totals.input_tokens},
                        {"cache_read_tokens", totals.cache_read_tokens},
                        {"cache_creation_tokens", totals.cache_creation_tokens},
                        {"output_tokens", totals.output_tokens},
                        {"reasoning_tokens", totals.reasoning_tokens},
                        {"total_input_tokens", totals.total_input_tokens},
                        {"total_billed_shape_tokens", totals.total_billed_shape_tokens},
                        {"by_outcome", outcomes},
                        {"cost_micros", totals.cost_micros},
                        {"requests_priced", totals.requests_priced}}},
        {"cache", cache_json},
        {"by_cache_epoch", epoch_json},
        {"by_purpose", breakdown_json(by_purpose)},
        {"by_model", breakdown_json(by_model)},
        {"by_run", breakdown_json(by_run)},
        {"by_outcome", breakdown_json(by_outcome)},
        {"legacy_samples", legacy_samples},
        {"incomplete_linkage_samples", incomplete_linkage_samples},
        {"warnings", warnings_json}};
}

}  // namespace lubancode::accounting
