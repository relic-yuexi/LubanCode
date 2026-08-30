// clear 换账各崩溃点的恢复测试(§3.3.1 末段/§3.3.2/§16.4):
//   崩在收口半路/封链后/session.json 转态前/新账开张半路,恢复器一律以
//   Journal 可证事实为准续办:不合并两本 JSONL,不复用旧 session_id,空
//   preparing 可标 aborted_before_start(tombstone 齐全)。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

struct FakeClock : SessionManagerClock {
    std::int64_t wall = 1759000000000LL;
    mutable int random_calls = 0;
    std::int64_t WallMs() const override { return wall; }
    std::int64_t MonotonicNs() const override { return 9000LL + random_calls; }
    std::string Random6() const override {
        ++random_calls;
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "R%05d", random_calls);
        return buffer;
    }
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-traj-recovery-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.trajectories_root = root / "trajectories";
    options.workspace_root = root / "ws";
    options.readable_workspace_name = "恢复测试";
    options.launch_cwd = "D:/tmp/ws";
    options.lubancode_version = "0.26.128-test";
    return options;
}

std::vector<nlohmann::json> Events(const std::filesystem::path& stream) {
    const auto lines = ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    std::vector<nlohmann::json> events;
    for (const std::string& line : *lines) {
        events.push_back(nlohmann::json::parse(line, nullptr, false));
        REQUIRE_FALSE(events.back().is_discarded());
    }
    return events;
}

std::vector<std::string> Kinds(const std::filesystem::path& stream) {
    std::vector<std::string> kinds;
    for (const nlohmann::json& event : Events(stream)) {
        kinds.push_back(event.at("kind").get<std::string>());
    }
    return kinds;
}

void SetSessionStatus(const std::filesystem::path& dir, const std::string& status) {
    auto manifest = ReadSessionJson(dir);
    REQUIRE(manifest.has_value());
    manifest->status = status;
    REQUIRE(WriteSessionJsonAtomic(dir, *manifest).has_value());
}

// 铺一场带活账的 session(活动 turn + 活 queue + 活选段 + 子流)。
struct CrashFixture {
    FakeClock clock;
    std::unique_ptr<SessionManager> manager;
    std::optional<TrajectoryRecorder> child;
    std::filesystem::path root;
    std::filesystem::path old_dir;
    std::string old_id;

    explicit CrashFixture(const char* tag, bool with_child = true) : root(MakeRoot(tag)) {
        manager = std::make_unique<SessionManager>(Opts(root), &clock);
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        old_id = active->session_id();
        old_dir = active->session_dir();
        TrajectoryRecorder* main = &*active->main;

        RecordRequest turn_start;
        turn_start.kind = EventKind::TurnStarted;
        turn_start.scope = main->base_scope();
        turn_start.scope.turn_id = "turn-0007";
        turn_start.payload["trigger"] = "external_user";
        REQUIRE(main->Record(turn_start, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        RecordRequest input;
        input.kind = EventKind::InputReceived;
        input.scope = main->base_scope();
        input.scope.turn_id = "turn-0007";
        input.scope.actor = Actor::User;
        input.scope.origin = Origin::ExternalUser;
        input.payload["input_id"] = "input-0007";
        input.payload["content"] = nlohmann::json::array({"干一半的话"});
        input.payload["channel"] = "terminal";
        input.payload["sender"] = nlohmann::json{{"kind", "local_user"}};
        REQUIRE(main->Record(input, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        RecordRequest queued;
        queued.kind = EventKind::ControlQueueItemEnqueued;
        queued.scope = main->base_scope();
        queued.payload["item_id"] = "q-0007";
        queued.payload["input_id"] = "input-0008";
        REQUIRE(main->Record(queued, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        RecordRequest selection;
        selection.kind = EventKind::RecordSelectionStarted;
        selection.scope = main->base_scope();
        selection.payload["record_id"] = "record-0007";
        REQUIRE(main->Record(selection, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        if (with_child) {
            auto stream = active->directory.ReserveSubagentStream("agent-0007");
            REQUIRE(stream.has_value());
            EventScope child_scope = main->base_scope();
            child_scope.run_id = "agent-0007";
            child_scope.run_kind = RunKind::Subagent;
            auto started = TrajectoryRecorder::Start(*stream, active->directory.artifacts_root(),
                                                     child_scope, RecorderOptions{}, &clock);
            REQUIRE(started.has_value());
            child = std::move(*started);
            nlohmann::json extra;
            extra["agent_run_id"] = "agent-0007";
            extra["owner_run_id"] = "main-0001";
            REQUIRE(child->WriteRunStarted(extra, Durability::PowerLoss).status ==
                    RecordReceipt::Status::Committed);
        }
    }
};

// 崩在收口半路的参与者:抛异常模拟进程暴毙(账面停在第 2/3 步之间)。
struct ExplodingParticipant : ClearParticipant {
    std::string CancelActiveTurn() override { throw std::runtime_error("boom"); }
    std::vector<ChildClosure> CancelActiveChildren() override { return {}; }
    std::vector<std::string> CancelQueuedItems() override { return {}; }
    std::string ActiveRecordSelectionId() override { return {}; }
    void ResetInMemoryState() override {}
};

struct QuietParticipant : ClearParticipant {
    std::string CancelActiveTurn() override { return {}; }
    std::vector<ChildClosure> CancelActiveChildren() override { return {}; }
    std::vector<std::string> CancelQueuedItems() override { return {}; }
    std::string ActiveRecordSelectionId() override { return {}; }
    void ResetInMemoryState() override {}
};

}  // namespace

// ---------------------------------------------------------------------------
// 崩在换账半路:恢复器续办
// ---------------------------------------------------------------------------

TEST_CASE("崩溃点: 崩在第 2 步后收口半路——旧账补封,新账续办开张") {
    CrashFixture fixture("step3");
    const std::string old_id = fixture.old_id;
    const std::filesystem::path old_dir = fixture.old_dir;

    // 崩:requested + clear_requested 已 durable,收口没跑。
    ExplodingParticipant exploding;
    CHECK_THROWS_AS(fixture.manager->Clear(ClearRequest{}, &exploding), std::runtime_error);
    const std::filesystem::path workspaces = fixture.manager->workspace_dir();
    std::string new_id;
    {
        const MainJournalFacts facts = ScanStreamFacts(old_dir / "main.jsonl");
        REQUIRE(facts.clear_requested);
        new_id = facts.clear_requested_next_session_id;
    }
    const std::filesystem::path new_dir =
        workspaces / "sessions" / std::filesystem::path(new_id);
    fixture.child.reset();     // 子流句柄也放干净
    fixture.manager.reset();  // "进程"死透:句柄全放

    // 重开:恢复器续办。
    FakeClock clock2;
    SessionManager recovered(Opts(fixture.root), &clock2);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();

    // 旧账:悬空全收,unknown(子流没终态)→ run.failed + incomplete。
    const std::vector<std::string> old_kinds = Kinds(old_dir / "main.jsonl");
    CHECK(std::find(old_kinds.begin(), old_kinds.end(), "control.queue.item.cancelled") !=
          old_kinds.end());
    CHECK(std::find(old_kinds.begin(), old_kinds.end(), "record.selection.interrupted") !=
          old_kinds.end());
    CHECK(std::find(old_kinds.begin(), old_kinds.end(), "turn.cancelled") != old_kinds.end());
    CHECK(old_kinds.back() == "session.ended");
    const std::vector<nlohmann::json> old_events = Events(old_dir / "main.jsonl");
    CHECK(old_events.back().at("payload").at("close_quality") == "incomplete");
    CHECK(old_events.back().at("payload").at("next_session_id") == new_id);
    CHECK(ReadSessionJson(old_dir)->status == "incomplete");
    // 子流在旧目录补 run.cancelled(副作用不明,不装成功)。
    const std::vector<std::string> child_kinds = Kinds(old_dir / "subagents" / "agent-0007.jsonl");
    CHECK(child_kinds.back() == "run.cancelled");

    // 新账:恢复器开张,反指旧终态;接作 active。
    CHECK(report.adopted_session_id == new_id);
    REQUIRE(recovered.active() != nullptr);
    CHECK(recovered.active()->session_id() == new_id);
    const std::vector<std::string> new_kinds = Kinds(new_dir / "main.jsonl");
    REQUIRE(new_kinds.size() == 2);
    CHECK(new_kinds[0] == "run.started");
    CHECK(new_kinds[1] == "control.command.completed");
    const std::vector<nlohmann::json> new_events = Events(new_dir / "main.jsonl");
    CHECK(new_events[0].at("payload").at("start_reason") == "clear");
    CHECK(new_events[0].at("payload").at("previous_session_id") == old_id);
    CHECK(new_events[0].at("payload").at("caused_by_event_ref").at("event_hash") ==
          old_events.back().at("event_hash"));
    CHECK(ReadSessionJson(new_dir)->status == "running");

    // 两本各自验得过,session_id 不复用,不拼接。
    CHECK(VerifyJournalFile(old_dir / "main.jsonl").ok);
    CHECK(VerifyJournalFile(new_dir / "main.jsonl").ok);
    CHECK(old_id != new_id);

    // 接续的 active 能继续写(control 面),链条不断。
    RecordRequest title;
    title.kind = EventKind::ControlTitleChanged;
    title.scope = recovered.active()->main->base_scope();
    title.payload["title"] = "恢复后第一笔";
    const auto receipt = recovered.active()->main->Record(title, Durability::ProcessCrash);
    CHECK(receipt.status == RecordReceipt::Status::Committed);
    CHECK(receipt.seq == 3);
}

namespace {

// 跑完整场 clear 再"倒带"出各崩溃点(第 4/5/6/7 步之间的盘上状态)。
struct RewoundClear {
    std::filesystem::path old_dir;
    std::filesystem::path new_dir;
    std::string old_id;
    std::string new_id;
    std::filesystem::path root;

    explicit RewoundClear(const char* tag) : root(MakeRoot(tag)) {
        FakeClock clock;
        SessionManager manager(Opts(root), &clock);
        auto* active = manager.LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        old_id = active->session_id();
        old_dir = active->session_dir();
        QuietParticipant participant;
        const ClearOutcome outcome = manager.Clear(ClearRequest{}, &participant);
        REQUIRE(outcome.error_code.empty());
        new_id = outcome.new_session_id;
        new_dir = manager.active()->session_dir();
    }

    void RewindOldStatus(const std::string& status) { SetSessionStatus(old_dir, status); }
    void DropNewMain() {
        std::error_code ec;
        std::filesystem::remove(new_dir / "main.jsonl", ec);
        SetSessionStatus(new_dir, "preparing");
    }
    void KeepOnlyNewRunStarted() {
        const auto lines = ReadJournalLines(new_dir / "main.jsonl");
        REQUIRE(lines.has_value());
        REQUIRE(lines->size() >= 1);
        std::ofstream file(new_dir / "main.jsonl", std::ios::binary | std::ios::trunc);
        file << (*lines)[0] << "\n";
        SetSessionStatus(new_dir, "preparing");
    }
    void TearNewSecondEvent() {
        const auto lines = ReadJournalLines(new_dir / "main.jsonl");
        REQUIRE(lines.has_value());
        REQUIRE(lines->size() >= 2);
        // 半行事件:没有换行符(崩溃截断,§16.3)。
        const std::string half = lines->at(1).substr(0, lines->at(1).size() / 2);
        std::ofstream file(new_dir / "main.jsonl", std::ios::binary | std::ios::trunc);
        file << (*lines)[0] << "\n" << half;
        SetSessionStatus(new_dir, "preparing");
    }
    void NewPreparingOnly() { SetSessionStatus(new_dir, "preparing"); }
};

}  // namespace

TEST_CASE("崩溃点: 崩在第 4 步后——旧账已封,新账空 preparing 被续办开张") {
    RewoundClear state("step4");
    state.RewindOldStatus("closing");  // 第 5 步没跑
    state.DropNewMain();               // 第 6 步没跑

    FakeClock clock;
    SessionManager recovered(Opts(state.root), &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    CHECK(report.adopted_session_id == state.new_id);
    // 旧账按事实补正 closed;新账开张。
    CHECK(ReadSessionJson(state.old_dir)->status == "closed");
    REQUIRE(recovered.active() != nullptr);
    const std::vector<std::string> new_kinds = Kinds(state.new_dir / "main.jsonl");
    REQUIRE(new_kinds.size() == 2);
    CHECK(new_kinds[0] == "run.started");
    CHECK(new_kinds[1] == "control.command.completed");
    CHECK(ReadSessionJson(state.new_dir)->status == "running");
    // main_run_id 沿用第 1 步写定的号,不给新账换第二号。
    CHECK(ReadSessionJson(state.new_dir)->main_run_id == "main-0002");
    CHECK(VerifyJournalFile(state.old_dir / "main.jsonl").ok);
    CHECK(VerifyJournalFile(state.new_dir / "main.jsonl").ok);
}

TEST_CASE("崩溃点: 崩在第 5 步后——session.json 停 closing,事实把它补正 closed") {
    RewoundClear state("step5");
    state.DropNewMain();

    FakeClock clock;
    SessionManager recovered(Opts(state.root), &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    CHECK(report.adopted_session_id == state.new_id);
    CHECK(ReadSessionJson(state.old_dir)->status == "closed");
    CHECK(ReadSessionJson(state.new_dir)->status == "running");
}

TEST_CASE("崩溃点: 崩在第 6 步半路——新账 run.started 在,completed 缺,补写") {
    RewoundClear state("step6a");
    state.KeepOnlyNewRunStarted();

    FakeClock clock;
    SessionManager recovered(Opts(state.root), &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    CHECK(report.adopted_session_id == state.new_id);
    const std::vector<nlohmann::json> new_events = Events(state.new_dir / "main.jsonl");
    REQUIRE(new_events.size() == 2);
    CHECK(new_events[1].at("kind") == "control.command.completed");
    // 恢复器代笔:origin=recovery_runtime。
    CHECK(new_events[1].at("origin") == "recovery_runtime");
    CHECK(new_events[1].at("payload").at("qualified_requested_ref").at("session_id") ==
          state.old_id);
    CHECK(VerifyJournalFile(state.new_dir / "main.jsonl").ok);
    CHECK(recovered.active() != nullptr);
}

TEST_CASE("崩溃点: 崩在第 6 步尾行半个事件——新账判 incomplete,不采纳") {
    RewoundClear state("step6b");
    state.TearNewSecondEvent();

    FakeClock clock;
    SessionManager recovered(Opts(state.root), &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    CHECK(report.adopted_session_id.empty());
    CHECK(recovered.active() == nullptr);
    CHECK(ReadSessionJson(state.new_dir)->status == "incomplete");
    // 旧账照旧完整。
    CHECK(ReadSessionJson(state.old_dir)->status == "closed");
    CHECK(VerifyJournalFile(state.old_dir / "main.jsonl").ok);
}

TEST_CASE("崩溃点: 崩在第 7 步前——只补 session.json,不添事件") {
    RewoundClear state("step7");
    state.NewPreparingOnly();

    FakeClock clock;
    SessionManager recovered(Opts(state.root), &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    CHECK(report.adopted_session_id == state.new_id);
    CHECK(ReadSessionJson(state.new_dir)->status == "running");
    // 不重复开账:新 main 还是那两条。
    const std::vector<std::string> new_kinds = Kinds(state.new_dir / "main.jsonl");
    REQUIRE(new_kinds.size() == 2);
    CHECK(new_kinds[0] == "run.started");
    CHECK(VerifyJournalFile(state.new_dir / "main.jsonl").ok);
}

// ---------------------------------------------------------------------------
// 空 preparing 清账与 tombstone
// ---------------------------------------------------------------------------

TEST_CASE("策略 AbortEmptyPreparing: 空 preparing 清账,tombstone 齐全") {
    CrashFixture fixture("abort", /*with_child=*/false);
    const std::filesystem::path workspace = fixture.manager->workspace_dir();
    ExplodingParticipant exploding;
    CHECK_THROWS_AS(fixture.manager->Clear(ClearRequest{}, &exploding), std::runtime_error);
    fixture.manager.reset();

    FakeClock clock;
    SessionManager recovered(Opts(fixture.root), &clock);
    const WorkspaceRecoveryReport report =
            recovered.RecoverWorkspace(ClearRecoveryPolicy::AbortEmptyPreparing);
    // 旧账照封(悬空 turn 收掉,没有子流,算干净)。
    CHECK(ReadSessionJson(fixture.old_dir)->status == "closed");
    // 新账清账,不留 active。
    CHECK(report.adopted_session_id.empty());
    CHECK(recovered.active() == nullptr);
    // tombstone 记 aborted_before_start,无末 hash(空账)。
    std::string new_id;
    {
        const MainJournalFacts facts = ScanStreamFacts(fixture.old_dir / "main.jsonl");
        new_id = facts.clear_requested_next_session_id;
    }
    CHECK_FALSE(std::filesystem::exists(workspace / "sessions" /
                                        std::filesystem::path(new_id)));
    const auto tombstone = ReadSessionTombstone(workspace / "tombstones", new_id);
    REQUIRE(tombstone.has_value());
    CHECK(tombstone->reason == "aborted_before_start");
    CHECK_FALSE(tombstone->last_event_hash.has_value());
    CHECK_FALSE(tombstone->operation_id.empty());
}

TEST_CASE("孤儿 preparing(开场半路崩): 恢复器清账 + tombstone") {
    const std::filesystem::path root = MakeRoot("orphan");
    FakeClock clock;
    SessionManagerOptions options = Opts(root);
    {
        // 只办到第 1 步:目录 + preparing manifest,没有 main.jsonl。
        SessionManager manager(options, &clock);
        auto* active = manager.LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
    }
    // 直接手植一只空 preparing(不带锁):launch 崩在锁之后、开张之前。
    SessionManifest manifest;
    manifest.schema_version = 1;
    manifest.workspace_key = ComputeWorkspaceKey(options.workspace_root);
    manifest.session_id = "20260830-040000-R90001";
    manifest.launch_cwd = options.launch_cwd;
    manifest.main_run_id = "main-0009";
    manifest.start_reason = "process_launch";
    manifest.status = "preparing";
    manifest.created_at_ms = clock.WallMs();
    manifest.lubancode_version = options.lubancode_version;
    auto directory = TrajectoryDirectory::CreateWorkspace(options.trajectories_root,
                                                          options.workspace_root, "孤儿测试",
                                                          clock.WallMs());
    REQUIRE(directory.has_value());
    REQUIRE(TrajectoryDirectory::CreateSession(options.trajectories_root / "workspaces",
                                               manifest.workspace_key, manifest)
                .has_value());

    SessionManager recovered(options, &clock);
    const WorkspaceRecoveryReport report = recovered.RecoverWorkspace();
    bool saw_abort = false;
    for (const SessionRecoveryEntry& entry : report.sessions) {
        if (entry.session_id == manifest.session_id) {
            saw_abort = entry.aborted_before_start;
        }
    }
    CHECK(saw_abort);
    CHECK_FALSE(std::filesystem::exists(recovered.SessionDirOf(manifest.session_id)));
    const auto tombstone =
        ReadSessionTombstone(recovered.workspace_dir() / "tombstones", manifest.session_id);
    REQUIRE(tombstone.has_value());
    CHECK(tombstone->reason == "aborted_before_start");
}
