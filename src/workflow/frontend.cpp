// Workflow 多前端合同实现(自然语言编排单第 6 批)。

#include "workflow/frontend.hpp"

#include <algorithm>

#include "platform/paths.hpp"

namespace lubancode::workflow {

nlohmann::json WorkflowRunSnapshot::ToJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    j["run_id"] = run_id;
    j["workflow_id"] = workflow_id;
    j["workflow_version"] = workflow_version;
    j["content_hash"] = content_hash;
    j["state"] = state;
    j["error_code"] = error_code;
    j["error_message"] = error_message;
    j["result"] = result;
    j["unavailable_sources"] = unavailable_sources;
    j["duration_ms"] = duration_ms;
    j["tokens_used"] = tokens_used;
    j["last_seq"] = last_seq;
    nlohmann::ordered_json nodes = nlohmann::ordered_json::object();
    for (const auto& [id, node] : this->nodes) {
        nodes[id] = node;
    }
    j["nodes"] = std::move(nodes);
    return j;
}

std::optional<WorkflowRunSnapshot> WorkflowRunSnapshot::FromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    WorkflowRunSnapshot snapshot;
    const auto str = [&](const char* key) {
        const auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    snapshot.run_id = str("run_id");
    snapshot.workflow_id = str("workflow_id");
    snapshot.workflow_version = str("workflow_version");
    snapshot.content_hash = str("content_hash");
    snapshot.state = str("state");
    snapshot.error_code = str("error_code");
    snapshot.error_message = str("error_message");
    if (const auto it = j.find("result"); it != j.end() && it->is_object()) snapshot.result = *it;
    if (const auto it = j.find("unavailable_sources"); it != j.end() && it->is_array()) {
        for (const auto& s : *it) {
            if (s.is_string()) snapshot.unavailable_sources.push_back(s.get<std::string>());
        }
    }
    if (const auto it = j.find("duration_ms"); it != j.end() && it->is_number()) {
        snapshot.duration_ms = it->get<std::int64_t>();
    }
    if (const auto it = j.find("tokens_used"); it != j.end() && it->is_number()) {
        snapshot.tokens_used = it->get<std::int64_t>();
    }
    if (const auto it = j.find("last_seq"); it != j.end() && it->is_number_unsigned()) {
        snapshot.last_seq = it->get<std::uint64_t>();
    }
    if (const auto it = j.find("nodes"); it != j.end() && it->is_object()) {
        for (auto node = it->begin(); node != it->end(); ++node) {
            snapshot.nodes.emplace(node.key(), *node);
        }
    }
    return snapshot;
}

WorkflowRunSnapshot BuildSnapshot(const WorkflowRunSummary& summary, const std::vector<JournalEvent>& events) {
    WorkflowRunSnapshot snapshot;
    snapshot.run_id = summary.run_id;
    snapshot.workflow_id = summary.workflow_id;
    snapshot.state = ToString(summary.state);
    snapshot.error_code = summary.error_code;
    snapshot.error_message = summary.error_message;
    snapshot.result = summary.result;
    snapshot.unavailable_sources = summary.unavailable_sources;
    snapshot.duration_ms = summary.duration_ms;
    snapshot.tokens_used = summary.tokens_used;
    for (const auto& event : events) {
        snapshot.last_seq = std::max(snapshot.last_seq, event.seq);
    }
    for (const auto& [id, record] : summary.nodes) {
        nlohmann::ordered_json node = nlohmann::ordered_json::object();
        node["state"] = ToString(record.state);
        node["attempt"] = record.attempt;
        node["error_code"] = record.error_code;
        node["duration_ms"] = record.ended_ms > 0 ? record.ended_ms - record.started_ms : 0;
        snapshot.nodes.emplace(id, std::move(node));
    }
    return snapshot;
}

std::optional<WorkflowRunSnapshot> LoadSnapshotFromDisk(const std::filesystem::path& run_dir) {
    std::error_code ec;
    if (!std::filesystem::exists(run_dir / "events.jsonl", ec)) return std::nullopt;
    const std::vector<JournalEvent> events = ReadJournalEvents(run_dir);

    WorkflowRunSnapshot snapshot;
    const std::vector<RunStatus> all = ListRuns(run_dir.parent_path());
    for (const auto& run : all) {
        if (run.dir == run_dir) {
            snapshot.run_id = run.run_id;
            snapshot.workflow_id = run.workflow_id;
            snapshot.workflow_version = run.workflow_version;
            snapshot.content_hash = run.content_hash;
            snapshot.state = run.final_state.empty() ? "interrupted" : run.final_state;
            break;
        }
    }
    // 节点账从事件重放。
    const auto replayed = ReplayNodes(events);
    for (const auto& [id, node] : replayed) {
        nlohmann::ordered_json entry = nlohmann::ordered_json::object();
        entry["state"] = node.state;
        snapshot.nodes.emplace(id, std::move(entry));
    }
    for (const auto& event : events) {
        snapshot.last_seq = std::max(snapshot.last_seq, event.seq);
        if (event.type == kEventNodeCompleted && event.data.contains("output")) {
            // completed 的节点带 output 尺寸(前端点开看详情另有 query)。
        }
    }
    if (snapshot.run_id.empty()) {
        snapshot.run_id = lubancode::platform::PathToUtf8(run_dir.filename());
    }
    return snapshot;
}

runtime::ServerEvent JournalEventToServerEvent(const JournalEvent& event, const std::string& thread_id) {
    runtime::ServerEvent out;
    out.envelope.thread_id = thread_id.empty() ? "workflow" : thread_id;
    out.envelope.seq = event.seq;
    out.envelope.timestamp_ms = event.ts_ms;
    out.kind = runtime::ServerEventKind::ItemDelta;
    out.item_kind = runtime::ItemKind::Command;
    out.item_id = event.run_id;
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    payload["type"] = event.type;
    payload["workflow_id"] = event.workflow_id;
    payload["node_id"] = event.node_id;
    payload["attempt"] = event.attempt;
    payload["data"] = event.data;
    out.payload = std::move(payload);
    return out;
}

std::vector<runtime::ServerEvent> BuildIncrementalEvents(const std::vector<JournalEvent>& events,
                                                          const std::string& thread_id, std::uint64_t last_seq) {
    std::vector<runtime::ServerEvent> out;
    for (const auto& event : events) {
        if (event.seq <= last_seq) continue;  // 从 last_seq+1 接
        out.push_back(JournalEventToServerEvent(event, thread_id));
    }
    return out;
}

}  // namespace lubancode::workflow
