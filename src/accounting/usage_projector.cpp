#include "accounting/usage_projector.hpp"

#include <map>
#include <utility>

namespace lubancode::accounting {
namespace {

struct AttemptKey {
    std::string request_id;
    int attempt = 1;
    bool operator<(const AttemptKey& other) const {
        if (request_id != other.request_id) {
            return request_id < other.request_id;
        }
        return attempt < other.attempt;
    }
};

// 一枚 request attempt 的中间账。prepared 侧按 request_id 共享(重试共用
// 同一 prepared 的 purpose/provider/wire/model);usage 侧与终态按 attempt
// 各自到齐,缺谁点名谁,不猜。
struct AttemptAccount {
    std::optional<trajectory::EventEnvelope> usage_owner;  // v2 owner 或 v1 completed
    std::string outcome;  // completed/failed/cancelled;空 = 没见终态
};

}  // namespace

UsageProjection ProjectUsage(const std::vector<trajectory::EventEnvelope>& events) {
    UsageProjection result;
    if (events.empty()) {
        result.ok = true;
        return result;
    }
    // 版本纯度:一条 stream 只得一个 schema major。
    const int version = events.front().schema_version;
    for (const auto& event : events) {
        if (event.schema_version != version) {
            result.error_code = "projection.schema_version_mixed";
            result.message = "一条 stream 不混 v1/v2";
            return result;
        }
    }

    std::map<std::string, trajectory::EventEnvelope> prepared_by_request;
    std::map<AttemptKey, AttemptAccount> accounts;
    // 保持 stream 内首现序,输出 sample 次序才稳定。
    std::vector<AttemptKey> order;
    const auto touch = [&](const std::string& request_id, int attempt) {
        const AttemptKey key{request_id, attempt};
        if (!accounts.contains(key)) {
            accounts.emplace(key, AttemptAccount{});
            order.push_back(key);
        }
        return key;
    };
    // request 已见的最大 attempt 号(completed 不带 attempt,挂最新)。
    std::map<std::string, int> latest_attempt;
    const auto bump = [&](const std::string& request_id, int attempt) {
        int& latest = latest_attempt[request_id];
        if (attempt > latest) {
            latest = attempt;
        }
    };

    for (const auto& event : events) {
        switch (event.kind) {
            case trajectory::EventKind::ModelRequestPrepared: {
                if (event.request_id.has_value()) {
                    prepared_by_request[*event.request_id] = event;
                }
                break;
            }
            case trajectory::EventKind::ModelRequestSent: {
                if (!event.request_id.has_value()) {
                    break;
                }
                const int attempt =
                    static_cast<int>(event.payload.value("attempt", std::uint64_t{1}));
                touch(*event.request_id, attempt);
                bump(*event.request_id, attempt);
                break;
            }
            case trajectory::EventKind::ModelUsageRecorded: {
                if (!event.request_id.has_value()) {
                    break;
                }
                const int attempt =
                    static_cast<int>(event.payload.value("attempt", std::uint64_t{1}));
                const auto key = touch(*event.request_id, attempt);
                AttemptAccount& account = accounts.at(key);
                if (account.usage_owner.has_value()) {
                    // 重复 owner:recorder 侧已拒;投影再撞见说明账被动过,
                    // 明报不作静默覆盖。
                    result.warnings.push_back("usage.owner_duplicate: " + *event.request_id);
                    break;
                }
                account.usage_owner = event;
                break;
            }
            case trajectory::EventKind::ModelOutputCompleted:
            case trajectory::EventKind::ModelOutputFailed:
            case trajectory::EventKind::ModelOutputCancelled: {
                if (!event.request_id.has_value()) {
                    break;
                }
                const std::string& request_id = *event.request_id;
                // failed/cancelled 可带自身 attempt;completed 不带,挂最新。
                int attempt = latest_attempt.contains(request_id) ? latest_attempt[request_id] : 1;
                if (event.payload.contains("attempt") &&
                    event.payload.at("attempt").is_number_unsigned()) {
                    attempt = static_cast<int>(event.payload.at("attempt").get<std::uint64_t>());
                }
                const auto key = touch(request_id, attempt);
                AttemptAccount& account = accounts.at(key);
                const char* outcome = event.kind == trajectory::EventKind::ModelOutputCompleted
                                          ? "completed"
                                  : event.kind == trajectory::EventKind::ModelOutputFailed
                                      ? "failed"
                                      : "cancelled";
                if (!account.outcome.empty()) {
                    result.warnings.push_back("usage.outcome_duplicate: " + request_id);
                    break;
                }
                account.outcome = outcome;
                // v1:completed.payload.usage 是 legacy owner。
                if (version == trajectory::kEnvelopeSchemaVersion &&
                    event.kind == trajectory::EventKind::ModelOutputCompleted &&
                    event.payload.contains("usage")) {
                    account.usage_owner = event;
                }
                break;
            }
            default:
                break;
        }
    }

    for (const auto& key : order) {
        const AttemptAccount& account = accounts.at(key);
        const auto prepared_it = prepared_by_request.find(key.request_id);
        const trajectory::EventEnvelope& any =
            account.usage_owner.has_value()
                ? *account.usage_owner
                : (prepared_it != prepared_by_request.end() ? prepared_it->second : events.front());
        UsageSample sample;
        sample.workspace_key = any.workspace_key;
        sample.session_id = any.session_id;
        sample.run_id = any.run_id;
        sample.run_kind = trajectory::RunKindName(any.run_kind);
        sample.turn_id = any.turn_id;
        sample.request_id = key.request_id;
        sample.attempt = key.attempt;
        sample.request_outcome = account.outcome;

        if (prepared_it != prepared_by_request.end()) {
            const auto& payload = prepared_it->second.payload;
            sample.provider = payload.value("provider", std::string());
            sample.wire = payload.value("wire", std::string());
            sample.model = payload.value("model", std::string());
            const std::string purpose_name = payload.value("purpose", std::string());
            if (!purpose_name.empty()) {
                const auto purpose = PurposeFromName(purpose_name);
                if (purpose.has_value()) {
                    sample.purpose = purpose;
                } else {
                    result.warnings.push_back("usage.purpose_unknown: " + purpose_name);
                    sample.incomplete_linkage = true;
                }
            } else {
                result.warnings.push_back("usage.purpose_missing: " + key.request_id);
                sample.incomplete_linkage = true;
            }
            if (payload.contains("cache_epoch") && payload.at("cache_epoch").is_number_unsigned()) {
                sample.cache_epoch =
                    static_cast<int>(payload.at("cache_epoch").get<std::uint64_t>());
            }
        } else {
            // usage 侧独自到齐:prepared 缺席,身份只知一半,点名。
            result.warnings.push_back("usage.prepared_missing: " + key.request_id);
            sample.incomplete_linkage = true;
            sample.provider = "unknown";
            sample.wire = "unknown";
        }

        if (account.usage_owner.has_value()) {
            const auto& owner = *account.usage_owner;
            if (owner.kind == trajectory::EventKind::ModelUsageRecorded) {
                sample.source_event =
                    SourceEventRef{owner.run_id, owner.event_id, owner.event_hash};
                const std::string response_id =
                    owner.payload.value("provider_response_id", std::string());
                if (!response_id.empty()) {
                    sample.provider_response_id = response_id;
                }
                if (owner.payload.contains("cache_epoch") &&
                    owner.payload.at("cache_epoch").is_number_unsigned()) {
                    sample.cache_epoch =
                        static_cast<int>(owner.payload.at("cache_epoch").get<std::uint64_t>());
                }
                if (owner.payload.contains("prefix_append_only") &&
                    owner.payload.at("prefix_append_only").is_boolean()) {
                    sample.prefix_append_only = owner.payload.at("prefix_append_only").get<bool>();
                }
                if (owner.payload.value("reported_by_provider", false)) {
                    api::Usage usage;
                    usage.input_tokens = owner.payload.value("input_tokens", std::int64_t{0});
                    usage.cache_read_tokens =
                        owner.payload.value("cache_read_tokens", std::int64_t{0});
                    usage.cache_creation_tokens =
                        owner.payload.value("cache_creation_tokens", std::int64_t{0});
                    usage.output_tokens = owner.payload.value("output_tokens", std::int64_t{0});
                    usage.output_reasoning_tokens =
                        owner.payload.value("reasoning_tokens", std::int64_t{0});
                    sample.usage = usage;
                    sample.usage_source = UsageSource::ProviderReported;
                    sample.total_input_tokens = api::TotalInputTokens(usage);
                    sample.total_billed_shape_tokens =
                        sample.total_input_tokens + usage.output_tokens;
                } else {
                    sample.usage_source = UsageSource::Unknown;
                }
            } else {
                // v1 legacy owner:completed.payload.usage(嵌套字段合同松,
                // §5.2 已点名;有什么读什么,推断位点名)。
                sample.legacy_owner = true;
                sample.legacy_inferred = true;
                sample.source_event =
                    SourceEventRef{owner.run_id, owner.event_id, owner.event_hash};
                const std::string response_id =
                    owner.payload.value("provider_response_id", std::string());
                if (!response_id.empty()) {
                    sample.provider_response_id = response_id;
                }
                const auto& usage_json = owner.payload.at("usage");
                const auto read_token = [&](const char* key) {
                    return usage_json.is_object() && usage_json.contains(key) &&
                                   usage_json.at(key).is_number_integer()
                               ? usage_json.at(key).get<std::int64_t>()
                               : std::int64_t{0};
                };
                api::Usage usage;
                usage.input_tokens = read_token("input_tokens");
                usage.cache_read_tokens = read_token("cache_read_tokens");
                usage.cache_creation_tokens = read_token("cache_creation_tokens");
                usage.output_tokens = read_token("output_tokens");
                usage.output_reasoning_tokens = read_token("output_reasoning_tokens");
                if (usage.input_tokens > 0 || usage.output_tokens > 0 ||
                    usage.cache_read_tokens > 0 || usage.cache_creation_tokens > 0 ||
                    usage.output_reasoning_tokens > 0) {
                    sample.usage = usage;
                    sample.usage_source = UsageSource::ProviderReported;
                    sample.total_input_tokens = api::TotalInputTokens(usage);
                    sample.total_billed_shape_tokens =
                        sample.total_input_tokens + usage.output_tokens;
                } else {
                    // v1 没有显式 reported 位:全零当 unknown,不冒充实测零。
                    sample.usage_source = UsageSource::Unknown;
                }
            }
        } else {
            // 没有 usage owner(v2 尚未写 unknown owner 或 v1 没带 usage):
            // 照投 unknown sample,coverage 靠它数。
            sample.usage_source = UsageSource::Unknown;
            if (version > trajectory::kEnvelopeSchemaVersion) {
                result.warnings.push_back("usage.owner_missing: " + key.request_id);
                sample.incomplete_linkage = true;
            }
        }
        result.samples.push_back(std::move(sample));
    }
    result.ok = true;
    return result;
}

}  // namespace lubancode::accounting
