#include "accounting/usage_projector.hpp"

#include <map>
#include <set>
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
                // 缓存读/写明报位(C2):新键优先;旧 owner 只有合并位
                // cache_reported_by_provider,读进 read 位,creation 留
                // nullopt(旧账分不开读写,不猜)。异常账(C4)非空才在。
                if (owner.payload.contains("cache_read_reported_by_provider") &&
                    owner.payload.at("cache_read_reported_by_provider").is_boolean()) {
                    sample.cache_read_reported_by_provider =
                        owner.payload.at("cache_read_reported_by_provider").get<bool>();
                } else if (owner.payload.contains("cache_reported_by_provider") &&
                           owner.payload.at("cache_reported_by_provider").is_boolean()) {
                    sample.cache_read_reported_by_provider =
                        owner.payload.at("cache_reported_by_provider").get<bool>();
                }
                if (owner.payload.contains("cache_creation_reported_by_provider") &&
                    owner.payload.at("cache_creation_reported_by_provider").is_boolean()) {
                    sample.cache_creation_reported_by_provider =
                        owner.payload.at("cache_creation_reported_by_provider").get<bool>();
                }
                if (owner.payload.contains("usage_anomaly") &&
                    owner.payload.at("usage_anomaly").is_string() &&
                    !owner.payload.at("usage_anomaly").get<std::string>().empty()) {
                    sample.usage_anomaly = owner.payload.at("usage_anomaly").get<std::string>();
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

// ---------------------------------------------------------------------------
// v3 半场(T06/V3-GAP-01):assistant usage owner 唯一可累计。
// ---------------------------------------------------------------------------

api::Usage UsageFromV3Owner(const nlohmann::json& usage) {
    api::Usage out;
    // 键集合同(schema §五):五键缺子项省略——省键保持 0,不补不猜。
    if (usage.is_object()) {
        if (usage.contains("inputTokens") && usage.at("inputTokens").is_number_integer()) {
            out.input_tokens = usage.at("inputTokens").get<std::int64_t>();
        }
        if (usage.contains("cacheReadTokens") && usage.at("cacheReadTokens").is_number_integer()) {
            out.cache_read_tokens = usage.at("cacheReadTokens").get<std::int64_t>();
        }
        if (usage.contains("cacheWriteTokens") &&
            usage.at("cacheWriteTokens").is_number_integer()) {
            out.cache_creation_tokens = usage.at("cacheWriteTokens").get<std::int64_t>();
        }
        if (usage.contains("outputTokens") && usage.at("outputTokens").is_number_integer()) {
            out.output_tokens = usage.at("outputTokens").get<std::int64_t>();
        }
        if (usage.contains("reasoningTokens") &&
            usage.at("reasoningTokens").is_number_integer()) {
            out.output_reasoning_tokens = usage.at("reasoningTokens").get<std::int64_t>();
        }
    }
    return out;
}

std::optional<RequestPurpose> MapV3Purpose(std::string_view name, bool is_subagent) {
    if (name == "conversation") {
        return is_subagent ? RequestPurpose::SubagentTurn : RequestPurpose::MainTurn;
    }
    if (name == "compact") {
        return RequestPurpose::CompactReduce;  // v3 单段归并,无 map 分账
    }
    if (name == "action_summary") {
        return RequestPurpose::ActionSummary;
    }
    if (name == "session_title") {
        return RequestPurpose::TitleRefine;
    }
    // goal_evaluation / context_summary / capability:RequestPurpose 无对应,
    // nullopt + 调用方点名,不硬塞近似枚举。
    return std::nullopt;
}

UsageProjection ProjectV3Usage(const trajectory::v3::V3Ledger& ledger,
                               const V3UsageProjectorContext& context) {
    namespace v3 = trajectory::v3;
    UsageProjection result;

    // 事件索引一遍收齐:prepared / sent / 终态 / appended 观察。
    std::map<std::string, const nlohmann::json*> prepared;  // requestId → payload
    std::map<std::string, std::string> outcome;             // requestId → completed/failed/cancelled
    std::vector<std::string> sent_order;                    // 实际发出的请求(首现序)
    std::set<std::string> sent_seen;
    for (const auto& event : ledger.events) {
        const std::string request_id = event.request_id.value_or("");
        switch (event.kind) {
            case v3::EventKindV3::ModelRequestPrepared: {
                if (!request_id.empty()) {
                    prepared[request_id] = &event.payload;
                }
                break;
            }
            case v3::EventKindV3::ModelRequestSent: {
                if (!request_id.empty() && sent_seen.insert(request_id).second) {
                    sent_order.push_back(request_id);
                }
                break;
            }
            case v3::EventKindV3::ModelResponseCompleted: {
                if (!request_id.empty()) {
                    outcome[request_id] = "completed";
                }
                break;
            }
            case v3::EventKindV3::ModelResponseFailed:
            case v3::EventKindV3::ModelRequestFailed: {
                if (!request_id.empty()) {
                    outcome[request_id] = "failed";
                }
                break;
            }
            case v3::EventKindV3::ModelResponseCancelled: {
                if (!request_id.empty()) {
                    outcome[request_id] = "cancelled";
                }
                break;
            }
            case v3::EventKindV3::ModelUsageAppended: {
                // §五 owner 表:迟到/更正/无消息请求的观察承载,不参与累计。
                // 单列点名(带数字,人工对账用),绝不升级成 owner。
                std::string note = "usage.v3_appended_observed: " +
                                   (request_id.empty() ? event.event_id : request_id);
                if (event.payload.contains("usage") && event.payload.at("usage").is_object()) {
                    const auto& usage = event.payload.at("usage");
                    if (usage.contains("inputTokens") &&
                        usage.at("inputTokens").is_number_integer()) {
                        note += " in=" + std::to_string(
                                             usage.at("inputTokens").get<std::int64_t>());
                    }
                    if (usage.contains("outputTokens") &&
                        usage.at("outputTokens").is_number_integer()) {
                        note += " out=" + std::to_string(
                                              usage.at("outputTokens").get<std::int64_t>());
                    }
                }
                note += "(观察,不入累计)";
                result.warnings.push_back(std::move(note));
                break;
            }
            default:
                break;
        }
    }

    const auto fill_request_side = [&](UsageSample& sample, const std::string& request_id,
                                       std::string_view purpose_fallback) {
        // purpose:优先 message.purpose(owner 驱动时的枚举);无 owner 的请求
        // 只能读 prepared.payload.purpose 字符串。
        if (!purpose_fallback.empty()) {
            const auto purpose = MapV3Purpose(purpose_fallback, context.is_subagent);
            if (purpose.has_value()) {
                sample.purpose = purpose;
            } else {
                result.warnings.push_back("usage.v3_purpose_unmapped: " +
                                          std::string(purpose_fallback));
            }
        }
        const auto prepared_it = prepared.find(request_id);
        if (prepared_it == prepared.end()) {
            result.warnings.push_back("usage.prepared_missing: " + request_id);
            sample.incomplete_linkage = true;
            return;
        }
        const nlohmann::json& payload = *prepared_it->second;
        const auto read_string = [&](const char* key) {
            return payload.contains(key) && payload.at(key).is_string()
                       ? payload.at(key).get<std::string>()
                       : std::string();
        };
        // 实际 provider/model:owner 驱动时 message 自带(§4.44),这里只在
        // 缺席时补 prepared 快照(sent-only 的失败请求没有 message 可读)。
        if (sample.provider.empty()) {
            sample.provider = read_string("provider");
        }
        if (sample.wire.empty()) {
            sample.wire = read_string("wire");
        }
        if (sample.model.empty()) {
            sample.model = read_string("model");
        }
    };

    // owner 驱动:模型生成的 assistant 一条一个 owner。
    std::set<std::string> owned_requests;
    for (const auto& message : ledger.messages) {
        const std::string role = message.message.value("role", "");
        if (role != "assistant") {
            continue;  // system/user/tool 不是 usage owner
        }
        const std::string request_id = message.request_id.value_or("");
        if (!owned_requests.insert(request_id).second) {
            // 同一请求第二条 assistant:账被动过的形状,点名不静默覆盖。
            result.warnings.push_back("usage.v3_owner_duplicate: " + request_id);
            continue;
        }
        UsageSample sample;
        sample.session_id = ledger.session_id;
        sample.run_id = ledger.run_id;
        sample.run_kind = context.run_kind;
        sample.turn_id = message.turn_id;
        sample.request_id = request_id;
        sample.attempt = 1;  // v3 每请求唯一 requestId,无 attempt 维度
        sample.provider = message.provider.value_or("");
        sample.wire = message.wire.value_or("");
        sample.model = message.model.value_or("");
        sample.source_event =
            SourceEventRef{ledger.run_id, message.message_id, message.line_hash};
        const auto outcome_it = outcome.find(request_id);
        if (outcome_it != outcome.end()) {
            sample.request_outcome = outcome_it->second;
        }
        const std::string purpose_name = v3::MessagePurposeName(message.purpose);
        fill_request_side(sample, request_id, purpose_name);
        if (message.usage.has_value() && !message.usage->is_null()) {
            const api::Usage usage = UsageFromV3Owner(*message.usage);
            sample.usage = usage;
            sample.usage_source = UsageSource::ProviderReported;
            sample.total_input_tokens = api::TotalInputTokens(usage);
            sample.total_billed_shape_tokens =
                sample.total_input_tokens + usage.output_tokens;
            // 明报位(C2 口径):键在场(0 也算)= 该明细 provider 明报;
            // 省键 = usage 报了但该子项没拆账。
            sample.cache_read_reported_by_provider = message.usage->contains("cacheReadTokens");
            sample.cache_creation_reported_by_provider =
                message.usage->contains("cacheWriteTokens");
        } else {
            // 缺实报(§五:不补 0)——照投 unknown sample,coverage 靠它数。
            sample.usage_source = UsageSource::Unknown;
        }
        result.samples.push_back(std::move(sample));
    }

    // 实际发出却无 owner 的请求:失败/取消无消息、completed 而消息丢失、
    // 崩溃未收口——各照投 unknown sample,不冒充零消耗也不虚构数字。
    for (const auto& request_id : sent_order) {
        if (owned_requests.count(request_id) > 0) {
            continue;
        }
        UsageSample sample;
        sample.session_id = ledger.session_id;
        sample.run_id = ledger.run_id;
        sample.run_kind = context.run_kind;
        sample.request_id = request_id;
        sample.attempt = 1;
        sample.usage_source = UsageSource::Unknown;
        const auto outcome_it = outcome.find(request_id);
        if (outcome_it != outcome.end()) {
            sample.request_outcome = outcome_it->second;
            if (sample.request_outcome == "completed") {
                // completed 事件在、assistant 没落盘:消息丢失,点名。
                result.warnings.push_back("usage.v3_owner_missing: " + request_id);
                sample.incomplete_linkage = true;
            }
        }
        const auto prepared_it = prepared.find(request_id);
        std::string purpose_name;
        if (prepared_it != prepared.end() && prepared_it->second->contains("purpose") &&
            prepared_it->second->at("purpose").is_string()) {
            purpose_name = prepared_it->second->at("purpose").get<std::string>();
        }
        fill_request_side(sample, request_id, purpose_name);
        if (sample.provider.empty()) {
            sample.provider = "unknown";
        }
        if (sample.wire.empty()) {
            sample.wire = "unknown";
        }
        result.samples.push_back(std::move(sample));
    }

    result.ok = true;
    return result;
}

}  // namespace lubancode::accounting
