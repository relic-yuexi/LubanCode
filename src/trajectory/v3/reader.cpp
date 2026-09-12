// v3 读取侧实现(P2)。纯读:不开写柄、不调模型、不重跑工具、不发外部
// 消息(§5.1"只读 replay 零调用零重跑")。验卷复用 VerifyV3File。
#include "trajectory/v3/reader.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include "platform/sha256.hpp"

namespace lubancode::trajectory::v3 {

namespace {

// 逐行读原始文本(与 writer.cpp ReadRawLines 同规则;验卷已拒截断尾)。
std::optional<std::vector<std::string>> ReadRawLines(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < data.size()) {
        std::size_t end = data.find('\n', start);
        if (end == std::string::npos) {
            break;
        }
        if (end > start) {
            lines.emplace_back(data.substr(start, end - start));
        }
        start = end + 1;
    }
    return lines;
}

std::optional<std::string> JsonString(const nlohmann::json& json, const char* key) {
    auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// 引用取 id:同会话 string;跨会话五键对象取 id 字段(§3.1)。
std::optional<std::string> RefId(const nlohmann::json& ref) {
    if (ref.is_string()) {
        return ref.get<std::string>();
    }
    if (ref.is_object()) {
        return JsonString(ref, "id");
    }
    return std::nullopt;
}

std::vector<std::string> RefIdArray(const nlohmann::json& payload, const char* key) {
    std::vector<std::string> ids;
    auto it = payload.find(key);
    if (it == payload.end() || !it->is_array()) {
        return ids;
    }
    for (const auto& ref : *it) {
        if (auto id = RefId(ref)) {
            ids.push_back(std::move(*id));
        }
    }
    return ids;
}

std::optional<std::uint64_t> JsonUint(const nlohmann::json& json, const char* key) {
    auto it = json.find(key);
    if (it == json.end() || !it->is_number_unsigned()) {
        return std::nullopt;
    }
    return it->get<std::uint64_t>();
}

// message 本体取 role(schema 保证合法;防御性给 user 兜底)。
MessageRole RoleOf(const MessageLine& line) {
    if (auto role = JsonString(line.message, "role")) {
        if (auto parsed = MessageRoleFromName(*role)) {
            return *parsed;
        }
    }
    return MessageRole::User;
}

// 压缩标记(applied → 视图)。
CompactMarkerView MakeCompactMarker(const EventLine& event) {
    CompactMarkerView view;
    view.event_id = event.event_id;
    view.seq = event.seq;
    view.timestamp = event.timestamp;
    view.compact_id = event.compact_id.value_or("");
    view.trigger = JsonString(event.payload, "trigger").value_or("");
    view.summary_message_ref = JsonString(event.payload, "summaryMessageRef").value_or("");
    view.validation_event_ref = JsonString(event.payload, "validationEventRef").value_or("");
    view.step_scope = event.payload.value("stepScope", nlohmann::json::object());
    view.removed_message_refs = RefIdArray(event.payload, "removedMessageRefs");
    view.retained_message_refs = RefIdArray(event.payload, "retainedMessageRefs");
    if (auto turns = event.payload.find("protectedTurnIds");
        turns != event.payload.end() && turns->is_array()) {
        for (const auto& turn : *turns) {
            if (turn.is_string()) {
                view.protected_turn_ids.push_back(turn.get<std::string>());
            }
        }
    }
    view.context_tokens_before = JsonUint(event.payload, "contextTokensBefore").value_or(0);
    view.context_tokens_after = JsonUint(event.payload, "contextTokensAfter").value_or(0);
    view.source_revision = JsonUint(event.payload, "sourceContextRevision").value_or(0);
    view.new_revision = JsonUint(event.payload, "newContextRevision").value_or(0);
    if (auto metric = event.payload.find("tokenMetric");
        metric != event.payload.end() && !metric->is_null()) {
        view.token_metric = metric->dump();
    }
    return view;
}

}  // namespace

// ---------------------------------------------------------------------------
// V3Ledger
// ---------------------------------------------------------------------------

const MessageLine* V3Ledger::FindMessage(std::string_view id) const {
    auto it = message_index.find(std::string(id));
    return it == message_index.end() ? nullptr : &messages[it->second];
}

const EventLine* V3Ledger::FindEvent(std::string_view id) const {
    auto it = event_index.find(std::string(id));
    return it == event_index.end() ? nullptr : &events[it->second];
}

std::optional<V3Ledger::Entry> V3Ledger::LastEntry() const {
    if (timeline.empty()) {
        return std::nullopt;
    }
    return timeline.back();
}

std::expected<V3Ledger, std::string> ReadV3Ledger(const std::filesystem::path& jsonl) {
    V3VerifyReport report = VerifyV3File(jsonl);
    if (!report.ok) {
        return std::unexpected("v3reader.verify_failed: " + report.error_code + " " + report.message);
    }
    auto raw = ReadRawLines(jsonl);
    if (!raw.has_value()) {
        return std::unexpected("v3reader.open_failed: " + jsonl.string());
    }
    V3Ledger ledger;
    ledger.path = jsonl;
    ledger.lines = report.lines;
    ledger.context = std::move(report.context);
    std::string error_code, error_message;
    for (const auto& line : *raw) {
        auto json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_discarded()) {
            return std::unexpected("v3reader.bad_json");
        }
        if (ledger.session_id.empty()) {
            ledger.session_id = json.value("sessionId", "");
            ledger.run_id = json.value("runId", "");
        }
        if (json.at("type").get<std::string>() == "message") {
            auto parsed = MessageLine::FromJsonStrict(json, &error_code, &error_message);
            if (!parsed.has_value()) {
                return std::unexpected("v3reader.parse_failed: " + error_code + " " + error_message);
            }
            V3Ledger::Entry entry{parsed->seq, true, ledger.messages.size()};
            ledger.message_index.emplace(parsed->message_id, ledger.messages.size());
            ledger.messages.push_back(std::move(*parsed));
            ledger.timeline.push_back(entry);
        } else {
            auto parsed = EventLine::FromJsonStrict(json, &error_code, &error_message);
            if (!parsed.has_value()) {
                return std::unexpected("v3reader.parse_failed: " + error_code + " " + error_message);
            }
            V3Ledger::Entry entry{parsed->seq, false, ledger.events.size()};
            ledger.event_index.emplace(parsed->event_id, ledger.events.size());
            ledger.events.push_back(std::move(*parsed));
            ledger.timeline.push_back(entry);
        }
    }
    // 每版链历史:重放提交事件,revision 前进即快照(单次请求输入回放对表)。
    ContextView view;
    std::uint64_t last_revision = 0;
    for (const auto& event : ledger.events) {
        std::string error = ApplyCommitEventToView(view, event);
        if (!error.empty()) {
            return std::unexpected("v3reader.replay_failed: " + error);
        }
        if (view.revision != last_revision && !view.chain.empty()) {
            std::vector<std::string> inputs;
            inputs.reserve(view.chain.size() - 1);
            for (std::size_t i = 1; i < view.chain.size(); ++i) {
                inputs.push_back(view.chain[i].message_ref);
            }
            ledger.revision_chains.emplace(
                view.revision, std::make_pair(view.system_message_ref, std::move(inputs)));
            last_revision = view.revision;
        }
    }
    // A summary candidate does not become main input until its selected tool
    // message is admitted. Reject broken adopted provenance on resume, rather
    // than silently treating the candidate as an ordinary tool preview.
    for (const auto& message : ledger.messages) {
        if (!message.result_selection_ref) continue;
        const auto* selected = ledger.FindEvent(*message.result_selection_ref);
        if (selected && selected->payload.contains("summaryEventRef")) {
            const auto preview = ExpandResultPreview(ledger, {}, message.message_id);
            if (!preview.summary_valid) return std::unexpected("v3reader.invalid_action_summary_selection");
        }
    }
    return ledger;
}

// ---------------------------------------------------------------------------
// 投影一:历史时间线
// ---------------------------------------------------------------------------

HistoryTimeline ProjectHistoryTimeline(const V3Ledger& ledger) {
    HistoryTimeline timeline;
    // 当前链成员。
    std::unordered_set<std::string> in_chain;
    for (const auto& node : ledger.context.chain) {
        in_chain.insert(node.message_ref);
    }
    // 被降档替换的原版:派生消息的 sourceToolMessageRef 指到的原消息退链仍在档。
    std::unordered_set<std::string> replaced;
    for (const auto& message : ledger.messages) {
        if (message.source_tool_message_ref.has_value()) {
            replaced.insert(*message.source_tool_message_ref);
        }
    }
    // 历次压缩移出上下文的消息 → compactId 列表。
    std::unordered_map<std::string, std::vector<std::string>> removed_by;
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::CompactApplied) {
            continue;
        }
        const std::string compact_id = event.compact_id.value_or("");
        for (const auto& ref : RefIdArray(event.payload, "removedMessageRefs")) {
            removed_by[ref].push_back(compact_id);
        }
    }
    // system.change → 随后的 context.system.applied(完成与否)。
    auto switch_followup = [&](const EventLine& change) {
        std::pair<std::string, bool> result{"", false};  // 新根, 是否完成
        for (const auto& event : ledger.events) {
            if (event.seq <= change.seq ||
                event.kind != EventKindV3::ContextSystemApplied) {
                continue;
            }
            result.first = JsonString(event.payload, "rootMessageRef").value_or("");
            result.second = true;
            break;
        }
        return result;
    };

    timeline.items.reserve(ledger.timeline.size());
    for (const auto& entry : ledger.timeline) {
        HistoryTimeline::Item item;
        item.seq = entry.seq;
        if (entry.is_message) {
            const MessageLine& line = ledger.messages[entry.index];
            item.kind = HistoryTimeline::Item::Kind::Message;
            item.id = line.message_id;
            item.timestamp = line.timestamp;
            item.message.seq = line.seq;
            item.message.message_id = line.message_id;
            item.message.timestamp = line.timestamp;
            item.message.role = RoleOf(line);
            item.message.purpose = line.purpose;
            item.message.origin = line.origin;
            item.message.display = line.display.value_or(DisplayMode::Visible);
            item.message.completion_status = line.completion_status;
            item.message.usage = line.usage;
            item.message.provider = line.provider;
            item.message.model = line.model;
            item.message.in_current_context = in_chain.count(line.message_id) > 0;
            item.message.replaced_by_derivation = replaced.count(line.message_id) > 0;
            if (line.source_tool_message_ref.has_value()) {
                item.message.derived_from = *line.source_tool_message_ref;
            }
            if (auto it = removed_by.find(line.message_id); it != removed_by.end()) {
                item.message.removed_by_compacts = it->second;
            }
            timeline.message_items.emplace(line.message_id, timeline.items.size());
        } else {
            const EventLine& line = ledger.events[entry.index];
            item.id = line.event_id;
            item.timestamp = line.timestamp;
            switch (line.kind) {
                case EventKindV3::CompactApplied:
                    item.kind = HistoryTimeline::Item::Kind::CompactMarker;
                    item.compact = MakeCompactMarker(line);
                    timeline.compact_items.emplace(item.compact.compact_id,
                                                   timeline.items.size());
                    break;
                case EventKindV3::SystemChange: {
                    item.kind = HistoryTimeline::Item::Kind::SystemSwitch;
                    item.system_switch.event_id = line.event_id;
                    item.system_switch.seq = line.seq;
                    item.system_switch.timestamp = line.timestamp;
                    item.system_switch.cause = JsonString(line.payload, "cause").value_or("");
                    if (auto changed = line.payload.find("systemChanged");
                        changed != line.payload.end() && changed->is_boolean()) {
                        item.system_switch.system_changed = changed->get<bool>();
                    }
                    item.system_switch.old_system_ref =
                        JsonString(line.payload, "oldSystemMessageRef").value_or("");
                    auto followup = switch_followup(line);
                    item.system_switch.new_system_ref = followup.first;
                    item.system_switch.completed = followup.second;
                    break;
                }
                case EventKindV3::ContextToolPreviewsReduced:
                    item.kind = HistoryTimeline::Item::Kind::PreviewReduction;
                    item.event_kind_name = EventKindV3Name(line.kind);
                    break;
                default:
                    item.kind = HistoryTimeline::Item::Kind::Event;
                    item.event_kind_name = EventKindV3Name(line.kind);
                    break;
            }
        }
        timeline.items.push_back(std::move(item));
    }
    return timeline;
}

// ---------------------------------------------------------------------------
// 投影二:当前模型上下文
// ---------------------------------------------------------------------------

ModelContext ProjectModelContext(const V3Ledger& ledger) {
    ModelContext context;
    context.context_id = ledger.context.context_id;
    context.revision = ledger.context.revision;
    context.preview_budget_bytes = ledger.context.preview_budget_bytes;
    context.open_compact_ids = ledger.context.open_compact_ids;
    if (ledger.context.chain.empty()) {
        context.missing_refs.push_back("<empty-chain>");
        return context;
    }
    const std::string& root = ledger.context.chain.front().message_ref;
    context.system_message_id = root;
    if (const MessageLine* system = ledger.FindMessage(root)) {
        if (auto content = JsonString(system->message, "content")) {
            context.system_content = *content;
        }
    } else {
        context.missing_refs.push_back(root);
    }
    // 同一 actionId 在链上只许一个预览版本(§4.38)。
    std::unordered_map<std::string, int> action_count;
    for (std::size_t i = 1; i < ledger.context.chain.size(); ++i) {
        const std::string& id = ledger.context.chain[i].message_ref;
        const MessageLine* line = ledger.FindMessage(id);
        if (line == nullptr) {
            context.missing_refs.push_back(id);
            continue;
        }
        ModelContextMessage input;
        input.message_id = id;
        input.role = RoleOf(*line);
        input.purpose = line->purpose;
        input.message = line->message;
        input.derived_preview = line->source_tool_message_ref.has_value();
        if (line->action_id.has_value() && RoleOf(*line) == MessageRole::Tool) {
            if (++action_count[*line->action_id] > 1) {
                context.duplicate_action_versions.push_back(*line->action_id);
            }
        }
        context.inputs.push_back(std::move(input));
    }
    return context;
}

std::string CheckPreparedAgainstChain(const V3Ledger& ledger, std::string_view prepared_event_id) {
    const EventLine* event = ledger.FindEvent(prepared_event_id);
    if (event == nullptr || event->kind != EventKindV3::ModelRequestPrepared) {
        return "v3reader.not_prepared: " + std::string(prepared_event_id);
    }
    auto revision = JsonUint(event->payload, "contextRevision");
    std::vector<std::string> inputs = RefIdArray(event->payload, "inputMessageRefs");
    const std::string purpose = JsonString(event->payload, "purpose").value_or("");
    if (purpose != "conversation") {
        // compact 等内部请求不走主链:只验引用都能落在账上(§4.4
        //"空数组、缺正文、找不到的引用不能用从历史猜一份填补")。
        for (const auto& id : inputs) {
            if (ledger.FindMessage(id) == nullptr) {
                return "v3reader.prepared_dangling: " + id;
            }
        }
        return "";
    }
    if (!revision.has_value()) {
        return "v3reader.prepared_missing_revision";
    }
    auto it = ledger.revision_chains.find(*revision);
    if (it == ledger.revision_chains.end()) {
        return "v3reader.prepared_revision_unknown: " + std::to_string(*revision);
    }
    const std::string& prepared_system = JsonString(event->payload, "systemMessageRef").value_or("");
    if (prepared_system != it->second.first) {
        return "v3reader.prepared_system_mismatch: " + prepared_system + " != " + it->second.first;
    }
    if (inputs != it->second.second) {
        return "v3reader.prepared_inputs_mismatch: 链投影与 inputMessageRefs 不一致";
    }
    return "";
}

// ---------------------------------------------------------------------------
// 按 actionId 折叠的工具快照(§4.19)
// ---------------------------------------------------------------------------

namespace {

// 事件载荷里取 attempt(缺省 1,正整数)。
std::uint64_t PayloadAttempt(const EventLine& event) {
    return JsonUint(event.payload, "attempt").value_or(1);
}

}  // namespace

std::vector<ToolActionSnapshot> FoldToolActions(const V3Ledger& ledger) {
    std::map<std::string, ToolActionSnapshot> folded;
    auto attempt_of = [](ToolActionSnapshot& snapshot, std::uint64_t number) -> ToolAttemptView* {
        for (auto& attempt : snapshot.attempts) {
            if (attempt.attempt == number) {
                return &attempt;
            }
        }
        ToolAttemptView view;
        view.attempt = number;
        view.status = "pending";  // 未见过生命周期事件的兜底
        snapshot.attempts.push_back(std::move(view));
        return &snapshot.attempts.back();
    };
    for (const auto& event : ledger.events) {
        const bool tool_event =
            event.action_id.has_value() &&
            std::string_view(EventKindV3Name(event.kind)).substr(0, 5) == "tool.";
        if (!tool_event) {
            continue;
        }
        ToolActionSnapshot& snapshot = folded[*event.action_id];
        snapshot.tool_call_id = *event.action_id;
        if (snapshot.turn_id.empty() && event.turn_id.has_value()) {
            snapshot.turn_id = *event.turn_id;
        }
        if (snapshot.step_id.empty() && event.step_id.has_value()) {
            snapshot.step_id = *event.step_id;
        }
        ToolAttemptView* attempt = attempt_of(snapshot, PayloadAttempt(event));
        attempt->event_ids.push_back(event.event_id);
        using K = EventKindV3;
        switch (event.kind) {
            case K::ToolExecutionPending:
                attempt->status = "pending";
                attempt->pending_reason = JsonString(event.payload, "reason").value_or("");
                if (auto ref = JsonString(event.payload, "assistantMessageRef")) {
                    snapshot.assistant_message_ref = *ref;
                }
                // fixture 双写期:writer 落 provider_tool_call_id,早期 fixture
                // 用 providerToolCallId——读取两侧都认。
                snapshot.provider_tool_call_id =
                    JsonString(event.payload, "provider_tool_call_id")
                        .value_or(JsonString(event.payload, "providerToolCallId").value_or(""));
                break;
            case K::ToolExecutionStarted:
                attempt->started = true;
                attempt->waiting = false;
                attempt->status = "running";
                attempt->effective_args_ref = JsonString(event.payload, "effectiveArgsRef");
                attempt->idempotency_key = JsonString(event.payload, "idempotencyKey");
                break;
            case K::ToolExecutionWaiting:
                attempt->waiting = true;
                attempt->status = "pending";
                attempt->waiting_reason = JsonString(event.payload, "reason").value_or("");
                break;
            case K::ToolExecutionResumed:
                attempt->waiting = false;
                attempt->status = "running";
                break;
            case K::ToolExecutionFinished:
                attempt->status = "done";
                if (auto it = event.payload.find("exit_code");
                    it != event.payload.end() && it->is_number_integer()) {
                    attempt->exit_code = it->get<std::int64_t>();
                }
                attempt->execution_duration_ms = JsonUint(event.payload, "executionDurationMs");
                break;
            case K::ToolExecutionFailed:
                attempt->status = "failed";
                break;
            case K::ToolExecutionCancelled:
                attempt->status = "cancelled";
                break;
            case K::ToolExecutionRejected:
                attempt->status = "rejected";
                break;
            case K::ToolExecutionUnknown:
                attempt->status = "unknown";
                break;
            case K::ToolResultPersisted:
                snapshot.persisted_event_refs.push_back(event.event_id);
                {
                    std::vector<nlohmann::json> refs;
                    if (auto it = event.payload.find("result_ref");
                        it != event.payload.end() && it->is_array()) {
                        refs = it->get<std::vector<nlohmann::json>>();
                    }
                    snapshot.result_refs.push_back(std::move(refs));
                }
                break;
            case K::ToolResultPersistFailed:
                snapshot.persist_failed_reason = JsonString(event.payload, "reason").value_or("");
                break;
            case K::ToolResultSelected:
                snapshot.selected_event_ref = event.event_id;
                snapshot.effective_outcome = JsonString(event.payload, "effectiveOutcome").value_or("");
                break;
            default:
                break;
        }
    }
    // 声明块:assistant 消息的 tool_calls 里按 provider 号配对(§4.15:
    // 宿主建立本地 ID 与 provider ID 映射;原始 args 的 owner 是调用块)。
    for (auto& [action_id, snapshot] : folded) {
        if (!snapshot.assistant_message_ref.has_value()) {
            continue;
        }
        const MessageLine* declaration = ledger.FindMessage(*snapshot.assistant_message_ref);
        if (declaration == nullptr) {
            continue;
        }
        auto calls = declaration->message.find("tool_calls");
        if (calls == declaration->message.end() || !calls->is_array()) {
            continue;
        }
        for (const auto& call : *calls) {
            const auto function = call.find("function");
            if (function == call.end()) {
                continue;
            }
            if (snapshot.provider_tool_call_id.has_value() &&
                JsonString(call, "id") != snapshot.provider_tool_call_id) {
                continue;
            }
            if (auto name = JsonString(*function, "name")) {
                snapshot.tool_name = *name;
            }
            if (auto args = function->find("arguments");
                args != function->end() && args->is_string()) {
                auto parsed = nlohmann::json::parse(args->get<std::string>(), nullptr, false);
                snapshot.declared_args = parsed.is_discarded()
                                             ? nlohmann::json(args->get<std::string>())
                                             : std::move(parsed);
            }
            break;
        }
    }
    // 消息版本(原版 + 降档派生,seq 序)+ 整体折叠。
    std::unordered_set<std::string> in_chain;
    for (const auto& node : ledger.context.chain) {
        in_chain.insert(node.message_ref);
    }
    // compact 正常移链的消息(与 ProjectHistoryTimeline 的 removed_by 同
    // 口径):退出模型上下文是既定事实,不算接纳缺口。
    std::unordered_set<std::string> removed_by_compact;
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::CompactApplied) {
            continue;
        }
        for (const auto& ref : RefIdArray(event.payload, "removedMessageRefs")) {
            removed_by_compact.insert(ref);
        }
    }
    for (auto& [action_id, snapshot] : folded) {
        for (const auto& message : ledger.messages) {
            if (message.action_id != action_id || RoleOf(message) != MessageRole::Tool) {
                continue;
            }
            ToolActionSnapshot::MessageVersion version;
            version.message_id = message.message_id;
            version.on_current_chain = in_chain.count(message.message_id) > 0;
            if (message.source_tool_message_ref.has_value()) {
                version.source_tool_message_ref = *message.source_tool_message_ref;
            }
            snapshot.message_versions.push_back(std::move(version));
        }
        // 折叠(§4.14:尝试链 + 结果持久 + 消息提交共同决定)。
        std::string status = "declared";  // 只有声明/消息,无执行事件
        if (!snapshot.attempts.empty()) {
            ToolAttemptView& last = snapshot.attempts.back();
            if (last.status == "running") {
                // started 无终态:恢复投影标 unknown(§4.20),只读不合成假终态。
                status = "unknown";
                last.unknown_recovery = true;
            } else {
                status = last.status;  // pending/done/failed/cancelled/rejected/unknown
            }
        }
        const bool has_message = !snapshot.message_versions.empty();
        if ((status == "done" || status == "failed") && !has_message) {
            // 执行已有终态、模型侧却没有可见 tool 消息(失败与恢复单
            // P1-A/FA-01):恢复投影必须列出结果缺口——执行终态保留,补
            // 保存/补接纳,不得重跑工具。已选用(selected 落稳)缺消息是
            // §4.59 的 selected_no_message;连选用都没有则是整条结果链
            // 没立起来(仓打不开/persist 失败后崩溃),另立 result_missing。
            status = snapshot.selected_event_ref.has_value() ? "selected_no_message" : "result_missing";
        } else if ((status == "done" || status == "failed") && has_message) {
            // 消息已写、接纳未成(失败与恢复单 P1-A):本 action 名下没有任何
            // tool 消息在当前链上(降档派生会顶上、compact 移链是既定事实,
            // 都不算缺口),不得冒充有效上下文,按提交链补接纳。
            bool any_covered = false;
            for (const auto& version : snapshot.message_versions) {
                if (version.on_current_chain || removed_by_compact.count(version.message_id) > 0) {
                    any_covered = true;
                    break;
                }
            }
            if (!any_covered) {
                status = "message_not_admitted";
            }
        }
        snapshot.folded_status = status;
    }
    std::vector<ToolActionSnapshot> result;
    result.reserve(folded.size());
    for (auto& [action_id, snapshot] : folded) {
        result.push_back(std::move(snapshot));
    }
    return result;
}

const ToolActionSnapshot* FindActionSnapshot(const std::vector<ToolActionSnapshot>& snapshots,
                                             std::string_view action_id) {
    for (const auto& snapshot : snapshots) {
        if (snapshot.tool_call_id == action_id) {
            return &snapshot;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// hook dispatch 折叠(LuaHook 单 P0-B):hook.* 事件 → 恢复视图。纯读。
// ---------------------------------------------------------------------------

std::vector<HookDispatchView> FoldHookDispatches(const V3Ledger& ledger) {
    // dispatch_id -> 视图;started 序即 invocations 序。map 保落盘首见序稳定。
    std::map<std::string, HookDispatchView> folded;
    // (dispatch_id, invocation_id) -> invocations 下标。
    std::map<std::pair<std::string, std::string>, std::size_t> invocation_index;

    const auto dispatch_of = [&](const EventLine& event) -> HookDispatchView* {
        if (!event.hook_dispatch_id.has_value()) {
            return nullptr;
        }
        auto it = folded.find(*event.hook_dispatch_id);
        return it == folded.end() ? nullptr : &it->second;
    };
    const auto invocation_of = [&](const EventLine& event) -> HookInvocationView* {
        HookDispatchView* dispatch = dispatch_of(event);
        if (dispatch == nullptr) {
            return nullptr;
        }
        const auto it = event.payload.find("hookInvocationId");
        if (it == event.payload.end() || !it->is_string()) {
            return nullptr;
        }
        const auto found = invocation_index.find({*event.hook_dispatch_id, it->get<std::string>()});
        return found == invocation_index.end() ? nullptr
                                               : &dispatch->invocations[found->second];
    };

    for (const EventLine& event : ledger.events) {
        using K = EventKindV3;
        if (event.kind == K::HookDispatchRequested) {
            if (!event.hook_dispatch_id.has_value()) {
                continue;
            }
            HookDispatchView& view = folded[*event.hook_dispatch_id];
            view.dispatch_id = *event.hook_dispatch_id;
            view.requested = true;
            view.turn_id = event.turn_id;
            view.step_id = event.step_id;
            view.action_id = event.action_id;
            view.request_id = event.request_id;
            if (auto point = JsonString(event.payload, "hookPoint")) {
                view.hook_point = *point;
            }
            if (const auto matched = event.payload.find("matchedHandlers");
                matched != event.payload.end() && matched->is_array()) {
                for (const auto& handler : *matched) {
                    HookHandlerSpec spec;
                    if (auto value = JsonString(handler, "hookId")) {
                        spec.hook_id = *value;
                    }
                    if (auto value = JsonString(handler, "definitionHash")) {
                        spec.definition_hash = *value;
                    }
                    if (auto value = JsonString(handler, "handlerKind")) {
                        spec.handler_kind = *value;
                    }
                    if (const auto order = handler.find("definitionOrder");
                        order != handler.end() && order->is_number_integer()) {
                        spec.definition_order = order->get<int>();
                    }
                    if (auto value = JsonString(handler, "failurePolicy")) {
                        spec.failure_policy = *value;
                    }
                    view.matched_handlers.push_back(std::move(spec));
                }
            }
            continue;
        }
        if (event.kind == K::HookSkipped) {
            if (event.hook_dispatch_id.has_value()) {
                HookDispatchView& view = folded[*event.hook_dispatch_id];
                view.dispatch_id = *event.hook_dispatch_id;
                view.skipped = true;
                if (auto point = JsonString(event.payload, "hookPoint")) {
                    view.hook_point = *point;
                }
                if (auto reason = JsonString(event.payload, "reason")) {
                    view.skip_reason = *reason;
                }
                view.turn_id = event.turn_id;
                view.step_id = event.step_id;
                view.action_id = event.action_id;
            }
            continue;
        }
        if (event.kind == K::HookStarted) {
            if (!event.hook_dispatch_id.has_value()) {
                continue;
            }
            HookDispatchView& view = folded[*event.hook_dispatch_id];
            HookInvocationView invocation;
            if (const auto id = event.payload.find("hookInvocationId");
                id != event.payload.end() && id->is_string()) {
                invocation.invocation_id = id->get<std::string>();
            }
            if (auto value = JsonString(event.payload, "hookId")) {
                invocation.hook_id = *value;
            }
            if (auto value = JsonString(event.payload, "handlerKind")) {
                invocation.handler_kind = *value;
            }
            if (auto value = JsonString(event.payload, "definitionHash")) {
                invocation.definition_hash = *value;
            }
            if (const auto order = event.payload.find("definitionOrder");
                order != event.payload.end() && order->is_number_integer()) {
                invocation.definition_order = order->get<int>();
            }
            invocation.status = "running";
            if (view.dispatch_id.empty()) {
                // started 先于 requested 落账(异常序):dispatch 骨架现造。
                view.dispatch_id = *event.hook_dispatch_id;
                view.requested = true;
            }
            invocation_index[{*event.hook_dispatch_id, invocation.invocation_id}] =
                view.invocations.size();
            view.invocations.push_back(std::move(invocation));
            continue;
        }
        if (HookInvocationView* invocation = invocation_of(event);
            invocation != nullptr && invocation->status == "running") {
            switch (event.kind) {
                case K::HookCompleted:
                    invocation->status = "completed";
                    invocation->terminal_event_id = event.event_id;
                    if (auto value = JsonString(event.payload, "decision")) {
                        invocation->decision = *value;
                    }
                    break;
                case K::HookFailed:
                    invocation->status = "failed";
                    invocation->terminal_event_id = event.event_id;
                    break;
                case K::HookCancelled:
                    invocation->status = "cancelled";
                    invocation->terminal_event_id = event.event_id;
                    break;
                case K::HookUnknown:
                    invocation->status = "unknown";
                    invocation->terminal_event_id = event.event_id;
                    break;
                default:
                    break;
            }
        }
        if (HookInvocationView* invocation = invocation_of(event); invocation != nullptr) {
            switch (event.kind) {
                case K::HookEffectsApplied:
                case K::HookEffectsRejected: {
                    HookEffectView effect;
                    effect.applied = event.kind == K::HookEffectsApplied;
                    if (auto value = JsonString(event.payload, "effectType")) {
                        effect.effect_type = *value;
                    }
                    if (auto value = JsonString(event.payload, "reason")) {
                        effect.reason = *value;
                    }
                    if (const auto applied = event.payload.find("appliedValueRef");
                        applied != event.payload.end()) {
                        effect.applied_value = *applied;
                    }
                    effect.event_id = event.event_id;
                    invocation->effects.push_back(std::move(effect));
                    break;
                }
                case K::HookOutputProposed: {
                    std::string phase;
                    nlohmann::json candidate;
                    if (auto value = JsonString(event.payload, "phase")) {
                        phase = *value;
                    }
                    if (const auto it = event.payload.find("candidate"); it != event.payload.end()) {
                        candidate = *it;
                    }
                    invocation->outputs_proposed.emplace_back(std::move(phase), std::move(candidate));
                    break;
                }
                case K::HookContinuationConsumed:
                    invocation->continuation_consumed = true;
                    break;
                default:
                    break;
            }
        }
    }

    // 折叠 dispatch 状态 + 恢复材料(工作版本/已采用 appends)。
    for (auto& [dispatch_id, view] : folded) {
        (void)dispatch_id;
        if (view.skipped) {
            view.folded_status = "skipped";
            continue;
        }
        bool any_running = false;
        bool any_failed = false;
        bool any_cancelled = false;
        bool any_unknown = false;
        bool any_denied = false;
        bool all_completed = true;
        for (const HookInvocationView& invocation : view.invocations) {
            if (invocation.status == "running") {
                any_running = true;
                all_completed = false;
            }
            if (invocation.status == "failed") {
                any_failed = true;
                all_completed = false;
            }
            if (invocation.status == "cancelled") {
                any_cancelled = true;
                all_completed = false;
            }
            if (invocation.status == "unknown") {
                any_unknown = true;
                all_completed = false;
            }
            if (invocation.decision.has_value() && *invocation.decision == "deny") {
                any_denied = true;
            }
        }
        if (view.invocations.empty()) {
            // requested 而无 started:输入已排队、hook 未 started(§7.3 行 1)。
            view.folded_status = "requested";
        } else if (any_running) {
            view.folded_status = "running";
        } else if (any_unknown) {
            view.folded_status = "unknown";
        } else if (any_failed) {
            view.folded_status = "failed";
        } else if (any_cancelled) {
            view.folded_status = "cancelled";
        } else if (any_denied) {
            view.folded_status = "denied";
        } else if (all_completed) {
            view.folded_status = "completed";
        } else {
            view.folded_status = "running";
        }
        // 工作版本:事件序里最后一枚 applied 的 input.rewrite 值。
        for (const HookInvocationView& invocation : view.invocations) {
            for (const HookEffectView& effect : invocation.effects) {
                if (effect.applied && effect.effect_type == "input.rewrite" &&
                    effect.applied_value.is_object()) {
                    if (const auto prompt = effect.applied_value.find("prompt");
                        prompt != effect.applied_value.end()) {
                        view.adopted_working_input = effect.applied_value;
                        view.has_adopted_working_input = true;
                    }
                }
                if (effect.applied && effect.effect_type == "context.append") {
                    if (const auto text = effect.applied_value.find("text");
                        text != effect.applied_value.end() && text->is_string()) {
                        view.adopted_context_appends.push_back(text->get<std::string>());
                    }
                }
            }
        }
    }

    std::vector<HookDispatchView> result;
    result.reserve(folded.size());
    for (auto& [dispatch_id, view] : folded) {
        result.push_back(std::move(view));
    }
    return result;
}

const HookDispatchView* FindHookDispatch(const std::vector<HookDispatchView>& dispatches,
                                         std::string_view dispatch_id) {
    for (const auto& dispatch : dispatches) {
        if (dispatch.dispatch_id == dispatch_id) {
            return &dispatch;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// result_preview 读取投影(§4.18)
// ---------------------------------------------------------------------------

ResultPreviewProjection ExpandResultPreview(const V3Ledger& ledger,
                                            const std::filesystem::path& session_dir,
                                            std::string_view tool_message_id) {
    ResultPreviewProjection projection;
    projection.tool_message_id = tool_message_id;
    const MessageLine* message = ledger.FindMessage(tool_message_id);
    if (message == nullptr) {
        projection.complete = false;
        projection.result_preview = "<message-not-found>";
        return projection;
    }
    if (auto content = JsonString(message->message, "content")) {
        projection.result_preview = *content;  // 读取投影 = tool 消息正文(§4.18)
    }
    // 选用链:tool 消息 → resultSelectionRef → selected → persisted。
    std::vector<std::string> persisted_refs;
    if (message->result_selection_ref.has_value()) {
        projection.result_selection_ref = *message->result_selection_ref;
        if (const EventLine* selected = ledger.FindEvent(*message->result_selection_ref)) {
            projection.source_result_event_refs = RefIdArray(selected->payload, "sourceResultEventRefs");
            persisted_refs = projection.source_result_event_refs;
            projection.summary_event_ref = JsonString(selected->payload, "summaryEventRef").value_or("");
            if (!projection.summary_event_ref.empty()) {
                // A later explicit preview reduction can derive a shorter tool
                // message while retaining the original summary selection. Verify
                // that selection against its original adopted body, not the new
                // body authorized by context.tool_previews.reduced.
                const MessageLine* selected_body = message;
                std::unordered_set<std::string> visited;
                while (selected_body && selected_body->source_tool_message_ref) {
                    if (!visited.insert(selected_body->message_id).second) { selected_body = nullptr; break; }
                    selected_body = ledger.FindMessage(*selected_body->source_tool_message_ref);
                }
                const auto selected_text = selected_body ? JsonString(selected_body->message, "content").value_or("") : "";
                const auto* summary = ledger.FindEvent(projection.summary_event_ref);
                if (summary == nullptr || summary->kind != EventKindV3::ToolResultSummaryFinished ||
                    summary->action_id != message->action_id || summary->seq >= selected->seq ||
                    JsonString(summary->payload, "state").value_or("") != "accepted" ||
                    !selected_body || JsonString(summary->payload, "previewSha256").value_or("") != platform::Sha256Hex(selected_text) ||
                    RefIdArray(summary->payload, "sourceResultEventRefs") != persisted_refs) {
                    projection.summary_valid = false;
                } else {
                    projection.summary_candidate_refs = RefIdArray(summary->payload, "candidateMessageRefs");
                    if (projection.summary_candidate_refs.empty()) projection.summary_valid = false;
                    for (const auto& ref : projection.summary_candidate_refs) {
                        const auto* candidate = ledger.FindMessage(ref);
                        if (!candidate || candidate->purpose != MessagePurpose::ActionSummary || candidate->seq >= summary->seq) {
                            projection.summary_valid = false;
                        }
                    }
                }
            }
        }
    }
    if (persisted_refs.empty() && message->action_id.has_value()) {
        // 回退:无选用引用时按 actionId 收全部 persisted(降档派生消息共享)。
        for (const auto& event : ledger.events) {
            if (event.kind == EventKindV3::ToolResultPersisted && event.action_id == message->action_id) {
                persisted_refs.push_back(event.event_id);
            }
        }
    }
    for (const auto& ref : persisted_refs) {
        const EventLine* persisted = ledger.FindEvent(ref);
        if (persisted == nullptr) {
            continue;
        }
        if (auto it = persisted->payload.find("result_ref");
            it != persisted->payload.end() && it->is_array()) {
            for (const auto& artifact : *it) {
                projection.result_refs.push_back(artifact);
            }
        }
    }
    // 逐枚 artifact 实探:存在 + sha256(§4.16"任何对正文的查阅均校验身份
    // 和 hash");缺件标缺口,不冒称完整(§4.10)。
    bool complete = projection.complete && projection.summary_valid;
    for (const auto& ref : projection.result_refs) {
        ArtifactProbe probe;
        probe.artifact_id = JsonString(ref, "artifactId").value_or("");
        probe.path = JsonString(ref, "path").value_or("");
        const std::string expect_hash = JsonString(ref, "sha256").value_or("");
        const std::uint64_t expect_bytes = JsonUint(ref, "bytes").value_or(0);
        if (session_dir.empty() || probe.path.empty()) {
            probe.gap_reason = "missing_blob";
            complete = false;
            projection.artifacts.push_back(std::move(probe));
            continue;
        }
        std::filesystem::path file = session_dir / probe.path;
        std::error_code ec;
        if (!std::filesystem::exists(file, ec) || ec) {
            probe.gap_reason = "missing_blob";
            complete = false;
            projection.artifacts.push_back(std::move(probe));
            continue;
        }
        std::ifstream input(file, std::ios::binary);
        if (!input.is_open()) {
            probe.gap_reason = "unreadable";
            probe.exists = true;
            complete = false;
            projection.artifacts.push_back(std::move(probe));
            continue;
        }
        probe.exists = true;
        std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        probe.bytes = data.size();
        probe.hash_ok = platform::Sha256Hex(data) == expect_hash;
        if (!probe.hash_ok || (expect_bytes != 0 && probe.bytes != expect_bytes)) {
            probe.gap_reason = probe.hash_ok ? "bytes_mismatch" : "hash_mismatch";
            complete = false;
        }
        projection.artifacts.push_back(std::move(probe));
    }
    projection.complete = complete;
    return projection;
}

// ---------------------------------------------------------------------------
// 跨会话五键引用
// ---------------------------------------------------------------------------

std::optional<CrossSessionRef> ParseCrossSessionRef(const nlohmann::json& ref) {
    if (!ref.is_object()) {
        return std::nullopt;
    }
    CrossSessionRef parsed;
    parsed.session_id = JsonString(ref, "sessionId").value_or("");
    parsed.run_id = JsonString(ref, "runId").value_or("");
    parsed.id = JsonString(ref, "id").value_or("");
    parsed.seq = JsonUint(ref, "seq").value_or(0);
    parsed.hash = JsonString(ref, "hash").value_or("");
    if (parsed.session_id.empty() || parsed.run_id.empty() || parsed.id.empty() ||
        parsed.seq == 0 || parsed.hash.empty()) {
        return std::nullopt;
    }
    return parsed;
}

CrossSessionRefCheck VerifyCrossSessionRef(const CrossSessionRef& ref, const V3Ledger& target) {
    CrossSessionRefCheck check;
    auto fail = [&check](std::string reason) {
        check.ok = false;
        check.reason = std::move(reason);
        return check;
    };
    if (target.session_id.empty()) {
        return fail("ledger_unreadable");
    }
    if (target.session_id != ref.session_id) {
        return fail("session_mismatch");
    }
    if (target.run_id != ref.run_id) {
        return fail("run_mismatch");
    }
    // timeline 按 seq 升序,二分定位。
    auto it = std::lower_bound(
        target.timeline.begin(), target.timeline.end(), ref.seq,
        [](const V3Ledger::Entry& entry, std::uint64_t seq) { return entry.seq < seq; });
    if (it == target.timeline.end() || it->seq != ref.seq) {
        return fail("seq_out_of_range");
    }
    const std::string line_id = it->is_message
                                    ? target.messages[it->index].message_id
                                    : target.events[it->index].event_id;
    const std::string line_hash = it->is_message ? target.messages[it->index].line_hash
                                                 : target.events[it->index].line_hash;
    if (line_id != ref.id) {
        return fail("id_mismatch");
    }
    if (line_hash != ref.hash) {
        return fail("hash_mismatch");
    }
    check.ok = true;
    return check;
}

// ---------------------------------------------------------------------------
// 父子账递归遍历(§4.31)
// ---------------------------------------------------------------------------

namespace {

void WalkSessionTreeRecursive(const std::filesystem::path& jsonl, SubagentSessionNode& node,
                              std::unordered_set<std::string>& visited, int depth,
                              int max_depth) {
    auto read = ReadV3Ledger(jsonl);
    if (!read.has_value()) {
        node.ledger.reset();
        node.link_status = std::filesystem::exists(jsonl) ? "unreadable" : "child_missing";
        return;
    }
    node.ledger = std::move(*read);
    if (depth >= max_depth) {
        return;
    }
    const V3Ledger& ledger = *node.ledger;
    for (const auto& event : ledger.events) {
        if (event.kind != EventKindV3::SubagentSpawnRequested) {
            continue;
        }
        SubagentSessionNode child;
        child.parent_session_id = ledger.session_id;
        child.task_id = event.task_id.value_or("");
        child.spawn_event_id = event.event_id;
        if (event.action_id.has_value()) {
            child.parent_action_id = *event.action_id;
        }
        auto child_ref = event.payload.find("childSessionRef");
        if (child_ref == event.payload.end() || !child_ref->is_object()) {
            child.link_status = "child_missing";
            node.children.push_back(std::move(child));
            continue;
        }
        child.session_id = JsonString(*child_ref, "sessionId").value_or("");
        child.run_id = JsonString(*child_ref, "runId").value_or("");
        child.jsonl_path = jsonl.parent_path() /
                           JsonString(*child_ref, "journalPath").value_or("");
        // 父侧终态:linked 落稳才算关联建立;spawn.failed 是失败终态。
        std::string link_status = "not_linked";
        for (const auto& probe : ledger.events) {
            if (probe.task_id != event.task_id) {
                continue;
            }
            if (probe.kind == EventKindV3::SubagentLinked) {
                link_status = "linked";
            } else if (probe.kind == EventKindV3::SubagentSpawnFailed) {
                link_status = "spawn_failed";
            }
        }
        if (visited.count(child.session_id) > 0) {
            child.link_status = "cycle";
            node.children.push_back(std::move(child));
            continue;
        }
        visited.insert(child.session_id);
        child.link_status = link_status;
        // 子账可读:验子账首行 systemMeta.spawnEventRef 五键(§4.31 派生来源)。
        WalkSessionTreeRecursive(child.jsonl_path, child, visited, depth + 1, max_depth);
        if (child.ledger.has_value() && !child.ledger->messages.empty()) {
            const auto& meta = child.ledger->messages.front().system_meta;
            if (meta.has_value()) {
                if (auto source = meta->find("spawnEventRef"); source != meta->end()) {
                    if (auto ref = ParseCrossSessionRef(*source)) {
                        child.source_check = VerifyCrossSessionRef(*ref, ledger);
                    }
                }
            }
        }
        node.children.push_back(std::move(child));
    }
}

}  // namespace

SubagentSessionNode WalkSessionTree(const std::filesystem::path& root_jsonl, int max_depth) {
    SubagentSessionNode root;
    root.jsonl_path = root_jsonl;
    auto read = ReadV3Ledger(root_jsonl);
    if (read.has_value()) {
        root.session_id = read->session_id;
        root.run_id = read->run_id;
    }
    std::unordered_set<std::string> visited;
    if (!root.session_id.empty()) {
        visited.insert(root.session_id);
    }
    root.link_status = "root";
    WalkSessionTreeRecursive(root_jsonl, root, visited, 0, max_depth);
    return root;
}

// ---------------------------------------------------------------------------
// 带来源链的 resume(§4.10/§4.59)
// ---------------------------------------------------------------------------

std::expected<ResumeProjection, std::string> ProjectResume(const std::filesystem::path& jsonl,
                                                           SourceLedgerResolver resolver,
                                                           int max_source_depth) {
    auto own = ReadV3Ledger(jsonl);
    if (!own.has_value()) {
        return std::unexpected(own.error());
    }
    ResumeProjection projection;
    projection.timeline = ProjectHistoryTimeline(*own);
    projection.model_context = ProjectModelContext(*own);
    // token 标记:读 applied 持久字段,resume 不重算(§4.11)。
    for (const auto& item : projection.timeline.items) {
        if (item.kind == HistoryTimeline::Item::Kind::CompactMarker) {
            projection.compact_markers.push_back(item.compact);
        }
    }
    // 执行状态:未收口工具(无终态/unknown/已选用无消息/结果缺失/消息未
    // 接纳)不重跑,原地续(§4.59;失败与恢复单 P1-A:执行终态、结果保存
    // 状态、消息接纳状态分别保留,缺口逐枚列出恢复工作)。
    std::vector<ToolActionSnapshot> snapshots = FoldToolActions(*own);
    for (auto& snapshot : snapshots) {
        const std::string& status = snapshot.folded_status;
        if (status == "done" || status == "failed" || status == "cancelled" ||
            status == "rejected" || status == "declared") {
            continue;
        }
        projection.execution.turns_with_open_work.push_back(snapshot.turn_id);
        projection.execution.open_actions.push_back(std::move(snapshot));
    }
    projection.execution.open_compact_ids = own->context.open_compact_ids;

    // 来源链:沿 resume.source.attached 回溯(直接源在前),逐级验 hash、
    // 去重、检环;祖先缺失只报缺口——本账链自足,精确上下文不受影响。
    // 深度护栏:环靠 visited 拦,超长链(max_source_depth 级)到此为止。
    std::unordered_set<std::string> visited{own->session_id};
    V3Ledger current = std::move(*own);
    int depth = 0;
    while (true) {
        // 取本账最后一枚 resume.source.attached(多次 resume 各有来源,
        // 最后一枚是最近的直接源)。
        const EventLine* attached = nullptr;
        for (const auto& event : current.events) {
            if (event.kind == EventKindV3::ResumeSourceAttached) {
                attached = &event;
            }
        }
        if (attached == nullptr) {
            break;
        }
        if (max_source_depth > 0 && depth >= max_source_depth) {
            ResumeSourceStep step;
            // 截断步仍指明"本要去的祖先",报缺口有头有脸。
            if (auto source_ref = attached->payload.find("sourceRef");
                source_ref != attached->payload.end() && source_ref->is_object()) {
                step.session_id = source_ref->value("sessionId", std::string());
                step.run_id = source_ref->value("runId", std::string());
            }
            step.check.ok = false;
            step.check.reason = "chain_depth_exceeded";
            projection.source_chain_ok = false;
            projection.source_chain.push_back(std::move(step));
            break;
        }
        ++depth;
        ResumeSourceStep step;
        auto source_ref = attached->payload.find("sourceRef");
        if (source_ref != attached->payload.end()) {
            step.ref = ParseCrossSessionRef(*source_ref);
        }
        if (!step.ref.has_value()) {
            step.check.ok = false;
            step.check.reason = "malformed_source_ref";
            projection.source_chain_ok = false;
            projection.source_chain.push_back(std::move(step));
            break;
        }
        step.session_id = step.ref->session_id;
        step.run_id = step.ref->run_id;
        if (resolver) {
            step.jsonl_path = resolver(step.session_id);
        } else {
            // 默认 sessions/<id>/<id>.jsonl(§4.16 目录合同)。
            step.jsonl_path = jsonl.parent_path().parent_path() / step.session_id /
                              (step.session_id + ".jsonl");
        }
        if (visited.count(step.session_id) > 0) {
            step.duplicate = true;  // 无重复显示(§4.10)
            projection.source_chain_ok = false;
            projection.source_chain.push_back(std::move(step));
            break;
        }
        visited.insert(step.session_id);
        auto ancestor = ReadV3Ledger(step.jsonl_path);
        if (!ancestor.has_value()) {
            step.check.ok = false;
            step.check.reason = "ledger_unreadable";
            projection.source_chain_ok = false;  // 报缺口,不中断本账恢复
            projection.source_chain.push_back(std::move(step));
            break;
        }
        step.ledger = *ancestor;  // 历史分页用副本;遍历游标另持
        step.check = VerifyCrossSessionRef(*step.ref, *ancestor);
        if (!step.check.ok) {
            projection.source_chain_ok = false;
        }
        projection.source_chain.push_back(std::move(step));
        current = std::move(*ancestor);
    }
    return projection;
}

}  // namespace lubancode::trajectory::v3
