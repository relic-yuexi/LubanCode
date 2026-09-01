// session_command_service.hpp 的实现:runtime 侧的会话查询与搬删服务
// (P0-2:数据源与执行体全换 workspace 新账——索引投影 + 管理自由函数)。

#include "runtime/session_command_service.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include "tools/path_utils.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"

namespace lubancode::runtime {

namespace {

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string JsonStr(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

std::size_t JsonSize(const nlohmann::json& payload, const char* key) {
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_number_integer()) {
        return 0;
    }
    const auto value = it->get<std::int64_t>();
    return value > 0 ? static_cast<std::size_t>(value) : 0;
}

// 查询 payload -> 索引查询(scope=cwd 即当前 workspace)。
trajectory::SessionIndexQuery ParseQuery(const nlohmann::json& payload,
                                         const std::string& workspace_key) {
    trajectory::SessionIndexQuery query;
    query.all_workspaces = JsonStr(payload, "scope") == "all";
    query.current_workspace_key = workspace_key;
    if (JsonStr(payload, "state") == "archived") {
        query.archived_only = true;
    } else {
        query.include_archived = false;
    }
    query.sort_by_created = JsonStr(payload, "sort") == "created";
    query.search = JsonStr(payload, "search");
    query.cursor = JsonSize(payload, "cursor");
    const std::size_t limit = JsonSize(payload, "limit");
    query.limit = limit;  // 0 = 不截(与旧协议口径一致:缺省由调用方给大数)
    return query;
}

// 按 id 跨 workspace 定位场次(搬删执行前先找到家)。
std::optional<trajectory::WorkspaceSessionSummary> LocateSession(
    const std::filesystem::path& workspaces_root, const std::string& session_id) {
    trajectory::SessionIndexQuery query;
    query.all_workspaces = true;
    query.include_archived = true;
    const auto page = trajectory::QueryWorkspaceSessions(workspaces_root, query);
    for (const auto& summary : page.entries) {
        if (summary.session_id == session_id) {
            return summary;
        }
    }
    return std::nullopt;
}

std::filesystem::path WorkspaceDirOf(const std::filesystem::path& workspaces_root,
                                     const std::string& workspace_key) {
    return workspaces_root / tools::Utf8ToPath(workspace_key);
}

SessionCommandOutcome FromAdminOutcome(const trajectory::SessionAdminOutcome& outcome,
                                       const std::string& thread_id) {
    SessionCommandOutcome result;
    if (outcome.ok()) {
        return result;
    }
    result.accepted = false;
    result.error_code = outcome.error_code;
    result.error_message = outcome.message.empty() ? thread_id : outcome.message;
    return result;
}

}  // namespace

SessionCommandService::SessionCommandService(Options options) : options_(std::move(options)) {}

SessionCommandService::~SessionCommandService() = default;

SessionCommandOutcome SessionCommandService::ListThreads(const nlohmann::json& query_payload) const {
    SessionCommandOutcome outcome;
    if (options_.workspaces_root.empty()) {
        outcome.payload = {{"threads", nlohmann::json::array()}, {"total", 0}};
        return outcome;
    }
    trajectory::SessionIndexQuery query = ParseQuery(query_payload, options_.workspace_key);
    if (query.limit == 0) {
        query.limit = 20;  // 缺省 20(旧协议口径;0 显式给大数 = 不截)
    }
    const auto page = trajectory::QueryWorkspaceSessions(options_.workspaces_root, query);
    nlohmann::json threads = nlohmann::json::array();
    for (const auto& entry : page.entries) {
        threads.push_back(SessionSummaryToJson(
            entry.session_id, entry.title, entry.first_user_text, entry.cwd, entry.model,
            trajectory::FormatMillisAsLocalTimestamp(entry.created_at_ms),
            trajectory::FormatMillisAsLocalTimestamp(entry.updated_at_ms), entry.message_count,
            entry.archived ? "archived" : "active", entry.damaged ? "damaged" : "ok"));
    }
    outcome.payload = {{"threads", std::move(threads)}, {"total", page.total}};
    return outcome;
}

SessionCommandOutcome SessionCommandService::ArchiveThread(const std::string& thread_id) {
    SessionCommandOutcome outcome;
    if (options_.workspaces_root.empty() || thread_id.empty()) {
        outcome.accepted = false;
        outcome.error_code = thread_id.empty() ? "invalid_request" : "not_found";
        return outcome;
    }
    const auto located = LocateSession(options_.workspaces_root, thread_id);
    if (!located.has_value()) {
        outcome.accepted = false;
        outcome.error_code = "not_found";
        outcome.error_message = thread_id;
        return outcome;
    }
    const auto result = trajectory::ArchiveSessionDir(
        WorkspaceDirOf(options_.workspaces_root, located->workspace_key), thread_id, NowMs());
    outcome = FromAdminOutcome(result, thread_id);
    if (outcome.accepted) {
        outcome.payload = {{"threadId", thread_id}, {"state", "archived"}};
    }
    return outcome;
}

SessionCommandOutcome SessionCommandService::UnarchiveThread(const std::string& thread_id) {
    SessionCommandOutcome outcome;
    if (options_.workspaces_root.empty() || thread_id.empty()) {
        outcome.accepted = false;
        outcome.error_code = thread_id.empty() ? "invalid_request" : "not_found";
        return outcome;
    }
    const auto located = LocateSession(options_.workspaces_root, thread_id);
    if (!located.has_value()) {
        outcome.accepted = false;
        outcome.error_code = "not_found";
        outcome.error_message = thread_id;
        return outcome;
    }
    const auto result = trajectory::UnarchiveSessionDir(
        WorkspaceDirOf(options_.workspaces_root, located->workspace_key), thread_id, NowMs());
    outcome = FromAdminOutcome(result, thread_id);
    if (outcome.accepted) {
        outcome.payload = {{"threadId", thread_id}, {"state", "active"}};
    }
    return outcome;
}

SessionCommandOutcome SessionCommandService::DeleteThread(const std::string& thread_id,
                                                          const nlohmann::json& payload) {
    SessionCommandOutcome outcome;
    if (thread_id.empty()) {
        outcome.accepted = false;
        outcome.error_code = "invalid_request";
        return outcome;
    }
    // 确认归调用方:终端确认屏/GUI 对话框收了确认才把 confirm=true 发来。
    // 没带一律拒绝,盘上不动。
    const auto it = payload.find("confirm");
    const bool confirmed = it != payload.end() && it->is_boolean() && it->get<bool>();
    if (!confirmed) {
        outcome.accepted = false;
        outcome.error_code = "confirmation_required";
        return outcome;
    }
    if (options_.workspaces_root.empty()) {
        outcome.accepted = false;
        outcome.error_code = "not_found";
        return outcome;
    }
    const auto located = LocateSession(options_.workspaces_root, thread_id);
    if (!located.has_value()) {
        outcome.accepted = false;
        outcome.error_code = "not_found";
        outcome.error_message = thread_id;
        return outcome;
    }
    const auto result = trajectory::DeleteSessionDir(
        WorkspaceDirOf(options_.workspaces_root, located->workspace_key), thread_id,
        /*reason=*/"user_delete", NowMs());
    outcome = FromAdminOutcome(result, thread_id);
    if (outcome.accepted) {
        outcome.payload = {{"threadId", thread_id}};
    }
    return outcome;
}

ClientReceipt SessionCommandService::HandleCommand(const ClientCommand& command) {
    switch (command.kind) {
        case ClientCommandKind::ListThreads:
            return ListThreads(command.payload).ToReceipt();
        case ClientCommandKind::ArchiveThread:
            return ArchiveThread(command.thread_id).ToReceipt();
        case ClientCommandKind::UnarchiveThread:
            return UnarchiveThread(command.thread_id).ToReceipt();
        case ClientCommandKind::DeleteThread:
            return DeleteThread(command.thread_id, command.payload).ToReceipt();
        default:
            break;  // 别的 kind 不归本服务
    }
    ClientReceipt receipt;
    receipt.accepted = false;
    receipt.error_code = "invalid_request";
    receipt.error_message = "session command service: not a session command";
    return receipt;
}

nlohmann::json SessionSummaryToJson(const std::string& id, const std::string& title,
                                    const std::string& first_user_text, const std::string& cwd,
                                    const std::string& model, const std::string& created_at,
                                    const std::string& updated_at, std::uint64_t message_count,
                                    const std::string& state, const std::string& health) {
    return nlohmann::json{
        {"threadId", id},
        {"title", title},
        {"firstUserText", first_user_text},
        {"cwd", cwd},
        {"model", model},
        {"createdAt", created_at},
        {"updatedAt", updated_at},
        {"messageCount", message_count},
        {"state", state},
        {"health", health},
    };
}

}  // namespace lubancode::runtime
