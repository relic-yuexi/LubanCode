#include "trajectory/session_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <utility>

#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/safety.hpp"
#include "workspace/identity.hpp"
#include "workspace/manifest.hpp"  // P0-1:RegisterCheckout 的登记体

namespace lubancode::trajectory {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

void NowStampParts(const SessionManagerClock* clock, std::tm* out) {
    const std::int64_t ms = clock->WallMs();
    std::time_t secs = static_cast<std::time_t>(ms / 1000);
#ifdef _WIN32
    gmtime_s(out, &secs);
#else
    gmtime_r(&secs, out);
#endif
}

// 独占创建写:文件已存在即失败(exists=true 标记),写失败 false。
// intent.json/tombstone 用——一次操作一只目录,绝不覆盖历史。
bool WriteTextExclusive(const std::filesystem::path& path, const std::string& content,
                        bool* exists) {
    *exists = false;
    std::FILE* file = nullptr;
#ifdef _WIN32
    file = _wfsopen(path.c_str(), L"wbx", _SH_DENYNO);
#else
    file = std::fopen(path.c_str(), "wbx");
#endif
    if (file == nullptr) {
        *exists = std::filesystem::exists(path);
        return false;
    }
    const bool wrote = std::fwrite(content.data(), 1, content.size(), file) == content.size() &&
                       std::fflush(file) == 0;
    std::fclose(file);
    return wrote;
}

// 临时文件 + 原子 rename;先验不存在,不覆盖历史(result.json 用)。
std::expected<void, std::string> WriteTextAtomicIfAbsent(const std::filesystem::path& path,
                                                         const std::string& content,
                                                         std::string exists_code) {
    if (std::filesystem::exists(path)) {
        return std::unexpected(std::move(exists_code) + ": " + platform::PathToUtf8(path));
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return std::unexpected("写不开临时文件: " + platform::PathToUtf8(tmp));
        }
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file.good()) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(tmp, ignored);
            return std::unexpected("临时文件写失败: " + platform::PathToUtf8(tmp));
        }
    }
    if (!platform::ReplaceFileAtomically(tmp, path).has_value()) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return std::unexpected("原子改名失败: " + platform::PathToUtf8(path));
    }
    return {};
}

std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

// 恢复期允许的"折叠"迁移:session.json 落后或抢跑于 Journal 可证事实时,
// 事实永远赢。但不许借恢复之名 archive、复活 running 或把 corrupt 洗白
// (preparing->running 本就是图上的边,不经折叠)。
bool RecoveryCollapseAllowed(SessionStatus from, SessionStatus to) {
    if (from == SessionStatus::Corrupt || to == SessionStatus::Archived ||
        to == SessionStatus::Running) {
        return false;
    }
    return to == SessionStatus::Closed || to == SessionStatus::Incomplete ||
           to == SessionStatus::Corrupt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期状态机(§3.3.2)
// ---------------------------------------------------------------------------

const char* SessionStatusName(SessionStatus status) {
    switch (status) {
        case SessionStatus::Preparing:
            return "preparing";
        case SessionStatus::Running:
            return "running";
        case SessionStatus::Closing:
            return "closing";
        case SessionStatus::Closed:
            return "closed";
        case SessionStatus::Incomplete:
            return "incomplete";
        case SessionStatus::Corrupt:
            return "corrupt";
        case SessionStatus::Archived:
            return "archived";
    }
    return "";
}

std::optional<SessionStatus> SessionStatusFromName(std::string_view name) {
    for (const SessionStatus status :
         {SessionStatus::Preparing, SessionStatus::Running, SessionStatus::Closing,
          SessionStatus::Closed, SessionStatus::Incomplete, SessionStatus::Corrupt,
          SessionStatus::Archived}) {
        if (name == SessionStatusName(status)) {
            return status;
        }
    }
    return std::nullopt;
}

bool CanTransitionSessionStatus(SessionStatus from, SessionStatus to) {
    if (from == to) {
        return false;
    }
    if (to == SessionStatus::Corrupt) {
        return from != SessionStatus::Corrupt;  // 任意可读态 -> corrupt
    }
    switch (from) {
        case SessionStatus::Preparing:
            return to == SessionStatus::Running;
        case SessionStatus::Running:
            return to == SessionStatus::Closing || to == SessionStatus::Incomplete;
        case SessionStatus::Closing:
            return to == SessionStatus::Closed || to == SessionStatus::Incomplete;
        case SessionStatus::Closed:
            return to == SessionStatus::Archived;
        case SessionStatus::Archived:
            return to == SessionStatus::Closed;  // closed <-> archived
        case SessionStatus::Incomplete:
        case SessionStatus::Corrupt:
            return false;  // 先 verify/recover;resume-as-new 另开新场
    }
    return false;
}

std::expected<void, std::string> TransitionSessionStatus(const std::filesystem::path& session_dir,
                                                         SessionManifest* manifest,
                                                         SessionStatus to, bool recovery_collapse) {
    const std::string from_text = manifest->status.empty()
                                      ? SessionStatusName(SessionStatus::Preparing)
                                      : manifest->status;
    const auto from = SessionStatusFromName(from_text);
    if (!from.has_value()) {
        return std::unexpected("session.status_unknown: session.json 的 status 认不得: " +
                               from_text);
    }
    if (!CanTransitionSessionStatus(*from, to) &&
        !(recovery_collapse && RecoveryCollapseAllowed(*from, to))) {
        return std::unexpected("session.transition_invalid: " + from_text + " -> " +
                               SessionStatusName(to));
    }
    SessionManifest updated = *manifest;
    updated.status = SessionStatusName(to);
    if (const auto written = WriteSessionJsonAtomic(session_dir, updated); !written.has_value()) {
        return std::unexpected(written.error());
    }
    *manifest = std::move(updated);
    return {};
}

// ---------------------------------------------------------------------------
// workspace lifecycle 账(§3.2)
// ---------------------------------------------------------------------------

const char* LifecycleOperationName(LifecycleOperation operation) {
    switch (operation) {
        case LifecycleOperation::CreateSession:
            return "create_session";
        case LifecycleOperation::ArchiveSession:
            return "archive_session";
        case LifecycleOperation::UnarchiveSession:
            return "unarchive_session";
        case LifecycleOperation::ResumeReference:
            return "resume_reference";
        case LifecycleOperation::DeleteSession:
            return "delete_session";
    }
    return "";
}

std::optional<LifecycleOperation> LifecycleOperationFromName(std::string_view name) {
    for (const LifecycleOperation operation :
         {LifecycleOperation::CreateSession, LifecycleOperation::ArchiveSession,
          LifecycleOperation::UnarchiveSession, LifecycleOperation::ResumeReference,
          LifecycleOperation::DeleteSession}) {
        if (name == LifecycleOperationName(operation)) {
            return operation;
        }
    }
    return std::nullopt;
}

nlohmann::json LifecycleIntent::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["operation_id"] = operation_id;
    json["operation"] = operation;
    json["workspace_key"] = workspace_key;
    json["session_id"] = session_id;
    json["requested_at_ms"] = requested_at_ms;
    json["parameters"] = parameters;
    return json;
}

std::optional<LifecycleIntent> LifecycleIntent::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    LifecycleIntent intent;
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    if (!read_string("operation_id", &intent.operation_id) ||
        !read_string("operation", &intent.operation) ||
        !read_string("session_id", &intent.session_id)) {
        return std::nullopt;
    }
    if (LifecycleOperationFromName(intent.operation) == std::nullopt) {
        return std::nullopt;
    }
    read_string("workspace_key", &intent.workspace_key);
    if (json.contains("requested_at_ms") && json.at("requested_at_ms").is_number_integer()) {
        intent.requested_at_ms = json.at("requested_at_ms").get<std::int64_t>();
    }
    if (json.contains("parameters") && json.at("parameters").is_object()) {
        intent.parameters = json.at("parameters");
    }
    return intent;
}

nlohmann::json LifecycleResult::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["operation_id"] = operation_id;
    json["status"] = status;
    json["completed_at_ms"] = completed_at_ms;
    json["outcome"] = outcome;
    return json;
}

std::optional<LifecycleResult> LifecycleResult::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    LifecycleResult result;
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    if (!read_string("operation_id", &result.operation_id) ||
        !read_string("status", &result.status)) {
        return std::nullopt;
    }
    if (result.status != "completed" && result.status != "failed") {
        return std::nullopt;
    }
    if (json.contains("completed_at_ms") && json.at("completed_at_ms").is_number_integer()) {
        result.completed_at_ms = json.at("completed_at_ms").get<std::int64_t>();
    }
    if (json.contains("outcome") && json.at("outcome").is_object()) {
        result.outcome = json.at("outcome");
    }
    return result;
}

std::expected<std::filesystem::path, std::string> WorkspaceLifecycle::WriteIntent(
    const LifecycleIntent& intent) const {
    const std::filesystem::path operation_dir =
        workspace_dir_ / "lifecycle" / platform::Utf8ToPath(intent.operation_id);
    std::error_code ec;
    std::filesystem::create_directories(operation_dir, ec);
    if (ec) {
        return std::unexpected("lifecycle 目录建不起: " + platform::PathToUtf8(operation_dir) +
                               ": " + ec.message());
    }
    bool exists = false;
    if (!WriteTextExclusive(operation_dir / "intent.json", intent.ToJson().dump(), &exists)) {
        if (exists) {
            return std::unexpected("lifecycle.intent_exists: operation_id 不得复用: " +
                                   intent.operation_id);
        }
        return std::unexpected("lifecycle.intent_write_failed: " +
                               platform::PathToUtf8(operation_dir / "intent.json"));
    }
    return operation_dir;
}

std::expected<void, std::string> WorkspaceLifecycle::WriteResult(
    const LifecycleResult& result) const {
    const std::filesystem::path operation_dir =
        workspace_dir_ / "lifecycle" / platform::Utf8ToPath(result.operation_id);
    return WriteTextAtomicIfAbsent(operation_dir / "result.json", result.ToJson().dump(),
                                   "lifecycle.result_exists");
}

std::optional<LifecycleIntent> WorkspaceLifecycle::ReadIntent(
    const std::filesystem::path& operation_dir) {
    const auto json = ReadJsonFile(operation_dir / "intent.json");
    if (!json.has_value()) {
        return std::nullopt;
    }
    return LifecycleIntent::FromJson(*json);
}

std::optional<LifecycleResult> WorkspaceLifecycle::ReadResult(
    const std::filesystem::path& operation_dir) {
    const auto json = ReadJsonFile(operation_dir / "result.json");
    if (!json.has_value()) {
        return std::nullopt;
    }
    return LifecycleResult::FromJson(*json);
}

nlohmann::json SessionTombstone::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema_version"] = schema_version;
    json["session_id"] = session_id;
    json["deleted_at_ms"] = deleted_at_ms;
    json["reason"] = reason;
    json["last_event_hash"] =
        last_event_hash.has_value() ? nlohmann::json(*last_event_hash) : nlohmann::json(nullptr);
    json["operation_id"] = operation_id;
    return json;
}

std::optional<SessionTombstone> SessionTombstone::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    SessionTombstone tombstone;
    if (!json.contains("session_id") || !json.at("session_id").is_string()) {
        return std::nullopt;
    }
    tombstone.session_id = json.at("session_id").get<std::string>();
    if (json.contains("deleted_at_ms") && json.at("deleted_at_ms").is_number_integer()) {
        tombstone.deleted_at_ms = json.at("deleted_at_ms").get<std::int64_t>();
    }
    if (json.contains("reason") && json.at("reason").is_string()) {
        tombstone.reason = json.at("reason").get<std::string>();
    }
    if (json.contains("last_event_hash") && json.at("last_event_hash").is_string()) {
        tombstone.last_event_hash = json.at("last_event_hash").get<std::string>();
    }
    if (json.contains("operation_id") && json.at("operation_id").is_string()) {
        tombstone.operation_id = json.at("operation_id").get<std::string>();
    }
    return tombstone;
}

std::expected<void, std::string> WriteSessionTombstone(const std::filesystem::path& tombstones_dir,
                                                       const SessionTombstone& tombstone) {
    std::error_code ec;
    std::filesystem::create_directories(tombstones_dir, ec);
    if (ec) {
        return std::unexpected("tombstones 目录建不起: " + platform::PathToUtf8(tombstones_dir));
    }
    const std::filesystem::path path =
        tombstones_dir / platform::Utf8ToPath(tombstone.session_id + ".json");
    bool exists = false;
    if (!WriteTextExclusive(path, tombstone.ToJson().dump(), &exists)) {
        if (exists) {
            return std::unexpected("lifecycle.tombstone_exists: 一枚 session 只删一次: " +
                                   tombstone.session_id);
        }
        return std::unexpected("tombstone 写失败: " + platform::PathToUtf8(path));
    }
    return {};
}

std::optional<SessionTombstone> ReadSessionTombstone(const std::filesystem::path& tombstones_dir,
                                                     const std::string& session_id) {
    const auto json = ReadJsonFile(tombstones_dir / platform::Utf8ToPath(session_id + ".json"));
    if (!json.has_value()) {
        return std::nullopt;
    }
    return SessionTombstone::FromJson(*json);
}

// ---------------------------------------------------------------------------
// 时钟注入默认实现
// ---------------------------------------------------------------------------

std::string SessionManagerClock::Random6() const {
    static const char kPool[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static std::mt19937 rng(std::random_device{}());
    std::string out;
    for (int index = 0; index < 6; ++index) {
        out += kPool[rng() % (sizeof(kPool) - 1)];
    }
    return out;
}

SessionLockOwner SessionManagerClock::LockOwner() const {
    SessionLockOwner owner;
    owner.pid = platform::CurrentProcessId();
    owner.process_start_token = CurrentProcessStartToken();
    owner.acquired_at_ms = WallMs();
    return owner;
}

// ---------------------------------------------------------------------------
// EventRef
// ---------------------------------------------------------------------------

nlohmann::json EventRef::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["session_id"] = session_id;
    json["event_id"] = event_id;
    json["event_hash"] = event_hash;
    return json;
}

std::optional<EventRef> EventRef::FromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    EventRef ref;
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    if (!read_string("session_id", &ref.session_id) || !read_string("event_id", &ref.event_id) ||
        !read_string("event_hash", &ref.event_hash)) {
        return std::nullopt;
    }
    return ref;
}

// ---------------------------------------------------------------------------
// Journal 事实扫描(恢复器/硬门共用;单遍,不重放状态机)
// ---------------------------------------------------------------------------

MainJournalFacts ScanStreamFacts(const std::filesystem::path& stream_path) {
    MainJournalFacts facts;
    std::ifstream file(stream_path, std::ios::binary);
    if (!file.is_open()) {
        return facts;  // journal_exists=false
    }
    facts.journal_exists = true;

    const auto touch = [&facts](const EventEnvelope& envelope) {
        ++facts.event_count;
        facts.last_event_id = envelope.event_id;
        facts.last_event_hash = envelope.event_hash;
        if (facts.event_count == 1) {
            facts.first_event_id = envelope.event_id;
            facts.first_event_hash = envelope.event_hash;
        }
    };
    const auto erase = [](std::vector<std::string>* list, const std::string& value) {
        std::erase(*list, value);
    };

    std::string line;
    bool truncated = false;
    facts.verify_ok = true;
    while (std::getline(file, line)) {
        if (file.eof()) {
            truncated = true;  // 尾行缺 '\n'(§16.3 崩溃截断)
        }
        if (line.empty()) {
            facts.verify_ok = false;
            facts.verify_error_code = "verify.empty_line";
            break;
        }
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        EventEnvelope envelope;
        if (parsed.is_discarded() || ParseAndValidateEventLine(parsed, &envelope).has_value()) {
            facts.verify_ok = false;
            facts.verify_error_code = "verify.bad_line";
            break;
        }
        touch(envelope);
        switch (envelope.kind) {
            case EventKind::RunStarted:
                facts.has_run_started = true;
                facts.start_reason = envelope.payload.value("start_reason", "");
                if (envelope.payload.contains("previous_session_id") &&
                    envelope.payload.at("previous_session_id").is_string()) {
                    facts.previous_session_id =
                        envelope.payload.at("previous_session_id").get<std::string>();
                }
                break;
            case EventKind::SessionClearRequested:
                facts.clear_requested = true;
                facts.clear_requested_next_session_id =
                    envelope.payload.value("next_session_id", "");
                if (envelope.correlation_id.has_value() && !envelope.correlation_id->empty()) {
                    facts.boundary_operation_id = *envelope.correlation_id;
                }
                break;
            case EventKind::ControlCommandRequested:
                facts.command_requested = true;
                facts.command_requested_event_id = envelope.event_id;
                facts.command_id = envelope.payload.value("command_id", "");
                if (facts.boundary_operation_id.empty() && envelope.correlation_id.has_value()) {
                    facts.boundary_operation_id = *envelope.correlation_id;
                }
                break;
            case EventKind::ControlCommandCompleted:
                facts.command_completed = true;
                break;
            case EventKind::RunCompleted:
            case EventKind::RunFailed:
            case EventKind::RunCancelled:
                facts.run_terminal = true;
                facts.run_terminal_kind = EventKindName(envelope.kind);
                break;
            case EventKind::SessionEnded:
                facts.session_ended = true;
                facts.close_quality = envelope.payload.value("close_quality", "");
                if (envelope.payload.contains("next_session_id") &&
                    envelope.payload.at("next_session_id").is_string()) {
                    facts.next_session_id = envelope.payload.at("next_session_id").get<std::string>();
                }
                facts.session_ended_event_id = envelope.event_id;
                facts.session_ended_event_hash = envelope.event_hash;
                break;
            case EventKind::TurnStarted:
                facts.dangling_turn_ids.push_back(envelope.turn_id.value_or(""));
                break;
            case EventKind::TurnCompleted:
            case EventKind::TurnFailed:
            case EventKind::TurnCancelled:
                erase(&facts.dangling_turn_ids, envelope.turn_id.value_or(""));
                break;
            case EventKind::ControlQueueItemEnqueued:
                facts.dangling_queue_item_ids.push_back(envelope.payload.value("item_id", ""));
                break;
            case EventKind::ControlQueueItemDequeued:
            case EventKind::ControlQueueItemCancelled:
            case EventKind::ControlQueueItemExpired:
                erase(&facts.dangling_queue_item_ids, envelope.payload.value("item_id", ""));
                break;
            case EventKind::RecordSelectionStarted:
                facts.dangling_selection_ids.push_back(envelope.payload.value("record_id", ""));
                break;
            case EventKind::RecordSelectionCompleted:
            case EventKind::RecordSelectionCancelled:
            case EventKind::RecordSelectionInterrupted:
                erase(&facts.dangling_selection_ids, envelope.payload.value("record_id", ""));
                break;
            default:
                break;
        }
    }
    if (truncated) {
        facts.truncated_tail = true;
        facts.verify_ok = false;
        facts.verify_error_code = "verify.truncated_tail";
    }
    return facts;
}

SessionStatus DeriveSessionStatusFromFacts(const MainJournalFacts& facts) {
    if (!facts.journal_exists || !facts.has_run_started) {
        // 目录占住了,main 尚未 durable started(§3.3.2 preparing)。
        return SessionStatus::Preparing;
    }
    if (facts.truncated_tail) {
        // 前面完整事件照常 replay,整本判 incomplete,不伪造终态(§16.3)。
        return SessionStatus::Incomplete;
    }
    if (!facts.verify_ok) {
        return SessionStatus::Corrupt;
    }
    if (facts.session_ended) {
        const bool clean =
            facts.close_quality == "clean" && facts.run_terminal_kind == "run.completed";
        return clean ? SessionStatus::Closed : SessionStatus::Incomplete;
    }
    return SessionStatus::Incomplete;  // run 没封/session 没封,都是半场
}

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

SessionManager::SessionManager(SessionManagerOptions options, SessionManagerClock* clock)
    : options_(std::move(options)) {
    if (clock != nullptr) {
        clock_ = clock;
    }
    // P0-1:key 只吃递进的身份;空身份(旧测试/兜底)按 workspace_root 退
    // cwd_fallback 形状,不再各算各的 hash。
    if (!options_.identity.valid()) {
        options_.identity = workspace::MakeFallbackIdentity(options_.workspace_root);
    }
    workspace_key_ = options_.identity.workspace_key;
    workspace_dir_ = options_.workspaces_root / platform::Utf8ToPath(workspace_key_);
}

SessionManager::~SessionManager() = default;

bool SessionManager::EnsureWorkspace(std::string* error) {
    // P0-1:每次开张都走 manifest 对账 + checkout 登记(幂等):首仓原子写
    // v2;已存在则 key 对账(不合即隔离失败,不自动改名)+ last_opened/
    // checkouts 更新。不再"目录在就跳过"——否则 linked worktree 的检出
    // 登记永远缺账。
    auto workspace =
        TrajectoryDirectory::CreateWorkspace(options_.workspaces_root, options_.identity, clock_->WallMs());
    if (!workspace.has_value()) {
        *error = workspace.error();
        return false;
    }
    workspace_dir_ = workspace->workspace_dir();
    return true;
}

std::expected<void, std::string> SessionManager::RegisterCheckout(
    const workspace::WorkspaceIdentity& identity) {
    if (identity.workspace_key != workspace_key_) {
        return std::unexpected("identity.key_mismatch: 登记 key=" + identity.workspace_key +
                               " 与本 workspace key=" + workspace_key_ + " 不合;跨 workspace 切换须封场换账");
    }
    if (const auto registered =
            workspace::OpenOrRegisterWorkspace(options_.workspaces_root, identity, clock_->WallMs());
        !registered.has_value()) {
        return std::unexpected(registered.error());
    }
    return {};
}

std::filesystem::path SessionManager::SessionDirOf(const std::string& session_id) const {
    return workspace_dir_ / "sessions" / platform::Utf8ToPath(session_id);
}

std::string SessionManager::NextMainRunId() const {
    // main run 号按 workspace 既有 session 推最大值:新账新号,新 session 的
    // turn/request/call/local seq 全从新命名空间起号(§3.3.1)。
    int max_number = 0;
    std::error_code ec;
    const std::filesystem::path sessions = workspace_dir_ / "sessions";
    if (std::filesystem::exists(sessions, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
            const auto manifest = ReadSessionJson(entry.path());
            if (!manifest.has_value() || manifest->main_run_id.rfind("main-", 0) != 0) {
                continue;
            }
            const std::string number = manifest->main_run_id.substr(5);
            if (!number.empty() && std::all_of(number.begin(), number.end(),
                                               [](char c) { return c >= '0' && c <= '9'; })) {
                max_number = std::max(max_number, std::atoi(number.c_str()));
            }
        }
    }
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "main-%04d", max_number + 1);
    return buffer;
}

std::string SessionManager::NewStampId() const {
    std::tm parts{};
    NowStampParts(clock_, &parts);
    return GenerateSessionId(parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                             parts.tm_min, parts.tm_sec, clock_->Random6());
}

EventScope SessionManager::MainBaseScope(const SessionManifest& manifest) const {
    // session 边界事件是宿主控制(§5.5:宿主落状态变更才 actor=host);
    // control plane 一律 training_policy=exclude(§5.1)。
    EventScope scope;
    scope.workspace_key = workspace_key_;
    scope.session_id = manifest.session_id;
    scope.run_id = manifest.main_run_id;
    scope.run_kind = options_.main_run_kind;
    scope.actor = Actor::Host;
    scope.origin = Origin::ScheduledHost;
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = TrainingPolicy::Exclude;
    return scope;
}

std::expected<std::string, std::string> SessionManager::RunLifecycleOp(
    LifecycleOperation operation, const std::string& session_id, const nlohmann::json& parameters,
    const nlohmann::json& outcome) {
    LifecycleIntent intent;
    intent.operation_id = NewStampId();
    intent.operation = LifecycleOperationName(operation);
    intent.workspace_key = workspace_key_;
    intent.session_id = session_id;
    intent.requested_at_ms = clock_->WallMs();
    intent.parameters = parameters;
    const auto intent_dir = lifecycle().WriteIntent(intent);
    if (!intent_dir.has_value()) {
        return std::unexpected(intent_dir.error());
    }
    LifecycleResult result;
    result.operation_id = intent.operation_id;
    result.status = "completed";
    result.completed_at_ms = clock_->WallMs();
    result.outcome = outcome;
    if (const auto written = lifecycle().WriteResult(result); !written.has_value()) {
        return std::unexpected(written.error());
    }
    return intent.operation_id;
}

std::expected<ActiveSession*, std::string> SessionManager::LaunchSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (boundary_in_progress_) {
        return std::unexpected("session.boundary_in_progress: clear/close 未收完");
    }
    if (active_.has_value()) {
        return std::unexpected("session.already_active: 已有 active session(" +
                               active_->session_id() + "),先 clear/close 再开");
    }
    std::string error;
    if (!EnsureWorkspace(&error)) {
        return std::unexpected(error);
    }

    // create-new 建目录,session.json(status=preparing)。
    SessionManifest manifest;
    manifest.schema_version = 2;
    manifest.workspace_key = workspace_key_;
    manifest.session_id = NewStampId();
    manifest.launch_cwd = options_.launch_cwd;
    manifest.main_run_id = NextMainRunId();
    manifest.run_kind = RunKindName(options_.main_run_kind);
    manifest.start_reason = "process_launch";
    manifest.status = SessionStatusName(SessionStatus::Preparing);
    manifest.created_at_ms = clock_->WallMs();
    manifest.lubancode_version = options_.lubancode_version;
    // event schema major 钉进 manifest(存储 v2:recorder 写 v2 就得报 v2,
    // 读侧不重放整本也能认);从前漏写,session.json 恒报 1。
    manifest.event_schema_version = options_.recorder.event_schema_version;

    auto directory = TrajectoryDirectory::CreateSession(options_.workspaces_root,
                                                        workspace_key_, manifest);
    if (!directory.has_value()) {
        return std::unexpected("session.create_failed: " + directory.error());
    }
    auto lock_file = SessionLock::Acquire(directory->session_dir(), clock_->LockOwner());
    if (!lock_file.has_value()) {
        return std::unexpected("session.lock_failed: " + lock_file.error());
    }
    // lifecycle:create_session 一次管理操作一只目录(§3.2)。
    const std::string create_op = NewStampId();
    LifecycleIntent intent;
    intent.operation_id = create_op;
    intent.operation = LifecycleOperationName(LifecycleOperation::CreateSession);
    intent.workspace_key = workspace_key_;
    intent.session_id = manifest.session_id;
    intent.requested_at_ms = clock_->WallMs();
    intent.parameters["start_reason"] = manifest.start_reason;
    if (const auto intent_dir = lifecycle().WriteIntent(intent); !intent_dir.has_value()) {
        return std::unexpected("session.lifecycle_intent_failed: " + intent_dir.error());
    }

    // 开张:独占锁已握,main recorder 起 run.started(process_launch)。
    auto recorder = TrajectoryRecorder::Start(directory->main_stream_path(),
                                              directory->artifacts_root(), MainBaseScope(manifest),
                                              options_.recorder, clock_);
    if (!recorder.has_value()) {
        return std::unexpected("session.recorder_failed: " + recorder.error());
    }
    const auto started = recorder->WriteRunStarted(nlohmann::json{{"start_reason", "process_launch"}},
                                                   Durability::PowerLoss);
    if (started.status != RecordReceipt::Status::Committed) {
        return std::unexpected("session.run_start_failed: " + started.error_code);
    }
    ActiveSession session;
    session.directory = *directory;
    session.main = std::move(*recorder);
    session.manifest = manifest;
    session.lock = std::move(*lock_file);
    if (const auto transition = TransitionSessionStatus(session.session_dir(), &session.manifest,
                                                        SessionStatus::Running);
        !transition.has_value()) {
        // Journal 已 durable run.started;session.json 落后由恢复器按事实补正。
        return std::unexpected("session.status_write_failed: " + transition.error());
    }
    session.status = SessionStatus::Running;

    LifecycleResult result;
    result.operation_id = create_op;
    result.status = "completed";
    result.completed_at_ms = clock_->WallMs();
    result.outcome["session_dir"] = platform::PathToUtf8(session.session_dir());
    result.outcome["main_run_id"] = session.manifest.main_run_id;
    if (const auto written = lifecycle().WriteResult(result); !written.has_value()) {
        return std::unexpected("session.lifecycle_result_failed: " + written.error());
    }
    active_ = std::move(session);
    return &*active_;
}

SessionManager::ClosureEvidence SessionManager::CloseActiveWork(
    ActiveSession* session, ClearParticipant* participant, const std::string& cancel_reason,
    const std::string& selection_interrupt_reason) {
    ClosureEvidence evidence;
    // 活动 main turn 先收——run terminal 的硬前提(§6.2)。
    const std::string turn_id = participant->CancelActiveTurn();
    if (!turn_id.empty()) {
        RecordRequest request;
        request.kind = EventKind::TurnCancelled;
        request.scope = session->main->base_scope();
        request.scope.turn_id = turn_id;
        request.payload["reason"] = cancel_reason;
        const auto receipt = session->main->Record(request, Durability::PowerLoss);
        if (receipt.status == RecordReceipt::Status::Committed) {
            evidence.turn_cancelled_event_id = receipt.event_id;
        } else {
            evidence.unknown_present = true;
        }
    }
    // 未送达 queue item 逐枚 cancelled,不能清完内存便没了下文(§3.3.1)。
    for (const std::string& item_id : participant->CancelQueuedItems()) {
        RecordRequest request;
        request.kind = EventKind::ControlQueueItemCancelled;
        request.scope = session->main->base_scope();
        request.payload["item_id"] = item_id;
        request.payload["reason"] = cancel_reason;
        const auto receipt = session->main->Record(request, Durability::PowerLoss);
        if (receipt.status == RecordReceipt::Status::Committed) {
            evidence.queue_cancelled_event_ids.push_back(receipt.event_id);
        } else {
            evidence.unknown_present = true;
        }
    }
    // /record 选段先封 interrupted,不得跨 session 暗续(§3.3.1/§14.3)。
    const std::string selection_id = participant->ActiveRecordSelectionId();
    if (!selection_id.empty()) {
        RecordRequest request;
        request.kind = EventKind::RecordSelectionInterrupted;
        request.scope = session->main->base_scope();
        request.payload["record_id"] = selection_id;
        request.payload["reason"] = selection_interrupt_reason;
        const auto receipt = session->main->Record(request, Durability::PowerLoss);
        if (receipt.status == RecordReceipt::Status::Committed) {
            evidence.selection_interrupted_event_id = receipt.event_id;
        } else {
            evidence.unknown_present = true;
        }
    }
    // 已在跑 child 的收口申报:没收口或 unknown 都不算干净(§3.3.1)。
    for (const ClearParticipant::ChildClosure& child : participant->CancelActiveChildren()) {
        if (!child.terminal_written || child.unknown) {
            evidence.unknown_present = true;
        }
    }
    return evidence;
}

ClearOutcome SessionManager::Clear(const ClearRequest& request, ClearParticipant* participant) {
    // 快路径:换账掌管中直接回忙,不排队压 mutex(§3.3.1"重复请求要排队
    // 或回 clear_in_progress",这里选回忙)。
    if (boundary_in_progress_.load()) {
        ClearOutcome busy;
        busy.error_code = "clear.busy";
        busy.message = "同一时刻只许一场 clear(§3.3.1)";
        return busy;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ClearOutcome outcome;
    const auto fail = [&outcome](std::string code, std::string message) {
        outcome.error_code = std::move(code);
        outcome.message = std::move(message);
        return outcome;
    };
    if (boundary_in_progress_.load()) {
        return fail("clear.busy", "同一时刻只许一场 clear(§3.3.1)");
    }
    if (!active_.has_value() || active_->status != SessionStatus::Running) {
        return fail("clear.no_active_session", "没有可换的 active running session");
    }
    NullClearParticipant null_participant;
    if (participant == nullptr) {
        participant = &null_participant;
    }
    // 串行闸(锁内置位,门闩析构时仍在锁内复位):八步掌管期间,并发
    // 重复请求一律 clear.busy。
    boundary_in_progress_ = true;
    struct Gate {
        std::atomic<bool>& flag;
        ~Gate() { flag = false; }
    } gate{boundary_in_progress_};

    ActiveSession& old = *active_;
    outcome.old_session_id = old.session_id();
    outcome.old_main_run_id = old.manifest.main_run_id;

    // ---- 第 1 步:新 session_id create-new 建新目录,session.json(preparing)。
    SessionManifest new_manifest;
    new_manifest.schema_version = 2;
    new_manifest.workspace_key = workspace_key_;
    new_manifest.session_id = NewStampId();
    new_manifest.launch_cwd = old.manifest.launch_cwd;
    new_manifest.main_run_id = NextMainRunId();
    new_manifest.start_reason = "clear";
    new_manifest.previous_session_id = old.session_id();
    new_manifest.status = SessionStatusName(SessionStatus::Preparing);
    new_manifest.created_at_ms = clock_->WallMs();
    new_manifest.lubancode_version = old.manifest.lubancode_version;
    new_manifest.run_kind = RunKindName(options_.main_run_kind);
    new_manifest.event_schema_version = options_.recorder.event_schema_version;

    auto new_directory = TrajectoryDirectory::CreateSession(options_.workspaces_root,
                                                            workspace_key_, new_manifest);
    if (!new_directory.has_value()) {
        // 第 1 步失败:旧 session 仍可用,clear 返回失败(§3.3.1)。
        return fail("clear.step1_failed", new_directory.error());
    }
    auto new_lock = SessionLock::Acquire(new_directory->session_dir(), clock_->LockOwner());
    if (!new_lock.has_value()) {
        return fail("clear.step1_failed", new_lock.error());
    }
    outcome.boundary_operation_id = NewStampId();
    const std::string create_op = NewStampId();
    LifecycleIntent intent;
    intent.operation_id = create_op;
    intent.operation = LifecycleOperationName(LifecycleOperation::CreateSession);
    intent.workspace_key = workspace_key_;
    intent.session_id = new_manifest.session_id;
    intent.requested_at_ms = clock_->WallMs();
    intent.parameters["start_reason"] = "clear";
    intent.parameters["previous_session_id"] = old.session_id();
    intent.parameters["boundary_operation_id"] = outcome.boundary_operation_id;
    if (const auto intent_dir = lifecycle().WriteIntent(intent); !intent_dir.has_value()) {
        return fail("clear.step1_failed", intent_dir.error());
    }
    LifecycleResult create_result;
    create_result.operation_id = create_op;
    create_result.status = "completed";
    create_result.completed_at_ms = clock_->WallMs();
    create_result.outcome["session_dir"] = platform::PathToUtf8(new_directory->session_dir());
    create_result.outcome["status"] = SessionStatusName(SessionStatus::Preparing);
    if (const auto written = lifecycle().WriteResult(create_result); !written.has_value()) {
        return fail("clear.step1_failed", written.error());
    }
    outcome.new_session_id = new_manifest.session_id;
    outcome.new_main_run_id = new_manifest.main_run_id;
    outcome.new_session_prepared = true;

    // ---- 第 2 步:旧 main 写 qualified requested + session.clear_requested,
    // 均落 PowerLoss 档(append 后即 fsync,§7.4)。
    RecordRequest command_request;
    command_request.kind = EventKind::ControlCommandRequested;
    command_request.scope = old.main->base_scope();
    if (request.user_initiated) {
        command_request.scope.actor = Actor::User;
        command_request.scope.origin = Origin::ExternalUser;
    }
    command_request.payload["command_id"] = request.command_id;
    command_request.payload["command_name"] = "clear";
    command_request.payload["action_name"] = "clear";
    command_request.payload["effect_class"] = "session_boundary";
    command_request.payload["args_ref"] =
        nlohmann::json{{"boundary_operation_id", outcome.boundary_operation_id}};
    command_request.links.correlation_id = outcome.boundary_operation_id;
    const auto requested = old.main->Record(command_request, Durability::PowerLoss);
    if (requested.status != RecordReceipt::Status::Committed) {
        return fail("clear.step2_failed", "control.command.requested 落不了: " + requested.error_code);
    }
    outcome.requested_event_id = requested.event_id;

    RecordRequest clear_request_event;
    clear_request_event.kind = EventKind::SessionClearRequested;
    clear_request_event.scope = old.main->base_scope();
    clear_request_event.payload["next_session_id"] = new_manifest.session_id;
    if (!request.reason.empty()) {
        clear_request_event.payload["reason"] = request.reason;
    }
    clear_request_event.links.correlation_id = outcome.boundary_operation_id;
    const auto clear_requested = old.main->Record(clear_request_event, Durability::PowerLoss);
    if (clear_requested.status != RecordReceipt::Status::Committed) {
        // 换账没立起来:补一枚 command failed,旧 session 仍可用。
        RecordRequest failed_event;
        failed_event.kind = EventKind::ControlCommandFailed;
        failed_event.scope = old.main->base_scope();
        if (request.user_initiated) {
            failed_event.scope.actor = Actor::User;
            failed_event.scope.origin = Origin::ExternalUser;
        }
        failed_event.payload["command_id"] = request.command_id;
        failed_event.payload["reason"] = "clear_requested_write_failed";
        failed_event.payload["error_code"] = clear_requested.error_code;
        (void)old.main->Record(failed_event, Durability::PowerLoss);
        return fail("clear.step2_failed",
                    "session.clear_requested 落不了: " + clear_requested.error_code);
    }
    outcome.clear_requested_event_id = clear_requested.event_id;

    // ---- 第 3 步:停接新活,收口活动执行。session.json 转 closing(照
    // §3.3.2 图;写失败不挡——Journal 才是事实,恢复器会补)。
    SessionManifest closing_manifest = old.manifest;
    (void)TransitionSessionStatus(old.session_dir(), &closing_manifest, SessionStatus::Closing);
    old.manifest = closing_manifest;
    old.status = SessionStatus::Closing;
    const ClosureEvidence evidence =
        CloseActiveWork(&old, participant, "clear", "interrupted_by_clear");
    outcome.turn_cancelled_event_id = evidence.turn_cancelled_event_id;
    outcome.queue_cancelled_event_ids = evidence.queue_cancelled_event_ids;
    outcome.selection_interrupted_event_id = evidence.selection_interrupted_event_id;

    // closed 硬门(§3.3.2):盘上还有未收口的 stream 就不算干净。
    const bool unterminated = !UnterminatedStreamsInSession(old.session_dir()).empty();
    const bool unknown_present = evidence.unknown_present || unterminated;

    // ---- 第 4 步:旧流收齐写 run terminal;有 unknown 便 run.failed,
    // 随后 session.ended 封链。
    const EventKind terminal_kind = unknown_present ? EventKind::RunFailed : EventKind::RunCompleted;
    const auto run_terminal = old.main->FinishRun(terminal_kind, "clear", Durability::PowerLoss);
    if (run_terminal.status != RecordReceipt::Status::Committed) {
        return fail("clear.step4_failed", "run terminal 落不了: " + run_terminal.error_code);
    }
    outcome.old_run_terminal_event_id = run_terminal.event_id;
    outcome.old_run_terminal_kind = EventKindName(terminal_kind);
    outcome.old_close_quality = unknown_present ? "incomplete" : "clean";
    const auto ended = old.main->EndSession("clear", new_manifest.session_id,
                                            outcome.old_close_quality, Durability::PowerLoss);
    if (ended.status != RecordReceipt::Status::Committed) {
        return fail("clear.step4_failed", "session.ended 落不了: " + ended.error_code);
    }
    outcome.old_session_ended_event_id = ended.event_id;
    outcome.old_session_ended_ref = EventRef{old.session_id(), ended.event_id, ended.event_hash};

    // ---- 第 5 步:原子更新旧 session.json——干净收口 closed,否则 incomplete。
    const SessionStatus old_final =
        unknown_present ? SessionStatus::Incomplete : SessionStatus::Closed;
    if (const auto transition = TransitionSessionStatus(old.session_dir(), &old.manifest, old_final,
                                                        /*recovery_collapse=*/true);
        !transition.has_value()) {
        return fail("clear.step5_failed", transition.error());
    }
    old.status = old_final;
    outcome.old_session_json_finalized = true;

    // ---- 第 6 步:新 main 首条 run.started(start_reason=clear,反指旧
    // session 终态事件)+ 第二条跨 session control.command.completed,两条
    // 均逐条 fsync("一并 flush/fsync":第二条落稳前不许走第 7 步)。
    auto new_recorder =
        TrajectoryRecorder::Start(new_directory->main_stream_path(),
                                  new_directory->artifacts_root(), MainBaseScope(new_manifest),
                                  options_.recorder, clock_);
    if (!new_recorder.has_value()) {
        return fail("clear.step6_failed", new_recorder.error());
    }
    nlohmann::json start_extra;
    start_extra["start_reason"] = "clear";
    start_extra["previous_session_id"] = old.session_id();
    start_extra["caused_by_event_ref"] = outcome.old_session_ended_ref.ToJson();
    const auto new_started = new_recorder->WriteRunStarted(start_extra, Durability::PowerLoss);
    if (new_started.status != RecordReceipt::Status::Committed) {
        return fail("clear.step6_failed", "新 main run.started 落不了: " + new_started.error_code);
    }
    outcome.new_run_started_event_id = new_started.event_id;

    RecordRequest completed;
    completed.kind = EventKind::ControlCommandCompleted;
    completed.scope = new_recorder->base_scope();
    if (request.user_initiated) {
        completed.scope.actor = Actor::User;
        completed.scope.origin = Origin::ExternalUser;
    }
    completed.payload["command_id"] = request.command_id;
    completed.payload["status"] = "completed";
    completed.payload["qualified_requested_ref"] =
        nlohmann::json{{"session_id", old.session_id()},
                       {"event_id", outcome.requested_event_id}};
    completed.payload["boundary_operation_id"] = outcome.boundary_operation_id;
    completed.links.correlation_id = outcome.boundary_operation_id;
    const auto new_completed = new_recorder->Record(completed, Durability::PowerLoss);
    if (new_completed.status != RecordReceipt::Status::Committed) {
        return fail("clear.step6_failed",
                    "跨 session command completed 落不了: " + new_completed.error_code);
    }
    outcome.new_command_completed_event_id = new_completed.event_id;

    // ---- 第 7 步:原子更新新 session.json(status=running),再切进程内
    // active 指针。
    ActiveSession new_session;
    new_session.directory = *new_directory;
    new_session.main = std::move(*new_recorder);
    new_session.manifest = new_manifest;
    new_session.lock = std::move(*new_lock);
    if (const auto transition = TransitionSessionStatus(
            new_session.session_dir(), &new_session.manifest, SessionStatus::Running);
        !transition.has_value()) {
        return fail("clear.step7_failed", transition.error());
    }
    new_session.status = SessionStatus::Running;
    outcome.new_session_running = true;

    // 旧 recorder 关柄算整本 hash(§8.3),旧锁放掉,末后一次换指针。
    if (const auto sha = old.main->Close(); sha.has_value()) {
        outcome.old_journal_sha256 = *sha;
    }
    old.lock.Release();
    active_ = std::move(new_session);
    outcome.active_switched = true;

    // ---- 第 8 步:清内存状态(main history、turn state、临时队列、缓存),
    // 向界面报告 clear 完成(话术归前端,不进 Journal)。
    participant->ResetInMemoryState();
    return outcome;
}

CloseOutcome SessionManager::Close(const CloseRequest& request, ClearParticipant* participant) {
    // 快路径:与 clear 同一把串行闸,掌管中直接回忙。
    if (boundary_in_progress_.load()) {
        CloseOutcome busy;
        busy.error_code = "close.busy";
        busy.message = "clear/close 未收完";
        return busy;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    CloseOutcome outcome;
    const auto fail = [&outcome](std::string code, std::string message) {
        outcome.error_code = std::move(code);
        outcome.message = std::move(message);
        return outcome;
    };
    if (boundary_in_progress_.load()) {
        return fail("close.busy", "clear/close 未收完");
    }
    if (!active_.has_value() || active_->status != SessionStatus::Running) {
        return fail("close.no_active_session", "没有可封的 active running session");
    }
    boundary_in_progress_ = true;
    struct Gate {
        std::atomic<bool>& flag;
        ~Gate() { flag = false; }
    } gate{boundary_in_progress_};

    ActiveSession& session = *active_;
    outcome.session_id = session.session_id();
    // running -> closing:拒绝新活,只收 terminal(§3.3.2)。
    SessionManifest closing_manifest = session.manifest;
    (void)TransitionSessionStatus(session.session_dir(), &closing_manifest, SessionStatus::Closing);
    session.manifest = closing_manifest;
    session.status = SessionStatus::Closing;

    const ClosureEvidence evidence =
        CloseActiveWork(&session, participant, request.reason, "interrupted_by_" + request.reason);
    // closed 硬门:session 内 active stream count 必须为 0;收不回记
    // unknown,不写 clean closed(§3.3.2)。
    const bool unterminated = !UnterminatedStreamsInSession(session.session_dir()).empty();
    const bool unknown_present = evidence.unknown_present || unterminated;
    outcome.close_quality = unknown_present ? "incomplete" : "clean";

    const EventKind terminal_kind = unknown_present ? EventKind::RunFailed : EventKind::RunCompleted;
    const auto run_terminal = session.main->FinishRun(terminal_kind, request.reason,
                                                      Durability::PowerLoss);
    if (run_terminal.status != RecordReceipt::Status::Committed) {
        return fail("close.step2_failed", "run terminal 落不了: " + run_terminal.error_code);
    }
    outcome.run_terminal_kind = EventKindName(terminal_kind);
    const auto ended = session.main->EndSession(request.reason, std::nullopt, outcome.close_quality,
                                                Durability::PowerLoss);
    if (ended.status != RecordReceipt::Status::Committed) {
        return fail("close.step3_failed", "session.ended 落不了: " + ended.error_code);
    }
    const SessionStatus final_status =
        unknown_present ? SessionStatus::Incomplete : SessionStatus::Closed;
    if (const auto transition = TransitionSessionStatus(
            session.session_dir(), &session.manifest, final_status, /*recovery_collapse=*/true);
        !transition.has_value()) {
        return fail("close.step4_failed", transition.error());
    }
    session.status = final_status;
    if (const auto sha = session.main->Close(); sha.has_value()) {
        outcome.journal_sha256 = *sha;
    }
    // 封口即放锁:没有活 writer 的 session 不许再攥独占锁(§3.3.2)。
    session.lock.Release();
    return outcome;
}

// ---------------------------------------------------------------------------
// resume-as-new(§10.4 七步)
// ---------------------------------------------------------------------------

std::string SessionManager::LatestResumableSessionId() {
    std::lock_guard<std::mutex> lock(mutex_);
    return LatestResumableSessionIdLocked();
}

// 调用方须已持 mutex_(ResumeAsNew 七步内取默认源用,不再二次加锁)。
std::string SessionManager::LatestResumableSessionIdLocked() {
    std::error_code ec;
    const auto sessions = workspace_dir_ / "sessions";
    if (!std::filesystem::exists(sessions, ec)) {
        return std::string();
    }
    std::string best;
    std::int64_t best_created = -1;
    for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string id = platform::PathToUtf8(entry.path().filename());
        if (active_.has_value() && id == active_->session_id()) {
            continue;  // 自己这场不作为 resume 源
        }
        const auto manifest = ReadSessionJson(entry.path());
        if (!manifest.has_value()) {
            continue;
        }
        const auto status = SessionStatusFromName(manifest->status);
        // 可恢复源:closed/archived 为正路;无活 writer 的 incomplete 按
        // 已验证前缀 resume-as-new(§3.3.2)。running/preparing 不碰。
        if (!status.has_value() || (*status != SessionStatus::Closed &&
                                    *status != SessionStatus::Archived &&
                                    *status != SessionStatus::Incomplete)) {
            continue;
        }
        // 单发场不作为恢复候选(单发轨迹断档单:单发语义不续,审计可读)
        //——--continue 与裸 /resume 不许悄悄把一场 one_shot 折叠成新交互场。
        if (manifest->run_kind == RunKindName(RunKind::OneShot)) {
            continue;
        }
        if (!std::filesystem::exists(entry.path() / "main.jsonl", ec)) {
            continue;
        }
        if (manifest->created_at_ms > best_created) {
            best_created = manifest->created_at_ms;
            best = id;
        }
    }
    return best;
}

ResumeOutcome SessionManager::ResumeAsNew(const ResumeRequest& request) {
    if (boundary_in_progress_.load()) {
        ResumeOutcome busy;
        busy.error_code = "resume.busy";
        busy.message = "clear/close 未收完";
        return busy;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ResumeOutcome outcome;
    const auto fail = [&outcome](std::string code, std::string message) {
        outcome.error_code = std::move(code);
        outcome.message = std::move(message);
        return outcome;
    };
    if (boundary_in_progress_.load()) {
        return fail("resume.busy", "clear/close 未收完");
    }
    // active 残留判定:Close 封口后 active_ 仍挂着(进程一场、封口即退的
    // 语义),但 status 已是 Closed/Incomplete——没有活 writer,不算占着;
    // 只有 Running/Preparing/Closing 的活场才挡 resume(交互 /resume 须先
    // Close(switch_to_resume))。
    const bool active_live =
        active_.has_value() && (active_->status == SessionStatus::Running ||
                                active_->status == SessionStatus::Preparing ||
                                active_->status == SessionStatus::Closing);
    if (active_live) {
        return fail("resume.busy",
                    "active session 还开着(" + active_->session_id() +
                        ");交互 /resume 先 Close(switch_to_resume) 再来");
    }
    std::string error;
    if (!EnsureWorkspace(&error)) {
        return fail("resume.step1_failed", error);
    }
    boundary_in_progress_ = true;
    struct Gate {
        std::atomic<bool>& flag;
        ~Gate() { flag = false; }
    } gate{boundary_in_progress_};

    // ---- 第 1 步:锁定并读取 source,验 schema/逐流 seq/hash chain/父子边。
    std::string source_id = request.source_session_id;
    if (source_id.empty()) {
        source_id = LatestResumableSessionIdLocked();
        if (source_id.empty()) {
            return fail("resume.source_not_found", "本 workspace 没有可恢复的 session");
        }
    }
    // §12.1:用户递进来的 session ref 先过单段名校验,拒绝路径逃逸。
    if (!IsSafeSingleSegment(source_id)) {
        return fail("resume.source_invalid_ref", "session id 须是单段名(不带路径): " + source_id);
    }
    outcome.source_session_id = source_id;
    const auto source_dir = SessionDirOf(source_id);
    if (!std::filesystem::is_directory(source_dir)) {
        return fail("resume.source_not_found", "source session 目录不存在");
    }
    // 单发场不可 resume(单发轨迹断档单):单发语义不续,审计可读——/resume
    // <id> 指名要续也明拒,不折叠成新交互场。manifest 读不动照旧往下走,
    // 由后面的验账说话。
    if (const auto source_manifest = ReadSessionJson(source_dir); source_manifest.has_value()) {
        if (source_manifest->run_kind == RunKindName(RunKind::OneShot)) {
            return fail("resume.source_not_resumable",
                        "单发场(one_shot)不参与 resume:轨迹可审计读取,不续聊");
        }
    }
    // 活锁在外进程:默认拒绝(§10.4 末段)。本进程的 active 已在上面拦了。
    if (const auto holder = SessionLock::Inspect(source_dir); holder.has_value()) {
        if (ProbeLockHolder(*holder) == LockHolderState::Alive) {
            return fail("resume.source_locked", "source session 仍被别的进程持写锁");
        }
    }
    // 逐流验链 + 父子边交叉核(§3.9)。尾行截断是可恢复缺口:按已验证
    // 前缀续(§3.3.2);链断/坏行才是 corrupt。
    const auto verify = VerifySessionDir(source_dir);
    bool truncated_tail = false;
    for (const auto& stream : verify.streams) {
        if (stream.error_code == "verify.truncated_tail") {
            truncated_tail = true;
        }
    }
    const bool corrupt = !verify.ok && !truncated_tail;
    if (corrupt) {
        return fail("resume.source_corrupt",
                    verify.message.empty() ? verify.error_code : verify.message);
    }
    outcome.source_verified = true;
    outcome.source_truncated_tail = truncated_tail;

    // ---- 第 2/3 步:找最后一枚完整 checkpoint;没有便从头折叠。折叠出
    // effective conversation、control state 与 state hash。
    const auto main_stream = source_dir / "main.jsonl";
    ReplayReport fold = FoldStreamReplay(main_stream);
    if (fold.ok()) {
        outcome.from_checkpoint = false;
    } else if (fold.error_code == "replay.unsupported") {
        return fail("resume.source_unsupported", fold.message);
    } else {
        return fail("resume.source_corrupt", fold.error_code + ": " + fold.message);
    }
    const auto checkpoint = FindLatestUsableCheckpoint(source_dir, "main", main_stream);
    if (checkpoint.has_value()) {
        ReplayState continued = checkpoint->folded;
        std::string continue_error;
        if (ContinueFoldFrom(main_stream, &continued, &continue_error)) {
            fold.state = std::move(continued);
            outcome.from_checkpoint = true;
            outcome.checkpoint_seq = checkpoint->source_seq;
            outcome.checkpoint_event_hash = checkpoint->source_event_hash;
        }
        // 续折失败:checkpoint 缓存靠不住,退回整折(上面的 fold 还在)。
    }
    outcome.source_event_count = fold.state.integrity.events_folded;
    outcome.source_main_last_event_hash = fold.state.integrity.last_event_hash;
    outcome.replay_version = std::to_string(kReplayProjectionVersion);
    outcome.imported_state_hash = ComputeReplayStateHash(fold.state);
    outcome.effective_conversation = fold.state.effective_conversation;
    outcome.control = fold.state.control;
    // 已完成的 child 只在 verifier 里核过 terminal hash;正文不进新 main
    //(§10.4"不把正文灌进新 main.jsonl",effective history 只引用 source
    // 事件——ReplayMessage 带的就是 source event id,不复制 child 细账)。

    // ---- 第 4 步:尾部悬空工具按三道账给明确状态;未知副作用不可重跑。
    outcome.dangling_tools = CollectDanglingTools(fold.state);

    // ---- 第 5 步:建新 session 与新 main.jsonl,首条 run.started(resume)。
    std::string previous_session_id = request.previous_session_id;
    if (previous_session_id.empty() && !request.interactive) {
        previous_session_id = source_id;  // --continue:直接前驱就是 source
    }
    SessionManifest manifest;
    manifest.schema_version = 2;
    manifest.workspace_key = workspace_key_;
    manifest.session_id = NewStampId();
    manifest.launch_cwd = options_.launch_cwd;
    manifest.main_run_id = NextMainRunId();
    manifest.start_reason = "resume";
    manifest.previous_session_id =
        previous_session_id.empty() ? std::optional<std::string>{} : std::optional(previous_session_id);
    manifest.status = SessionStatusName(SessionStatus::Preparing);
    manifest.created_at_ms = clock_->WallMs();
    manifest.lubancode_version = options_.lubancode_version;
    manifest.run_kind = RunKindName(options_.main_run_kind);
    manifest.event_schema_version = options_.recorder.event_schema_version;

    auto directory = TrajectoryDirectory::CreateSession(options_.workspaces_root,
                                                        workspace_key_, manifest);
    if (!directory.has_value()) {
        return fail("resume.step5_failed", directory.error());
    }
    auto lock_file = SessionLock::Acquire(directory->session_dir(), clock_->LockOwner());
    if (!lock_file.has_value()) {
        return fail("resume.step5_failed", lock_file.error());
    }
    // lifecycle:create_session + resume_reference(§3.2 恢复引用账)。
    const std::string create_op = NewStampId();
    LifecycleIntent intent;
    intent.operation_id = create_op;
    intent.operation = LifecycleOperationName(LifecycleOperation::CreateSession);
    intent.workspace_key = workspace_key_;
    intent.session_id = manifest.session_id;
    intent.requested_at_ms = clock_->WallMs();
    intent.parameters["start_reason"] = "resume";
    intent.parameters["resumed_from_session_id"] = source_id;
    if (const auto intent_dir = lifecycle().WriteIntent(intent); !intent_dir.has_value()) {
        return fail("resume.step5_failed", intent_dir.error());
    }
    auto recorder = TrajectoryRecorder::Start(directory->main_stream_path(),
                                              directory->artifacts_root(), MainBaseScope(manifest),
                                              options_.recorder, clock_);
    if (!recorder.has_value()) {
        return fail("resume.step5_failed", recorder.error());
    }
    nlohmann::json start_extra;
    start_extra["start_reason"] = "resume";
    start_extra["resumed_from_session_id"] = source_id;
    if (!previous_session_id.empty()) {
        start_extra["previous_session_id"] = previous_session_id;
    }
    // source 末枚事件的 qualified ref(seq 折叠高水位)。
    const std::string source_last_event_id =
        FormatEventId(fold.state.run_id, fold.state.integrity.events_folded);
    start_extra["caused_by_event_ref"] =
        EventRef{source_id, source_last_event_id, fold.state.integrity.last_event_hash}.ToJson();
    const auto started = recorder->WriteRunStarted(start_extra, Durability::PowerLoss);
    if (started.status != RecordReceipt::Status::Committed) {
        return fail("resume.step5_failed", "新 main run.started 落不了: " + started.error_code);
    }
    outcome.new_session_id = manifest.session_id;
    outcome.new_main_run_id = manifest.main_run_id;
    outcome.new_run_started_event_id = started.event_id;

    // ---- 第 6 步:resume.source.attached(source id/末 hash/replay 版本/
    // imported state hash/checkpoint ref/qualified refs);交互路再补跨
    // session command.completed(qualified ref 指回旧 requested)。
    RecordRequest attached;
    attached.kind = EventKind::ResumeSourceAttached;
    attached.scope = recorder->base_scope();
    attached.payload["source_session_id"] = source_id;
    attached.payload["source_terminal_event_hash"] = outcome.source_main_last_event_hash;
    attached.payload["replay_version"] = outcome.replay_version;
    attached.payload["imported_state_hash"] = outcome.imported_state_hash;
    if (outcome.from_checkpoint) {
        attached.payload["checkpoint_ref"] = nlohmann::json{
            {"seq", outcome.checkpoint_seq}, {"source_event_hash", outcome.checkpoint_event_hash}};
    }
    attached.payload["qualified_event_refs"] = nlohmann::json::array({EventRef{
        source_id, source_last_event_id, outcome.source_main_last_event_hash}.ToJson()});
    const auto attached_receipt = recorder->Record(attached, Durability::PowerLoss);
    if (attached_receipt.status != RecordReceipt::Status::Committed) {
        return fail("resume.step6_failed",
                    "resume.source.attached 落不了: " + attached_receipt.error_code);
    }
    outcome.resume_attached_event_id = attached_receipt.event_id;

    if (request.interactive) {
        RecordRequest completed;
        completed.kind = EventKind::ControlCommandCompleted;
        completed.scope = recorder->base_scope();
        if (request.user_initiated) {
            completed.scope.actor = Actor::User;
            completed.scope.origin = Origin::ExternalUser;
        }
        completed.payload["command_id"] = request.boundary_command.command_id;
        completed.payload["status"] = "completed";
        completed.payload["qualified_requested_ref"] =
            nlohmann::json{{"session_id", request.boundary_command.requested_session_id},
                           {"event_id", request.boundary_command.requested_event_id}};
        completed.payload["boundary_operation_id"] = request.boundary_command.boundary_operation_id;
        completed.links.correlation_id = request.boundary_command.boundary_operation_id;
        const auto completed_receipt = recorder->Record(completed, Durability::PowerLoss);
        if (completed_receipt.status != RecordReceipt::Status::Committed) {
            return fail("resume.step6_failed",
                        "跨 session command completed 落不了: " + completed_receipt.error_code);
        }
        outcome.command_completed_event_id = completed_receipt.event_id;
    }

    // ---- 第 7 步:session.json 转 running,切 active;新 turn/request/
    // call/seq 全从新命名空间起号(新 recorder 天然新号,§10.4 第 7 步)。
    ActiveSession session;
    session.directory = *directory;
    session.main = std::move(*recorder);
    session.manifest = manifest;
    session.lock = std::move(*lock_file);
    if (const auto transition = TransitionSessionStatus(session.session_dir(), &session.manifest,
                                                        SessionStatus::Running);
        !transition.has_value()) {
        return fail("resume.step7_failed", transition.error());
    }
    session.status = SessionStatus::Running;
    outcome.new_session_running = true;

    LifecycleResult result;
    result.operation_id = create_op;
    result.status = "completed";
    result.completed_at_ms = clock_->WallMs();
    result.outcome["session_dir"] = platform::PathToUtf8(session.session_dir());
    result.outcome["resumed_from_session_id"] = source_id;
    result.outcome["imported_state_hash"] = outcome.imported_state_hash;
    if (const auto written = lifecycle().WriteResult(result); !written.has_value()) {
        return fail("resume.step5_failed", written.error());
    }
    active_ = std::move(session);
    outcome.active_switched = true;
    return outcome;
}

std::vector<std::filesystem::path> SessionManager::UnterminatedStreamsInSession(
    const std::filesystem::path& session_dir) {
    // main.jsonl 不算——封口时它正在写 terminal。goal/loop 首版只有占位
    // 目录,有文件照算(§3.7)。
    std::vector<std::filesystem::path> unterminated;
    std::error_code ec;
    const auto scan_dir = [&](const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir, ec)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".jsonl") {
                continue;
            }
            if (!ScanStreamFacts(entry.path()).run_terminal) {
                unterminated.push_back(entry.path());
            }
        }
    };
    scan_dir(session_dir / "subagents");
    scan_dir(session_dir / "goals");
    scan_dir(session_dir / "loops");
    const std::filesystem::path workflows = session_dir / "workflows";
    if (std::filesystem::exists(workflows, ec)) {
        for (const auto& run : std::filesystem::directory_iterator(workflows, ec)) {
            if (!run.is_directory(ec)) {
                continue;
            }
            const std::filesystem::path main_stream = run.path() / "workflow.jsonl";
            if (std::filesystem::exists(main_stream, ec) &&
                !ScanStreamFacts(main_stream).run_terminal) {
                unterminated.push_back(main_stream);
            }
            scan_dir(run.path() / "nodes");
        }
    }
    return unterminated;
}

// ---------------------------------------------------------------------------
// 恢复器(§3.3.1 末段/§3.3.2)
// ---------------------------------------------------------------------------

namespace {

// 陈旧/无主的旧账续办:收悬空、封 run terminal、封 session.ended。
// 返回 false = 没封干净(调用方按事实标 incomplete,不假装 closed)。
bool SealSessionForClear(TrajectoryRecorder* main, const std::filesystem::path& session_dir,
                         const MainJournalFacts& facts) {
    bool unknown = false;
    const auto recovery_event = [main](EventKind kind, std::optional<std::string> turn_id,
                                       nlohmann::json payload) {
        RecordRequest request;
        request.kind = kind;
        request.scope = main->base_scope();
        request.scope.actor = Actor::Host;
        request.scope.origin = Origin::RecoveryRuntime;
        request.scope.turn_id = std::move(turn_id);
        request.payload = std::move(payload);
        return main->Record(request, Durability::PowerLoss);
    };
    for (const std::string& record_id : facts.dangling_selection_ids) {
        const auto receipt =
            recovery_event(EventKind::RecordSelectionInterrupted, std::nullopt,
                           nlohmann::json{{"record_id", record_id},
                                          {"reason", "interrupted_by_clear"}});
        unknown |= receipt.status != RecordReceipt::Status::Committed;
    }
    for (const std::string& item_id : facts.dangling_queue_item_ids) {
        const auto receipt =
            recovery_event(EventKind::ControlQueueItemCancelled, std::nullopt,
                           nlohmann::json{{"item_id", item_id},
                                          {"reason", "clear_crash_recovery"}});
        unknown |= receipt.status != RecordReceipt::Status::Committed;
    }
    for (const std::string& turn_id : facts.dangling_turn_ids) {
        const auto receipt = recovery_event(
            EventKind::TurnCancelled, turn_id, nlohmann::json{{"reason", "clear_crash_recovery"}});
        unknown |= receipt.status != RecordReceipt::Status::Committed;
    }
    // 子流没 terminal 的:续账后补 run.cancelled。崩前没收口的执行,副作用
    // 便是不可判——补写成功也不许算 clean(§3.3.2:收不回记 unknown,不写
    // clean closed)。
    for (const std::filesystem::path& stream :
         SessionManager::UnterminatedStreamsInSession(session_dir)) {
        unknown = true;
        auto child = TrajectoryRecorder::Continue(stream, session_dir / "artifacts");
        if (!child.has_value()) {
            continue;
        }
        const MainJournalFacts child_facts = ScanStreamFacts(stream);
        for (const std::string& turn_id : child_facts.dangling_turn_ids) {
            RecordRequest cancel;
            cancel.kind = EventKind::TurnCancelled;
            cancel.scope = child->base_scope();
            cancel.scope.actor = Actor::Host;
            cancel.scope.origin = Origin::RecoveryRuntime;
            cancel.scope.turn_id = turn_id;
            cancel.payload["reason"] = "clear_crash_recovery";
            (void)child->Record(cancel, Durability::PowerLoss);
        }
        (void)child->FinishRun(EventKind::RunCancelled, "crash_recovery_unknown",
                               Durability::PowerLoss);
    }
    const EventKind terminal = unknown ? EventKind::RunFailed : EventKind::RunCompleted;
    const auto terminal_receipt = main->FinishRun(terminal, "clear", Durability::PowerLoss);
    if (terminal_receipt.status != RecordReceipt::Status::Committed) {
        return false;
    }
    const std::string close_quality = unknown ? "incomplete" : "clean";
    const auto ended = main->EndSession("clear", facts.clear_requested_next_session_id,
                                        close_quality, Durability::PowerLoss);
    return ended.status == RecordReceipt::Status::Committed;
}

}  // namespace

void SessionManager::ContinueNewSide(const std::filesystem::path& next_dir,
                                     const std::string& next_id, const MainJournalFacts& next_facts,
                                     const MainJournalFacts& old_facts, SessionRecoveryEntry* old_entry,
                                     WorkspaceRecoveryReport* report) {
    SessionRecoveryEntry next_entry;
    next_entry.session_id = next_id;
    next_entry.clear_continued = true;

    auto next_lock = SessionLock::Acquire(next_dir, clock_->LockOwner());
    if (!next_lock.has_value()) {
        next_entry.status = SessionStatus::Incomplete;
        next_entry.notes.push_back("新账锁拿不到: " + next_lock.error());
        report->sessions.push_back(std::move(next_entry));
        return;
    }
    const std::filesystem::path next_main = next_dir / "main.jsonl";
    std::optional<TrajectoryRecorder> recorder;
    if (next_facts.has_run_started) {
        // 半开的新账:合法 run.started 在,它便是一场独立 session(§3.3.1)。
        auto continued = TrajectoryRecorder::Continue(next_main, next_dir / "artifacts",
                                                      options_.recorder, clock_);
        if (!continued.has_value()) {
            next_entry.status = SessionStatus::Incomplete;
            next_entry.notes.push_back("新 main 续账拒开: " + continued.error());
            report->sessions.push_back(std::move(next_entry));
            return;
        }
        recorder = std::move(*continued);
    } else {
        // 空 preparing 的新账:替它把 run.started 开张。
        if (next_facts.journal_exists && next_facts.event_count > 0) {
            // 有字节却没有 run.started:事实可疑,只读隔离,不删不写。
            next_entry.status = SessionStatus::Incomplete;
            next_entry.notes.push_back("新 main 有行无 run.started,只读隔离");
            report->sessions.push_back(std::move(next_entry));
            return;
        }
        if (next_facts.journal_exists) {
            // 零字节占位可回收(§3.10),删了再 create-new。
            std::error_code remove_ec;
            std::filesystem::remove(next_main, remove_ec);
        }
        SessionManifest manifest;
        manifest.schema_version = 2;
        manifest.workspace_key = workspace_key_;
        manifest.session_id = next_id;
        manifest.launch_cwd = options_.launch_cwd;
        // main_run_id 用 step 1 已写进 session.json 的那个:半途的换账不许
        // 给新账换第二个 run 号。
        const auto disk_manifest = ReadSessionJson(next_dir);
        manifest.main_run_id =
            disk_manifest.has_value() ? disk_manifest->main_run_id : NextMainRunId();
        manifest.start_reason = "clear";
        manifest.previous_session_id = old_entry->session_id;
        manifest.status = SessionStatusName(SessionStatus::Preparing);
        manifest.created_at_ms = clock_->WallMs();
        manifest.lubancode_version = options_.lubancode_version;
        manifest.run_kind = RunKindName(options_.main_run_kind);
        manifest.event_schema_version = options_.recorder.event_schema_version;
        const TrajectoryDirectory directory = TrajectoryDirectory::OpenExisting(next_dir);
        auto started = TrajectoryRecorder::Start(directory.main_stream_path(),
                                                 directory.artifacts_root(),
                                                 MainBaseScope(manifest), options_.recorder, clock_);
        if (!started.has_value()) {
            next_entry.status = SessionStatus::Incomplete;
            next_entry.notes.push_back("新 main 开张拒: " + started.error());
            report->sessions.push_back(std::move(next_entry));
            return;
        }
        nlohmann::json start_extra;
        start_extra["start_reason"] = "clear";
        start_extra["previous_session_id"] = old_entry->session_id;
        if (!old_facts.session_ended_event_id.empty()) {
            start_extra["caused_by_event_ref"] =
                EventRef{old_entry->session_id, old_facts.session_ended_event_id,
                         old_facts.session_ended_event_hash}
                    .ToJson();
        }
        const auto started_receipt = started->WriteRunStarted(start_extra, Durability::PowerLoss);
        if (started_receipt.status != RecordReceipt::Status::Committed) {
            next_entry.status = SessionStatus::Incomplete;
            next_entry.notes.push_back("run.started 补写失败: " + started_receipt.error_code);
            report->sessions.push_back(std::move(next_entry));
            return;
        }
        recorder = std::move(*started);
    }

    // 补第 6 步第二条:跨 session control.command.completed(两个分支共用;
    // 空账开张刚落 run.started,半账续写前查过 facts)。
    if (!next_facts.command_completed) {
        RecordRequest completed;
        completed.kind = EventKind::ControlCommandCompleted;
        completed.scope = recorder->base_scope();
        completed.scope.actor = Actor::Host;
        completed.scope.origin = Origin::RecoveryRuntime;
        completed.payload["command_id"] =
            old_facts.command_id.empty() ? "cmd-clear-recovered" : old_facts.command_id;
        completed.payload["status"] = "completed";
        completed.payload["qualified_requested_ref"] =
            nlohmann::json{{"session_id", old_entry->session_id},
                           {"event_id", old_facts.command_requested_event_id}};
        if (!old_facts.boundary_operation_id.empty()) {
            completed.payload["boundary_operation_id"] = old_facts.boundary_operation_id;
            completed.links.correlation_id = old_facts.boundary_operation_id;
        }
        const auto receipt = recorder->Record(completed, Durability::PowerLoss);
        next_entry.notes.push_back(receipt.status == RecordReceipt::Status::Committed
                                       ? "补跨 session command completed"
                                       : "command completed 补写失败: " + receipt.error_code);
    }

    // session.json 转 running(空账新写,半账按事实折叠补正)。
    auto manifest = ReadSessionJson(next_dir);
    if (!manifest.has_value()) {
        manifest = SessionManifest{};
        manifest->schema_version = 2;
        manifest->workspace_key = workspace_key_;
        manifest->session_id = next_id;
        manifest->main_run_id = recorder->base_scope().run_id;
        manifest->start_reason = "clear";
        manifest->previous_session_id = old_entry->session_id;
        manifest->status = SessionStatusName(SessionStatus::Preparing);
    }
    if (TransitionSessionStatus(next_dir, &*manifest, SessionStatus::Running,
                                /*recovery_collapse=*/true)
            .has_value()) {
        next_entry.session_json_corrected = true;
    }
    next_entry.status = SessionStatus::Running;
    next_entry.notes.push_back("换账续办完成,新账 running");

    if (!active_.has_value()) {
        ActiveSession session;
        session.directory = TrajectoryDirectory::OpenExisting(next_dir);
        session.main = std::move(*recorder);
        session.manifest = *manifest;
        session.lock = std::move(*next_lock);
        session.status = SessionStatus::Running;
        active_ = std::move(session);
        report->adopted_session_id = next_id;
    }
    report->sessions.push_back(std::move(next_entry));
}

SessionRecoveryEntry SessionManager::AbortEmptyPreparing(const std::filesystem::path& session_dir,
                                                         const std::string& session_id) {
    SessionRecoveryEntry entry;
    entry.session_id = session_id;
    entry.aborted_before_start = true;
    const auto operation = RunLifecycleOp(
        LifecycleOperation::DeleteSession, session_id,
        nlohmann::json{{"reason", "aborted_before_start"}}, nlohmann::json{{"tombstone", true}});
    if (!operation.has_value()) {
        entry.aborted_before_start = false;
        entry.notes.push_back("lifecycle 删账落不了: " + operation.error());
        return entry;
    }
    SessionTombstone tombstone;
    tombstone.session_id = session_id;
    tombstone.deleted_at_ms = clock_->WallMs();
    tombstone.reason = "aborted_before_start";
    tombstone.operation_id = *operation;
    if (const auto written = WriteSessionTombstone(workspace_dir_ / "tombstones", tombstone);
        !written.has_value()) {
        entry.aborted_before_start = false;
        entry.notes.push_back(written.error());
        return entry;
    }
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);
    entry.notes.push_back("空 preparing 清账,tombstone 已留");
    return entry;
}

WorkspaceRecoveryReport SessionManager::RecoverWorkspace(ClearRecoveryPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    WorkspaceRecoveryReport report;
    std::string error;
    if (!EnsureWorkspace(&error)) {
        return report;
    }

    // 第一遍:只扫事实。换账新旧两侧可能任意排序(session_id 的时间戳不
    // 保证旧侧在前),先把 old -> next 的换账关系找齐再按依赖次序办。
    struct Scanned {
        std::filesystem::path dir;
        MainJournalFacts facts;
    };
    std::map<std::string, Scanned> scanned;
    std::error_code ec;
    const std::filesystem::path sessions = workspace_dir_ / "sessions";
    if (std::filesystem::exists(sessions, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(sessions, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            const std::string id = platform::PathToUtf8(entry.path().filename());
            scanned.emplace(id, Scanned{entry.path(), ScanStreamFacts(entry.path() / "main.jsonl")});
        }
    }
    // next_targets:某场换账要开的新账(它们归旧侧那一轮续办,不走通用路径)。
    std::map<std::string, std::string> next_targets;  // next_id -> old_id
    for (const auto& [id, scan] : scanned) {
        if (scan.facts.clear_requested && !scan.facts.clear_requested_next_session_id.empty()) {
            next_targets.emplace(scan.facts.clear_requested_next_session_id, id);
        }
    }

    std::set<std::string> handled;  // 已出账的 session
    // 单场 session 的通用恢复(不含换账新侧续办——那归旧侧办完后触发)。
    const auto process = [&](const std::string& session_id) {
        const Scanned& scan = scanned.at(session_id);
        const std::filesystem::path& session_dir = scan.dir;
        MainJournalFacts facts = scan.facts;

        SessionRecoveryEntry entry;
        entry.session_id = session_id;
        entry.status = DeriveSessionStatusFromFacts(facts);

        // 活锁在外进程:只读不动(§7.5 inspect 可并行,不抢活人的账)。
        const auto holder = SessionLock::Inspect(session_dir);
        const bool foreign_live = holder.has_value() &&
                                  ProbeLockHolder(*holder) == LockHolderState::Alive &&
                                  holder->pid != clock_->LockOwner().pid;
        if (foreign_live) {
            entry.owned_by_live_process = true;
            entry.notes.push_back("活锁 pid=" + std::to_string(holder->pid));
            report.sessions.push_back(std::move(entry));
            return;
        }

        auto manifest = ReadSessionJson(session_dir);
        if (!manifest.has_value() && facts.has_run_started) {
            // session.json 坏/丢:按 Journal 可证事实重建索引(§3.3.2)。
            SessionManifest rebuilt;
            rebuilt.schema_version = 2;
            rebuilt.workspace_key = workspace_key_;
            rebuilt.session_id = session_id;
            rebuilt.launch_cwd = options_.launch_cwd;
            const auto colon = facts.first_event_id.find(':');
            rebuilt.main_run_id = colon == std::string::npos
                                      ? facts.first_event_id
                                      : facts.first_event_id.substr(0, colon);
            rebuilt.start_reason = facts.start_reason.empty() ? "unknown" : facts.start_reason;
            rebuilt.previous_session_id = facts.previous_session_id;
            rebuilt.status = SessionStatusName(entry.status);
            rebuilt.created_at_ms = clock_->WallMs();
            rebuilt.lubancode_version = options_.lubancode_version;
            manifest = std::move(rebuilt);
            entry.session_json_corrected = true;
        }
        if (!manifest.has_value()) {
            // 连 Journal 都没有事实:空 preparing 孤儿,走清账。
            if (next_targets.count(session_id) == 0) {
                const SessionRecoveryEntry aborted = AbortEmptyPreparing(session_dir, session_id);
                entry.aborted_before_start = aborted.aborted_before_start;
                for (const std::string& note : aborted.notes) {
                    entry.notes.push_back(note);
                }
            }
            report.sessions.push_back(std::move(entry));
            return;
        }

        // ---- 换账旧侧续办:clear_requested 已 durable、账未封 ----
        if (facts.clear_requested && !facts.session_ended && entry.status != SessionStatus::Corrupt) {
            auto stale_lock = SessionLock::Acquire(session_dir, clock_->LockOwner());
            if (!stale_lock.has_value()) {
                entry.notes.push_back("旧账锁拿不到: " + stale_lock.error());
            } else {
                auto recorder = TrajectoryRecorder::Continue(session_dir / "main.jsonl",
                                                             session_dir / "artifacts",
                                                             options_.recorder, clock_);
                if (!recorder.has_value()) {
                    entry.notes.push_back("旧 main 续账拒开: " + recorder.error());
                    entry.status = SessionStatus::Corrupt;
                } else if (SealSessionForClear(&*recorder, session_dir, facts)) {
                    entry.clear_continued = true;
                    stale_lock->Release();
                    facts = ScanStreamFacts(session_dir / "main.jsonl");
                    entry.status = DeriveSessionStatusFromFacts(facts);
                    entry.notes.push_back("旧账已按 clear 封链: " + facts.session_ended_event_id);
                } else {
                    entry.notes.push_back("旧账续封失败,按事实标 incomplete");
                    entry.status = SessionStatus::Incomplete;
                }
            }
        }

        // session.json 与 Journal 事实不符:恢复器重建(折叠迁移,事实赢)。
        const std::string disk_status = manifest->status.empty()
                                            ? SessionStatusName(SessionStatus::Preparing)
                                            : manifest->status;
        if (disk_status != SessionStatusName(entry.status) &&
            entry.status != SessionStatus::Preparing) {
            if (TransitionSessionStatus(session_dir, &*manifest, entry.status,
                                        /*recovery_collapse=*/true)
                    .has_value()) {
                entry.session_json_corrected = true;
            } else {
                entry.notes.push_back("状态补正被拒: " + disk_status + " -> " +
                                      SessionStatusName(entry.status));
            }
        }

        // ---- 换账新侧续办:旧账已封(或本就封好),照 next_session_id 办 ----
        if (facts.clear_requested && facts.session_ended &&
            !facts.clear_requested_next_session_id.empty()) {
            const std::string next_id = facts.clear_requested_next_session_id;
            const auto next_it = scanned.find(next_id);
            if (next_it != scanned.end()) {
                const MainJournalFacts& next_facts = next_it->second.facts;
                if (next_facts.truncated_tail ||
                    (next_facts.journal_exists && !next_facts.verify_ok)) {
                    SessionRecoveryEntry next_entry;
                    next_entry.session_id = next_id;
                    next_entry.status = DeriveSessionStatusFromFacts(next_facts);
                    next_entry.notes.push_back("新账验不过,只读隔离");
                    // session.json 按事实补正(截断→incomplete,坏行→corrupt)。
                    auto next_manifest = ReadSessionJson(next_it->second.dir);
                    if (next_manifest.has_value() &&
                        next_manifest->status != SessionStatusName(next_entry.status)) {
                        if (TransitionSessionStatus(next_it->second.dir, &*next_manifest,
                                                    next_entry.status,
                                                    /*recovery_collapse=*/true)
                                .has_value()) {
                            next_entry.session_json_corrected = true;
                        }
                    }
                    report.sessions.push_back(std::move(next_entry));
                } else if (!next_facts.has_run_started &&
                           policy != ClearRecoveryPolicy::CompleteSwitch) {
                    // 空 preparing 的新账按策略清账(§3.3.1"可标 aborted_before_start")。
                    report.sessions.push_back(AbortEmptyPreparing(next_it->second.dir, next_id));
                } else {
                    ContinueNewSide(next_it->second.dir, next_id, next_facts, facts, &entry,
                                    &report);
                }
                handled.insert(next_id);
            }
        }

        // ---- 空 preparing 孤儿:无换账关系、无 run.started、无活锁 ----
        if (entry.status == SessionStatus::Preparing && !facts.clear_requested &&
            !facts.has_run_started && !facts.journal_exists &&
            next_targets.count(session_id) == 0) {
            const SessionRecoveryEntry aborted = AbortEmptyPreparing(session_dir, session_id);
            entry.aborted_before_start = aborted.aborted_before_start;
            for (const std::string& note : aborted.notes) {
                entry.notes.push_back(note);
            }
        }
        report.sessions.push_back(std::move(entry));
    };

    // 依名序跑通用路径;换账新侧若先到,推迟到旧侧那轮(依赖次序)。
    std::vector<std::string> deferred;
    for (const auto& [id, scan] : scanned) {
        (void)scan;
        const auto target = next_targets.find(id);
        if (target != next_targets.end() && handled.count(id) == 0 &&
            scanned.contains(target->second) && id < target->second) {
            // 新账排序在旧账之前:等旧账那轮顺手办它。
            deferred.push_back(id);
            continue;
        }
        if (handled.insert(id).second) {
            process(id);
        }
    }
    for (const std::string& id : deferred) {
        if (handled.insert(id).second) {
            // 旧账那轮没办到它(旧账被活锁挡住/不存在):按通用路径出账,
            // 不确定的账不删不动。
            process(id);
        }
    }
    return report;
}

// ---------------------------------------------------------------------------
// workspace 管理操作(§3.2/§14.2)
// ---------------------------------------------------------------------------

std::expected<void, std::string> SessionManager::ArchiveSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    // §12.1:归档也是目录管理操作,先过单段名校验。
    if (!IsSafeSingleSegment(session_id)) {
        return std::unexpected("session.invalid_ref: session id 须是单段名(不带路径)");
    }
    if (active_.has_value() && active_->session_id() == session_id &&
        active_->status == SessionStatus::Running) {
        return std::unexpected("session.archive_active: active session 先 close 再归档");
    }
    // P0-2:搬删路径收进自由函数(命令面/成员版同一条路)。
    const SessionAdminOutcome outcome =
        ArchiveSessionDir(workspace_dir_, session_id, clock_->WallMs());
    if (!outcome.ok()) {
        return std::unexpected(outcome.error_code + ": " + outcome.message);
    }
    return {};
}

std::expected<void, std::string> SessionManager::UnarchiveSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsSafeSingleSegment(session_id)) {
        return std::unexpected("session.invalid_ref: session id 须是单段名(不带路径)");
    }
    const SessionAdminOutcome outcome =
        UnarchiveSessionDir(workspace_dir_, session_id, clock_->WallMs());
    if (!outcome.ok()) {
        return std::unexpected(outcome.error_code + ": " + outcome.message);
    }
    return {};
}

std::expected<void, std::string> SessionManager::DeleteSession(const std::string& session_id,
                                                               const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    // §12.1:删除是毁档操作,用户递的名字先过单段名校验再拼路径。
    if (!IsSafeSingleSegment(session_id)) {
        return std::unexpected("session.invalid_ref: session id 须是单段名(不带路径)");
    }
    if (active_.has_value() && active_->session_id() == session_id &&
        active_->status == SessionStatus::Running) {
        return std::unexpected("session.delete_active: active session 不得删");
    }
    const SessionAdminOutcome outcome =
        DeleteSessionDir(workspace_dir_, session_id, reason, clock_->WallMs());
    if (!outcome.ok()) {
        return std::unexpected(outcome.error_code + ": " + outcome.message);
    }
    return {};
}

std::expected<void, std::string> SessionManager::RecordResumeReference(
    const std::string& source_session_id, const std::string& note) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::filesystem::path source_dir = SessionDirOf(source_session_id);
    if (!std::filesystem::exists(source_dir)) {
        return std::unexpected("session.not_found: " + source_session_id);
    }
    // resume source 通常须 closed/archived;无活 writer 的 incomplete 也可按
    // 已验证前缀 resume-as-new(§3.3.2)。这里只记引用,不校验投影。
    nlohmann::json parameters;
    parameters["source_session_id"] = source_session_id;
    if (!note.empty()) {
        parameters["note"] = note;
    }
    if (const auto op = RunLifecycleOp(LifecycleOperation::ResumeReference, source_session_id,
                                       parameters, nlohmann::json{{"referenced", true}});
        !op.has_value()) {
        return std::unexpected(op.error());
    }
    return {};
}


// ---------------------------------------------------------------------------
// P0-2:管理操作自由函数(命令面/session 服务用;与成员版同一条路)
// ---------------------------------------------------------------------------

namespace {

// lifecycle intent + result 一笔(自由函数版:operation_id 由操作名 +
// session_id + 时刻拼单段名,同毫秒同场次同操作的重复请求会被
// lifecycle intent 的 create-new 占位拒——正好是抢占语义)。
SessionAdminOutcome RunDirLifecycleOp(const std::filesystem::path& workspace_dir,
                                      LifecycleOperation operation, const std::string& session_id,
                                      const nlohmann::json& parameters, const nlohmann::json& outcome_json,
                                      std::int64_t now_ms, std::string* operation_id_out) {
    const WorkspaceLifecycle lifecycle(workspace_dir);
    LifecycleIntent intent;
    intent.operation_id = std::string(LifecycleOperationName(operation)) + "-" + session_id + "-" +
                          std::to_string(now_ms);
    intent.operation = LifecycleOperationName(operation);
    intent.workspace_key = platform::PathToUtf8(workspace_dir.filename());
    intent.session_id = session_id;
    intent.requested_at_ms = now_ms;
    intent.parameters = parameters;
    const auto intent_dir = lifecycle.WriteIntent(intent);
    if (!intent_dir.has_value()) {
        return SessionAdminOutcome{"lifecycle.intent_failed", intent_dir.error()};
    }
    LifecycleResult result;
    result.operation_id = intent.operation_id;
    result.status = "completed";
    result.completed_at_ms = now_ms;
    result.outcome = outcome_json;
    if (const auto written = lifecycle.WriteResult(result); !written.has_value()) {
        return SessionAdminOutcome{"lifecycle.result_failed", written.error()};
    }
    if (operation_id_out != nullptr) {
        *operation_id_out = intent.operation_id;
    }
    return SessionAdminOutcome{};
}

}  // namespace

SessionAdminOutcome ArchiveSessionDir(const std::filesystem::path& workspace_dir,
                                      const std::string& session_id, std::int64_t now_ms) {
    if (!IsSafeSingleSegment(session_id)) {
        return SessionAdminOutcome{"session.invalid_ref", "session id 须是单段名(不带路径)"};
    }
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id);
    auto manifest = ReadSessionJson(session_dir);
    if (!manifest.has_value()) {
        return SessionAdminOutcome{"session.not_found", session_id};
    }
    // session.json 落后于 Journal 可证事实(崩溃残留 running/preparing)时,
    // 先按事实推导收口(recovery_collapse 允许折掉中间态),再走 closed ->
    // archived 的正门;推导不出 closed 的(incomplete/corrupt)不归档——
    // 半场账先 resume/verify 收口,不拿归档遮坏账。
    if (manifest->status != SessionStatusName(SessionStatus::Closed)) {
        const auto holder = SessionLock::Inspect(session_dir);
        if (holder.has_value() && ProbeLockHolder(*holder) == LockHolderState::Alive) {
            return SessionAdminOutcome{"session.locked", "活进程正持有此 session"};
        }
        const MainJournalFacts facts = ScanStreamFacts(session_dir / "main.jsonl");
        const SessionStatus derived = DeriveSessionStatusFromFacts(facts);
        if (const auto transition = TransitionSessionStatus(session_dir, &*manifest, derived,
                                                            /*recovery_collapse=*/true);
            !transition.has_value()) {
            return SessionAdminOutcome{"session.archive_rejected",
                                       "状态按 Journal 事实收口失败: " + transition.error()};
        }
        if (derived != SessionStatus::Closed) {
            return SessionAdminOutcome{"session.archive_rejected",
                                       "状态是 " + std::string(SessionStatusName(derived)) +
                                           ",不是 closed;先 resume/verify 收口再归档"};
        }
    }
    // archived 是 closed 的目录管理标记;正文与 hash 不变(§3.3.2)。
    if (const auto transition =
            TransitionSessionStatus(session_dir, &*manifest, SessionStatus::Archived);
        !transition.has_value()) {
        return SessionAdminOutcome{"session.archive_rejected", transition.error()};
    }
    return RunDirLifecycleOp(workspace_dir, LifecycleOperation::ArchiveSession, session_id,
                             nlohmann::json{{"from", "closed"}}, nlohmann::json{{"status", "archived"}},
                             now_ms, nullptr);
}

SessionAdminOutcome UnarchiveSessionDir(const std::filesystem::path& workspace_dir,
                                        const std::string& session_id, std::int64_t now_ms) {
    if (!IsSafeSingleSegment(session_id)) {
        return SessionAdminOutcome{"session.invalid_ref", "session id 须是单段名(不带路径)"};
    }
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id);
    auto manifest = ReadSessionJson(session_dir);
    if (!manifest.has_value()) {
        return SessionAdminOutcome{"session.not_found", session_id};
    }
    if (manifest->status != SessionStatusName(SessionStatus::Archived)) {
        return SessionAdminOutcome{"session.unarchive_rejected",
                                   "状态是 " + manifest->status + ",不是 archived"};
    }
    if (const auto transition =
            TransitionSessionStatus(session_dir, &*manifest, SessionStatus::Closed);
        !transition.has_value()) {
        return SessionAdminOutcome{"session.unarchive_rejected", transition.error()};
    }
    return RunDirLifecycleOp(workspace_dir, LifecycleOperation::UnarchiveSession, session_id,
                             nlohmann::json{{"from", "archived"}}, nlohmann::json{{"status", "closed"}},
                             now_ms, nullptr);
}

SessionAdminOutcome DeleteSessionDir(const std::filesystem::path& workspace_dir,
                                     const std::string& session_id, const std::string& reason,
                                     std::int64_t now_ms) {
    if (!IsSafeSingleSegment(session_id)) {
        return SessionAdminOutcome{"session.invalid_ref", "session id 须是单段名(不带路径)"};
    }
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id);
    if (!std::filesystem::exists(session_dir)) {
        return SessionAdminOutcome{"session.not_found", session_id};
    }
    const auto holder = SessionLock::Inspect(session_dir);
    if (holder.has_value() && ProbeLockHolder(*holder) == LockHolderState::Alive) {
        return SessionAdminOutcome{"session.delete_locked", "活进程正持有此 session"};
    }
    const MainJournalFacts facts = ScanStreamFacts(session_dir / "main.jsonl");
    if (facts.journal_exists && !facts.run_terminal) {
        // 未封口的账不许删:删了就丢了"跑到一半"的事实(§14.5 先封再删)。
        return SessionAdminOutcome{"session.delete_unsealed", "run 没 terminal,先 close/verify"};
    }
    // durable intent 先行,再留 tombstone,末后删目录(§3.2)。
    std::string operation_id;
    SessionAdminOutcome outcome =
        RunDirLifecycleOp(workspace_dir, LifecycleOperation::DeleteSession, session_id,
                          nlohmann::json{{"reason", reason}}, nlohmann::json{{"tombstone", true}},
                          now_ms, &operation_id);
    if (!outcome.ok()) {
        return outcome;
    }
    SessionTombstone tombstone;
    tombstone.session_id = session_id;
    tombstone.deleted_at_ms = now_ms;
    tombstone.reason = reason;
    tombstone.last_event_hash =
        facts.last_event_hash.empty() ? std::nullopt : std::optional(facts.last_event_hash);
    tombstone.operation_id = operation_id;
    if (const auto written = WriteSessionTombstone(workspace_dir / "tombstones", tombstone);
        !written.has_value()) {
        return SessionAdminOutcome{"session.delete_tombstone_failed", written.error()};
    }
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);
    if (ec) {
        return SessionAdminOutcome{"session.delete_remove_failed",
                                   platform::PathToUtf8(session_dir) + ": " + ec.message()};
    }
    return SessionAdminOutcome{};
}

}  // namespace lubancode::trajectory
