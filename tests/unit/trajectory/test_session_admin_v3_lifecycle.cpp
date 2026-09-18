// T15-B / V3-GAP-09(Session v3 旧设计清理单 §八"完整生命周期"):v3 场
// archive/unarchive/delete 的生命周期合同。钉六件事:
//   - 执行状态从 v3 首行/终态推导,不造 session.json(勾一);
//   - 归档是 lifecycle 账上的目录管理状态,与执行 closed 分开;<id>.jsonl
//     在 archive/unarchive 前后逐字节不变(勾二);
//   - archive/unarchive 幂等;活锁/未封口/坏账/两账并存拒绝;索引重建后
//     仍读回同样归档状态(勾三);
//   - 删除前核验 incoming refs(resume 链/memory 溯源),被引用的源拒删,
//     目录原样(勾四);
//   - 删除四段 intent→tombstone→remove→result,崩溃后 RecoverPendingDeletes
//     各段可恢复;tombstone 存实际首/末 hash/行数;失败不先报成功(勾五);
//   - 删除限定规范化路径,软链接拒绝;测试全用独立临时根(勾六)。
// 删除准入的未封口/坏账/活锁/歧义四拒由 test_session_admin_v3_delete 册守,
// 本册不重钉。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"
#include "api/types.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/v3/envelope.hpp"  // EventKindV3/EventDraft(引用场手植)
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"
#include "workspace/identity.hpp"

namespace platform = lubancode::platform;
using namespace lubancode;
using namespace lubancode::trajectory;
using lubancode::runtime::TrajectorySessionLedger;

namespace {

struct EnvGuard {
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
#ifdef _WIN32
        _putenv((std::string(name_) + "=" + value).c_str());
#else
        setenv(name_, value, 1);
#endif
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv((std::string(name_) + "=").c_str());
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
};

std::filesystem::path FreshRoot(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lubancode-v3-lifecycle-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

TrajectorySessionLedger::Options LedgerOptions(const std::filesystem::path& root,
                                               const std::string& resume_source = {}) {
    TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.260-test";
    options.v3_system_content = "你是 LubanCode,读写跑都走工具。";
    if (!resume_source.empty()) {
        options.resume_at_launch = true;
        options.resume_source_session_id = resume_source;
    }
    return options;
}

api::Message UserMessage(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Message AssistantText(const std::string& text) {
    api::Message message;
    message.role = api::Role::Assistant;
    message.content.push_back(api::TextBlock{text});
    return message;
}

api::Request MakeRequest(const std::string& system, const std::vector<api::Message>& messages) {
    api::Request request;
    request.model = "kimi-k2.6";
    request.system = system;
    request.messages = messages;
    return request;
}

agent::RequestPreparedContext PreparedContext() { return agent::RequestPreparedContext{}; }

// 完整一轮(user -> prepared -> sent -> assistant -> turn 收口)。
void DriveTurn(TrajectorySessionLedger& ledger, const std::string& text) {
    auto bridge = ledger.NewTurnBridge({"moonshot", "openai-chat-completions", "terminal"});
    REQUIRE(bridge != nullptr);
    bridge->BeginTurn("turn-1", "external_user");
    bridge->RecordInput(UserMessage(text));
    const std::string request_id = bridge->OnRequestPrepared(
        MakeRequest("你是 LubanCode,读写跑都走工具。", {UserMessage(text)}), PreparedContext());
    REQUIRE_FALSE(request_id.empty());
    bridge->OnRequestSent(request_id);
    REQUIRE(bridge->OnOutputCompleted(request_id, AssistantText("收到。"), "end_turn", "resp-1"));
    bridge->EndTurn(/*ok=*/true, /*cancelled=*/false, "");
}

std::filesystem::path V3StreamOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir() /
           platform::Utf8ToPath(platform::PathToUtf8(ledger.session_dir().filename()) + ".jsonl");
}

std::filesystem::path WorkspaceDirOf(const TrajectorySessionLedger& ledger) {
    return ledger.session_dir().parent_path().parent_path();
}

// 开场→一轮→封场。回 session_id(workspace 房已立)。resume_source 在
// 2026-09-19 落点拍板后续接源场(同 id 续写),不再是"造一枚引用源场
// 的后代场"——要 fork 形状的后代用 PlantReferringFork。
std::string RunSealedRound(const std::filesystem::path& root, const std::string& text,
                           const std::string& resume_source = {}) {
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root, resume_source));
    REQUIRE(ledger.has_value());
    const std::string session_id = ledger->session_id();
    DriveTurn(*ledger, text);
    REQUIRE(ledger->CloseSession("exit").error_code.empty());
    return session_id;
}

// 手工铸一枚带 resume.source.attached(五键指源末行)的引用场:删除守卫
// 的 incoming refs 扫描只认这一枚事实,不追会话内容。
std::string PlantReferringFork(const std::filesystem::path& root, const std::string& fork_id,
                               const std::string& source_id) {
    const std::filesystem::path source_dir = [&] {
        std::error_code ec;
        for (const auto& room : std::filesystem::directory_iterator(root / "workspaces", ec)) {
            const auto dir = room.path() / "sessions" / platform::Utf8ToPath(source_id);
            if (std::filesystem::exists(dir, ec)) {
                return dir;
            }
        }
        return std::filesystem::path();
    }();
    REQUIRE_FALSE(source_dir.empty());
    const std::filesystem::path source_stream =
        source_dir / platform::Utf8ToPath(source_id + ".jsonl");
    std::ifstream in(source_stream, std::ios::binary);
    REQUIRE(in.is_open());
    std::string line;
    nlohmann::json last;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        last = nlohmann::json::parse(line, nullptr, false);
    }
    REQUIRE_FALSE(last.is_discarded());
    const std::filesystem::path fork_dir =
        source_dir.parent_path() / platform::Utf8ToPath(fork_id);
    std::error_code ec;
    std::filesystem::create_directories(fork_dir, ec);
    REQUIRE_FALSE(ec);
    auto writer = v3::V3Writer::Start(
        fork_dir / platform::Utf8ToPath(fork_id + ".jsonl"), fork_id, "run-000009",
        "你是 LubanCode,读写跑都走工具。");
    REQUIRE(writer.has_value());
    v3::EventDraft attached;
    attached.kind = v3::EventKindV3::ResumeSourceAttached;
    attached.payload = nlohmann::json{{"sourceRef",
                                       nlohmann::json::object(
                                           {{"sessionId", source_id},
                                            {"runId", last.value("runId", std::string())},
                                            {"seq", last.value("seq", std::uint64_t(0))},
                                            {"id", last.value("eventId",
                                                              last.value("messageId",
                                                                         std::string()))},
                                            {"hash", last.value("lineHash", std::string())}})}};
    REQUIRE(writer->AppendEvent(std::move(attached), trajectory::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
    return fork_id;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::map<std::string, std::string> SnapshotDir(const std::filesystem::path& dir) {
    std::map<std::string, std::string> snap;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::error_code ec;
        const auto rel = std::filesystem::relative(entry.path(), dir, ec);
        snap[platform::PathToUtf8(rel)] = ReadFileBytes(entry.path());
    }
    return snap;
}

// lifecycle 账里某 session 指定操作族(archive/unarchive/delete)的笔数。
std::size_t CountLifecycleOps(const std::filesystem::path& workspace_dir,
                              const std::string& session_id, const std::string& operation) {
    std::size_t count = 0;
    std::error_code ec;
    const std::filesystem::path lifecycle_dir = workspace_dir / "lifecycle";
    if (!std::filesystem::exists(lifecycle_dir, ec)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::directory_iterator(lifecycle_dir, ec)) {
        const auto intent = WorkspaceLifecycle::ReadIntent(entry.path());
        if (intent.has_value() && intent->session_id == session_id &&
            intent->operation == operation) {
            ++count;
        }
    }
    return count;
}

// 索引查询的小包装(默认列表 / 含归档 / 只归档)。
std::vector<std::string> ListedSessions(const std::filesystem::path& workspaces_root,
                                        const std::string& workspace_key, bool archived_only,
                                        bool include_archived) {
    SessionIndexQuery query;
    query.current_workspace_key = workspace_key;
    query.archived_only = archived_only;
    query.include_archived = include_archived;
    query.limit = 0;
    const SessionIndexPage page = QueryWorkspaceSessions(workspaces_root, query);
    std::vector<std::string> ids;
    for (const auto& entry : page.entries) {
        ids.push_back(entry.session_id);
    }
    return ids;
}

bool Contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// v3 账首/末行的 lineHash(对 tombstone 断言用;与实现同一读取面)。
std::pair<std::string, std::string> FirstLastHash(const std::filesystem::path& stream) {
    const auto ledger = v3::ReadV3Ledger(stream);
    REQUIRE(ledger.has_value());
    REQUIRE_FALSE(ledger->timeline.empty());
    const auto& first = ledger->timeline.front();
    const std::string first_hash = first.is_message
                                       ? ledger->messages[first.index].line_hash
                                       : ledger->events[first.index].line_hash;
    std::string last_hash;
    if (const auto last = ledger->LastEntry(); last.has_value()) {
        last_hash = last->is_message ? ledger->messages[last->index].line_hash
                                     : ledger->events[last->index].line_hash;
    }
    return {first_hash, last_hash};
}

// 手写一笔挂起的 delete intent(模拟"intent 后崩溃")。
void WriteDanglingDeleteIntent(const std::filesystem::path& workspace_dir,
                               const std::string& session_id, const std::string& suffix) {
    const std::string operation_id = "delete_session-" + session_id + "-1759468800" + suffix;
    const std::filesystem::path op_dir =
        workspace_dir / "lifecycle" / platform::Utf8ToPath(operation_id);
    std::error_code ec;
    std::filesystem::create_directories(op_dir, ec);
    const nlohmann::json intent{{"schema_version", 1},
                                {"operation_id", operation_id},
                                {"operation", "delete_session"},
                                {"workspace_key", "lifecycle-test"},
                                {"session_id", session_id},
                                {"requested_at_ms", 1759468800000LL},
                                {"parameters", nlohmann::json::object()}};
    std::ofstream file(op_dir / "intent.json", std::ios::binary);
    REQUIRE(file.is_open());
    file << intent.dump() << "\n";
}

std::optional<nlohmann::json> FindLifecycleResult(const std::filesystem::path& workspace_dir,
                                                  const std::string& operation_id) {
    const auto result = WorkspaceLifecycle::ReadResult(
        workspace_dir / "lifecycle" / platform::Utf8ToPath(operation_id));
    if (!result.has_value()) {
        return std::nullopt;
    }
    return result->ToJson();
}

constexpr std::int64_t kNowMs = 1759468800000LL;

}  // namespace

// ---------------------------------------------------------------------------
// 勾二/勾三:归档正常路 + 幂等 + 正文不变 + 索引投影
// ---------------------------------------------------------------------------

TEST_CASE("v3 归档: 封口场可归档,账逐字节不变,索引只归档口可见;unarchive 复原") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("archive-ok");
    std::string session_id;
    std::filesystem::path workspace_dir;
    std::filesystem::path workspaces_root;
    std::string workspace_key;
    std::string stream_text;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        session_id = ledger->session_id();
        workspace_dir = WorkspaceDirOf(*ledger);
        workspaces_root = root / "workspaces";
        workspace_key = ledger->workspace_key();
        DriveTurn(*ledger, "要归档的一轮");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }
    const std::filesystem::path stream =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id) /
        platform::Utf8ToPath(session_id + ".jsonl");
    stream_text = ReadFileBytes(stream);
    const auto session_snapshot = SnapshotDir(stream.parent_path());

    REQUIRE(trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs).ok());
    // 勾二:已封口 JSONL 不为切 archived 改原行——目录逐字节不变。
    CHECK(SnapshotDir(stream.parent_path()) == session_snapshot);
    CHECK(ReadFileBytes(stream) == stream_text);

    // 管理状态:lifecycle 账折出 archived,与执行状态分开。
    const auto states = trajectory::ScanSessionArchiveState(workspace_dir);
    const auto state = states.find(session_id);
    REQUIRE(state != states.end());
    CHECK(state->second.archived);
    CHECK(CountLifecycleOps(workspace_dir, session_id, "archive_session") == 1);

    // 查询投影合并:默认列表不含,archived_only 含;状态列仍是执行态。
    {
        const auto plain = ListedSessions(workspaces_root, workspace_key, false, false);
        CHECK_FALSE(Contains(plain, session_id));
        const auto archived = ListedSessions(workspaces_root, workspace_key, true, false);
        REQUIRE(archived.size() == 1);
        CHECK(archived[0] == session_id);
    }

    // 幂等(勾三):重复 archive 成功且不新落笔。
    REQUIRE(trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs + 1).ok());
    CHECK(CountLifecycleOps(workspace_dir, session_id, "archive_session") == 1);
    CHECK(SnapshotDir(stream.parent_path()) == session_snapshot);

    // unarchive:默认列表又见;幂等同款。
    REQUIRE(trajectory::UnarchiveSessionDir(workspace_dir, session_id, kNowMs + 2).ok());
    CHECK(ReadFileBytes(stream) == stream_text);
    {
        const auto plain = ListedSessions(workspaces_root, workspace_key, false, false);
        CHECK(Contains(plain, session_id));
        const auto archived = ListedSessions(workspaces_root, workspace_key, true, false);
        CHECK(archived.empty());
    }
    REQUIRE(trajectory::UnarchiveSessionDir(workspace_dir, session_id, kNowMs + 3).ok());
    CHECK(CountLifecycleOps(workspace_dir, session_id, "unarchive_session") == 1);

    // 操作后重建索引(勾三):整份丢弃缓存,仍读回同样的未归档状态;再归档
    // 再重建,读回归档。
    std::error_code ec;
    std::filesystem::remove(workspace_dir / "indexes" / "sessions.json", ec);
    {
        const auto plain = ListedSessions(workspaces_root, workspace_key, false, false);
        CHECK(Contains(plain, session_id));
    }
    REQUIRE(trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs + 4).ok());
    std::filesystem::remove(workspace_dir / "indexes" / "sessions.json", ec);
    {
        const auto archived = ListedSessions(workspaces_root, workspace_key, true, false);
        REQUIRE(archived.size() == 1);
        CHECK(archived[0] == session_id);
        const auto plain = ListedSessions(workspaces_root, workspace_key, false, false);
        CHECK_FALSE(Contains(plain, session_id));
    }
}

TEST_CASE("v3 归档拒绝面: 活锁/未封口崩溃残留/坏账/两账并存") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    SUBCASE("活锁在场拒绝,目录字节原样") {
        const auto root = FreshRoot("archive-locked");
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        DriveTurn(*ledger, "活锁场的一轮");
        const auto before = SnapshotDir(V3StreamOf(*ledger).parent_path());
        const auto outcome =
            trajectory::ArchiveSessionDir(WorkspaceDirOf(*ledger), ledger->session_id(), kNowMs);
        CHECK(outcome.error_code == "session.locked");
        CHECK(SnapshotDir(V3StreamOf(*ledger).parent_path()) == before);
    }
    SUBCASE("未封口且无活锁(崩溃残留)拒绝:执行状态不是 closed") {
        const auto root = FreshRoot("archive-unsealed");
        std::filesystem::path workspace_dir;
        std::filesystem::path session_dir;
        {
            auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
            REQUIRE(ledger.has_value());
            DriveTurn(*ledger, "没收尾的一轮");
            workspace_dir = WorkspaceDirOf(*ledger);
            session_dir = ledger->session_dir();
        }  // ledger 析构:锁释放(RAII),账未封口——崩溃残留形态
        const auto before = SnapshotDir(session_dir);
        const std::string session_id = platform::PathToUtf8(session_dir.filename());
        const auto outcome = trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs);
        CHECK(outcome.error_code == "session.archive_rejected");
        CHECK(SnapshotDir(session_dir) == before);
        CHECK(CountLifecycleOps(workspace_dir, session_id, "archive_session") == 0);
    }
    SUBCASE("坏账(封口后追加垃圾)拒绝") {
        const auto root = FreshRoot("archive-corrupt");
        std::filesystem::path workspace_dir;
        std::filesystem::path stream;
        {
            auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
            REQUIRE(ledger.has_value());
            DriveTurn(*ledger, "封口后坏尾的一轮");
            workspace_dir = WorkspaceDirOf(*ledger);
            stream = V3StreamOf(*ledger);
            REQUIRE(ledger->CloseSession("exit").error_code.empty());
        }
        {
            std::ofstream file(stream, std::ios::binary | std::ios::app);
            REQUIRE(file.is_open());
            file << "{\"seq\":99,\"kind\":\"session.started\"";
        }
        const std::string session_id = platform::PathToUtf8(stream.parent_path().filename());
        const auto outcome = trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs);
        CHECK(outcome.error_code == "session.archive_v3_unreadable");
    }
    SUBCASE("两账并存拒绝") {
        const auto root = FreshRoot("archive-ambiguous");
        std::filesystem::path workspace_dir;
        std::filesystem::path stream;
        std::string session_id;
        {
            auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
            REQUIRE(ledger.has_value());
            DriveTurn(*ledger, "歧义场的一轮");
            workspace_dir = WorkspaceDirOf(*ledger);
            stream = V3StreamOf(*ledger);
            session_id = ledger->session_id();
            REQUIRE(ledger->CloseSession("exit").error_code.empty());
        }
        {
            std::ofstream file(stream.parent_path() / "main.jsonl", std::ios::binary);
            REQUIRE(file.is_open());
            file << "{\"event\":\"run.started\"}\n";
        }
        const auto outcome = trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs);
        CHECK(outcome.error_code == "session.archive_format_ambiguous");
    }
}

// ---------------------------------------------------------------------------
// 勾四:删除的 incoming refs 栘验
// ---------------------------------------------------------------------------

TEST_CASE("v3 删除: 被别场 resume 引用的源拒删,目录原样") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("delete-resume-ref");
    const std::string source_id = RunSealedRound(root, "被引用的源场");
    // 一枚带 resume.source.attached 五键指源的引用场(2026-09-19 落点
    // 拍板后 resume 不再造 fork,存量/手植形状照旧受删除守卫保护)。
    const std::string referrer_id = PlantReferringFork(root, "20260919-000001-FORKR", source_id);
    REQUIRE_FALSE(referrer_id.empty());
    REQUIRE(referrer_id != source_id);

    auto ref_ledger_probe = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ref_ledger_probe.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ref_ledger_probe);
    const std::filesystem::path source_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(source_id);
    const auto before = SnapshotDir(source_dir);

    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, source_id, "user_delete", kNowMs);
    CHECK(outcome.error_code == "session.delete_referenced");
    CHECK(outcome.message.find(referrer_id) != std::string::npos);
    CHECK(SnapshotDir(source_dir) == before);  // 不放行就不动目录
    // 引用方自己的删除不受影响(它没被别人引用)。
    const auto refs = trajectory::ScanIncomingSessionRefs(workspace_dir, source_id);
    REQUIRE(refs.resume_referrers.size() == 1);
    CHECK(refs.resume_referrers[0] == referrer_id);
}

TEST_CASE("v3 删除: workspace 记忆溯源引用同样拒删") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("delete-memory-ref");
    const std::string session_id = RunSealedRound(root, "被记忆溯源的场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    // 造一条项目记忆:frontmatter 的 source_sessions 指向该场。
    std::error_code ec;
    std::filesystem::create_directories(workspace_dir / "memory" / "fact", ec);
    {
        std::ofstream file(workspace_dir / "memory" / "fact" / "saw-the-session.md",
                           std::ios::binary);
        REQUIRE(file.is_open());
        file << "---\nsource_sessions: [" << session_id << "]\n---\n# 痕迹\n";
    }
    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", kNowMs);
    CHECK(outcome.error_code == "session.delete_referenced");
    CHECK(outcome.message.find("memory") != std::string::npos);
}

TEST_CASE("v3 删除: 跨目录子账引用(childSessionRef)拒删——行级宽松提取") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("delete-subagent-ref");
    const std::string target_id = RunSealedRound(root, "被跨场 spawn 指到的场");
    const std::string referrer_id = RunSealedRound(root, "手工造引用的场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const std::filesystem::path referrer_stream = workspace_dir / "sessions" /
                                                   platform::Utf8ToPath(referrer_id) /
                                                   platform::Utf8ToPath(referrer_id + ".jsonl");
    // 手工追加 subagent.linked(childSessionRef 指目标)——追加行会断链,但
    // 引用提取是行级宽松解析(不整卷验链),坏尾不挡前段引用。
    {
        nlohmann::json child_ref = nlohmann::json::object();
        child_ref["sessionId"] = target_id;
        child_ref["runId"] = "run-x";
        child_ref["journalPath"] = "sessions/" + target_id + "/subagents/x";
        nlohmann::json payload = nlohmann::json::object();
        payload["childSessionRef"] = child_ref;
        nlohmann::json row = nlohmann::json::object();
        row["seq"] = 999;
        row["kind"] = "subagent.linked";
        row["payload"] = payload;
        std::ofstream file(referrer_stream, std::ios::binary | std::ios::app);
        REQUIRE(file.is_open());
        file << row.dump() << "\n";
    }
    const auto outcome =
        trajectory::DeleteSessionDir(workspace_dir, target_id, "user_delete", kNowMs);
    CHECK(outcome.error_code == "session.delete_referenced");
}

// ---------------------------------------------------------------------------
// 勾五:删除四段 + tombstone 范围 + 崩溃恢复
// ---------------------------------------------------------------------------

TEST_CASE("v3 删除: tombstone 存实际首/末 hash 与行数,收据 completed") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("delete-tombstone");
    const std::string session_id = RunSealedRound(root, "要删的一场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const std::filesystem::path stream = workspace_dir / "sessions" /
                                         platform::Utf8ToPath(session_id) /
                                         platform::Utf8ToPath(session_id + ".jsonl");
    const auto [first_hash, last_hash] = FirstLastHash(stream);
    const auto ledger_read = v3::ReadV3Ledger(stream);
    REQUIRE(ledger_read.has_value());
    const std::uint64_t lines = ledger_read->lines;

    REQUIRE(trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", kNowMs).ok());
    CHECK_FALSE(std::filesystem::exists(stream.parent_path()));
    const auto tombstone = trajectory::ReadSessionTombstone(workspace_dir / "tombstones", session_id);
    REQUIRE(tombstone.has_value());
    REQUIRE(tombstone->first_event_hash.has_value());
    REQUIRE(tombstone->last_event_hash.has_value());
    CHECK(*tombstone->first_event_hash == first_hash);
    CHECK(*tombstone->last_event_hash == last_hash);
    CHECK(tombstone->event_count == lines);
    CHECK(tombstone->reason == "user_delete");
    // 收据在(intent + result 两件齐,result=completed)。
    const auto result = FindLifecycleResult(workspace_dir, tombstone->operation_id);
    REQUIRE(result.has_value());
    CHECK((*result)["status"].get<std::string>() == "completed");
}

TEST_CASE("删除恢复: intent 后崩溃——tombstone 未落不代删,目录原样,可重删") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("recover-intent");
    const std::string session_id = RunSealedRound(root, "intent 断点场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id);
    WriteDanglingDeleteIntent(workspace_dir, session_id, "11");
    const auto before = SnapshotDir(session_dir);

    const auto recovered = trajectory::RecoverPendingDeletes(workspace_dir, kNowMs);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].action == "failed_interrupted");
    CHECK(std::filesystem::exists(session_dir));  // 未承诺即不代删
    CHECK(SnapshotDir(session_dir) == before);

    // 幂等:再跑一遍没有新动作。
    CHECK(trajectory::RecoverPendingDeletes(workspace_dir, kNowMs + 1).empty());

    // 用户重新请求删除:走新笔,正常成功。
    REQUIRE(trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", kNowMs + 2).ok());
    CHECK_FALSE(std::filesystem::exists(session_dir));
    CHECK(trajectory::ReadSessionTombstone(workspace_dir / "tombstones", session_id).has_value());
}

TEST_CASE("删除恢复: tombstone 后崩溃——续办物理删除 + 补收据") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("recover-tombstone");
    const std::string session_id = RunSealedRound(root, "tombstone 断点场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const std::filesystem::path session_dir =
        workspace_dir / "sessions" / platform::Utf8ToPath(session_id);
    // 手造 intent + tombstone(目录还在)的中间态。
    WriteDanglingDeleteIntent(workspace_dir, session_id, "22");
    SessionTombstone tombstone;
    tombstone.session_id = session_id;
    tombstone.deleted_at_ms = kNowMs;
    tombstone.reason = "user_delete";
    tombstone.operation_id = "delete_session-" + session_id + "-175946880022";
    REQUIRE(trajectory::WriteSessionTombstone(workspace_dir / "tombstones", tombstone).has_value());

    const auto recovered = trajectory::RecoverPendingDeletes(workspace_dir, kNowMs);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].action == "completed_resumed_remove");
    CHECK_FALSE(std::filesystem::exists(session_dir));
    const auto result = FindLifecycleResult(workspace_dir, tombstone.operation_id);
    REQUIRE(result.has_value());
    CHECK((*result)["status"].get<std::string>() == "completed");
    CHECK(trajectory::RecoverPendingDeletes(workspace_dir, kNowMs + 1).empty());
}

TEST_CASE("删除恢复: remove 后崩溃——目录已删,补完成收据") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("recover-remove");
    const std::string session_id = RunSealedRound(root, "remove 断点场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    REQUIRE(trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", kNowMs).ok());
    // 模拟"remove 成功、result 没落":抽掉收据(tombstone 反查 operation_id)。
    const auto tombstone = trajectory::ReadSessionTombstone(workspace_dir / "tombstones", session_id);
    REQUIRE(tombstone.has_value());
    std::error_code ec;
    std::filesystem::remove(workspace_dir / "lifecycle" /
                                platform::Utf8ToPath(tombstone->operation_id) / "result.json",
                            ec);

    const auto recovered = trajectory::RecoverPendingDeletes(workspace_dir, kNowMs + 1);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].action == "completed_receipt_backlog");
    const auto result = FindLifecycleResult(workspace_dir, tombstone->operation_id);
    REQUIRE(result.has_value());
    CHECK((*result)["status"].get<std::string>() == "completed");
}

TEST_CASE("删除恢复: 目录不在且无 tombstone——异常现场留 failed 收据") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("recover-missing");
    const std::string session_id = "20990101-000000-GHOST1";
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    WriteDanglingDeleteIntent(workspace_dir, session_id, "33");

    const auto recovered = trajectory::RecoverPendingDeletes(workspace_dir, kNowMs);
    REQUIRE(recovered.size() == 1);
    CHECK(recovered[0].action == "failed_missing");
    CHECK(trajectory::RecoverPendingDeletes(workspace_dir, kNowMs + 1).empty());
}

// ---------------------------------------------------------------------------
// 验收尾巴:删除后不凭索引残留复活
// ---------------------------------------------------------------------------

TEST_CASE("v3 删除后: 索引残留自愈,列表不再列已删场") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("delete-index-residue");
    const std::string session_id = RunSealedRound(root, "删后防复活场");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const std::filesystem::path workspaces_root = root / "workspaces";
    const std::string workspace_key = ledger->workspace_key();

    // 先让索引见到这场(查询触发写回)。
    REQUIRE(Contains(ListedSessions(workspaces_root, workspace_key, false, false), session_id));
    REQUIRE(trajectory::DeleteSessionDir(workspace_dir, session_id, "user_delete", kNowMs).ok());

    // 索引文件里还留着行(残留),查询按目录事实剔除,不冒充在场。
    const std::filesystem::path index_path = workspace_dir / "indexes" / "sessions.json";
    REQUIRE(std::filesystem::exists(index_path));
    CHECK_FALSE(Contains(ListedSessions(workspaces_root, workspace_key, false, false), session_id));
    // tombstone 可查(收据);probe 已删场报 source_not_found,不重新执行。
    CHECK(trajectory::ReadSessionTombstone(workspace_dir / "tombstones", session_id).has_value());
    SessionManagerOptions manager_options;
    manager_options.workspaces_root = workspaces_root;
    manager_options.workspace_root = root / "ws";
    manager_options.identity = lubancode::workspace::MakeFallbackIdentity(root / "ws");
    manager_options.launch_cwd = "D:/tmp/ws";
    manager_options.lubancode_version = "0.26.260-test";
    SessionManager manager(manager_options);
    const auto probe = manager.ProbeResumeSource(session_id);
    CHECK(probe.error_code == "resume.source_not_found");
}

// ---------------------------------------------------------------------------
// 勾六:路径门(软链接不跟;规范化边界)
// ---------------------------------------------------------------------------

TEST_CASE("管理操作路径门: 软链接 session 目录拒绝,不跟链") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("path-symlink");
    std::filesystem::path workspace_dir;
    std::string session_id;
    {
        auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
        REQUIRE(ledger.has_value());
        workspace_dir = WorkspaceDirOf(*ledger);
        session_id = ledger->session_id();
        DriveTurn(*ledger, "路径门的一轮");
        REQUIRE(ledger->CloseSession("exit").error_code.empty());
    }  // ledger 离场:独占锁随 RAII 释放

    const std::filesystem::path sessions_root = workspace_dir / "sessions";
    const std::filesystem::path real_dir = sessions_root / platform::Utf8ToPath(session_id);
#ifndef _WIN32
    // POSIX:造一只软链接名字的假场,指向界外目录——删除/归档都不许跟链。
    const std::filesystem::path outside = root / "outside-decoy";
    std::error_code ec;
    std::filesystem::create_directories(outside, ec);
    std::filesystem::create_directory_symlink(outside,
                                              sessions_root / platform::Utf8ToPath("20990101-000000-LINK01"),
                                              ec);
    REQUIRE_FALSE(ec);
    const auto delete_outcome = trajectory::DeleteSessionDir(
        workspace_dir, "20990101-000000-LINK01", "user_delete", kNowMs);
    CHECK(delete_outcome.error_code == "session.path_symlink");
    CHECK(std::filesystem::exists(outside));  // 界外目标分毫未动
    const auto archive_outcome =
        trajectory::ArchiveSessionDir(workspace_dir, "20990101-000000-LINK01", kNowMs);
    CHECK(archive_outcome.error_code == "session.path_symlink");
#endif
    // 普通目录不受误伤:真场照常可归档。
    REQUIRE(trajectory::ArchiveSessionDir(workspace_dir, session_id, kNowMs).ok());
    CHECK(std::filesystem::exists(real_dir));
}

TEST_CASE("管理操作路径门: 单段名校验先于一切") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const auto root = FreshRoot("path-segment");
    auto ledger = TrajectorySessionLedger::Open(LedgerOptions(root));
    REQUIRE(ledger.has_value());
    const std::filesystem::path workspace_dir = WorkspaceDirOf(*ledger);
    const auto escape = trajectory::DeleteSessionDir(workspace_dir, "../..", "user_delete", kNowMs);
    CHECK(escape.error_code == "session.invalid_ref");
    const auto slash = trajectory::ArchiveSessionDir(workspace_dir, "a/b", kNowMs);
    CHECK(slash.error_code == "session.invalid_ref");
}
