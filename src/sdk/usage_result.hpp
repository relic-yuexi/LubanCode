#pragma once

#include <stdexcept>
#include <lubancore/core.hpp>
#include "api/usage_json.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore::detail::usage_result {
namespace api = ::lubancode::api;
namespace facts = ::lubancore::usage::v1;
using Json = nlohmann::json;

inline std::expected<void, std::string_view> ValidateRequestBinding(const UsageAttempt& record,
    const ::lubancode::trajectory::v3::V3Ledger& ledger) {
    namespace v3 = ::lubancode::trajectory::v3;
    if (record.source_session_id != ledger.session_id || record.source_run_id != ledger.run_id ||
        record.trajectory_request_id.empty() || record.turn_id.empty())
        return std::unexpected("sdk.usage.request_owner");
    const v3::EventLine* prepared = nullptr;
    for (const auto& event : ledger.events) {
        if (event.kind != v3::EventKindV3::ModelRequestPrepared || event.request_id != record.trajectory_request_id) continue;
        if (prepared) return std::unexpected("sdk.usage.request_ambiguous");
        prepared = &event;
    }
    if (!prepared || prepared->session_id != ledger.session_id || prepared->run_id != ledger.run_id ||
        prepared->turn_id != record.turn_id || prepared->payload.value("model", Json()) != record.model ||
        prepared->payload.value("requestPurpose", Json()) != record.purpose ||
        prepared->payload.value("producerStepId", Json()) != record.step_id)
        return std::unexpected("sdk.usage.request_prepared_mismatch");
    const auto prefix = prepared->payload.find("prefixAccount");
    if (prefix == prepared->payload.end() || !prefix->is_object() || prefix->value("cacheEpoch", Json()) != record.cache_epoch)
        return std::unexpected("sdk.usage.request_epoch_mismatch");
    if (!v3::CheckPreparedAgainstChain(ledger, prepared->event_id).empty())
        return std::unexpected("sdk.usage.request_chain_mismatch");
    const v3::EventLine* observed = nullptr;
    for (const auto& event : ledger.events) {
        if (event.kind != v3::EventKindV3::ModelUsageObserved || event.request_id != record.trajectory_request_id) continue;
        if (observed) return std::unexpected("sdk.usage.observation_ambiguous");
        observed = &event;
    }
    const Json numbers = std::array<std::int64_t, 5>{record.usage.input_tokens, record.usage.output_tokens,
        record.usage.cache_read_tokens, record.usage.cache_creation_tokens, record.usage.output_reasoning_tokens};
    const Json response_id = record.provider_response_id.empty() ? Json(nullptr) : Json(record.provider_response_id);
    if (!observed || observed->seq <= prepared->seq || observed->session_id != ledger.session_id ||
        observed->run_id != ledger.run_id || observed->turn_id != record.turn_id || observed->step_id != prepared->step_id ||
        observed->payload.value("numbers", Json()) != numbers || observed->payload.value("providerResponseId", Json()) != response_id ||
        observed->payload.value("reportedByProvider", Json()) != record.reported_by_provider ||
        observed->payload.value("incomplete", Json()) != record.incomplete)
        return std::unexpected("sdk.usage.observation_mismatch");
    if (record.observation) {
        const api::Usage selected{record.usage.input_tokens, record.usage.output_tokens, record.usage.cache_read_tokens,
            record.usage.cache_creation_tokens, record.usage.output_reasoning_tokens};
        const auto material = api::usage_json::Encode(*record.observation, selected);
        if (!material || observed->payload.value("observation", Json()) != *material)
            return std::unexpected("sdk.usage.observation_material_mismatch");
    } else if (observed->payload.contains("observation")) return std::unexpected("sdk.usage.observation_material_mismatch");
    for (const auto& message : ledger.messages) {
        if (message.request_id != record.trajectory_request_id || message.message.value("role", Json()) != "assistant") continue;
        if (message.session_id != ledger.session_id || message.run_id != ledger.run_id || message.turn_id != record.turn_id)
            return std::unexpected("sdk.usage.response_owner_mismatch");
        if (const auto id = message.message.find("provider_response_id"); id != message.message.end()) {
            const Json expected = record.provider_response_id.empty() ? Json(nullptr) : Json(record.provider_response_id);
            if (*id != expected) return std::unexpected("sdk.usage.response_identity_mismatch");
        }
        if (record.reported_by_provider && message.usage && message.usage->is_object()) {
            const std::array<const char*, 5> keys{"inputTokens", "outputTokens", "cacheReadTokens", "cacheWriteTokens", "reasoningTokens"};
            const std::array<std::int64_t, 5> numbers{record.usage.input_tokens, record.usage.output_tokens,
                record.usage.cache_read_tokens, record.usage.cache_creation_tokens, record.usage.output_reasoning_tokens};
            for (std::size_t i = 0; i < keys.size(); ++i)
                if (message.usage->value(keys[i], Json()) != numbers[i]) return std::unexpected("sdk.usage.response_numbers_mismatch");
        }
    }
    return {};
}

inline api::Usage Native(const Usage& value) {
    return {value.input_tokens, value.output_tokens, value.cache_read_tokens,
            value.cache_creation_tokens, value.output_reasoning_tokens};
}
inline Usage Public(const api::Usage& value) {
    return {value.input_tokens, value.output_tokens, value.cache_read_tokens,
            value.cache_creation_tokens, value.output_reasoning_tokens};
}
// Borrowed typed admission: fixed-size aggregation precedes UsageReport copies
// and event JSON construction. No model, persistence or host callback runs here.
inline void CaptureTyped(OperationUsage& result, const api::Usage& source,
                         const facts::Observation* observation, bool subordinate, bool incomplete = false) {
    auto& target = subordinate ? result.subordinate : result.direct;
    const auto before = target.coverage;
    auto accumulated = Native(target.total);
    api::usage_aggregation::Add(accumulated, target.coverage, source, observation);
    target.total = Public(accumulated);
    if (incomplete) for (std::size_t i = 0; i < facts::kFieldCount; ++i)
        if (target.coverage.fields[i].anomalous == before.fields[i].anomalous)
            api::usage_aggregation::Increment(target.coverage.fields[i].anomalous, target.coverage);
}
template <typename Context>
inline void CaptureAttempt(OperationUsage& result, const api::Usage& source,
    const facts::Observation* observation, bool subordinate, bool incomplete, const Context& context) {
    // Numeric ownership comes first, even when record allocation/admission fails.
    const auto before = subordinate ? result.subordinate.coverage : result.direct.coverage;
    CaptureTyped(result, source, observation, subordinate, incomplete);
    try {
        if (result.attempts.size() >= kMaxUsageAttempts) throw std::runtime_error("sdk.usage.attempt_limit");
        for (const auto text : {context.trajectory_request_id, context.provider_response_id, context.model,
                               context.step_id, context.turn_id, context.purpose, context.source_session_id, context.source_run_id})
            if (text.size() > facts::kMaxResponseIdBytes || text.find('\0') != std::string_view::npos)
                throw std::runtime_error("sdk.usage.attempt_identity");
        if (context.cache_epoch < 0) throw std::runtime_error("sdk.usage.attempt_epoch");
        UsageAttempt record;
        record.usage = Public(source); record.subordinate = subordinate; record.incomplete = incomplete;
        record.trajectory_request_id = context.trajectory_request_id; record.provider_response_id = context.provider_response_id;
        record.model = context.model; record.step_id = context.step_id; record.turn_id = context.turn_id;
        record.purpose = context.purpose; record.cache_epoch = context.cache_epoch;
        record.reported_by_provider = context.reported_by_provider;
        record.source_session_id = context.source_session_id; record.source_run_id = context.source_run_id;
        for (const auto* text : {&record.trajectory_request_id, &record.provider_response_id, &record.model,
                                &record.step_id, &record.turn_id, &record.purpose, &record.source_session_id, &record.source_run_id})
            if (!api::usage_observation::TextFits(*text, facts::kMaxResponseIdBytes))
                throw std::runtime_error("sdk.usage.attempt_identity");
        if (observation) {
            const auto valid = api::usage_observation::Validate(*observation, api::usage_aggregation::Values(source));
            if (valid) record.observation = *observation;
            else record.incomplete = true; // Invalid evidence was already counted anomalous by CaptureTyped.
        }
        result.attempts.push_back(std::move(record));
    } catch (...) {
        result.attempts_complete = false;
        auto& coverage = subordinate ? result.subordinate.coverage : result.direct.coverage;
        for (std::size_t i = 0; i < facts::kFieldCount; ++i)
            if (coverage.fields[i].anomalous == before.fields[i].anomalous)
                api::usage_aggregation::Increment(coverage.fields[i].anomalous, coverage);
        throw; // Stop the turn; never continue producing unrecordable observations.
    }
}
inline void Capture(OperationUsage& result, const Json& payload) {
    result.attempts_complete = false; // Compatibility event lacks typed owner/context proof.
    for (const char* key : {"input_tokens", "output_tokens", "cache_read_tokens", "cache_creation_tokens", "reasoning_tokens"})
        if (!api::usage_json::Signed(api::usage_json::Member(payload, key)))
            throw std::runtime_error("sdk.usage.event_number_invalid");
    api::Usage source;
    source.input_tokens = payload.at("input_tokens").get<std::int64_t>();
    source.output_tokens = payload.at("output_tokens").get<std::int64_t>();
    source.cache_read_tokens = payload.at("cache_read_tokens").get<std::int64_t>();
    source.cache_creation_tokens = payload.at("cache_creation_tokens").get<std::int64_t>();
    source.output_reasoning_tokens = payload.at("reasoning_tokens").get<std::int64_t>();
    auto& target = payload.value("subordinate", false) ? result.subordinate : result.direct;
    const auto before = Native(target.total);
    const auto coverage_before = target.coverage;
    auto accumulated = before;
    // Publish numeric facts into the operation before any material copy. A later
    // decode/allocation failure leaves this observation unknown, not absent.
    api::usage_aggregation::Add(accumulated, target.coverage, source, nullptr);
    target.total = Public(accumulated);
    try {
        api::UsageReport report; report.usage = source;
        api::usage_json::Restore(report, payload);
        if (report.usage_observation) {
            // Refine this observation from the same previous prefix. Commit only its
            // fixed-size coverage; the actual totals above are never charged twice.
            auto refined = coverage_before;
            auto same_prefix = before;
            api::usage_aggregation::Add(same_prefix, refined, source, &*report.usage_observation);
            target.coverage = refined;
        } else if (!report.usage_observation_error.empty()) {
            for (auto& field : target.coverage.fields)
                api::usage_aggregation::Increment(field.anomalous, target.coverage);
        }
    } catch (...) {
        // Metadata cannot unwind away numeric facts already owned by this turn.
        // Mark every field imprecise without allocating an exception message.
        for (auto& field : target.coverage.fields)
            api::usage_aggregation::Increment(field.anomalous, target.coverage);
    }
}

inline Json Summary(const UsageSummary& value) {
    const auto numbers = api::usage_aggregation::Values(Native(value.total));
    Json fields = Json::array();
    for (const auto& field : value.coverage.fields) {
        fields.push_back({{"observed", field.observed}, {"missing", field.missing},
                          {"valid", field.valid}, {"inferred", field.inferred},
                          {"anomalous", field.anomalous}, {"included", field.included},
                          {"omitted", field.omitted}, {"arithmetic_overflow", field.arithmetic_overflow}});
    }
    return {{"total", numbers}, {"samples", value.coverage.samples},
            {"counter_overflow", value.coverage.counter_overflow}, {"fields", std::move(fields)}};
}
inline Json Encode(const OperationUsage& value) {
    if (value.attempts.size() > kMaxUsageAttempts) throw std::runtime_error("sdk.usage.attempt_limit");
    Json attempts = Json::array();
    for (const auto& record : value.attempts) {
        Json item{{"numbers", api::usage_aggregation::Values(Native(record.usage))},
            {"trajectory_request_id", record.trajectory_request_id}, {"provider_response_id", record.provider_response_id},
            {"model", record.model}, {"step_id", record.step_id}, {"turn_id", record.turn_id}, {"purpose", record.purpose},
            {"cache_epoch", record.cache_epoch}, {"subordinate", record.subordinate}, {"incomplete", record.incomplete},
            {"reported_by_provider", record.reported_by_provider}};
        item["source_session_id"] = record.source_session_id; item["source_run_id"] = record.source_run_id;
        if (record.observation) {
            auto material = api::usage_json::Encode(*record.observation, Native(record.usage));
            if (!material) throw std::runtime_error(std::string(material.error()));
            item["observation"] = std::move(*material);
        }
        attempts.push_back(std::move(item));
    }
    return {{"version", 2}, {"direct", Summary(value.direct)}, {"subordinate", Summary(value.subordinate)},
        {"attempts_complete", value.attempts_complete}, {"attempts", std::move(attempts)}};
}
inline std::expected<UsageSummary, std::string_view> ReadSummary(const Json& encoded) {
    using api::usage_json::Member;
    using api::usage_json::Unsigned;
    const auto* numbers = Member(encoded, "total");
    const auto* fields = Member(encoded, "fields");
    const auto* overflow = Member(encoded, "counter_overflow");
    const auto* samples = Member(encoded, "samples");
    if (!numbers || !numbers->is_array() || numbers->size() != facts::kFieldCount ||
        !fields || !fields->is_array() || fields->size() != facts::kFieldCount ||
        !overflow || !overflow->is_boolean() || !Unsigned(samples, std::numeric_limits<std::uint64_t>::max()))
        return std::unexpected("sdk.usage.summary_shape");
    UsageSummary result;
    std::array<std::int64_t, facts::kFieldCount> values{};
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        if (!api::usage_json::Signed(&(*numbers)[i])) return std::unexpected("sdk.usage.summary_number");
        values[i] = (*numbers)[i].get<std::int64_t>();
    }
    api::Usage native; api::usage_aggregation::Assign(native, values); result.total = Public(native);
    result.coverage.samples = samples->get<std::uint64_t>();
    result.coverage.counter_overflow = overflow->get<bool>();
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        const auto& field = (*fields)[i]; auto& count = result.coverage.fields[i];
        for (const char* key : {"observed", "missing", "valid", "inferred", "anomalous", "included", "omitted"})
            if (!Unsigned(Member(field, key), result.coverage.samples))
                return std::unexpected("sdk.usage.summary_count");
        const auto* arithmetic = Member(field, "arithmetic_overflow");
        if (!arithmetic || !arithmetic->is_boolean()) return std::unexpected("sdk.usage.summary_overflow");
        count.observed = field.at("observed").get<std::uint64_t>();
        count.missing = field.at("missing").get<std::uint64_t>();
        count.valid = field.at("valid").get<std::uint64_t>();
        count.inferred = field.at("inferred").get<std::uint64_t>();
        count.anomalous = field.at("anomalous").get<std::uint64_t>();
        count.included = field.at("included").get<std::uint64_t>();
        count.omitted = field.at("omitted").get<std::uint64_t>();
        count.arithmetic_overflow = arithmetic->get<bool>();
        if (!result.coverage.counter_overflow &&
            (count.observed != result.coverage.samples - count.missing ||
             count.included != result.coverage.samples - count.omitted ||
             (count.arithmetic_overflow && count.omitted == 0)))
            return std::unexpected("sdk.usage.summary_inconsistent");
        if (!count.arithmetic_overflow && count.omitted != 0)
            return std::unexpected("sdk.usage.summary_inconsistent");
        if (result.coverage.samples == 0 && values[i] != 0)
            return std::unexpected("sdk.usage.summary_unobserved_total");
    }
    return result;
}
inline std::expected<OperationUsage, std::string_view> Decode(const Json& encoded) {
    const auto* version = api::usage_json::Member(encoded, "version");
    if (!api::usage_json::Unsigned(version, 2) || (*version != 1 && *version != 2))
        return std::unexpected("sdk.usage.summary_version");
    const auto* direct = api::usage_json::Member(encoded, "direct");
    const auto* subordinate = api::usage_json::Member(encoded, "subordinate");
    if (!direct || !subordinate) return std::unexpected("sdk.usage.summary_shape");
    auto first = ReadSummary(*direct); if (!first) return std::unexpected(first.error());
    auto second = ReadSummary(*subordinate); if (!second) return std::unexpected(second.error());
    OperationUsage result{*first, *second};
    if (*version == 1) return result; // Old summaries never manufacture original materials.
    const auto* attempts = api::usage_json::Member(encoded, "attempts");
    const auto* complete = api::usage_json::Member(encoded, "attempts_complete");
    if (!attempts || !attempts->is_array() || attempts->size() > kMaxUsageAttempts || !complete || !complete->is_boolean())
        return std::unexpected("sdk.usage.attempt_shape");
    result.attempts_complete = complete->get<bool>();
    result.attempts.reserve(attempts->size());
    OperationUsage rebuilt;
    for (const auto& item : *attempts) {
        const auto* numbers = api::usage_json::Member(item, "numbers");
        const auto* subordinate_flag = api::usage_json::Member(item, "subordinate");
        const auto* incomplete_flag = api::usage_json::Member(item, "incomplete");
        const auto* reported_flag = api::usage_json::Member(item, "reported_by_provider");
        const auto* epoch = api::usage_json::Member(item, "cache_epoch");
        if (!numbers || !numbers->is_array() || numbers->size() != facts::kFieldCount ||
            !subordinate_flag || !subordinate_flag->is_boolean() || !incomplete_flag || !incomplete_flag->is_boolean() ||
            !reported_flag || !reported_flag->is_boolean() ||
            !api::usage_json::Unsigned(epoch, std::numeric_limits<int>::max()))
            return std::unexpected("sdk.usage.attempt_shape");
        for (const char* key : {"trajectory_request_id", "provider_response_id", "model", "step_id", "turn_id", "purpose", "source_session_id", "source_run_id"}) {
            const auto* text = api::usage_json::Member(item, key);
            if (!text || !text->is_string() || !api::usage_observation::TextFits(text->get_ref<const std::string&>(), facts::kMaxResponseIdBytes))
                return std::unexpected("sdk.usage.attempt_identity");
        }
        std::array<std::int64_t, facts::kFieldCount> values{};
        for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
            if (!api::usage_json::Signed(&(*numbers)[i])) return std::unexpected("sdk.usage.attempt_number");
            values[i] = (*numbers)[i].get<std::int64_t>();
        }
        api::Usage native; api::usage_aggregation::Assign(native, values);
        UsageAttempt record; record.usage = Public(native);
        record.subordinate = subordinate_flag->get<bool>(); record.incomplete = incomplete_flag->get<bool>();
        record.cache_epoch = epoch->get<int>();
        record.reported_by_provider = reported_flag->get<bool>();
        if (const auto* material = api::usage_json::Member(item, "observation")) {
            auto decoded = api::usage_json::Decode(*material, native);
            if (!decoded) return std::unexpected(decoded.error());
            record.observation = std::move(*decoded);
        }
        record.trajectory_request_id = item.at("trajectory_request_id").get_ref<const std::string&>();
        record.provider_response_id = item.at("provider_response_id").get_ref<const std::string&>();
        record.model = item.at("model").get_ref<const std::string&>(); record.step_id = item.at("step_id").get_ref<const std::string&>();
        record.turn_id = item.at("turn_id").get_ref<const std::string&>(); record.purpose = item.at("purpose").get_ref<const std::string&>();
        record.source_session_id = item.at("source_session_id").get_ref<const std::string&>();
        record.source_run_id = item.at("source_run_id").get_ref<const std::string&>();
        CaptureTyped(rebuilt, native, record.observation ? &*record.observation : nullptr, record.subordinate, record.incomplete);
        result.attempts.push_back(std::move(record));
    }
    if (rebuilt.direct.coverage.samples > result.direct.coverage.samples || rebuilt.subordinate.coverage.samples > result.subordinate.coverage.samples)
        return std::unexpected("sdk.usage.attempt_summary_mismatch");
    if (result.attempts_complete && (Summary(rebuilt.direct) != Summary(result.direct) || Summary(rebuilt.subordinate) != Summary(result.subordinate)))
        return std::unexpected("sdk.usage.attempt_summary_mismatch");
    return result;
}
}  // namespace lubancore::detail::usage_result
