#include "trajectory/recorder.hpp"

#include <chrono>
#include <map>
#include <set>
#include <utility>

#include "platform/paths.hpp"
#include "trajectory/canonical_json.hpp"

namespace lubancode::trajectory {
namespace {

// 可 offload 的正文字段键(§8.2):schema 标 "B"(string 或 BlobRef)的字段
// 与嵌套块里的 text。其余字段超限也算调用方数据错误,不偷偷变形。
bool IsOffloadableTextKey(std::string_view key) {
    return key == "text" || key == "system_ref" || key == "toolset_ref";
}

// 稳定错误码总表(单测钉;调用方按码分支,不读人话):
//   io.closed           Close 之后一切提交拒绝
//   io.broken           journal 写失败后句柄判坏,后续提交一律 IoFailed
//   io.append_failed    本次 append/flush 失败
//   io.blob_failed      blob offload 失败
//   io.canonical_failed canonical 序列化失败(理论不可达:payload 已过校验)
//   scope.mismatch      scope 与 recorder 底座不同(workspace/session/run/kind)
//   state.*             §6.2 状态机硬约束 18 条(见各检查点)

std::int64_t DefaultWallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t DefaultMonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct CallState {
    bool declared = false;   // 本 turn 某份 model output 声明过
    bool planned = false;
    bool effective = false;
    bool started = false;
    bool terminal = false;
    bool cancelled_terminal = false;
    bool result_committed = false;
    bool approval_policy = false;  // tool.input.effective 带可追溯 policy
    std::string effect_class;
};

struct TurnState {
    bool started = false;
    bool terminal = false;
    bool input_received = false;
    bool first_request_sent = false;
    std::string queue_item_input_id;
    std::map<std::string, std::string> prepared_events;  // event_id -> request_id
    std::set<std::string> sent_requests;
    std::set<std::string> completed_requests;
    // v2 model.usage.recorded 的 owner 键("request_id:attempt"):一枚
    // request attempt 至多一条 usage owner,重复提交拒绝(Token 账本单 §6.1.1)。
    std::set<std::string> usage_owners;
    std::map<std::string, CallState> calls;
};

struct ApprovalState {
    std::string call_id;
    std::string decision;  // 空=未决;allow/deny
    bool expired = false;
};

struct QueueItemState {
    std::string input_id;
    bool terminal = false;  // dequeued/cancelled/expired 任一即终(约束 17)
    bool dequeued = false;  // 只有 dequeued 能触发 turn(约束 18)
};

}  // namespace

std::int64_t RecorderClock::WallMs() const { return DefaultWallMs(); }
std::int64_t RecorderClock::MonotonicNs() const { return DefaultMonotonicNs(); }

struct TrajectoryRecorder::Impl {
    mutable std::mutex mutex;
    JournalWriter writer;
    std::filesystem::path stream_path;  // 关柄后 path() 仍可问
    BlobStore blobs;
    EventScope base;
    RecorderOptions options;
    RecorderClock default_clock;
    const RecorderClock* clock = &default_clock;

    std::uint64_t next_seq = 1;
    std::string last_event_hash{kGenesisHash};
    std::string first_event_hash;
    bool has_first_event = false;

    bool run_started = false;
    bool run_terminal = false;
    bool session_ended = false;
    bool closed = false;

    std::map<std::string, TurnState> turns;
    std::string active_turn;
    std::map<std::string, ApprovalState> approvals;
    std::map<std::string, QueueItemState> queue_items;

    // ---- 状态机检查(§6.2;只读,commit 成功后再 Apply) ----

    // 返回 nullopt = 过;否则给稳定 error_code。
    std::optional<std::string> Check(const RecordRequest& request,
                                     const EventEnvelope& envelope) const {
        const EventKind kind = request.kind;

        // 约束 1:run.started 只能一枚,且 seq=1。
        if (kind == EventKind::RunStarted) {
            if (run_started) {
                return "state.run_started_duplicate";
            }
            if (next_seq != 1) {
                return "state.run_started_not_first";
            }
        } else if (!run_started) {
            return "state.run_not_started";
        }

        // 约束 13:run terminal 至多一枚(先于 run_closed 报,违例码才分得清)。
        if ((kind == EventKind::RunCompleted || kind == EventKind::RunFailed ||
             kind == EventKind::RunCancelled) &&
            run_terminal) {
            return "state.run_terminal_duplicate";
        }

        // run terminal 之后只许 session.ended 收链(§5.1:它必是最后一枚)。
        if (run_terminal && kind != EventKind::SessionEnded) {
            return "state.run_closed";
        }
        // §5.1:run terminal 前活动 turn 须收齐;session.ended 须在 run
        // terminal 之后且必是最后一枚。
        if ((kind == EventKind::RunCompleted || kind == EventKind::RunFailed ||
             kind == EventKind::RunCancelled) &&
            !active_turn.empty()) {
            return "state.run_active_turn";
        }
        if (kind == EventKind::SessionEnded) {
            if (!run_terminal) {
                return "state.session_before_run_terminal";
            }
            if (!active_turn.empty()) {
                return "state.session_active_turn";
            }
        }

        // session.* 只许 main stream(§5.1)。one_shot 一场也是进程的 main
        // stream(单发轨迹断档单),同放行;subagent/workflow 照旧拒。
        if ((kind == EventKind::SessionClearRequested || kind == EventKind::SessionEnded) &&
            base.run_kind != RunKind::MainSession && base.run_kind != RunKind::OneShot) {
            return "state.session_event_not_main";
        }

        const TurnState* turn = nullptr;
        if (envelope.turn_id.has_value()) {
            const auto it = turns.find(*envelope.turn_id);
            if (it == turns.end()) {
                // turn.started 自己登记在 Apply;走到这里说明引用了没开过的 turn。
                if (kind != EventKind::TurnStarted) {
                    return "state.turn_not_started";
                }
            } else {
                turn = &it->second;
                if (kind != EventKind::TurnStarted && kind != EventKind::TurnCompleted &&
                    kind != EventKind::TurnFailed && kind != EventKind::TurnCancelled &&
                    turn->terminal) {
                    return "state.turn_closed";
                }
            }
        }

        switch (kind) {
            case EventKind::TurnStarted: {
                if (turns.contains(*envelope.turn_id)) {
                    return "state.turn_duplicate";
                }
                if (!active_turn.empty()) {
                    // 外层 turn 顺序流转;并行 turn 不是 v1 合同。
                    return "state.turn_overlap";
                }
                // 约束 18 上半:queue 触发的 turn,其 item 必须 dequeued 且
                // input id 对得上。
                const auto trigger_input = request.payload.value("queue_item_input_id", "");
                if (!trigger_input.empty()) {
                    bool dequeued = false;
                    for (const auto& [item_id, item] : queue_items) {
                        (void)item_id;
                        if (item.input_id == trigger_input && item.dequeued) {
                            dequeued = true;
                        }
                    }
                    if (!dequeued) {
                        return "state.queue_item_not_dequeued";
                    }
                }
                return std::nullopt;
            }
            case EventKind::TurnCompleted:
            case EventKind::TurnFailed:
            case EventKind::TurnCancelled: {
                if (turn == nullptr || !turn->started) {
                    // 约束 2:turn 终态前必须有 turn.started。
                    return "state.turn_not_started";
                }
                if (turn->terminal) {
                    return "state.turn_terminal_duplicate";
                }
                if (kind == EventKind::TurnCompleted) {
                    // 约束 12:completed 前不留悬空 request 或 tool call。
                    for (const auto& request_id : turn->sent_requests) {
                        if (!turn->completed_requests.contains(request_id)) {
                            return "state.turn_dangling_request";
                        }
                    }
                    for (const auto& [call_id, call] : turn->calls) {
                        (void)call_id;
                        if (call.declared && !call.terminal) {
                            return "state.turn_dangling_call";
                        }
                        if (call.terminal && !call.cancelled_terminal && !call.result_committed) {
                            return "state.turn_dangling_result";
                        }
                    }
                }
                return std::nullopt;
            }
            case EventKind::InputReceived: {
                // 约束 18 下半:queue 触发的 turn,input id 必须与 dequeued 的
                // 那枚一致,不得另造。
                if (turn != nullptr && !turn->queue_item_input_id.empty()) {
                    const auto input_id = request.payload.value("input_id", "");
                    if (input_id != turn->queue_item_input_id) {
                        return "state.queue_input_mismatch";
                    }
                }
                return std::nullopt;
            }
            case EventKind::ModelRequestPrepared: {
                // 约束 11:下一请求前,本批每枚 ToolCall 都有 ToolResult。
                if (turn != nullptr) {
                    for (const auto& [call_id, call] : turn->calls) {
                        (void)call_id;
                        if (call.terminal && !call.cancelled_terminal && !call.result_committed) {
                            return "state.tool_results_pending";
                        }
                        if (call.started && !call.terminal) {
                            return "state.tool_results_pending";
                        }
                    }
                }
                return std::nullopt;
            }
            case EventKind::ModelRequestSent: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                // 约束 3:input.received 先于本 turn 首次 sent。
                if (!turn->first_request_sent && !turn->input_received) {
                    return "state.turn_missing_input";
                }
                // 约束 4:sent 必须引用已提交的 prepared event。
                const auto prepared_id = request.payload.value("prepared_event_id", "");
                const auto it = turn->prepared_events.find(prepared_id);
                if (it == turn->prepared_events.end()) {
                    return "state.request_not_prepared";
                }
                if (envelope.request_id.has_value() && it->second != *envelope.request_id) {
                    return "state.request_not_prepared";
                }
                return std::nullopt;
            }
            case EventKind::ModelOutputCompleted:
            case EventKind::ModelOutputFailed:
            case EventKind::ModelOutputCancelled: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                // 约束 5:output 必须引用已发送 request。
                if (!envelope.request_id.has_value() ||
                    !turn->sent_requests.contains(*envelope.request_id)) {
                    return "state.request_not_sent";
                }
                if (kind == EventKind::ModelOutputCompleted) {
                    // 约束 6:call_id 在 run 内唯一(两份 output 声明同一
                    // call 即撞号)。
                    if (request.payload.contains("blocks") &&
                        request.payload.at("blocks").is_array()) {
                        for (const auto& block : request.payload.at("blocks")) {
                            if (block.is_object() && block.value("type", "") == "tool_call") {
                                if (turn->calls.contains(block.value("call_id", ""))) {
                                    return "state.call_id_duplicate";
                                }
                            }
                        }
                    }
                }
                return std::nullopt;
            }
            case EventKind::ModelUsageRecorded: {
                // v2 usage owner(Token 账本单 §6.1.1):v1 stream 已在 schema
                // 层拒收,走到这里必是 v2。usage 必须挂在已发送的 request 上;
                // 一枚 request_id+attempt 至多一条 owner。
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                if (!envelope.request_id.has_value() ||
                    !turn->sent_requests.contains(*envelope.request_id)) {
                    return "state.request_not_sent";
                }
                const std::string key = *envelope.request_id + ":" +
                                        std::to_string(request.payload.value("attempt",
                                                                              std::uint64_t{1}));
                if (turn->usage_owners.contains(key)) {
                    return "state.usage_owner_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ToolExecutionPlanned: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                const auto it = turn->calls.find(*envelope.call_id);
                if (it != turn->calls.end() && it->second.planned) {
                    // 约束 6:call_id 在 run 内唯一(planned 过再 planned 即撞)。
                    return "state.call_id_duplicate";
                }
                if (it == turn->calls.end()) {
                    // planned 引用 model output 声明过的 call_id(§6.1 次序)。
                    return "state.call_not_declared";
                }
                return std::nullopt;
            }
            case EventKind::ToolInputEffective: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                if (!turn->calls.contains(*envelope.call_id)) {
                    return "state.call_not_declared";
                }
                return std::nullopt;
            }
            case EventKind::ToolExecutionStarted: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                const auto it = turn->calls.find(*envelope.call_id);
                if (it == turn->calls.end()) {
                    return "state.call_not_declared";
                }
                // 约束 7:started 前必须有 effective input。
                if (!it->second.effective) {
                    return "state.tool_missing_effective_input";
                }
                if (it->second.started || it->second.terminal) {
                    return "state.tool_duplicate_start";
                }
                // 约束 15/16:审批决议门。
                for (const auto& [approval_id, approval] : approvals) {
                    (void)approval_id;
                    if (approval.call_id != *envelope.call_id) {
                        continue;
                    }
                    if (approval.expired || approval.decision == "deny") {
                        return "state.approval_denied";
                    }
                    if (approval.decision != "allow" && !it->second.approval_policy) {
                        return "state.approval_pending";
                    }
                }
                return std::nullopt;
            }
            case EventKind::ToolExecutionFinished:
            case EventKind::ToolExecutionFailed:
            case EventKind::ToolExecutionCancelled:
            case EventKind::ToolExecutionUnknown: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                const auto it = turn->calls.find(*envelope.call_id);
                if (it == turn->calls.end()) {
                    return "state.call_unknown";
                }
                // cancelled 是唯一不须 started 的终态:闸前被收掉的调用
                //(审批拒绝/钩子拦下/Plan 闸/ESC 未轮到)没越过执行边界,
                // 却仍要落明确 execution 终态(§6.2 约束 16"取消后须落到
                // 明确 execution/turn/run 终态";约束 12 的悬空检查也靠它
                // 收口)。finished/failed/unknown 仍须 started——那三态
                // 声称的是执行边界之后的事实。
                if (!it->second.started && kind != EventKind::ToolExecutionCancelled) {
                    return "state.tool_not_started";
                }
                // 约束 9:四选一,至多一枚。
                if (it->second.terminal) {
                    return "state.tool_terminal_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ToolResultCommitted: {
                if (turn == nullptr) {
                    return "state.turn_not_started";
                }
                const auto it = turn->calls.find(*envelope.call_id);
                if (it == turn->calls.end()) {
                    // 约束 10:必须引用已知 ToolCall。
                    return "state.call_unknown";
                }
                if (!it->second.terminal) {
                    // 约束 10:与执行终态。
                    return "state.tool_not_terminal";
                }
                if (it->second.result_committed) {
                    return "state.tool_result_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ControlQueueItemEnqueued: {
                const auto item_id = request.payload.value("item_id", "");
                if (queue_items.contains(item_id)) {
                    return "state.queue_item_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ControlQueueItemDequeued:
            case EventKind::ControlQueueItemCancelled:
            case EventKind::ControlQueueItemExpired: {
                const auto item_id = request.payload.value("item_id", "");
                const auto it = queue_items.find(item_id);
                if (it == queue_items.end()) {
                    return "state.queue_item_unknown";
                }
                // 约束 17:三选一,至多一枚。
                if (it->second.terminal) {
                    return "state.queue_terminal_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ControlApprovalRequested: {
                const auto approval_id = request.payload.value("approval_id", "");
                if (approvals.contains(approval_id)) {
                    return "state.approval_duplicate";
                }
                return std::nullopt;
            }
            case EventKind::ControlApprovalResolved:
            case EventKind::ControlApprovalExpired: {
                const auto approval_id = request.payload.value("approval_id", "");
                if (!approvals.contains(approval_id)) {
                    return "state.approval_unknown";
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    // commit 成功后更新状态机账(Check 已放行)。
    void Apply(const RecordRequest& request, const EventEnvelope& envelope) {
        const EventKind kind = request.kind;
        if (!has_first_event) {
            first_event_hash = envelope.event_hash;
            has_first_event = true;
        }
        if (kind == EventKind::RunStarted) {
            run_started = true;
            return;
        }
        if (kind == EventKind::RunCompleted || kind == EventKind::RunFailed ||
            kind == EventKind::RunCancelled) {
            run_terminal = true;
            return;
        }
        if (kind == EventKind::SessionEnded) {
            session_ended = true;
            return;
        }

        TurnState* turn = nullptr;
        if (envelope.turn_id.has_value()) {
            turn = &turns[*envelope.turn_id];
        }

        switch (kind) {
            case EventKind::TurnStarted: {
                turn->started = true;
                turn->queue_item_input_id = request.payload.value("queue_item_input_id", "");
                active_turn = *envelope.turn_id;
                return;
            }
            case EventKind::TurnCompleted:
            case EventKind::TurnFailed:
            case EventKind::TurnCancelled: {
                turn->terminal = true;
                if (active_turn == *envelope.turn_id) {
                    active_turn.clear();
                }
                return;
            }
            case EventKind::InputReceived: {
                turn->input_received = true;
                return;
            }
            case EventKind::ModelRequestPrepared: {
                turn->prepared_events[envelope.event_id] = envelope.request_id.value_or("");
                return;
            }
            case EventKind::ModelRequestSent: {
                turn->first_request_sent = true;
                turn->sent_requests.insert(envelope.request_id.value_or(""));
                return;
            }
            case EventKind::ModelOutputCompleted: {
                turn->completed_requests.insert(envelope.request_id.value_or(""));
                // 声明本份 output 里的 tool_call(§6.1:call 由模型输出定义)。
                if (request.payload.contains("blocks") && request.payload.at("blocks").is_array()) {
                    for (const auto& block : request.payload.at("blocks")) {
                        if (block.is_object() && block.value("type", "") == "tool_call") {
                            const auto call_id = block.value("call_id", "");
                            if (!call_id.empty()) {
                                turn->calls[call_id].declared = true;
                            }
                        }
                    }
                }
                return;
            }
            case EventKind::ModelOutputFailed:
            case EventKind::ModelOutputCancelled: {
                turn->completed_requests.insert(envelope.request_id.value_or(""));
                return;
            }
            case EventKind::ModelUsageRecorded: {
                turn->usage_owners.insert(*envelope.request_id + ":" +
                                          std::to_string(request.payload.value("attempt",
                                                                                std::uint64_t{1})));
                return;
            }
            case EventKind::ToolExecutionPlanned: {
                turn->calls[*envelope.call_id].planned = true;
                return;
            }
            case EventKind::ToolInputEffective: {
                CallState& call = turn->calls[*envelope.call_id];
                call.effective = true;
                call.effect_class = request.payload.value("effect_class", "");
                call.approval_policy = request.payload.contains("approval_policy_ref");
                return;
            }
            case EventKind::ToolExecutionStarted: {
                turn->calls[*envelope.call_id].started = true;
                return;
            }
            case EventKind::ToolExecutionFinished:
            case EventKind::ToolExecutionFailed:
            case EventKind::ToolExecutionUnknown: {
                CallState& call = turn->calls[*envelope.call_id];
                call.terminal = true;
                return;
            }
            case EventKind::ToolExecutionCancelled: {
                CallState& call = turn->calls[*envelope.call_id];
                call.terminal = true;
                call.cancelled_terminal = true;
                return;
            }
            case EventKind::ToolResultCommitted: {
                turn->calls[*envelope.call_id].result_committed = true;
                return;
            }
            case EventKind::ControlQueueItemEnqueued: {
                const auto item_id = request.payload.value("item_id", "");
                queue_items[item_id] = QueueItemState{request.payload.value("input_id", ""), false};
                return;
            }
            case EventKind::ControlQueueItemDequeued:
                queue_items[request.payload.value("item_id", "")].dequeued = true;
                queue_items[request.payload.value("item_id", "")].terminal = true;
                return;
            case EventKind::ControlQueueItemCancelled:
            case EventKind::ControlQueueItemExpired:
                queue_items[request.payload.value("item_id", "")].terminal = true;
                return;
            case EventKind::ControlApprovalRequested: {
                const auto approval_id = request.payload.value("approval_id", "");
                approvals[approval_id] =
                    ApprovalState{request.payload.value("call_id", ""), "", false};
                return;
            }
            case EventKind::ControlApprovalResolved: {
                const auto approval_id = request.payload.value("approval_id", "");
                approvals[approval_id].decision = request.payload.value("decision", "");
                return;
            }
            case EventKind::ControlApprovalExpired: {
                const auto approval_id = request.payload.value("approval_id", "");
                approvals[approval_id].expired = true;
                return;
            }
            default:
                return;
        }
    }
};

std::expected<TrajectoryRecorder, std::string> TrajectoryRecorder::Start(
    const std::filesystem::path& stream_path, const std::filesystem::path& artifact_root,
    EventScope base_scope, RecorderOptions options, const RecorderClock* clock) {
    auto writer = JournalWriter::Open(stream_path, JournalWriter::OpenMode::CreateNew);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer).error());
    }
    if (options.event_schema_version < kEnvelopeSchemaVersion ||
        options.event_schema_version > kMaxEnvelopeSchemaVersion) {
        return std::unexpected("schema.unsupported_version: event_schema_version 只认 1(v1)或 2(v2 usage owner)");
    }
    auto impl = std::make_shared<Impl>();
    impl->writer = std::move(*writer);
    impl->stream_path = stream_path;
    impl->blobs = BlobStore(artifact_root, options.blobs);
    impl->base = std::move(base_scope);
    impl->options = options;
    if (clock != nullptr) {
        impl->clock = clock;
    }
    return TrajectoryRecorder(std::move(impl));
}

std::expected<TrajectoryRecorder, std::string> TrajectoryRecorder::Continue(
    const std::filesystem::path& stream_path, const std::filesystem::path& artifact_root,
    RecorderOptions options, const RecorderClock* clock) {
    // 先整本验账:hash 链、schema、canonical 字节全绿才许续;截断尾/坏行
    // 一律拒开(§16.3:不伪造终态,交给上层标 incomplete/corrupt)。
    const JournalVerifyReport report = VerifyJournalFile(stream_path);
    if (!report.ok) {
        return std::unexpected("recorder.continue_not_clean: " + report.error_code + ": " +
                               report.message);
    }
    const auto lines = ReadJournalLines(stream_path);
    if (!lines.has_value() || lines->empty()) {
        return std::unexpected("recorder.continue_no_scope: 账本没有可推身份的事件: " +
                               platform::PathToUtf8(stream_path));
    }

    // base scope 从首行推:身份四件 + actor/origin/visibility 照实取自首事件。
    const auto first_parsed = nlohmann::json::parse(lines->front(), nullptr, false);
    EventEnvelope first;
    if (auto error = ParseAndValidateEventLine(first_parsed, &first)) {
        return std::unexpected("recorder.continue_no_scope: 首行解析失败: " + error->message);
    }
    EventScope base;
    base.workspace_key = first.workspace_key;
    base.session_id = first.session_id;
    base.run_id = first.run_id;
    base.run_kind = first.run_kind;
    base.actor = first.actor;
    base.origin = first.origin;
    base.visibility = first.visibility;
    base.training_policy = first.training_policy;

    auto writer = JournalWriter::Open(stream_path, JournalWriter::OpenMode::Append);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer).error());
    }
    auto impl = std::make_shared<Impl>();
    impl->writer = std::move(*writer);
    impl->stream_path = stream_path;
    impl->blobs = BlobStore(artifact_root, options.blobs);
    impl->base = std::move(base);
    impl->options = options;
    if (clock != nullptr) {
        impl->clock = clock;
    }
    // 重放状态机账:seq/hash 接链尾,Apply 重建 run/turn/call/queue 全账
    // (Check 不走——事实已 committed,这里只恢复记忆)。
    for (const std::string& line : *lines) {
        EventEnvelope envelope;
        if (auto error = ParseAndValidateEventLine(
                nlohmann::json::parse(line, nullptr, false), &envelope)) {
            return std::unexpected("recorder.continue_not_clean: " + error->error_code + ": " +
                                   error->message);
        }
        RecordRequest replay;
        replay.kind = envelope.kind;
        replay.scope = impl->base;
        replay.scope.turn_id = envelope.turn_id;
        replay.scope.request_id = envelope.request_id;
        replay.scope.call_id = envelope.call_id;
        replay.payload = envelope.payload;
        impl->next_seq = envelope.seq + 1;
        impl->last_event_hash = envelope.event_hash;
        impl->Apply(replay, envelope);
    }
    return TrajectoryRecorder(std::move(impl));
}

namespace {

// 递归 offload 超限正文字段(键集见 IsOffloadableTextKey)。返回 false =
// blob 写失败,提交整体回 IoFailed。
bool OffloadTextFields(nlohmann::json* value, BlobStore& blobs, Durability durability,
                       std::string* error) {
    if (value->is_object()) {
        for (auto it = value->begin(); it != value->end(); ++it) {
            if (it->is_string() && IsOffloadableTextKey(it.key()) &&
                it->get_ref<const std::string&>().size() > blobs.inline_limit()) {
                const auto stored = blobs.Store(it->get_ref<const std::string&>(), "text/plain",
                                                Durability::PowerLoss);
                if (!stored.has_value()) {
                    *error = stored.error();
                    return false;
                }
                *it = stored->ToJson();
            } else if (it->is_object() || it->is_array()) {
                if (!OffloadTextFields(&*it, blobs, durability, error)) {
                    return false;
                }
            }
        }
    } else if (value->is_array()) {
        for (auto& item : *value) {
            if (item.is_object() || item.is_array()) {
                if (!OffloadTextFields(&item, blobs, durability, error)) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

RecordReceipt TrajectoryRecorder::Record(RecordRequest request, Durability durability) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    RecordReceipt receipt;
    const auto reject = [&receipt](std::string code) {
        receipt.status = RecordReceipt::Status::Rejected;
        receipt.error_code = std::move(code);
        return receipt;
    };
    const auto io_fail = [&receipt](std::string code) {
        receipt.status = RecordReceipt::Status::IoFailed;
        receipt.error_code = std::move(code);
        return receipt;
    };

    if (impl_->closed) {
        return reject("io.closed");
    }
    if (impl_->writer.broken()) {
        return io_fail("io.broken");
    }
    if (impl_->session_ended) {
        // §5.1:session.ended 之后本 session 各 Recorder 一概拒写。
        return reject("state.session_ended");
    }

    // scope 恒等:一场 run 一只 recorder,身份不一致便是接错线。
    if (request.scope.workspace_key != impl_->base.workspace_key ||
        request.scope.session_id != impl_->base.session_id ||
        request.scope.run_id != impl_->base.run_id ||
        request.scope.run_kind != impl_->base.run_kind) {
        return reject("scope.mismatch");
    }

    // 信封(schema 前半:类型与结构)。
    EventEnvelope envelope;
    envelope.schema_version = impl_->options.event_schema_version;
    envelope.workspace_key = request.scope.workspace_key;
    envelope.session_id = request.scope.session_id;
    envelope.run_id = request.scope.run_id;
    envelope.run_kind = request.scope.run_kind;
    envelope.seq = impl_->next_seq;  // 占位,canonical 前定稿
    envelope.kind = request.kind;
    envelope.plane = EventKindInfoOf(request.kind).plane;
    envelope.actor = request.scope.actor;
    envelope.origin = request.scope.origin;
    envelope.visibility = request.scope.visibility;
    envelope.training_policy = request.scope.training_policy;
    envelope.turn_id = request.scope.turn_id;
    envelope.request_id = request.scope.request_id;
    envelope.call_id = request.scope.call_id;
    envelope.causation_id = request.links.causation_id;
    envelope.correlation_id = request.links.correlation_id;
    if (request.links.retry_of.has_value() || request.links.compensates.has_value() ||
        request.links.parent_call_id.has_value() || request.links.parent_run_id.has_value() ||
        request.links.child_run_id.has_value() || !request.links.blocked_by.empty()) {
        nlohmann::json relations = nlohmann::json::object();
        if (request.links.retry_of.has_value()) {
            relations["retry_of"] = *request.links.retry_of;
        }
        if (request.links.compensates.has_value()) {
            relations["compensates"] = *request.links.compensates;
        }
        if (request.links.parent_call_id.has_value()) {
            relations["parent_call_id"] = *request.links.parent_call_id;
        }
        if (request.links.parent_run_id.has_value()) {
            relations["parent_run_id"] = *request.links.parent_run_id;
        }
        if (request.links.child_run_id.has_value()) {
            relations["child_run_id"] = *request.links.child_run_id;
        }
        if (!request.links.blocked_by.empty()) {
            relations["blocked_by"] = request.links.blocked_by;
        }
        envelope.relations = std::move(relations);
    }
    envelope.wall_time_ms = impl_->clock->WallMs();
    envelope.monotonic_ns = impl_->clock->MonotonicNs();
    envelope.payload = request.payload;
    envelope.prev_hash = impl_->last_event_hash;

    // schema validate(信封语义 + payload 强校验)。
    envelope.seq = 1;  // ValidateEnvelope 只看形状;seq 真值在链路里定稿
    envelope.event_id = FormatEventId(envelope.run_id, 1);
    envelope.event_hash = std::string(kGenesisHash);
    if (auto error = ValidateEnvelope(envelope)) {
        return reject(error->error_code);
    }
    if (auto error = ValidatePayloadWithVersion(impl_->options.event_schema_version, request.kind,
                                                request.payload)) {
        return reject(error->error_code);
    }

    // state-machine validate(§6.2 十八条)。
    if (auto code = impl_->Check(request, envelope)) {
        return reject(*code);
    }

    // 发号(§2.3:调用方不传 seq/event_id/hash,Recorder 一处生成)。
    const std::uint64_t seq = impl_->next_seq;
    envelope.seq = seq;
    envelope.event_id = FormatEventId(envelope.run_id, seq);

    // 约束 8 的落法:副作用工具的 started/finished 与 run/session 终态、
    // checkpoint 一律提档 PowerLoss(§7.4 副作用栅栏)。提档不改字节,
    // 只加 flush 强度;调用方给低档不构成绕栅栏。
    Durability effective_durability = durability;
    const bool side_effect_tool =
        envelope.call_id.has_value() &&
        (request.kind == EventKind::ToolExecutionStarted ||
         request.kind == EventKind::ToolExecutionFinished ||
         request.kind == EventKind::ToolExecutionFailed ||
         request.kind == EventKind::ToolExecutionUnknown);
    bool is_side_effect = false;
    if (side_effect_tool) {
        const auto it = impl_->turns.find(envelope.turn_id.value_or(""));
        if (it != impl_->turns.end()) {
            const auto call = it->second.calls.find(*envelope.call_id);
            if (call != it->second.calls.end() && !call->second.effect_class.empty() &&
                call->second.effect_class.rfind("read_only", 0) != 0) {
                is_side_effect = true;
            }
        }
    }
    if (is_side_effect || request.kind == EventKind::RunCompleted ||
        request.kind == EventKind::RunFailed || request.kind == EventKind::RunCancelled ||
        request.kind == EventKind::SessionEnded ||
        request.kind == EventKind::ControlCheckpointCreated) {
        effective_durability = Durability::PowerLoss;
    }

    // blob offload(超限正文换 BlobRef;事件不指向未落稳的 blob)。
    std::string blob_error;
    if (!OffloadTextFields(&envelope.payload, impl_->blobs, effective_durability, &blob_error)) {
        return io_fail("io.blob_failed: " + blob_error);
    }

    // canonical + hash chain。
    nlohmann::json without_hash = envelope.ToJson();
    without_hash.erase("event_hash");
    const auto canonical = CanonicalJsonDump(without_hash);
    if (!canonical.has_value()) {
        return io_fail("io.canonical_failed: " + canonical.error());
    }
    envelope.event_hash = ComputeEventHash(envelope.prev_hash, *canonical);
    const auto line = CanonicalJsonDump(envelope.ToJson());
    if (!line.has_value()) {
        return io_fail("io.canonical_failed: " + line.error());
    }

    // append + flush。
    if (!impl_->writer.AppendLine(*line, effective_durability)) {
        return io_fail("io.append_failed");
    }

    // 索引与状态机账后移。
    impl_->next_seq = seq + 1;
    impl_->last_event_hash = envelope.event_hash;
    impl_->Apply(request, envelope);

    receipt.status = RecordReceipt::Status::Committed;
    receipt.event_id = envelope.event_id;
    receipt.seq = seq;
    receipt.event_hash = envelope.event_hash;
    return receipt;
}

RecordReceipt TrajectoryRecorder::WriteRunStarted(nlohmann::json extra, Durability durability,
                                                  EventLinks links) {
    nlohmann::json payload = nlohmann::json::object();
    payload["run_kind"] = RunKindName(impl_->base.run_kind);
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        payload[it.key()] = it.value();
    }
    RecordRequest request;
    request.kind = EventKind::RunStarted;
    request.scope = impl_->base;
    request.scope.turn_id = std::nullopt;
    request.scope.request_id = std::nullopt;
    request.scope.call_id = std::nullopt;
    request.links = std::move(links);
    request.payload = std::move(payload);
    return Record(std::move(request), durability);
}

RecordReceipt TrajectoryRecorder::FinishRun(EventKind terminal_kind, std::string reason,
                                            Durability durability) {
    RecordRequest request;
    request.kind = terminal_kind;
    request.scope = impl_->base;
    request.scope.turn_id = std::nullopt;
    request.scope.request_id = std::nullopt;
    request.scope.call_id = std::nullopt;
    // §8.3 终态封口四件套。
    request.payload = MakeTerminalSealPayload(impl_->first_event_hash,
                                              impl_->next_seq - 1,
                                              impl_->options.event_schema_version,
                                              impl_->options.recorder_version);
    if (!reason.empty()) {
        request.payload["reason"] = std::move(reason);
    }
    return Record(std::move(request), durability);
}

RecordReceipt TrajectoryRecorder::EndSession(std::string reason,
                                             std::optional<std::string> next_session_id,
                                             std::string close_quality, Durability durability) {
    RecordRequest request;
    request.kind = EventKind::SessionEnded;
    request.scope = impl_->base;
    request.scope.turn_id = std::nullopt;
    request.scope.request_id = std::nullopt;
    request.scope.call_id = std::nullopt;
    request.payload = MakeTerminalSealPayload(impl_->first_event_hash, impl_->next_seq - 1,
                                              impl_->options.event_schema_version,
                                              impl_->options.recorder_version);
    request.payload["reason"] = std::move(reason);
    if (next_session_id.has_value()) {
        request.payload["next_session_id"] = *next_session_id;
    }
    if (!close_quality.empty()) {
        request.payload["close_quality"] = std::move(close_quality);
    }
    return Record(std::move(request), durability);
}

std::expected<std::string, std::string> TrajectoryRecorder::Close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closed) {
        return std::unexpected("io.closed: 已关柄");
    }
    impl_->closed = true;
    // 先算整本 hash(§8.3),再放掉句柄:封了口的账不该再攥着文件——
    // Windows 下攥着句柄的文件删不掉、搬不动(/delete、/archive 要用)。
    auto sha = JournalWriter::ComputeJournalSha256(impl_->stream_path);
    impl_->writer = JournalWriter{};
    return sha;
}

const std::filesystem::path& TrajectoryRecorder::stream_path() const {
    return impl_->stream_path;
}

const EventScope& TrajectoryRecorder::base_scope() const {
    return impl_->base;
}

std::uint64_t TrajectoryRecorder::next_seq() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->next_seq;
}

std::string TrajectoryRecorder::last_event_hash() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_event_hash;
}

bool TrajectoryRecorder::broken() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writer.broken();
}

}  // namespace lubancode::trajectory
