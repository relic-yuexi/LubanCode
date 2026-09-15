// v3_facts.hpp 的实现:WalkSessionTree -> 逐账折事实。纯读。
#include "insights/v3_facts.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

#include "api/types.hpp"
#include "accounting/usage_projector.hpp"  // UsageFromV3Owner(五项折算唯一口)
#include "platform/paths.hpp"
#include "trajectory/v3/envelope.hpp"

namespace lubancode::insights {
namespace {

std::string Trimmed(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    return std::string(text.substr(0, limit)) + "…";
}

// payload 键取字符串;缺键/类型不合给空(不猜)。
std::string StringOf(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

// payload 键取可选字符串;缺键/空串给 nullopt(缺与空不分开装)。
std::optional<std::string> OptionalStringOf(const nlohmann::json& payload, const char* key) {
    const std::string value = StringOf(payload, key);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

// 一份已验卷的账 -> V3SessionFacts(请求/工具/失败/缺口)。artifact 实探
// 以 jsonl 所在目录为该账 session 根(子账的 artifacts 在子账目录下)。
V3SessionFacts BuildSessionFacts(const trajectory::v3::V3Ledger& ledger,
                                 const std::filesystem::path& jsonl_path, bool is_subagent,
                                 const std::string& link_status) {
    const std::filesystem::path session_dir = jsonl_path.parent_path();
    V3SessionFacts facts;
    facts.session_id = ledger.session_id;
    facts.run_id = ledger.run_id;
    facts.jsonl_path = jsonl_path;
    facts.is_subagent = is_subagent;
    facts.link_status = link_status;

    if (const auto last = ledger.LastEntry(); last.has_value()) {
        facts.terminal_seq = last->seq;
        facts.terminal_hash = last->is_message ? ledger.messages[last->index].line_hash
                                               : ledger.events[last->index].line_hash;
    }

    // usage owner 预索引:requestId -> owner assistant(唯一 owner;同
    // requestId 多条定稿属账面异常,取最后一条并点名)。
    std::map<std::string, std::size_t> owner_index;
    for (std::size_t i = 0; i < ledger.messages.size(); ++i) {
        const auto& message = ledger.messages[i];
        if (message.role != trajectory::v3::MessageRole::Assistant ||
            !message.request_id.has_value() || message.request_id->empty()) {
            continue;
        }
        if (!message.usage.has_value() || message.usage->is_null()) {
            continue;  // usage:null = 缺实报,不补 0(schema §五)
        }
        const auto inserted = owner_index.emplace(*message.request_id, i);
        if (!inserted.second) {
            facts.notes.push_back("facts.duplicate_usage_owner: " + *message.request_id);
            inserted.first->second = i;
        }
    }

    // 请求事实:prepared 建条,后续事件族按 requestId 回填。
    std::map<std::string, std::size_t> request_index;
    for (const auto& event : ledger.events) {
        switch (event.kind) {
            case trajectory::v3::EventKindV3::ModelRequestPrepared: {
                V3RequestFacts request;
                request.session_id = facts.session_id;
                request.run_id = facts.run_id;
                request.request_id = event.request_id.value_or("");
                request.turn_id = event.turn_id.value_or("");
                request.step_id = event.step_id.value_or("");
                request.purpose = StringOf(event.payload, "purpose");
                if (request.purpose.empty()) {
                    request.purpose = "unknown";
                }
                request.event_id = event.event_id;
                request.seq = event.seq;
                if (const auto revision = event.payload.find("contextRevision");
                    revision != event.payload.end() && revision->is_number_unsigned()) {
                    request.context_revision = revision->get<std::uint64_t>();
                }
                request.system_message_ref = StringOf(event.payload, "systemMessageRef");
                if (const auto inputs = event.payload.find("inputMessageRefs");
                    inputs != event.payload.end() && inputs->is_array()) {
                    for (const auto& ref : *inputs) {
                        if (ref.is_string()) {
                            request.input_message_refs.push_back(ref.get<std::string>());
                        }
                    }
                }
                request.provider = OptionalStringOf(event.payload, "provider");
                request.wire = OptionalStringOf(event.payload, "wire");
                request.model = OptionalStringOf(event.payload, "model");
                // inputView(V3-REAL-06):实际发送视图指纹账;缺席如实缺。
                if (const auto view = event.payload.find("inputView");
                    view != event.payload.end() && view->is_object()) {
                    if (const auto count = view->find("messageCount");
                        count != view->end() && count->is_number_unsigned()) {
                        request.wire_message_count = count->get<std::uint64_t>();
                    }
                    if (const auto divergent = view->find("divergent");
                        divergent != view->end() && divergent->is_boolean()) {
                        request.wire_view_divergent = divergent->get<bool>();
                    }
                    if (const auto fp = view->find("systemFingerprint");
                        fp != view->end() && fp->is_string()) {
                        request.system_fingerprint = fp->get<std::string>();
                    }
                }
                if (const auto tools = event.payload.find("toolNames");
                    tools != event.payload.end() && tools->is_array()) {
                    request.tool_names_recorded = true;
                    for (const auto& name : *tools) {
                        if (name.is_string()) {
                            request.tool_names.push_back(name.get<std::string>());
                        }
                    }
                }
                request.has_token_estimate = event.payload.contains("tokenEstimateRef");
                // owner 关联(assistant 定稿可能落在 prepared 之后;这里先记
                // 索引,折完后统一回填——见下方第二轮)。
                const auto inserted =
                    request_index.emplace(request.request_id, facts.requests.size());
                if (inserted.second) {
                    facts.requests.push_back(std::move(request));
                } else {
                    // 同 requestId 二次 prepared:网络重试另有 requestId
                    //(schema §六),同 id 重复属账面异常,点名不改写首笔。
                    facts.notes.push_back("facts.duplicate_prepared: " + request.request_id);
                }
                break;
            }
            case trajectory::v3::EventKindV3::ModelRequestSent:
            case trajectory::v3::EventKindV3::ModelRequestFailed:
            case trajectory::v3::EventKindV3::ModelResponseCompleted:
            case trajectory::v3::EventKindV3::ModelResponseFailed:
            case trajectory::v3::EventKindV3::ModelResponseCancelled: {
                const auto it = request_index.find(event.request_id.value_or(""));
                if (it == request_index.end()) {
                    // 无 prepared 的终态(流式重放/旧段):不虚构请求条目,
                    // 点名即可。
                    facts.notes.push_back("facts.terminal_without_prepared: " +
                                          event.event_id);
                    break;
                }
                V3RequestFacts& request = facts.requests[it->second];
                switch (event.kind) {
                    case trajectory::v3::EventKindV3::ModelRequestSent:
                        request.sent = true;
                        break;
                    case trajectory::v3::EventKindV3::ModelRequestFailed:
                    case trajectory::v3::EventKindV3::ModelResponseFailed:
                    case trajectory::v3::EventKindV3::ModelResponseCancelled:
                    case trajectory::v3::EventKindV3::ModelResponseCompleted: {
                        const char* outcome = event.kind ==
                                                      trajectory::v3::EventKindV3::
                                                          ModelResponseCancelled
                                                  ? "cancelled"
                                                  : (event.kind ==
                                                              trajectory::v3::EventKindV3::
                                                                  ModelResponseCompleted
                                                          ? "completed"
                                                          : "failed");
                        // 首个终态生效(与流式唯一 assistant 定稿同口径);
                        // 迟到终态不改写,证据锚留在首行。
                        if (request.outcome.empty()) {
                            request.outcome = outcome;
                            request.outcome_event_id = event.event_id;
                            request.outcome_seq = event.seq;
                        }
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case trajectory::v3::EventKindV3::ModelUsageAppended: {
                // 迟到/失败/更正观察:不进 owner、不倒改旧消息(§五)。
                facts.notes.push_back("facts.usage_appended_observation: " +
                                      event.request_id.value_or(std::string("(no request)")));
                break;
            }
            case trajectory::v3::EventKindV3::SessionEnded: {
                facts.sealed = true;  // v3 封口唯一事实
                break;
            }
            default:
                break;
        }
    }

    // owner 回填(消息定稿可能先于/晚于各终态事件,单独一轮)。
    for (auto& request : facts.requests) {
        const auto owner = owner_index.find(request.request_id);
        if (owner == owner_index.end()) {
            continue;  // 无 owner:unknown,不拿 appended 顶上
        }
        const auto& message = ledger.messages[owner->second];
        const api::Usage usage = accounting::UsageFromV3Owner(*message.usage);
        request.usage_reported = true;
        request.usage_owner_message_id = message.message_id;
        request.input_tokens = usage.input_tokens;
        request.cache_read_tokens = usage.cache_read_tokens;
        request.cache_creation_tokens = usage.cache_creation_tokens;
        request.output_tokens = usage.output_tokens;
        request.reasoning_tokens = usage.output_reasoning_tokens;
    }

    // 工具折叠:reader 现成投影,不另写一套(§4.19)。
    for (auto& snapshot : trajectory::v3::FoldToolActions(ledger)) {
        V3ToolActionFacts action;
        action.session_id = facts.session_id;
        action.snapshot = std::move(snapshot);
        facts.tool_actions.push_back(std::move(action));
    }

    // 失败明细:tool.execution.failed(执行失败)与 tool.result.persist_failed
    //(落盘失败分开记,phase 区分)。reason 只留 80 字。
    for (const auto& event : ledger.events) {
        const char* phase = nullptr;
        if (event.kind == trajectory::v3::EventKindV3::ToolExecutionFailed) {
            phase = "execution";
        } else if (event.kind == trajectory::v3::EventKindV3::ToolResultPersistFailed) {
            phase = "persist";
        }
        if (phase == nullptr) {
            continue;
        }
        V3ToolFailureFacts failure;
        failure.session_id = facts.session_id;
        failure.action_id = event.action_id.value_or("");
        failure.turn_id = event.turn_id.value_or("");
        failure.event_id = event.event_id;
        failure.seq = event.seq;
        failure.phase = phase;
        // 失败文本:execution.failed 落 error_code 键,persist_failed 落
        // reason 键(载荷命名定案,§四"工具"条);两处都认,只取真有的一边。
        failure.reason = Trimmed(StringOf(event.payload, "reason"), 80);
        if (failure.reason.empty()) {
            failure.reason = Trimmed(StringOf(event.payload, "error_code"), 80);
        }
        if (const auto attempt = event.payload.find("attempt");
            attempt != event.payload.end() && attempt->is_number_unsigned()) {
            failure.attempt = attempt->get<std::uint64_t>();
        }
        // 工具名从折叠快照取(声明块 owner 在 assistant,信封不带)。
        for (const auto& action : facts.tool_actions) {
            if (action.snapshot.tool_call_id == failure.action_id &&
                action.snapshot.tool_name.has_value()) {
                failure.tool_name = *action.snapshot.tool_name;
                break;
            }
        }
        facts.tool_failures.push_back(std::move(failure));
    }

    // artifact 缺 blob:对 tool.result.persisted 的 result_ref[].path 做
    // 存在性实探(hash 全量校验归 ExpandResultPreview 深查;这里只探缺)。
    if (!session_dir.empty()) {
        for (const auto& event : ledger.events) {
            if (event.kind != trajectory::v3::EventKindV3::ToolResultPersisted) {
                continue;
            }
            const auto refs = event.payload.find("result_ref");
            if (refs == event.payload.end() || !refs->is_array()) {
                continue;
            }
            for (const auto& ref : *refs) {
                const auto path = ref.find("path");
                if (path == ref.end() || !path->is_string()) {
                    continue;
                }
                std::error_code ec;
                const std::filesystem::path artifact = session_dir / path->get<std::string>();
                if (!std::filesystem::is_regular_file(artifact, ec)) {
                    facts.notes.push_back("facts.missing_blob: " +
                                          event.action_id.value_or(std::string()) + " " +
                                          path->get<std::string>());
                }
            }
        }
    }
    return facts;
}

// 树内递归:主账在前,子账树序;cycle/missing/unreadable 各自点名
//(WalkSessionTree 已做环检测与 visited 去重,这里只转记)。
void CollectTree(const trajectory::v3::SubagentSessionNode& node, bool is_root,
                 V3FactsRead& result) {
    if (node.ledger.has_value()) {
        result.sessions.push_back(BuildSessionFacts(*node.ledger, node.jsonl_path, !is_root,
                                                    node.link_status));
        return;
    }
    if (!is_root) {
        result.warnings.push_back("facts.subsession_unreadable: " + node.session_id);
    }
}

void WalkChildren(const trajectory::v3::SubagentSessionNode& node, V3FactsRead& result) {
    for (const auto& child : node.children) {
        if (child.link_status == "cycle") {
            result.warnings.push_back("facts.subsession_cycle_dedup: " + child.session_id);
            continue;
        }
        if (child.link_status == "child_missing") {
            result.warnings.push_back("facts.subsession_missing: " + child.session_id);
            continue;
        }
        if (child.link_status == "unreadable") {
            result.warnings.push_back("facts.subsession_unreadable: " + child.session_id);
            continue;
        }
        if (child.link_status != "linked" && child.link_status != "spawned" &&
            child.link_status != "root") {
            // not_linked/spawn_failed 等异常关联:子账可读则照收(执行过就
            // 有真实事实),关联状态由 link_status 带走。
            result.warnings.push_back("facts.subsession_link_status: " + child.session_id +
                                      "(" + child.link_status + ")");
        }
        CollectTree(child, /*is_root=*/false, result);
        WalkChildren(child, result);
    }
}

}  // namespace

V3FactsRead CollectV3SessionFacts(const std::filesystem::path& session_dir,
                                  const std::filesystem::path& v3_stream) {
    V3FactsRead result;
    const auto tree = trajectory::v3::WalkSessionTree(v3_stream);
    if (!tree.ledger.has_value()) {
        // 错误详情再验一遍拿回来(坏账要人工修,双读一次可接受;与
        // session_usage_reader 同款)。
        const auto own = trajectory::v3::ReadV3Ledger(v3_stream);
        result.error_code = "facts.v3_ledger_unreadable";
        result.message = "v3 主账验卷不过: " + platform::PathToUtf8(v3_stream) +
                         " (" + (own.has_value() ? std::string("tree walk 失败")
                                                  : own.error()) +
                         ")";
        result.session_id = session_dir.filename().string();
        return result;
    }
    CollectTree(tree, /*is_root=*/true, result);
    WalkChildren(tree, result);
    if (result.sessions.empty()) {
        result.error_code = "facts.v3_ledger_empty";
        result.message = "v3 树没有可读账: " + platform::PathToUtf8(v3_stream);
        result.session_id = session_dir.filename().string();
        return result;
    }
    result.session_id = result.sessions.front().session_id;
    result.ok = true;
    return result;
}

}  // namespace lubancode::insights
