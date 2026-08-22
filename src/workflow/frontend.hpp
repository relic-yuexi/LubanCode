// Workflow 多前端合同(自然语言编排单第 6 批):snapshot 与事件流。
//
// 单子"Runtime 事件与多前端":Web/Tauri 不读 workflow-runs 私有目录,所有
// 状态只从 query/snapshot/event 来;前端重连先取 WorkflowRunSnapshot,再
// 从 last_seq+1 接事件。这里是那份 snapshot 与事件序的纯逻辑层;app-server
// 的接线由宿主做(本仓库 app_server 是另一张单的主战场,这里只给合同与
// 适配函数,不动它的协议骨架——接缝在交付报告注明)。

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/event.hpp"
#include "workflow/definition.hpp"
#include "workflow/journal.hpp"
#include "workflow/runtime.hpp"

namespace lubancode::workflow {

// 一场 run 的可查询快照:前端重连/首屏都吃它。
struct WorkflowRunSnapshot {
    std::string run_id;
    std::string workflow_id;
    std::string workflow_version;
    std::string content_hash;
    std::string state;       // ToString(RunState)
    std::string error_code;
    std::string error_message;
    nlohmann::json result = nlohmann::json::object();
    std::vector<std::string> unavailable_sources;
    std::int64_t duration_ms = 0;
    std::int64_t tokens_used = 0;
    std::uint64_t last_seq = 0;  // 前端从 last_seq+1 接事件
    // 节点账:node_id -> {state, attempt, error_code, duration_ms}。
    std::map<std::string, nlohmann::json> nodes;

    nlohmann::json ToJson() const;
    static std::optional<WorkflowRunSnapshot> FromJson(const nlohmann::json& j);
};

// 从 run 摘要 + journal 事件拼 snapshot(重连路径)。
WorkflowRunSnapshot BuildSnapshot(const WorkflowRunSummary& summary, const std::vector<JournalEvent>& events);

// 从磁盘 run 目录拼 snapshot(/workflow show 运行态、app-server query 用)。
std::optional<WorkflowRunSnapshot> LoadSnapshotFromDisk(const std::filesystem::path& run_dir);

// journal 事件 -> runtime::ServerEvent(单子:WorkflowRuntime 不自己 cout
// 一套画面;事件只放领域数据,不带 ANSI/终端宽/本地化成句)。
// thread_id 给信封;seq 用事件自身的(单调)。
runtime::ServerEvent JournalEventToServerEvent(const JournalEvent& event, const std::string& thread_id);

// 从 last_seq+1 起的增量事件(前端重连补账;last_seq=0 给全量)。
std::vector<runtime::ServerEvent> BuildIncrementalEvents(const std::vector<JournalEvent>& events,
                                                          const std::string& thread_id,
                                                          std::uint64_t last_seq);

}  // namespace lubancode::workflow
