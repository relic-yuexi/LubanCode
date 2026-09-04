// Replay Projection 实现(P0 新轨迹记录单 §十)。合同见 replay.hpp。
//
// 折叠是纯函数:同一段事件序列折两次,ReplayState 与 state hash 逐位相同。
// 时间戳、I/O 顺序、map 迭代序一概不进 hash 输入——可变容器只在折叠内部
// 用,hash 走"按事件折叠序排定的向量"。

#include "trajectory/replay.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "trajectory/canonical_json.hpp"
#include "trajectory/schema.hpp"

namespace lubancode::trajectory {
namespace {

// JSON 取值的轻捷口:键在且类型对给值,否则给缺省。
std::string GetString(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    return it != payload.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool GetBool(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    return it != payload.end() && it->is_boolean() ? it->get<bool>() : false;
}

}  // namespace

// 对话轮的 purpose 白名单(Token 账本单 A1,§6.2 十二值的分家):main/
// subagent/goal 续跑/循环迭代是"与用户/目标对话"的轮,输出是会话历史;
// 其余(compact_map/compact_reduce/memory_extract/title_refine/doctor_probe/
// insights_model_review/other_host_request/workflow_node 的 SampleModel 路)
// 是回合外的宿主工作请求,输出由宿主消费,不折进 effective_conversation。
// purpose 为空(v1 旧账/未标号的旧 v2 账)按对话轮处理——老行为零变。
bool PurposeFoldsIntoConversation(const std::string& purpose) {
    if (purpose.empty() || purpose == "main_turn" || purpose == "subagent_turn" ||
        purpose == "goal_continue" || purpose == "loop_iteration") {
        return true;
    }
    return false;
}

namespace {

std::uint64_t GetUint(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    if (it == payload.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        return static_cast<std::uint64_t>(it->get<std::int64_t>());
    }
    return 0;
}

// 折叠内部索引:call_id/request_id/turn_id -> 向量位次。折叠器单线程;
// 续折时由 RebuildIndex 按向量顺序重建。
struct FoldIndex {
    std::map<std::string, std::size_t> calls;
    std::map<std::string, std::size_t> requests;
    std::map<std::string, std::size_t> turns;
    // turn 内已折进 effective_conversation 的 input 消息位次(Token 账本单
    // A1):旁路用途的 turn(compact/抽取/起名一类)整 turn 的 input 也是
    // 宿主材料,prepared 的 purpose 一到就回溯剔除——input.received 先于
    // prepared 落账(状态机约束 3),折的时候 purpose 还没见着,只能记账
    // 后裁。折叠局部账,不进 ReplayState 序列化。
    std::map<std::string, std::vector<std::size_t>> turn_input_slots;
};

void RebuildIndex(const ReplayState& state, FoldIndex* index) {
    index->calls.clear();
    index->requests.clear();
    index->turns.clear();
    index->turn_input_slots.clear();
    for (std::size_t i = 0; i < state.tools.size(); ++i) {
        index->calls[state.tools[i].call_id] = i;
    }
    for (std::size_t i = 0; i < state.requests.size(); ++i) {
        index->requests[state.requests[i].request_id] = i;
    }
    for (std::size_t i = 0; i < state.turns.size(); ++i) {
        index->turns[state.turns[i].turn_id] = i;
    }
}

// payload 里的 blob ref({sha256,...})抽进 artifact_refs。
void CollectArtifactRefs(const nlohmann::json& value, std::vector<std::string>* refs) {
    if (value.is_object()) {
        const auto sha = value.find("sha256");
        if (sha != value.end() && sha->is_string()) {
            refs->push_back(sha->get<std::string>());
        }
    }
}

// 一枚事件折进 state。返回 false = fail-closed(unsupported),reason 带出。
bool FoldEvent(const EventEnvelope& envelope, ReplayState* state, FoldIndex* index,
               std::string* unsupported_reason) {
    const nlohmann::json& payload = envelope.payload;
    switch (envelope.kind) {
        case EventKind::RunStarted: {
            state->workspace_key = envelope.workspace_key;
            state->session_id = envelope.session_id;
            state->run_id = envelope.run_id;
            state->run_kind = envelope.run_kind;
            state->start_reason = GetString(payload, "start_reason");
            // §8.4:run.started 写 min_reader_version;超前的明报 unsupported,
            // 不判 corrupt、不猜着读。
            const auto min_reader = payload.find("min_reader_version");
            if (min_reader != payload.end() && min_reader->is_number_unsigned()) {
                const std::uint64_t need = min_reader->get<std::uint64_t>();
                if (need > static_cast<std::uint64_t>(kMaxEnvelopeSchemaVersion)) {
                    *unsupported_reason = "min_reader_version=" + std::to_string(need) +
                                          " > 支持的 " + std::to_string(kMaxEnvelopeSchemaVersion);
                    return false;
                }
            }
            return true;
        }
        case EventKind::RunCompleted:
        case EventKind::RunFailed:
        case EventKind::RunCancelled:
            state->run_terminal_state = EventKindName(envelope.kind);
            return true;
        case EventKind::SessionClearRequested:
        case EventKind::SessionEnded:
            state->session_end_state = "ended";
            if (!GetString(payload, "next_session_id").empty()) {
                state->next_session_id = GetString(payload, "next_session_id");
            }
            return true;
        case EventKind::TurnStarted: {
            ReplayTurnEntry turn;
            turn.turn_id = envelope.turn_id.value_or(std::string());
            turn.trigger = GetString(payload, "trigger");
            state->turns.push_back(std::move(turn));
            index->turns[envelope.turn_id.value_or(std::string())] = state->turns.size() - 1;
            return true;
        }
        case EventKind::TurnCompleted:
        case EventKind::TurnFailed:
        case EventKind::TurnCancelled: {
            const auto it = index->turns.find(envelope.turn_id.value_or(std::string()));
            if (it != index->turns.end()) {
                state->turns[it->second].terminal_state = EventKindName(envelope.kind);
                state->turns[it->second].outcome = GetString(payload, "outcome");
                if (state->turns[it->second].outcome.empty()) {
                    state->turns[it->second].outcome = GetString(payload, "reason");
                }
            }
            return true;
        }
        case EventKind::InputReceived: {
            ReplayMessage message;
            message.role = ReplayMessage::Role::User;
            message.origin = OriginName(envelope.origin);
            message.blocks = payload.contains("content") && payload["content"].is_array()
                                 ? payload["content"]
                                 : nlohmann::json::array();
            message.source_event_id = envelope.event_id;
            message.source_event_hash = envelope.event_hash;
            state->effective_conversation.push_back(std::move(message));
            // 记进 turn 的 input 位次账:turn 若被旁路用途的 prepared 认领,
            // 这些 input 回溯剔除(见 ModelRequestPrepared)。
            if (envelope.turn_id.has_value()) {
                index->turn_input_slots[*envelope.turn_id].push_back(
                    state->effective_conversation.size() - 1);
            }
            return true;
        }
        case EventKind::ContextAttached:
        case EventKind::ContextDetached:
            // 宿主注入不冒充 user(§5.2),不进 effective conversation;
            // 内容 ref 进 artifact 账。
            CollectArtifactRefs(payload.value("content_ref", nlohmann::json()), &state->artifact_refs);
            return true;
        case EventKind::ContextInjected: {
            // 存储 v2 P0-3:记忆召回快照。不进 effective conversation(正文
            // 已经活在当轮 input 里);快照是内容寻址 blob,content_sha256 即
            // 文件名,进 artifact 账——Replay 重建"当时模型看见哪一版"凭
            // 这枚 hash 去仓里取,不读今天的 Memory。
            const auto content_hash = payload.find("content_sha256");
            if (content_hash != payload.end() && content_hash->is_string()) {
                state->artifact_refs.push_back(content_hash->get<std::string>());
            }
            return true;
        }
        case EventKind::MemorySaveRequested:
        case EventKind::MemorySaveCommitted:
        case EventKind::MemorySaveFailed:
            // Memory 写入因果边:纯账目,不参与会话投影。
            return true;
        case EventKind::ModelRequestPrepared: {
            ReplayRequestStep step;
            step.request_id = envelope.request_id.value_or(std::string());
            step.prepared_event_id = envelope.event_id;
            step.model = GetString(payload, "model");
            step.provider = GetString(payload, "provider");
            step.wire = GetString(payload, "wire");
            step.purpose = GetString(payload, "purpose");
            // 旁路用途(Token 账本单 A1):本 turn 是回合外的宿主小请求,
            // 已折进去的 input 是宿主喂的材料,回溯剔出会话历史(input
            // 落在 prepared 之前,折的时候 purpose 还没见着)。
            if (envelope.turn_id.has_value() && !PurposeFoldsIntoConversation(step.purpose)) {
                const auto slots = index->turn_input_slots.find(*envelope.turn_id);
                if (slots != index->turn_input_slots.end()) {
                    for (auto it = slots->second.rbegin(); it != slots->second.rend(); ++it) {
                        if (*it < state->effective_conversation.size()) {
                            state->effective_conversation.erase(
                                state->effective_conversation.begin() +
                                static_cast<std::ptrdiff_t>(*it));
                        }
                    }
                    slots->second.clear();
                }
            }
            if (payload.contains("parameters") && payload["parameters"].is_object()) {
                step.parameters = payload["parameters"];
            }
            if (payload.contains("message_refs") && payload["message_refs"].is_array()) {
                for (const auto& ref : payload["message_refs"]) {
                    if (ref.is_string()) {
                        step.message_refs.push_back(ref.get<std::string>());
                    }
                }
            }
            state->requests.push_back(std::move(step));
            index->requests[envelope.request_id.value_or(std::string())] = state->requests.size() - 1;
            return true;
        }
        case EventKind::ModelRequestSent: {
            const auto it = index->requests.find(envelope.request_id.value_or(std::string()));
            if (it != index->requests.end()) {
                state->requests[it->second].sent = true;
            }
            return true;
        }
        case EventKind::ModelOutputCompleted:
        case EventKind::ModelOutputFailed:
        case EventKind::ModelOutputCancelled: {
            const auto it = index->requests.find(envelope.request_id.value_or(std::string()));
            if (it == index->requests.end()) {
                return true;  // 状态机/verify 已拦过的畸形,折叠不重复报
            }
            ReplayRequestStep& step = state->requests[it->second];
            if (envelope.kind == EventKind::ModelOutputCompleted) {
                step.output_state = "completed";
                step.output_event_id = envelope.event_id;
                step.output_blocks = payload.contains("blocks") && payload["blocks"].is_array()
                                         ? payload["blocks"]
                                         : nlohmann::json::array();
                step.stop_reason = GetString(payload, "stop_reason");
                // §5.3:同一份输出不另落 assistant message——会话历史由这枚
                // 事件投影。宿主旁路用途(Token 账本单 A1)例外:compact/
                // 起名/抽取的输出是宿主吃掉的工作产物,折进去会污染 resume
                // 的会话历史——账照记在 step 里,人不进对话。
                if (PurposeFoldsIntoConversation(step.purpose)) {
                    ReplayMessage message;
                    message.role = ReplayMessage::Role::Assistant;
                    message.origin = OriginName(envelope.origin);
                    message.blocks = step.output_blocks;
                    message.source_event_id = envelope.event_id;
                    message.source_event_hash = envelope.event_hash;
                    state->effective_conversation.push_back(std::move(message));
                }
            } else {
                step.output_state = envelope.kind == EventKind::ModelOutputFailed ? "failed" : "cancelled";
                step.output_event_id = envelope.event_id;
            }
            return true;
        }
        case EventKind::ModelUsageRecorded: {
            const auto it = index->requests.find(envelope.request_id.value_or(std::string()));
            if (it != index->requests.end()) {
                state->requests[it->second].usage_recorded = true;
            }
            return true;
        }
        case EventKind::ToolExecutionPlanned: {
            const std::string call_id = GetString(payload, "call_id");
            ReplayToolEntry entry;
            entry.call_id = call_id;
            entry.tool_name = GetString(payload, "tool_name");
            entry.planned = true;
            state->tools.push_back(std::move(entry));
            index->calls[call_id] = state->tools.size() - 1;
            return true;
        }
        case EventKind::ToolInputEffective: {
            const auto it = index->calls.find(GetString(payload, "call_id"));
            if (it != index->calls.end()) {
                state->tools[it->second].effective = true;
                state->tools[it->second].effective_arguments_sha256 =
                    GetString(payload, "effective_arguments_sha256");
                if (state->tools[it->second].tool_name.empty()) {
                    state->tools[it->second].tool_name = GetString(payload, "tool_name");
                }
            }
            return true;
        }
        case EventKind::ToolExecutionStarted: {
            const std::string call_id = GetString(payload, "call_id");
            const auto it = index->calls.find(call_id);
            if (it != index->calls.end()) {
                state->tools[it->second].started = true;
                state->tools[it->second].started_event_id = envelope.event_id;
                // 子代理派工边:relations.child_run_id(§3.5)。
                const auto child = envelope.relations.find("child_run_id");
                if (child != envelope.relations.end() && child->is_string()) {
                    state->tools[it->second].child_run_id = child->get<std::string>();
                }
            }
            return true;
        }
        case EventKind::ToolExecutionFinished:
        case EventKind::ToolExecutionFailed:
        case EventKind::ToolExecutionCancelled:
        case EventKind::ToolExecutionUnknown: {
            const std::string call_id = envelope.call_id.value_or(GetString(payload, "call_id"));
            auto it = index->calls.find(call_id);
            if (it == index->calls.end()) {
                ReplayToolEntry entry;
                entry.call_id = call_id;
                state->tools.push_back(std::move(entry));
                it = index->calls.emplace(call_id, state->tools.size() - 1).first;
            }
            ReplayToolEntry& entry = state->tools[it->second];
            if (entry.terminal) {
                return true;  // 终态唯一,迟到不覆盖
            }
            entry.terminal = true;
            entry.terminal_kind = EventKindName(envelope.kind);
            entry.terminal_event_id = envelope.event_id;
            if (envelope.kind == EventKind::ToolExecutionFinished) {
                entry.outcome = GetString(payload, "outcome");
            } else {
                entry.outcome = GetString(payload, "reason");
            }
            if (envelope.kind == EventKind::ToolExecutionUnknown) {
                state->integrity.unknown_side_effects = true;
            }
            // 子代理边界:relations.child_run_id + 父侧记录的子账终态 hash
            //(§3.5;payload 键集封闭,hash 只住在 result_ref 里)。
            const auto child = envelope.relations.find("child_run_id");
            if (child != envelope.relations.end() && child->is_string()) {
                entry.child_run_id = child->get<std::string>();
                const auto ref = payload.find("result_ref");
                if (ref != payload.end() && ref->is_object()) {
                    entry.child_terminal_event_hash = GetString(*ref, "child_terminal_event_hash");
                }
            }
            return true;
        }
        case EventKind::ToolResultCommitted: {
            const auto it = index->calls.find(GetString(payload, "call_id"));
            if (it != index->calls.end()) {
                state->tools[it->second].result_committed = true;
            }
            ReplayMessage message;
            message.role = ReplayMessage::Role::Tool;
            message.origin = OriginName(envelope.origin);
            message.call_id = GetString(payload, "call_id");
            message.blocks = payload.contains("content") && payload["content"].is_array()
                                 ? payload["content"]
                                 : nlohmann::json::array();
            message.source_event_id = envelope.event_id;
            message.source_event_hash = envelope.event_hash;
            state->effective_conversation.push_back(std::move(message));
            return true;
        }
        case EventKind::ControlTitleChanged:
            state->control.title = GetString(payload, "title");
            return true;
        case EventKind::ControlCwdChanged:
            state->control.cwd = GetString(payload, "cwd");
            return true;
        case EventKind::ControlModeChanged:
            state->control.mode = GetString(payload, "mode");
            return true;
        case EventKind::ControlContextWindowChanged:
            state->control.context_window = GetUint(payload, "context_window");
            return true;
        case EventKind::CompactApplied:
            state->control.compact_epoch = static_cast<int>(GetUint(payload, "epoch"));
            state->control.last_compact_new_state_hash = GetString(payload, "new_state_hash");
            return true;
        case EventKind::ControlQueueItemEnqueued: {
            const std::string item_id = GetString(payload, "item_id");
            if (std::find(state->control.open_queue_items.begin(), state->control.open_queue_items.end(),
                          item_id) == state->control.open_queue_items.end()) {
                state->control.open_queue_items.push_back(item_id);
            }
            return true;
        }
        case EventKind::ControlQueueItemDequeued:
        case EventKind::ControlQueueItemCancelled:
        case EventKind::ControlQueueItemExpired: {
            const std::string item_id = GetString(payload, "item_id");
            auto& items = state->control.open_queue_items;
            items.erase(std::remove(items.begin(), items.end(), item_id), items.end());
            return true;
        }
        case EventKind::ControlQueueSnapshot: {
            // 快照即事实:开着的清单照快照重建。
            state->control.open_queue_items.clear();
            if (payload.contains("items") && payload["items"].is_array()) {
                for (const auto& item : payload["items"]) {
                    if (item.is_string()) {
                        state->control.open_queue_items.push_back(item.get<std::string>());
                    }
                }
            }
            return true;
        }
        case EventKind::RecordSelectionStarted:
            state->control.active_record_selection = GetString(payload, "record_id");
            return true;
        case EventKind::RecordSelectionCompleted:
        case EventKind::RecordSelectionCancelled:
        case EventKind::RecordSelectionInterrupted:
            if (state->control.active_record_selection == GetString(payload, "record_id")) {
                state->control.active_record_selection.clear();
            }
            return true;
        case EventKind::ResumeSourceAttached:
            state->control.resumed_from_session_id = GetString(payload, "source_session_id");
            return true;
        case EventKind::VerificationRecorded: {
            ReplayEvidenceEntry entry;
            entry.verification_id = GetString(payload, "verification_id");
            entry.kind = GetString(payload, "kind");
            entry.passed = GetBool(payload, "passed");
            entry.observed_after_seq = GetUint(payload, "observed_after_seq");
            state->evidence.push_back(std::move(entry));
            return true;
        }
        case EventKind::VerificationInvalidated: {
            const std::string id = GetString(payload, "verification_id");
            for (auto& entry : state->evidence) {
                if (entry.verification_id == id) {
                    entry.invalidated = true;
                }
            }
            return true;
        }
        case EventKind::OutcomeAssessed:
            // 终局裁决落到所属 turn(有 turn_id 时)。
            if (envelope.turn_id.has_value()) {
                const auto it = index->turns.find(*envelope.turn_id);
                if (it != index->turns.end()) {
                    state->turns[it->second].outcome = GetString(payload, "outcome");
                }
            }
            return true;
        case EventKind::RunEnvironmentCaptured: {
            // 环境快照的 blob 引用进 artifact 账(细账归 P0-4,这里先收 ref)。
            for (const auto& [key, value] : payload.items()) {
                CollectArtifactRefs(value, &state->artifact_refs);
            }
            return true;
        }
        case EventKind::ControlCommandRequested:
        case EventKind::ControlCommandCompleted:
        case EventKind::ControlCommandFailed:
        case EventKind::ControlCommandCancelled:
        case EventKind::ControlCommandRejected:
        case EventKind::ControlCheckpointCreated:
        case EventKind::ControlApprovalRequested:
        case EventKind::ControlApprovalResolved:
        case EventKind::ControlApprovalExpired:
        case EventKind::ControlCancellationRequested:
        case EventKind::ControlCancellationApplied:
        case EventKind::ContextPressureRecorded:
        case EventKind::CompactRequested:
        case EventKind::CompactRequestPrepared:
        case EventKind::CompactOutputGenerated:
        case EventKind::CompactValidationCompleted:
        case EventKind::CompactFailed:
        case EventKind::CompactCancelled:
        case EventKind::CompactRejected:
        case EventKind::RecordSelectionPaused:
        case EventKind::RecordSelectionResumed:
        case EventKind::RecordSelectionNoteAdded:
        case EventKind::VerificationStarted:
        // 子代理空轨迹单 P0-B:子账开张失败的父侧诊断事实,不改 effective
        // state(失败的派工没有子事实可折;父侧那枚调用的终态由它自己的
        // tool.execution.* 承载)。
        case EventKind::SubagentRunStartFailed:
            // 纯控制/生命周期事件:不改 effective state(§14.2 只记 command
            // lifecycle 的不动 ReplayState)。schema 已验过形状。
            return true;
    }
    // 新 kind 落了 enum 却没进折叠 switch:兜底明报 unsupported。
    *unsupported_reason = std::string("kind=") + EventKindName(envelope.kind) + " 未进折叠表";
    return false;
}

// 确定性投影:ReplayState -> canonical JSON(hash 输入)。只取向量与标量,
// 顺序全部由折叠序决定;wall/monotonic 时间不进。
nlohmann::json HashProjection(const ReplayState& state) {
    nlohmann::json out = nlohmann::json::object();
    out["workspace_key"] = state.workspace_key;
    out["session_id"] = state.session_id;
    out["run_id"] = state.run_id;
    out["run_kind"] = RunKindName(state.run_kind);
    out["start_reason"] = state.start_reason;
    out["run_terminal_state"] = state.run_terminal_state;
    out["session_end_state"] = state.session_end_state;
    out["next_session_id"] = state.next_session_id.value_or(std::string());
    out["folded_seq"] = state.folded_seq;
    out["integrity"] = state.integrity.ToJson();
    nlohmann::json turns = nlohmann::json::array();
    for (const auto& turn : state.turns) {
        turns.push_back(turn.ToJson());
    }
    out["turns"] = std::move(turns);
    nlohmann::json conversation = nlohmann::json::array();
    for (const auto& message : state.effective_conversation) {
        conversation.push_back(message.ToJson());
    }
    out["effective_conversation"] = std::move(conversation);
    nlohmann::json requests = nlohmann::json::array();
    for (const auto& step : state.requests) {
        requests.push_back(step.ToJson());
    }
    out["requests"] = std::move(requests);
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : state.tools) {
        tools.push_back(tool.ToJson());
    }
    out["tools"] = std::move(tools);
    out["control"] = state.control.ToJson();
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& entry : state.evidence) {
        evidence.push_back(entry.ToJson());
    }
    out["evidence"] = std::move(evidence);
    out["artifact_refs"] = state.artifact_refs;
    return out;
}

// checkpoint 文件路径:<session>/checkpoints/<stream_name>-<seq>.json。
std::filesystem::path CheckpointFilePath(const std::filesystem::path& session_dir,
                                         const std::string& stream_name, std::uint64_t seq) {
    return session_dir / "checkpoints" / (stream_name + "-" + std::to_string(seq) + ".json");
}

}  // namespace

// ---------------------------------------------------------------------------
// ToJson/FromJson(零件)
// ---------------------------------------------------------------------------

nlohmann::json ReplayMessage::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["role"] = role == Role::User ? "user" : (role == Role::Assistant ? "assistant" : "tool");
    out["origin"] = origin;
    if (call_id.has_value()) {
        out["call_id"] = *call_id;
    }
    out["blocks"] = blocks;
    out["source_event_id"] = source_event_id;
    out["source_event_hash"] = source_event_hash;
    return out;
}

std::optional<ReplayMessage> ReplayMessage::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayMessage message;
    const std::string role = GetString(json, "role");
    if (role == "user") {
        message.role = Role::User;
    } else if (role == "assistant") {
        message.role = Role::Assistant;
    } else if (role == "tool") {
        message.role = Role::Tool;
    } else {
        return std::nullopt;
    }
    message.origin = GetString(json, "origin");
    if (json.contains("call_id") && json["call_id"].is_string()) {
        message.call_id = json["call_id"].get<std::string>();
    }
    if (json.contains("blocks")) {
        message.blocks = json["blocks"];
    }
    message.source_event_id = GetString(json, "source_event_id");
    message.source_event_hash = GetString(json, "source_event_hash");
    return message;
}

nlohmann::json ReplayRequestStep::ToJson() const {
    return nlohmann::json{{"request_id", request_id},
                          {"prepared_event_id", prepared_event_id},
                          {"model", model},
                          {"provider", provider},
                          {"wire", wire},
                          {"purpose", purpose},
                          {"parameters", parameters},
                          {"message_refs", message_refs},
                          {"sent", sent},
                          {"output_state", output_state},
                          {"output_event_id", output_event_id},
                          {"output_blocks", output_blocks},
                          {"stop_reason", stop_reason},
                          {"usage_recorded", usage_recorded}};
}

std::optional<ReplayRequestStep> ReplayRequestStep::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayRequestStep step;
    step.request_id = GetString(json, "request_id");
    step.prepared_event_id = GetString(json, "prepared_event_id");
    step.model = GetString(json, "model");
    step.provider = GetString(json, "provider");
    step.wire = GetString(json, "wire");
    step.purpose = GetString(json, "purpose");
    if (json.contains("parameters") && json["parameters"].is_object()) {
        step.parameters = json["parameters"];
    }
    if (json.contains("message_refs") && json["message_refs"].is_array()) {
        for (const auto& ref : json["message_refs"]) {
            if (ref.is_string()) {
                step.message_refs.push_back(ref.get<std::string>());
            }
        }
    }
    step.sent = GetBool(json, "sent");
    step.output_state = GetString(json, "output_state");
    step.output_event_id = GetString(json, "output_event_id");
    if (json.contains("output_blocks")) {
        step.output_blocks = json["output_blocks"];
    }
    step.stop_reason = GetString(json, "stop_reason");
    step.usage_recorded = GetBool(json, "usage_recorded");
    return step;
}

nlohmann::json ReplayToolEntry::ToJson() const {
    return nlohmann::json{{"call_id", call_id},
                          {"tool_name", tool_name},
                          {"planned", planned},
                          {"effective", effective},
                          {"started", started},
                          {"terminal", terminal},
                          {"terminal_kind", terminal_kind},
                          {"outcome", outcome},
                          {"effective_arguments_sha256", effective_arguments_sha256},
                          {"result_committed", result_committed},
                          {"child_run_id", child_run_id.value_or(std::string())},
                          {"child_terminal_event_hash", child_terminal_event_hash},
                          {"started_event_id", started_event_id},
                          {"terminal_event_id", terminal_event_id}};
}

std::optional<ReplayToolEntry> ReplayToolEntry::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayToolEntry entry;
    entry.call_id = GetString(json, "call_id");
    entry.tool_name = GetString(json, "tool_name");
    entry.planned = GetBool(json, "planned");
    entry.effective = GetBool(json, "effective");
    entry.started = GetBool(json, "started");
    entry.terminal = GetBool(json, "terminal");
    entry.terminal_kind = GetString(json, "terminal_kind");
    entry.outcome = GetString(json, "outcome");
    entry.effective_arguments_sha256 = GetString(json, "effective_arguments_sha256");
    entry.result_committed = GetBool(json, "result_committed");
    const std::string child = GetString(json, "child_run_id");
    if (!child.empty()) {
        entry.child_run_id = child;
    }
    entry.child_terminal_event_hash = GetString(json, "child_terminal_event_hash");
    entry.started_event_id = GetString(json, "started_event_id");
    entry.terminal_event_id = GetString(json, "terminal_event_id");
    return entry;
}

nlohmann::json ReplayControlState::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["title"] = title.value_or(std::string());
    out["cwd"] = cwd.value_or(std::string());
    out["mode"] = mode.value_or(std::string());
    out["context_window"] = context_window.has_value() ? nlohmann::json(*context_window) : nlohmann::json();
    out["compact_epoch"] = compact_epoch;
    out["last_compact_new_state_hash"] = last_compact_new_state_hash;
    out["open_queue_items"] = open_queue_items;
    out["active_record_selection"] = active_record_selection;
    out["resumed_from_session_id"] = resumed_from_session_id.value_or(std::string());
    return out;
}

std::optional<ReplayControlState> ReplayControlState::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayControlState control;
    if (const std::string value = GetString(json, "title"); !value.empty()) {
        control.title = value;
    }
    if (const std::string value = GetString(json, "cwd"); !value.empty()) {
        control.cwd = value;
    }
    if (const std::string value = GetString(json, "mode"); !value.empty()) {
        control.mode = value;
    }
    const auto window = json.find("context_window");
    if (window != json.end() && window->is_number_unsigned()) {
        control.context_window = window->get<std::uint64_t>();
    }
    control.compact_epoch =
        json.contains("compact_epoch") && json["compact_epoch"].is_number_integer()
            ? json["compact_epoch"].get<int>()
            : 0;
    control.last_compact_new_state_hash = GetString(json, "last_compact_new_state_hash");
    if (json.contains("open_queue_items") && json["open_queue_items"].is_array()) {
        for (const auto& item : json["open_queue_items"]) {
            if (item.is_string()) {
                control.open_queue_items.push_back(item.get<std::string>());
            }
        }
    }
    control.active_record_selection = GetString(json, "active_record_selection");
    if (const std::string value = GetString(json, "resumed_from_session_id"); !value.empty()) {
        control.resumed_from_session_id = value;
    }
    return control;
}

nlohmann::json ReplayEvidenceEntry::ToJson() const {
    return nlohmann::json{{"verification_id", verification_id},
                          {"kind", kind},
                          {"passed", passed},
                          {"invalidated", invalidated},
                          {"observed_after_seq", observed_after_seq}};
}

std::optional<ReplayEvidenceEntry> ReplayEvidenceEntry::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayEvidenceEntry entry;
    entry.verification_id = GetString(json, "verification_id");
    entry.kind = GetString(json, "kind");
    entry.passed = GetBool(json, "passed");
    entry.invalidated = GetBool(json, "invalidated");
    entry.observed_after_seq = GetUint(json, "observed_after_seq");
    return entry;
}

nlohmann::json ReplayTurnEntry::ToJson() const {
    return nlohmann::json{{"turn_id", turn_id},
                          {"trigger", trigger},
                          {"terminal_state", terminal_state},
                          {"outcome", outcome}};
}

std::optional<ReplayTurnEntry> ReplayTurnEntry::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayTurnEntry turn;
    turn.turn_id = GetString(json, "turn_id");
    turn.trigger = GetString(json, "trigger");
    turn.terminal_state = GetString(json, "terminal_state");
    turn.outcome = GetString(json, "outcome");
    return turn;
}

nlohmann::json ReplayIntegrity::ToJson() const {
    return nlohmann::json{{"events_folded", events_folded},
                          {"last_event_hash", last_event_hash},
                          {"truncated_tail", truncated_tail},
                          {"unsupported", unsupported},
                          {"unsupported_reason", unsupported_reason},
                          {"dangling_tools", dangling_tools},
                          {"unknown_side_effects", unknown_side_effects}};
}

std::optional<ReplayIntegrity> ReplayIntegrity::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayIntegrity integrity;
    integrity.events_folded = GetUint(json, "events_folded");
    integrity.last_event_hash = GetString(json, "last_event_hash");
    integrity.truncated_tail = GetBool(json, "truncated_tail");
    integrity.unsupported = GetBool(json, "unsupported");
    integrity.unsupported_reason = GetString(json, "unsupported_reason");
    integrity.dangling_tools = GetUint(json, "dangling_tools");
    integrity.unknown_side_effects = GetBool(json, "unknown_side_effects");
    return integrity;
}

nlohmann::json ReplayState::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["schema"] = "lubancode.trajectory.replay_state";
    out["replay_version"] = kReplayProjectionVersion;
    out["workspace_key"] = workspace_key;
    out["session_id"] = session_id;
    out["run_id"] = run_id;
    out["run_kind"] = RunKindName(run_kind);
    out["start_reason"] = start_reason;
    out["run_terminal_state"] = run_terminal_state;
    out["session_end_state"] = session_end_state;
    out["next_session_id"] = next_session_id.value_or(std::string());
    nlohmann::json turns = nlohmann::json::array();
    for (const auto& turn : this->turns) {
        turns.push_back(turn.ToJson());
    }
    out["turns"] = std::move(turns);
    nlohmann::json conversation = nlohmann::json::array();
    for (const auto& message : effective_conversation) {
        conversation.push_back(message.ToJson());
    }
    out["effective_conversation"] = std::move(conversation);
    nlohmann::json requests = nlohmann::json::array();
    for (const auto& step : this->requests) {
        requests.push_back(step.ToJson());
    }
    out["requests"] = std::move(requests);
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : this->tools) {
        tools.push_back(tool.ToJson());
    }
    out["tools"] = std::move(tools);
    out["control"] = control.ToJson();
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& entry : this->evidence) {
        evidence.push_back(entry.ToJson());
    }
    out["evidence"] = std::move(evidence);
    out["artifact_refs"] = artifact_refs;
    out["integrity"] = integrity.ToJson();
    out["folded_seq"] = folded_seq;
    return out;
}

std::optional<ReplayState> ReplayState::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayState state;
    state.workspace_key = GetString(json, "workspace_key");
    state.session_id = GetString(json, "session_id");
    state.run_id = GetString(json, "run_id");
    const auto kind = RunKindFromName(GetString(json, "run_kind"));
    state.run_kind = kind.value_or(RunKind::MainSession);
    state.start_reason = GetString(json, "start_reason");
    state.run_terminal_state = GetString(json, "run_terminal_state");
    state.session_end_state = GetString(json, "session_end_state");
    if (const std::string next = GetString(json, "next_session_id"); !next.empty()) {
        state.next_session_id = next;
    }
    if (json.contains("turns") && json["turns"].is_array()) {
        for (const auto& entry : json["turns"]) {
            if (const auto turn = ReplayTurnEntry::FromJson(entry)) {
                state.turns.push_back(*turn);
            }
        }
    }
    if (json.contains("effective_conversation") && json["effective_conversation"].is_array()) {
        for (const auto& entry : json["effective_conversation"]) {
            if (const auto message = ReplayMessage::FromJson(entry)) {
                state.effective_conversation.push_back(*message);
            }
        }
    }
    if (json.contains("requests") && json["requests"].is_array()) {
        for (const auto& entry : json["requests"]) {
            if (const auto step = ReplayRequestStep::FromJson(entry)) {
                state.requests.push_back(*step);
            }
        }
    }
    if (json.contains("tools") && json["tools"].is_array()) {
        for (const auto& entry : json["tools"]) {
            if (const auto tool = ReplayToolEntry::FromJson(entry)) {
                state.tools.push_back(*tool);
            }
        }
    }
    if (const auto control =
            ReplayControlState::FromJson(json.value("control", nlohmann::json::object()))) {
        state.control = *control;
    }
    if (json.contains("evidence") && json["evidence"].is_array()) {
        for (const auto& entry : json["evidence"]) {
            if (const auto evidence = ReplayEvidenceEntry::FromJson(entry)) {
                state.evidence.push_back(*evidence);
            }
        }
    }
    if (json.contains("artifact_refs") && json["artifact_refs"].is_array()) {
        for (const auto& ref : json["artifact_refs"]) {
            if (ref.is_string()) {
                state.artifact_refs.push_back(ref.get<std::string>());
            }
        }
    }
    if (const auto integrity =
            ReplayIntegrity::FromJson(json.value("integrity", nlohmann::json::object()))) {
        state.integrity = *integrity;
    }
    state.folded_seq = GetUint(json, "folded_seq");
    return state;
}

// ---------------------------------------------------------------------------
// 折叠入口
// ---------------------------------------------------------------------------

ReplayReport FoldStreamReplay(const std::filesystem::path& stream_path) {
    ReplayReport report;
    const auto verify = VerifyJournalFile(stream_path);
    if (!verify.ok && !verify.truncated_tail) {
        // 链断/schema 坏:不折(§16.3 前缀 replay 只对"尾行截断"这一种
        // 可恢复缺口开;坏行后面的账没法证连续)。
        report.error_code = "replay.verify_failed";
        report.message = verify.error_code + ": " + verify.message;
        return report;
    }
    const auto lines = ReadJournalLines(stream_path);
    if (!lines.has_value()) {
        report.error_code = "replay.read_failed";
        report.message = "读不开 Journal";
        return report;
    }
    ReplayState state;
    FoldIndex index;
    // 只折已验证前缀:verify.events 是过账事件数,截断/坏尾行不计。
    const std::size_t fold_count = std::min<std::size_t>(static_cast<std::size_t>(verify.events),
                                                         lines->size());
    for (std::size_t i = 0; i < fold_count; ++i) {
        const auto parsed = nlohmann::json::parse((*lines)[i], nullptr, false);
        EventEnvelope envelope;
        if (parsed.is_discarded() || ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            report.error_code = "replay.verify_failed";
            report.message = "前缀事件验不过: 行 " + std::to_string(i + 1);
            return report;
        }
        std::string unsupported_reason;
        if (!FoldEvent(envelope, &state, &index, &unsupported_reason)) {
            report.error_code = "replay.unsupported";
            report.message = unsupported_reason;
            state.integrity.unsupported = true;
            state.integrity.unsupported_reason = unsupported_reason;
            report.state = std::move(state);
            return report;
        }
        state.folded_seq = envelope.seq;
        state.integrity.events_folded = envelope.seq;
        state.integrity.last_event_hash = envelope.event_hash;
    }
    state.integrity.truncated_tail = verify.truncated_tail;
    state.integrity.dangling_tools = static_cast<std::uint64_t>(CollectDanglingTools(state).size());
    report.state = std::move(state);
    return report;
}

std::string ComputeReplayStateHash(const ReplayState& state) {
    const auto canonical = CanonicalJsonDump(HashProjection(state));
    const std::string prefix = "replay-v" + std::to_string(kReplayProjectionVersion) + "\n";
    return hooks::Sha256Hex(prefix + canonical.value_or(std::string("<uncanonicalizable>")));
}

// ---------------------------------------------------------------------------
// checkpoint
// ---------------------------------------------------------------------------

nlohmann::json ReplayCheckpoint::ToJson() const {
    return nlohmann::json{{"schema", "lubancode.trajectory.replay_checkpoint"},
                          {"schema_version", schema_version},
                          {"replay_version", replay_version},
                          {"stream_name", stream_name},
                          {"source_seq", source_seq},
                          {"source_event_hash", source_event_hash},
                          {"state_hash", state_hash},
                          {"folded_state", folded.ToJson()}};
}

std::optional<ReplayCheckpoint> ReplayCheckpoint::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ReplayCheckpoint checkpoint;
    checkpoint.schema_version =
        json.contains("schema_version") && json["schema_version"].is_number_integer()
            ? json["schema_version"].get<int>()
            : 1;
    checkpoint.replay_version =
        json.contains("replay_version") && json["replay_version"].is_number_integer()
            ? json["replay_version"].get<int>()
            : 0;
    checkpoint.stream_name = GetString(json, "stream_name");
    checkpoint.source_seq = GetUint(json, "source_seq");
    checkpoint.source_event_hash = GetString(json, "source_event_hash");
    checkpoint.state_hash = GetString(json, "state_hash");
    const auto folded = json.find("folded_state");
    if (folded == json.end()) {
        return std::nullopt;
    }
    auto state = ReplayState::FromJson(*folded);
    if (!state.has_value()) {
        return std::nullopt;
    }
    checkpoint.folded = std::move(*state);
    return checkpoint;
}

std::expected<void, std::string> WriteReplayCheckpoint(const std::filesystem::path& session_dir,
                                                       const ReplayCheckpoint& checkpoint) {
    // 统一原子写(审计 P1):同 seq 重写允许(幂等),平台原子替换替掉旧写法
    // 的"先删旧再换新"。checkpoint 本就是缓存,断档至多回退从头折叠,
    // 不动 canonical 账。
    const auto path = CheckpointFilePath(session_dir, checkpoint.stream_name, checkpoint.source_seq);
    const auto written = platform::AtomicWriteFile(path, checkpoint.ToJson().dump());
    if (!written.has_value()) {
        return std::unexpected("checkpoint 原子写失败: " + written.error().message);
    }
    return {};
}

std::optional<ReplayCheckpoint> FindLatestUsableCheckpoint(const std::filesystem::path& session_dir,
                                                           const std::string& stream_name,
                                                           const std::filesystem::path& stream_path) {
    const auto dir = session_dir / "checkpoints";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return std::nullopt;
    }
    // 候选:同 stream 的全部 checkpoint 文件,按 seq 降序取高水位。
    std::vector<std::uint64_t> seqs;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
            continue;
        }
        const std::string stem = entry.path().stem().generic_string();
        const std::string prefix = stream_name + "-";
        if (stem.rfind(prefix, 0) != 0) {
            continue;
        }
        const std::string number = stem.substr(prefix.size());
        if (!number.empty() && std::all_of(number.begin(), number.end(),
                                           [](char c) { return c >= '0' && c <= '9'; })) {
            seqs.push_back(static_cast<std::uint64_t>(std::stoull(number)));
        }
    }
    std::sort(seqs.begin(), seqs.end(), std::greater<std::uint64_t>());
    if (seqs.empty()) {
        return std::nullopt;
    }
    // Journal 第 seq 枚事件的 hash:checkpoint 对不上便丢(§10.4 末段)。
    const auto lines = ReadJournalLines(stream_path);
    if (!lines.has_value()) {
        return std::nullopt;
    }
    const auto hash_at = [&lines](std::uint64_t seq) -> std::string {
        if (seq == 0 || seq > lines->size()) {
            return std::string();
        }
        const auto parsed =
            nlohmann::json::parse((*lines)[static_cast<std::size_t>(seq - 1)], nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("event_hash")) {
            return std::string();
        }
        return parsed["event_hash"].get<std::string>();
    };
    for (const std::uint64_t seq : seqs) {
        std::ifstream file(CheckpointFilePath(session_dir, stream_name, seq), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
        if (json.is_discarded()) {
            continue;
        }
        auto checkpoint = ReplayCheckpoint::FromJson(json);
        if (!checkpoint.has_value()) {
            continue;
        }
        if (checkpoint->replay_version != kReplayProjectionVersion || checkpoint->source_seq != seq) {
            continue;  // 版本/文件名不符:作废
        }
        if (hash_at(seq) != checkpoint->source_event_hash) {
            continue;  // Journal 已变:作废
        }
        // 折叠账自洽:账内高水位与 checkpoint 声明一致。
        if (checkpoint->folded.folded_seq != seq ||
            checkpoint->folded.integrity.last_event_hash != checkpoint->source_event_hash) {
            continue;
        }
        return checkpoint;
    }
    return std::nullopt;
}

bool ContinueFoldFrom(const std::filesystem::path& stream_path, ReplayState* state, std::string* error) {
    const auto lines = ReadJournalLines(stream_path);
    if (!lines.has_value()) {
        *error = "replay.read_failed";
        return false;
    }
    FoldIndex index;
    RebuildIndex(*state, &index);
    for (std::size_t i = 0; i < lines->size(); ++i) {
        const auto parsed = nlohmann::json::parse((*lines)[i], nullptr, false);
        EventEnvelope envelope;
        if (parsed.is_discarded() || ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            *error = "replay.verify_failed: 行 " + std::to_string(i + 1);
            return false;
        }
        if (envelope.seq <= state->folded_seq) {
            continue;  // checkpoint 已折的前缀
        }
        std::string unsupported_reason;
        if (!FoldEvent(envelope, state, &index, &unsupported_reason)) {
            state->integrity.unsupported = true;
            state->integrity.unsupported_reason = unsupported_reason;
            *error = "replay.unsupported: " + unsupported_reason;
            return false;
        }
        state->folded_seq = envelope.seq;
        state->integrity.events_folded = envelope.seq;
        state->integrity.last_event_hash = envelope.event_hash;
    }
    state->integrity.dangling_tools = static_cast<std::uint64_t>(CollectDanglingTools(*state).size());
    return true;
}

std::vector<ReplayDanglingTool> CollectDanglingTools(const ReplayState& state) {
    std::vector<ReplayDanglingTool> out;
    for (const auto& tool : state.tools) {
        // 子代理派工的收口以子账 run terminal 为准,不按 result_committed 判悬空。
        if (tool.child_run_id.has_value()) {
            continue;
        }
        ReplayDanglingTool dangling;
        dangling.call_id = tool.call_id;
        dangling.tool_name = tool.tool_name;
        if (!tool.terminal) {
            dangling.state = tool.started ? "started_no_terminal" : "planned_not_started";
        } else if (!tool.result_committed) {
            dangling.state = "terminal_no_result";
        } else {
            continue;
        }
        dangling.unknown_side_effect = tool.UnknownSideEffect();
        out.push_back(std::move(dangling));
    }
    return out;
}

// ---------------------------------------------------------------------------
// session verifier
// ---------------------------------------------------------------------------

nlohmann::json VerifiedStream::ToJson() const {
    return nlohmann::json{{"relative_path", relative_path},
                          {"ok", ok},
                          {"error_code", error_code},
                          {"message", message},
                          {"run_id", run_id},
                          {"run_kind", RunKindName(run_kind)},
                          {"events", events},
                          {"last_event_hash", last_event_hash},
                          {"run_terminal", run_terminal},
                          {"terminal_kind", terminal_kind}};
}

nlohmann::json ChildEdgeReport::ToJson() const {
    return nlohmann::json{{"child_run_id", child_run_id},
                          {"parent_run_id", parent_run_id},
                          {"parent_call_id", parent_call_id},
                          {"background_spawn", background_spawn},
                          {"child_stream_found", child_stream_found},
                          {"child_has_terminal", child_has_terminal},
                          {"child_terminal_hash", child_terminal_hash},
                          {"parent_recorded_hash", parent_recorded_hash},
                          {"owner_matches", owner_matches},
                          {"spawn_reference_found", spawn_reference_found},
                          {"dispatch_on_started", dispatch_on_started},
                          {"hash_matches", hash_matches},
                          {"accepted_once", accepted_once},
                          {"error_code", error_code}};
}

namespace {

// 一份 stream 的骨架事实 + 父子边素材(单遍扫描;与 replay 折叠互不依赖)。
struct StreamScan {
    VerifiedStream verified;
    std::string parent_run_id;             // 子账 run.started relations.parent_run_id
    std::string parent_call_id;            // 子账 relations.parent_call_id(前台派工才有)
    std::string last_terminal_event_hash;  // run 终态事件 hash(无则末事件 hash)
    // 父侧派发素材:child_run_id -> {call_id, 记录 hash, 接受次数, started}。
    struct ParentDispatch {
        std::string call_id;
        std::string recorded_hash;
        int accepts = 0;
        bool started_seen = false;
    };
    std::map<std::string, ParentDispatch> dispatches;
    // 任务 turn 账素材(§13.7,P1-1):sent 边界的 turn 坐标逐枚 + 输出三态
    // 的 request_id 集,扫描收尾一并核对。
    struct TurnSequence {
        std::string request_id;
        int task_turn_index = 0;
        int turn_limit = 0;
        bool after_terminal = false;  // sent 落在 run 终态之后(账面破)
    };
    std::vector<TurnSequence> turns;
    std::set<std::string> request_outputs;
};

StreamScan ScanStream(const std::filesystem::path& session_dir, const std::filesystem::path& path) {
    StreamScan scan;
    std::error_code ec;
    scan.verified.relative_path = std::filesystem::relative(path, session_dir, ec).generic_string();
    if (scan.verified.relative_path.empty()) {
        scan.verified.relative_path = path.filename().generic_string();
    }
    const auto report = VerifyJournalFile(path);
    scan.verified.ok = report.ok;
    scan.verified.error_code = report.error_code;
    scan.verified.events = report.events;
    scan.verified.last_event_hash = report.last_event_hash;
    const auto lines = ReadJournalLines(path);
    if (!lines.has_value()) {
        scan.verified.ok = false;
        if (scan.verified.error_code.empty()) {
            scan.verified.error_code = "verify.read_failed";
        }
        return scan;
    }
    const std::size_t count =
        report.truncated_tail
            ? std::min<std::size_t>(static_cast<std::size_t>(report.events), lines->size())
            : lines->size();
    bool terminal_seen = false;  // run 终态是否已扫过(turn 序列核对用)
    for (std::size_t i = 0; i < count; ++i) {
        const auto parsed = nlohmann::json::parse((*lines)[i], nullptr, false);
        if (parsed.is_discarded()) {
            continue;
        }
        EventEnvelope envelope;
        if (ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            continue;  // verify 已报;骨架扫描不重复报
        }
        scan.verified.run_id = envelope.run_id;
        scan.verified.run_kind = envelope.run_kind;
        if (envelope.kind == EventKind::RunStarted) {
            const auto parent = envelope.relations.find("parent_run_id");
            if (parent != envelope.relations.end() && parent->is_string()) {
                scan.parent_run_id = parent->get<std::string>();
            }
            const auto parent_call = envelope.relations.find("parent_call_id");
            if (parent_call != envelope.relations.end() && parent_call->is_string()) {
                scan.parent_call_id = parent_call->get<std::string>();
            }
            // 回滚纪律(§十七:超前的 min_reader_version 明拒,不判 corrupt、
            // 不猜着读):verifier 与 replay 同一道门——旧 binary 遇新账,
            // verify 也得报 unsupported,不能只验链路后装作可读。链路本身
            // 已经红了的流不盖错码(红因照旧报)。
            if (scan.verified.ok) {
                const auto min_reader = envelope.payload.find("min_reader_version");
                if (min_reader != envelope.payload.end() && min_reader->is_number_unsigned() &&
                    min_reader->get<std::uint64_t>() >
                        static_cast<std::uint64_t>(kMaxEnvelopeSchemaVersion)) {
                    scan.verified.ok = false;
                    scan.verified.error_code = "verify.unsupported_reader_version";
                }
            }
        }
        if (envelope.kind == EventKind::RunCompleted || envelope.kind == EventKind::RunFailed ||
            envelope.kind == EventKind::RunCancelled) {
            scan.verified.run_terminal = true;
            scan.verified.terminal_kind = EventKindName(envelope.kind);
            scan.last_terminal_event_hash = envelope.event_hash;
            terminal_seen = true;
        }
        // 父侧派发边:relations.child_run_id(§3.5)。
        const auto child = envelope.relations.find("child_run_id");
        if (child != envelope.relations.end() && child->is_string()) {
            const std::string child_run_id = child->get<std::string>();
            StreamScan::ParentDispatch& dispatch = scan.dispatches[child_run_id];
            dispatch.call_id = envelope.call_id.value_or(std::string());
            if (envelope.kind == EventKind::ToolExecutionStarted) {
                dispatch.started_seen = true;
            }
            if (envelope.kind == EventKind::ToolExecutionFinished ||
                envelope.kind == EventKind::ToolExecutionFailed ||
                envelope.kind == EventKind::ToolExecutionCancelled ||
                envelope.kind == EventKind::ToolExecutionUnknown) {
                dispatch.accepts += 1;
                const auto ref = envelope.payload.find("result_ref");
                if (ref != envelope.payload.end() && ref->is_object()) {
                    dispatch.recorded_hash = GetString(*ref, "child_terminal_event_hash");
                }
            }
            // workflow 编排账的 node 终态(workflow 会话归属统一单):同样算
            // 一次接受;hash 走 payload.child_terminal_event_hash(编排事实
            // 不开 result_ref,键名直给)。
            if ((envelope.kind == EventKind::WorkflowNodeCompleted ||
                 envelope.kind == EventKind::WorkflowNodeFailed) &&
                child->is_string()) {
                dispatch.accepts += 1;
                const auto hash = envelope.payload.find("child_terminal_event_hash");
                if (hash != envelope.payload.end() && hash->is_string() &&
                    !hash->get<std::string>().empty()) {
                    dispatch.recorded_hash = hash->get<std::string>();
                }
            }
        }
        // 任务级 turn 账(§13.7,P1-1):sent 边界的 task_turn_index 逐枚收账,
        // 收口三态按 request_id 记终态——收尾一并核"不重号、不超 limit、
        // 终态后无悬空请求"。旧 stream 没有这些键,序列为空,核账自然跳过。
        if (envelope.kind == EventKind::ModelRequestSent) {
            if (const auto index = envelope.payload.find("task_turn_index");
                index != envelope.payload.end() && index->is_number_unsigned()) {
                StreamScan::TurnSequence entry;
                entry.request_id = envelope.request_id.value_or(std::string());
                entry.task_turn_index = index->get<int>();
                entry.after_terminal = terminal_seen;
                const auto limit = envelope.payload.find("turn_limit");
                if (limit != envelope.payload.end() && limit->is_number_unsigned()) {
                    entry.turn_limit = limit->get<int>();
                }
                scan.turns.push_back(entry);
            }
        }
        if (envelope.kind == EventKind::ModelOutputCompleted || envelope.kind == EventKind::ModelOutputFailed ||
            envelope.kind == EventKind::ModelOutputCancelled) {
            const std::string request_id = envelope.request_id.value_or(std::string());
            if (!request_id.empty()) {
                scan.request_outputs.insert(request_id);
            }
        }
    }
    // ---- 任务 turn 序列核对(§13.7)------------------------------------------
    if (!scan.turns.empty()) {
        int expected = 1;
        for (const auto& entry : scan.turns) {
            if (entry.task_turn_index != expected) {
                scan.verified.ok = false;
                if (scan.verified.error_code.empty()) {
                    scan.verified.error_code =
                        entry.task_turn_index < expected ? "turn.index_repeated" : "turn.index_skipped";
                    scan.verified.message = "model.request.sent 的 task_turn_index 不从 1 起严格递增(期望 " +
                                             std::to_string(expected) + ",实得 " +
                                             std::to_string(entry.task_turn_index) + ")";
                }
                break;
            }
            if (entry.turn_limit > 0 && entry.task_turn_index > entry.turn_limit) {
                scan.verified.ok = false;
                if (scan.verified.error_code.empty()) {
                    scan.verified.error_code = "turn.index_over_limit";
                    scan.verified.message = "task_turn_index " + std::to_string(entry.task_turn_index) +
                                             " 越过 turn_limit " + std::to_string(entry.turn_limit);
                }
                break;
            }
            // 终态后不得再出现新的 turn 请求:run terminal 之后的 sent 直接红。
            if (entry.after_terminal) {
                scan.verified.ok = false;
                if (scan.verified.error_code.empty()) {
                    scan.verified.error_code = "turn.sent_after_terminal";
                    scan.verified.message = "run 终态之后还有 task_turn_index " +
                                             std::to_string(entry.task_turn_index) + " 的请求边界";
                }
                break;
            }
            ++expected;
        }
        // 悬空核对:run 已终态,而某枚带 turn 坐标的请求没有对应的输出三态
        //(completed/failed/cancelled 任一)——started 无收口,账面破。
        if (scan.verified.ok && scan.verified.run_terminal) {
            for (const auto& entry : scan.turns) {
                if (!entry.request_id.empty() && scan.request_outputs.count(entry.request_id) == 0) {
                    scan.verified.ok = false;
                    scan.verified.error_code = "turn.request_dangling";
                    scan.verified.message = "task_turn_index " + std::to_string(entry.task_turn_index) +
                                             " 的请求只有 sent 边界,没有 completed/failed/cancelled 收口";
                    break;
                }
            }
        }
    }
    if (scan.last_terminal_event_hash.empty()) {
        scan.last_terminal_event_hash = report.last_event_hash;
    }
    return scan;
}

}  // namespace

SessionVerifyReport VerifySessionDir(const std::filesystem::path& session_dir) {
    SessionVerifyReport report;
    std::error_code ec;
    if (!std::filesystem::is_directory(session_dir, ec)) {
        report.error_code = "verify.no_session_dir";
        report.message = "session 目录不存在";
        return report;
    }
    // 采集 stream 清单:main + subagents + workflow/node + goal/loop(§3.1)。
    std::vector<std::filesystem::path> paths;
    const auto main_path = session_dir / "main.jsonl";
    if (std::filesystem::exists(main_path, ec)) {
        paths.push_back(main_path);
    }
    const auto scan_dir = [&paths](const std::filesystem::path& dir) {
        std::error_code inner_ec;
        if (!std::filesystem::exists(dir, inner_ec)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, inner_ec)) {
            if (entry.is_regular_file(inner_ec) && entry.path().extension() == ".jsonl") {
                paths.push_back(entry.path());
            }
        }
    };
    scan_dir(session_dir / "subagents");
    scan_dir(session_dir / "goals");
    scan_dir(session_dir / "loops");
    const auto workflows = session_dir / "workflows";
    if (std::filesystem::exists(workflows, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
            if (!run.is_directory(ec)) {
                continue;
            }
            const auto stream = run.path() / "workflow.jsonl";
            if (std::filesystem::exists(stream, ec)) {
                paths.push_back(stream);
            }
            scan_dir(run.path() / "nodes");
        }
    }
    // 排序稳定(目录迭代序跨平台不定):相对路径字典序。
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        report.error_code = "verify.no_streams";
        report.message = "session 目录里没有 JSONL";
        return report;
    }

    std::map<std::string, StreamScan> scans;  // run_id -> scan
    bool all_ok = true;
    for (const auto& path : paths) {
        StreamScan scan = ScanStream(session_dir, path);
        if (!scan.verified.ok) {
            all_ok = false;
        }
        report.streams.push_back(scan.verified);
        report.run_kinds[scan.verified.run_id] = RunKindName(scan.verified.run_kind);
        scans[scan.verified.run_id] = std::move(scan);
    }

    // 父子边交叉核(§3.9):以子账为纲——每份带 owner 的非 main stream 一条边。
    for (const auto& [run_id, scan] : scans) {
        if (run_id.empty() || scan.parent_run_id.empty()) {
            continue;  // main 或无主孤儿:链验过即可,边核跳过
        }
        ChildEdgeReport edge;
        edge.child_run_id = run_id;
        edge.parent_run_id = scan.parent_run_id;
        edge.parent_call_id = scan.parent_call_id;
        // 后台判别(§3.5/P0-2 后台派工合同):子账 relations 不带
        // parent_call_id = 后台——父轮收口在先,父账不落派发边,terminal
        // hash 由 verifier 实读子文件回填核对。
        edge.background_spawn = scan.parent_call_id.empty();
        const auto parent_it = scans.find(scan.parent_run_id);
        const bool parent_exists = parent_it != scans.end();
        // 1. child owner 与 parent run 对得上。
        edge.owner_matches = parent_exists;
        // 2. 父账确有派发引用(started 带 relations.child_run_id)——前台
        // 派工的硬要求;后台派工父账不落边,不算坏账。
        edge.spawn_reference_found = false;
        if (parent_exists) {
            const auto dispatch = parent_it->second.dispatches.find(run_id);
            if (dispatch != parent_it->second.dispatches.end()) {
                // P0-2(生产时序收口):父侧派发引用认"任何带
                // relations.child_run_id 的事件"——started 先于派工落地是
                // 生产时序的常态(agent 工具的子 run id 在 execute 里才
                // 出生,AttachChildRun 只来得及挂在终态上)。started_seen
                // 仍如实上报;双向引用(parent_call_id/child hash)照旧逐位
                // 对账,不因挂点后移而放松。
                edge.spawn_reference_found = true;
                edge.dispatch_on_started = dispatch->second.started_seen;
                if (edge.parent_call_id.empty()) {
                    edge.parent_call_id = dispatch->second.call_id;
                }
                edge.parent_recorded_hash = dispatch->second.recorded_hash;
                edge.accepted_once = dispatch->second.accepts == 1;
            }
        }
        // 3/4. child 终态与 hash:子文件实读回填(P0-2 遗留#5),父侧记了
        // hash 才逐位比对。
        edge.child_stream_found = true;  // 扫描来源就是子文件本身
        edge.child_has_terminal = scan.verified.run_terminal;
        edge.child_terminal_hash = scan.last_terminal_event_hash;
        if (edge.parent_recorded_hash.empty()) {
            // 父侧没记 hash(后台派工/父轮先收口):核"子账有终态"即过。
            edge.hash_matches = true;
        } else {
            edge.hash_matches = edge.parent_recorded_hash == scan.last_terminal_event_hash;
        }
        if (!edge.owner_matches) {
            edge.error_code = "edge.owner_mismatch";
        } else if (!edge.background_spawn && !edge.spawn_reference_found) {
            // 前台派工却无父侧边:账缺了派发事实,明报。
            edge.error_code = "edge.no_parent_dispatch";
        } else if (!edge.child_has_terminal && !edge.parent_recorded_hash.empty()) {
            // 父侧记了 hash、子账却没有终态:那枚 hash 无从对起。
            edge.error_code = "edge.child_not_terminal";
        } else if (!edge.hash_matches) {
            edge.error_code = "edge.child_hash_mismatch";
        } else {
            // 5. 同一 child 结果至多接受一次(后台未接受不算坏账,只标未收)。
            if (parent_exists) {
                const auto dispatch = parent_it->second.dispatches.find(run_id);
                if (dispatch != parent_it->second.dispatches.end() && dispatch->second.accepts > 1) {
                    edge.error_code = "edge.double_accept";
                }
            }
        }
        if (!edge.error_code.empty()) {
            all_ok = false;
        }
        report.child_edges.push_back(std::move(edge));
    }

    // 孤儿检查:父账声明派发、session 里却没有子文件的,明报。
    for (const auto& [run_id, scan] : scans) {
        for (const auto& [child_run_id, dispatch] : scan.dispatches) {
            if (scans.count(child_run_id) == 0) {
                ChildEdgeReport edge;
                edge.child_run_id = child_run_id;
                edge.parent_run_id = run_id;
                edge.parent_call_id = dispatch.call_id;
                edge.child_stream_found = false;
                edge.spawn_reference_found = dispatch.started_seen;
                edge.parent_recorded_hash = dispatch.recorded_hash;
                edge.accepted_once = dispatch.accepts == 1;
                edge.error_code = "edge.child_stream_missing";
                report.child_edges.push_back(std::move(edge));
                all_ok = false;
            }
        }
    }

    report.ok = all_ok;
    if (!all_ok && report.error_code.empty()) {
        report.error_code = "verify.session_failed";
    }
    return report;
}

}  // namespace lubancode::trajectory
