// session_command_service.hpp 的实现:runtime 侧的会话查询与搬删服务。

#include "runtime/session_command_service.hpp"

#include <utility>

#include "sessions/session_catalog.hpp"
#include "sessions/session_lifecycle.hpp"

namespace lubancode::runtime {

namespace {

// lifecycle 结果码 -> 稳定字符串(协议侧的错误码,单子"代码边界"一节)。
const char* LifecycleCodeToString(agent::SessionLifecycleCode code) {
    switch (code) {
        case agent::SessionLifecycleCode::Ok: return "";
        case agent::SessionLifecycleCode::NotFound: return "not_found";
        case agent::SessionLifecycleCode::Ambiguous: return "ambiguous";
        case agent::SessionLifecycleCode::ActiveTurn: return "active_turn";
        case agent::SessionLifecycleCode::ConfirmationRequired: return "confirmation_required";
        case agent::SessionLifecycleCode::PathOutsideRoot: return "path_outside_root";
        case agent::SessionLifecycleCode::TargetExists: return "target_exists";
        case agent::SessionLifecycleCode::IoError: return "io_error";
    }
    return "io_error";
}

// 查询 payload 的字符串档位 -> 枚举(认不出给缺省,不抛——协议演进里
// "新加的档位老对端还没跟上"是常态)。
agent::SessionScope ParseScope(const nlohmann::json& payload) {
    const auto it = payload.find("scope");
    if (it != payload.end() && it->is_string() && *it == "all") {
        return agent::SessionScope::All;
    }
    return agent::SessionScope::Cwd;
}

agent::SessionState ParseState(const nlohmann::json& payload) {
    const auto it = payload.find("state");
    if (it != payload.end() && it->is_string() && *it == "archived") {
        return agent::SessionState::Archived;
    }
    return agent::SessionState::Active;
}

agent::SessionSort ParseSort(const nlohmann::json& payload) {
    const auto it = payload.find("sort");
    if (it != payload.end() && it->is_string() && *it == "created") {
        return agent::SessionSort::Created;
    }
    return agent::SessionSort::Updated;
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
    // 整数字面量在 nlohmann 里是 number_integer(带符号),显式 uint 才是
    // number_unsigned——两种都认,负数折 0。
    if (it == payload.end() || !it->is_number_integer()) {
        return 0;
    }
    const auto value = it->get<std::int64_t>();
    return value > 0 ? static_cast<std::size_t>(value) : 0;
}

const char* StateToString(agent::SessionState state) {
    return state == agent::SessionState::Archived ? "archived" : "active";
}

const char* HealthToString(agent::SessionHealth health) {
    return health == agent::SessionHealth::Damaged ? "damaged" : "ok";
}

}  // namespace

SessionCommandService::SessionCommandService(std::string sessions_dir)
    : sessions_dir_(std::move(sessions_dir)) {
    if (!sessions_dir_.empty()) {
        lifecycle_ = std::make_unique<agent::SessionLifecycle>(sessions_dir_);
    }
}

SessionCommandService::~SessionCommandService() = default;

void SessionCommandService::SetActiveFile(std::string active_file,
                                          std::function<bool(const std::string&)> flush_close) {
    if (lifecycle_ != nullptr) {
        lifecycle_->SetActiveFile(std::move(active_file), std::move(flush_close));
    }
}

SessionCommandOutcome SessionCommandService::ListThreads(const nlohmann::json& query_payload) const {
    SessionCommandOutcome outcome;
    if (sessions_dir_.empty()) {
        outcome.payload = {{"threads", nlohmann::json::array()}, {"total", 0}};
        return outcome;
    }
    agent::SessionCatalog catalog(sessions_dir_);
    catalog.Scan();
    agent::SessionQuery query;
    query.scope = ParseScope(query_payload);
    query.state = ParseState(query_payload);
    query.sort = ParseSort(query_payload);
    query.search = JsonStr(query_payload, "search");
    query.cwd = JsonStr(query_payload, "cwd");
    query.cursor = JsonSize(query_payload, "cursor");
    const std::size_t limit = JsonSize(query_payload, "limit");
    query.limit = limit == 0 ? 20 : limit;  // 缺省 20;0 是"不截",协议里
                                            // 显式 0 也照给(payload 里给
                                            // number 0 会被当缺省,想要不截
                                            // 给大数——协议文档口径)
    const auto page = catalog.Query(query);
    nlohmann::json threads = nlohmann::json::array();
    for (const auto& entry : page.entries) {
        threads.push_back(SessionSummaryToJson(entry.id, entry.title, entry.first_user_text, entry.cwd,
                                               entry.model, entry.created_at, entry.updated_at,
                                               entry.message_count, StateToString(entry.state),
                                               HealthToString(entry.health)));
    }
    outcome.payload = {{"threads", std::move(threads)}, {"total", page.total}};
    return outcome;
}

SessionCommandOutcome SessionCommandService::ArchiveThread(const std::string& thread_id) {
    SessionCommandOutcome outcome;
    if (lifecycle_ == nullptr || thread_id.empty()) {
        outcome.accepted = false;
        outcome.error_code = thread_id.empty() ? "invalid_request" : "not_found";
        return outcome;
    }
    const auto result = lifecycle_->ArchiveSession(thread_id);
    if (!result.ok()) {
        outcome.accepted = false;
        outcome.error_code = LifecycleCodeToString(result.code);
        outcome.error_message = result.message;
        return outcome;
    }
    outcome.payload = {{"threadId", thread_id}, {"state", "archived"}};
    return outcome;
}

SessionCommandOutcome SessionCommandService::UnarchiveThread(const std::string& thread_id) {
    SessionCommandOutcome outcome;
    if (lifecycle_ == nullptr || thread_id.empty()) {
        outcome.accepted = false;
        outcome.error_code = thread_id.empty() ? "invalid_request" : "not_found";
        return outcome;
    }
    const auto result = lifecycle_->UnarchiveSession(thread_id);
    if (!result.ok()) {
        outcome.accepted = false;
        outcome.error_code = LifecycleCodeToString(result.code);
        outcome.error_message = result.message;
        return outcome;
    }
    outcome.payload = {{"threadId", thread_id}, {"state", "active"}};
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
    if (lifecycle_ == nullptr) {
        outcome.accepted = false;
        outcome.error_code = "not_found";
        return outcome;
    }
    const auto result = lifecycle_->DeleteSession(thread_id, /*confirmed=*/true);
    if (!result.ok()) {
        outcome.accepted = false;
        outcome.error_code = LifecycleCodeToString(result.code);
        outcome.error_message = result.message;
        return outcome;
    }
    outcome.payload = {{"threadId", thread_id}};
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
