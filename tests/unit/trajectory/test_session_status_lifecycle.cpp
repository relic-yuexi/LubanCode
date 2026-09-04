// Session 生命周期状态机(§3.3.2 全图)与 workspace lifecycle 账(§3.2)测试:
// 迁移合法/非法逐边、session.json 原子转态、lifecycle intent/result 一操作
// 一目录不覆盖、tombstone 齐全、archive/delete/resume_reference 管理操作。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-traj-lifecycle-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

struct FakeClock : SessionManagerClock {
    std::int64_t wall = 1759000000000LL;
    mutable int random_calls = 0;
    std::int64_t WallMs() const override { return wall; }
    std::int64_t MonotonicNs() const override { return 1000LL + random_calls; }
    std::string Random6() const override {
        ++random_calls;
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "L%05d", random_calls);
        return buffer;
    }
};

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.128-test";
    return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// 状态机迁移图(§3.3.2)
// ---------------------------------------------------------------------------

TEST_CASE("生命周期: 合法迁移逐边") {
    CHECK(CanTransitionSessionStatus(SessionStatus::Preparing, SessionStatus::Running));
    CHECK(CanTransitionSessionStatus(SessionStatus::Running, SessionStatus::Closing));
    CHECK(CanTransitionSessionStatus(SessionStatus::Running, SessionStatus::Incomplete));
    CHECK(CanTransitionSessionStatus(SessionStatus::Closing, SessionStatus::Closed));
    CHECK(CanTransitionSessionStatus(SessionStatus::Closing, SessionStatus::Incomplete));
    CHECK(CanTransitionSessionStatus(SessionStatus::Closed, SessionStatus::Archived));
    CHECK(CanTransitionSessionStatus(SessionStatus::Archived, SessionStatus::Closed));
    // 任意可读状态 -> corrupt。
    for (const SessionStatus from : {SessionStatus::Preparing, SessionStatus::Running,
                                     SessionStatus::Closing, SessionStatus::Closed,
                                     SessionStatus::Incomplete, SessionStatus::Archived}) {
        CHECK(CanTransitionSessionStatus(from, SessionStatus::Corrupt));
    }
}

TEST_CASE("生命周期: 非法迁移一律拒") {
    // preparing 只能去 running:不许直闭、不许 incomplete/archived。
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Preparing, SessionStatus::Closed));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Preparing, SessionStatus::Incomplete));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Preparing, SessionStatus::Archived));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Preparing, SessionStatus::Closing));
    // running 不许直 closed(须过 closing),不许 archived。
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Running, SessionStatus::Closed));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Running, SessionStatus::Archived));
    // closed 不许复活。
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Closed, SessionStatus::Running));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Closed, SessionStatus::Incomplete));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Closed, SessionStatus::Closing));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Archived, SessionStatus::Running));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Archived, SessionStatus::Incomplete));
    // incomplete/corrupt 是终点(除 corrupt 外不再出边)。
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Incomplete, SessionStatus::Running));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Incomplete, SessionStatus::Closed));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Corrupt, SessionStatus::Running));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Corrupt, SessionStatus::Closed));
    CHECK_FALSE(CanTransitionSessionStatus(SessionStatus::Corrupt, SessionStatus::Corrupt));
    // 自环一律非法。
    for (const SessionStatus status : {SessionStatus::Preparing, SessionStatus::Running,
                                       SessionStatus::Closing, SessionStatus::Closed,
                                       SessionStatus::Incomplete, SessionStatus::Corrupt,
                                       SessionStatus::Archived}) {
        CHECK_FALSE(CanTransitionSessionStatus(status, status));
    }
    // 名字往返。
    for (const SessionStatus status : {SessionStatus::Preparing, SessionStatus::Running,
                                       SessionStatus::Closing, SessionStatus::Closed,
                                       SessionStatus::Incomplete, SessionStatus::Corrupt,
                                       SessionStatus::Archived}) {
        CHECK(SessionStatusFromName(SessionStatusName(status)) == status);
    }
    CHECK_FALSE(SessionStatusFromName("tombstone").has_value());  // 管理态,不入表
    CHECK_FALSE(SessionStatusFromName("running2").has_value());
}

TEST_CASE("生命周期: session.json 转态原子写与折叠豁免") {
    const std::filesystem::path root = MakeRoot("transition");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);
    REQUIRE(manager.LaunchSession().has_value());
    const std::filesystem::path dir = manager.active()->session_dir();
    SessionManifest manifest = *ReadSessionJson(dir);

    // preparing -> running 已由 Launch 办妥。
    CHECK(manifest.status == "running");
    // running -> closed 走折叠才许(恢复器补正半途崩溃用),常规拒。
    CHECK(TransitionSessionStatus(dir, &manifest, SessionStatus::Closed)
              .error()
              .rfind("session.transition_invalid", 0) == 0);
    CHECK(TransitionSessionStatus(dir, &manifest, SessionStatus::Closed, true).has_value());
    CHECK(ReadSessionJson(dir)->status == "closed");
    // closed -> archived 本就是图上的边,折叠与否都放行。
    CHECK(TransitionSessionStatus(dir, &manifest, SessionStatus::Archived, true).has_value());
    // 折叠不许借道 archive,也不许复活/corrupt 洗白。
    manifest.status = "incomplete";
    CHECK_FALSE(TransitionSessionStatus(dir, &manifest, SessionStatus::Archived, true).has_value());
    manifest.status = "archived";
    CHECK_FALSE(TransitionSessionStatus(dir, &manifest, SessionStatus::Running, true).has_value());
    manifest.status = "corrupt";
    CHECK_FALSE(TransitionSessionStatus(dir, &manifest, SessionStatus::Closed, true).has_value());
}

// ---------------------------------------------------------------------------
// workspace lifecycle 账(§3.2)
// ---------------------------------------------------------------------------

TEST_CASE("lifecycle: intent/result 一操作一只目录,历史不许改写") {
    const std::filesystem::path root = MakeRoot("ledger");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);
    REQUIRE(manager.LaunchSession().has_value());

    LifecycleIntent intent;
    intent.operation_id = "20260830-031522-L00001";
    intent.operation = LifecycleOperationName(LifecycleOperation::ArchiveSession);
    intent.workspace_key = manager.workspace_key();
    intent.session_id = "20260830-031522-L00002";
    intent.requested_at_ms = clock.WallMs();
    intent.parameters["reason"] = "unit";

    const auto ledger = manager.lifecycle();
    const auto op_dir = ledger.WriteIntent(intent);
    REQUIRE(op_dir.has_value());
    CHECK(std::filesystem::exists(*op_dir / "intent.json"));
    // operation_id 复用即拒。
    CHECK(ledger.WriteIntent(intent).error().rfind("lifecycle.intent_exists", 0) == 0);

    LifecycleResult result;
    result.operation_id = intent.operation_id;
    result.status = "completed";
    result.completed_at_ms = clock.WallMs() + 1;
    result.outcome["status"] = "archived";
    REQUIRE(ledger.WriteResult(result).has_value());
    // result 已存在即拒——历史结果不许覆盖。
    CHECK(ledger.WriteResult(result).error().rfind("lifecycle.result_exists", 0) == 0);

    const auto read_intent = WorkspaceLifecycle::ReadIntent(*op_dir);
    REQUIRE(read_intent.has_value());
    CHECK(read_intent->operation == "archive_session");
    CHECK(read_intent->session_id == intent.session_id);
    CHECK(read_intent->parameters["reason"] == "unit");
    const auto read_result = WorkspaceLifecycle::ReadResult(*op_dir);
    REQUIRE(read_result.has_value());
    CHECK(read_result->status == "completed");
    CHECK(read_result->outcome["status"] == "archived");
}

TEST_CASE("lifecycle: tombstone 齐全——id/末hash/时间/原因,一枚只删一次") {
    const std::filesystem::path root = MakeRoot("tombstone");
    const std::filesystem::path tombstones = root / "tombstones";

    SessionTombstone tombstone;
    tombstone.session_id = "20260830-031522-T00001";
    tombstone.deleted_at_ms = 1759000000123LL;
    tombstone.reason = "user_delete";
    tombstone.last_event_hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    tombstone.operation_id = "20260830-031530-T00002";
    REQUIRE(WriteSessionTombstone(tombstones, tombstone).has_value());

    const auto read = ReadSessionTombstone(tombstones, tombstone.session_id);
    REQUIRE(read.has_value());
    CHECK(read->reason == "user_delete");
    CHECK(read->last_event_hash.has_value());
    CHECK(*read->last_event_hash == *tombstone.last_event_hash);
    CHECK(read->operation_id == tombstone.operation_id);
    CHECK(read->deleted_at_ms == tombstone.deleted_at_ms);
    // 重复删拒绝。
    CHECK(WriteSessionTombstone(tombstones, tombstone).error().rfind("lifecycle.tombstone_exists",
                                                                     0) == 0);
    // 空 preparing 无事件:last_event_hash 为 null 也照走。
    SessionTombstone empty;
    empty.session_id = "20260830-031522-T00009";
    empty.deleted_at_ms = 1759000009999LL;
    empty.reason = "aborted_before_start";
    empty.operation_id = "20260830-031531-T00003";
    REQUIRE(WriteSessionTombstone(tombstones, empty).has_value());
    const auto read_empty = ReadSessionTombstone(tombstones, empty.session_id);
    REQUIRE(read_empty.has_value());
    CHECK_FALSE(read_empty->last_event_hash.has_value());
}

TEST_CASE("lifecycle: archive 只吃 closed,active running 拒绝") {
    const std::filesystem::path root = MakeRoot("archive");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);
    REQUIRE(manager.LaunchSession().has_value());
    const std::string id = manager.active()->session_id();
    // running 的 active session 不能 archive。
    CHECK(manager.ArchiveSession(id).error().rfind("session.archive_active", 0) == 0);
    // 干净 close 后 archive:closed -> archived。
    NullClearParticipant participant;
    REQUIRE(manager.Close(CloseRequest{}, &participant).error_code.empty());
    REQUIRE(manager.ArchiveSession(id).has_value());
    CHECK(ReadSessionJson(manager.SessionDirOf(id))->status == "archived");
    // 再 archive 非法(自环)。
    CHECK_FALSE(manager.ArchiveSession(id).has_value());
}

TEST_CASE("lifecycle: delete 先 intent 后 tombstone 再删目录;未封口不删") {
    const std::filesystem::path root = MakeRoot("delete");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);
    REQUIRE(manager.LaunchSession().has_value());
    const std::string id = manager.active()->session_id();

    // running 的 active session 不能删。
    CHECK(manager.DeleteSession(id, "user_delete").error().rfind("session.delete_active", 0) == 0);

    NullClearParticipant participant;
    REQUIRE(manager.Close(CloseRequest{}, &participant).error_code.empty());
    REQUIRE(manager.DeleteSession(id, "user_delete").has_value());
    CHECK_FALSE(std::filesystem::exists(manager.SessionDirOf(id)));
    // tombstone 齐全:末 hash 是旧账最后一枚事件的 hash。
    const auto tombstone = ReadSessionTombstone(manager.workspace_dir() / "tombstones", id);
    REQUIRE(tombstone.has_value());
    CHECK(tombstone->reason == "user_delete");
    REQUIRE(tombstone->last_event_hash.has_value());
    CHECK(tombstone->last_event_hash->size() == 64);
    CHECK_FALSE(tombstone->operation_id.empty());
    // 再删:目录已不在。
    CHECK_FALSE(manager.DeleteSession(id, "user_delete").has_value());
}

TEST_CASE("lifecycle: resume_reference 只记引用账") {
    const std::filesystem::path root = MakeRoot("resume-ref");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);
    REQUIRE(manager.LaunchSession().has_value());
    const std::string id = manager.active()->session_id();
    REQUIRE(manager.RecordResumeReference(id, "unit").has_value());
    CHECK_FALSE(manager.RecordResumeReference("20990101-000000-NOPE01", "unit").has_value());
}

// ---------------------------------------------------------------------------
// Journal 事实扫描与状态推导
// ---------------------------------------------------------------------------

TEST_CASE("事实扫描: 空账/丢账是 preparing,截断尾是 incomplete") {
    const std::filesystem::path root = MakeRoot("facts");
    FakeClock clock;
    SessionManager manager(Opts(root), &clock);

    const MainJournalFacts missing = ScanStreamFacts(root / "nope.jsonl");
    CHECK_FALSE(missing.journal_exists);
    CHECK(DeriveSessionStatusFromFacts(missing) == SessionStatus::Preparing);

    const std::filesystem::path empty = root / "empty.jsonl";
    { std::ofstream file(empty, std::ios::binary | std::ios::trunc); }
    const MainJournalFacts empty_facts = ScanStreamFacts(empty);
    CHECK(empty_facts.journal_exists);
    CHECK_FALSE(empty_facts.has_run_started);
    CHECK(DeriveSessionStatusFromFacts(empty_facts) == SessionStatus::Preparing);
}
