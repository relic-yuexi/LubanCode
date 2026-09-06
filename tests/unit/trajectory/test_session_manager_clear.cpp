// SessionManager 与 clear 八步换账测试(§3.3.1 逐字/§16.4 clear 幕):
// 开场换账全程落盘次序、旧账封链后拒写、新账新命名空间、并发重复请求回
// clear_in_progress、unknown child 不装成功、/exit 封口与 closed 硬门。
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "platform/process.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/session_index.hpp"
#include "trajectory/session_manager.hpp"

using namespace lubancode::trajectory;

namespace {

struct FakeClock : SessionManagerClock {
    std::int64_t wall = 1759000000000LL;
    mutable int random_calls = 0;
    std::int64_t WallMs() const override { return wall; }
    std::int64_t MonotonicNs() const override { return 7000LL + random_calls; }
    std::string Random6() const override {
        ++random_calls;
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "C%05d", random_calls);
        return buffer;
    }
};

std::filesystem::path MakeRoot(const char* tag) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("lubancode-traj-clear-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

SessionManagerOptions Opts(const std::filesystem::path& root) {
    SessionManagerOptions options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "ws";
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

long IndexOf(const std::vector<std::string>& kinds, const std::string& kind) {
    const auto it = std::find(kinds.begin(), kinds.end(), kind);
    REQUIRE(it != kinds.end());
    return static_cast<long>(it - kinds.begin());
}

// 旧 session 里铺一层"正在干活"的事实:一场完整 turn + 活 queue item +
// 活动 /record 选段 + 一只 subagent 子流(recorder 归测试掌管)。
struct BusySessionFixture {
    FakeClock clock;
    std::unique_ptr<SessionManager> manager;
    std::optional<TrajectoryRecorder> child;
    std::filesystem::path old_dir;
    std::string old_id;
    bool reset_called = false;
    bool child_terminal_written = false;

    explicit BusySessionFixture(const char* tag, bool with_child = true) {
        manager = std::make_unique<SessionManager>(Opts(MakeRoot(tag)), &clock);
        auto* active = manager->LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        old_id = active->session_id();
        old_dir = active->session_dir();
        TrajectoryRecorder* main = &*active->main;

        // 完整 turn:user 输入 → turn 收口。
        RecordRequest turn_start;
        turn_start.kind = EventKind::TurnStarted;
        turn_start.scope = main->base_scope();
        turn_start.scope.turn_id = "turn-0001";
        turn_start.payload["trigger"] = "external_user";
        REQUIRE(main->Record(turn_start, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        RecordRequest input;
        input.kind = EventKind::InputReceived;
        input.scope = main->base_scope();
        input.scope.turn_id = "turn-0001";
        input.scope.actor = Actor::User;
        input.scope.origin = Origin::ExternalUser;
        input.payload["input_id"] = "input-0001";
        input.payload["content"] = nlohmann::json::array({"查一下目录"});
        input.payload["channel"] = "terminal";
        input.payload["sender"] = nlohmann::json{{"kind", "local_user"}};
        REQUIRE(main->Record(input, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);
        RecordRequest turn_done;
        turn_done.kind = EventKind::TurnCompleted;
        turn_done.scope = main->base_scope();
        turn_done.scope.turn_id = "turn-0001";
        turn_done.payload["outcome"] = "done";
        REQUIRE(main->Record(turn_done, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        // 活 queue item(未送达)。
        RecordRequest queued;
        queued.kind = EventKind::ControlQueueItemEnqueued;
        queued.scope = main->base_scope();
        queued.payload["item_id"] = "q-0001";
        queued.payload["input_id"] = "input-0002";
        REQUIRE(main->Record(queued, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        // 活动 /record 选段。
        RecordRequest selection;
        selection.kind = EventKind::RecordSelectionStarted;
        selection.scope = main->base_scope();
        selection.payload["record_id"] = "record-0001";
        REQUIRE(main->Record(selection, Durability::ProcessCrash).status ==
                RecordReceipt::Status::Committed);

        if (with_child) {
            auto stream = active->directory.ReserveSubagentStream("agent-0001");
            REQUIRE(stream.has_value());
            EventScope child_scope = main->base_scope();
            child_scope.run_id = "agent-0001";
            child_scope.run_kind = RunKind::Subagent;
            auto started = TrajectoryRecorder::Start(*stream, active->directory.artifacts_root(),
                                                     child_scope, RecorderOptions{}, &clock);
            REQUIRE(started.has_value());
            child = std::move(*started);
            nlohmann::json extra;
            extra["agent_run_id"] = "agent-0001";
            extra["owner_run_id"] = "main-0001";
            REQUIRE(child->WriteRunStarted(extra, Durability::PowerLoss).status ==
                    RecordReceipt::Status::Committed);
        }
    }

    // clear 参与者:child 在旧目录落 terminal(或不落),queue/选段照实申报。
    struct Participant : ClearParticipant {
        BusySessionFixture* owner;
        bool close_child = true;
        bool report_unknown = false;
        std::string CancelActiveTurn() override { return {}; }
        std::vector<ChildClosure> CancelActiveChildren() override {
            if (!owner->child.has_value()) {
                return {};
            }
            ChildClosure closure;
            closure.run_id = "agent-0001";
            if (close_child) {
                const auto receipt =
                    owner->child->FinishRun(EventKind::RunCancelled, "clear", Durability::PowerLoss);
                closure.terminal_written =
                    receipt.status == RecordReceipt::Status::Committed;
                owner->child_terminal_written = closure.terminal_written;
            }
            closure.unknown = report_unknown || !closure.terminal_written;
            return {closure};
        }
        std::vector<std::string> CancelQueuedItems() override { return {"q-0001"}; }
        std::string ActiveRecordSelectionId() override { return "record-0001"; }
        void ResetInMemoryState() override { owner->reset_called = true; }
    };
};

}  // namespace

// ---------------------------------------------------------------------------
// 开场与换账主路
// ---------------------------------------------------------------------------

TEST_CASE("开场: preparing->running,run.started(process_launch),lifecycle 齐") {
    BusySessionFixture fixture("launch", /*with_child=*/false);
    const ActiveSession* active = fixture.manager->active();
    REQUIRE(active != nullptr);
    CHECK(active->status == SessionStatus::Running);
    // 账本制:房门是门牌(≠key),尾巴带 key 的哈希段(尾 16 hex 的前 8)。
    {
        const std::string key = fixture.manager->workspace_key();
        const std::string door = fixture.manager->workspace_dir().filename().string();
        CHECK(door != key);
        CHECK(door.ends_with("-" + key.substr(key.size() - 16, 8)));
    }

    const auto manifest = ReadSessionJson(active->session_dir());
    REQUIRE(manifest.has_value());
    CHECK(manifest->status == "running");
    CHECK(manifest->start_reason == "process_launch");
    CHECK(manifest->main_run_id == "main-0001");

    // fixture 铺了完整 turn + queue + 选段:开场仍是 run.started 打头。
    const std::vector<std::string> kinds = Kinds(active->session_dir() / "main.jsonl");
    REQUIRE(kinds.size() == 6);
    CHECK(kinds[0] == "run.started");

    const auto report = VerifyJournalFile(active->session_dir() / "main.jsonl");
    CHECK(report.ok);
    // lifecycle:create_session 的 intent/result 一只目录。
    std::error_code ec;
    int op_dirs = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.manager->workspace_dir() / "lifecycle", ec)) {
        CHECK(std::filesystem::exists(entry.path() / "intent.json"));
        CHECK(std::filesystem::exists(entry.path() / "result.json"));
        ++op_dirs;
    }
    CHECK(op_dirs == 1);
    // 独占锁在,身份是本进程。
    const auto holder = SessionLock::Inspect(active->session_dir());
    REQUIRE(holder.has_value());
    CHECK(holder->pid == lubancode::platform::CurrentProcessId());
}

TEST_CASE("clear 八步: 旧账封口新账开张,落盘次序与证据齐") {
    BusySessionFixture fixture("full");
    const std::string old_id = fixture.old_id;
    const std::filesystem::path old_dir = fixture.old_dir;

    BusySessionFixture::Participant participant;
    participant.owner = &fixture;
    participant.close_child = true;
    const ClearOutcome outcome = fixture.manager->Clear(ClearRequest{}, &participant);

    // ---- 总账 ----
    REQUIRE(outcome.error_code.empty());
    CHECK(fixture.reset_called);                    // 第 8 步
    CHECK(fixture.child_terminal_written);          // child 在旧目录收口
    CHECK(outcome.old_session_id == old_id);
    CHECK(outcome.new_session_id != old_id);        // 绝不复用 session_id
    CHECK(outcome.old_main_run_id == "main-0001");
    CHECK(outcome.new_main_run_id == "main-0002");  // 新命名空间
    CHECK(outcome.new_session_prepared);
    CHECK(outcome.old_session_json_finalized);
    CHECK(outcome.new_session_running);
    CHECK(outcome.active_switched);
    CHECK(outcome.old_close_quality == "clean");
    CHECK(outcome.old_run_terminal_kind == "run.completed");
    CHECK_FALSE(outcome.old_journal_sha256.empty());
    CHECK_FALSE(outcome.boundary_operation_id.empty());

    const ActiveSession* active = fixture.manager->active();
    REQUIRE(active != nullptr);
    const std::filesystem::path new_dir = active->session_dir();
    CHECK(new_dir != old_dir);
    CHECK(active->session_id() == outcome.new_session_id);

    // ---- 旧 main:落盘次序 ----
    const std::vector<std::string> old_kinds = Kinds(old_dir / "main.jsonl");
    const long requested = IndexOf(old_kinds, "control.command.requested");
    const long clear_requested = IndexOf(old_kinds, "session.clear_requested");
    const long queue_cancelled = IndexOf(old_kinds, "control.queue.item.cancelled");
    const long selection_interrupted = IndexOf(old_kinds, "record.selection.interrupted");
    const long run_terminal = IndexOf(old_kinds, "run.completed");
    const long session_ended = IndexOf(old_kinds, "session.ended");
    CHECK(requested < clear_requested);            // requested 先 durable
    CHECK(clear_requested < queue_cancelled);      // 先立换账再收活
    CHECK(queue_cancelled < selection_interrupted);
    CHECK(selection_interrupted < run_terminal);   // 收齐才写 run terminal
    CHECK(run_terminal < session_ended);
    CHECK(session_ended == static_cast<long>(old_kinds.size()) - 1);  // 必是最后一枚

    const std::vector<nlohmann::json> old_events = Events(old_dir / "main.jsonl");
    const nlohmann::json& ended = old_events.back();
    CHECK(ended.at("payload").at("reason") == "clear");
    CHECK(ended.at("payload").at("next_session_id") == outcome.new_session_id);
    CHECK(ended.at("payload").at("close_quality") == "clean");

    // clear_requested 不投成 user 训练消息:control 面、training exclude。
    const nlohmann::json& clear_event = old_events[clear_requested];
    CHECK(clear_event.at("plane") == "control");
    CHECK(clear_event.at("training_policy") == "exclude");
    CHECK(clear_event.at("payload").at("next_session_id") == outcome.new_session_id);
    const nlohmann::json& requested_event = old_events[requested];
    CHECK(requested_event.at("actor") == "user");           // 真人敲 /clear
    CHECK(requested_event.at("origin") == "external_user");
    CHECK(requested_event.at("training_policy") == "exclude");
    // boundary_operation_id 走 correlation_id 落在换账三枚关键事件上:
    // requested / clear_requested(旧)与 command.completed(新)。
    CHECK(clear_event.at("correlation_id") == outcome.boundary_operation_id);
    CHECK(requested_event.at("correlation_id") == outcome.boundary_operation_id);

    // queue/选段收口证据。
    const nlohmann::json& queue_event = old_events[queue_cancelled];
    CHECK(queue_event.at("payload").at("item_id") == "q-0001");
    CHECK(queue_event.at("payload").at("reason") == "clear");
    CHECK(old_events[selection_interrupted].at("payload").at("reason") ==
          "interrupted_by_clear");

    // ---- 旧 session.json:closed ----
    const auto old_manifest = ReadSessionJson(old_dir);
    REQUIRE(old_manifest.has_value());
    CHECK(old_manifest->status == "closed");

    // ---- 旧 child 在旧目录收口;新目录无旧 child 正文 ----
    const std::vector<std::string> child_kinds = Kinds(old_dir / "subagents" / "agent-0001.jsonl");
    CHECK(child_kinds.back() == "run.cancelled");
    std::error_code ec;
    CHECK(std::filesystem::is_empty(new_dir / "subagents", ec));

    // ---- 新 main:恰好两条,新 run 号起账 ----
    const std::vector<std::string> new_kinds = Kinds(new_dir / "main.jsonl");
    REQUIRE(new_kinds.size() == 2);
    CHECK(new_kinds[0] == "run.started");
    CHECK(new_kinds[1] == "control.command.completed");
    const std::vector<nlohmann::json> new_events = Events(new_dir / "main.jsonl");
    const nlohmann::json& run_started = new_events[0];
    CHECK(run_started.at("run_id") == "main-0002");
    CHECK(run_started.at("event_id") == "main-0002:evt-00000001");
    CHECK(run_started.at("payload").at("start_reason") == "clear");
    CHECK(run_started.at("payload").at("previous_session_id") == old_id);
    // 反指旧 session 终态事件(session.ended)。
    const nlohmann::json& caused = run_started.at("payload").at("caused_by_event_ref");
    CHECK(caused.at("session_id") == old_id);
    CHECK(caused.at("event_id") == ended.at("event_id"));
    CHECK(caused.at("event_hash") == ended.at("event_hash"));

    const nlohmann::json& completed = new_events[1];
    CHECK(completed.at("payload").at("boundary_operation_id") == outcome.boundary_operation_id);
    CHECK(completed.at("correlation_id") == outcome.boundary_operation_id);
    CHECK(completed.at("payload").at("qualified_requested_ref").at("session_id") == old_id);
    CHECK(completed.at("payload").at("qualified_requested_ref").at("event_id") ==
          requested_event.at("event_id"));

    // ---- 新 session.json:running,来历引用 ----
    const auto new_manifest = ReadSessionJson(new_dir);
    REQUIRE(new_manifest.has_value());
    CHECK(new_manifest->status == "running");
    CHECK(new_manifest->start_reason == "clear");
    CHECK(new_manifest->previous_session_id == old_id);
    CHECK(new_manifest->main_run_id == "main-0002");

    // ---- 两本各自验得过;不拼接 ----
    CHECK(VerifyJournalFile(old_dir / "main.jsonl").ok);
    CHECK(VerifyJournalFile(new_dir / "main.jsonl").ok);
    const MainJournalFacts new_facts = ScanStreamFacts(new_dir / "main.jsonl");
    CHECK(new_facts.previous_session_id == old_id);  // 只留来历引用
    CHECK(new_facts.event_count == 2);

    // lifecycle:launch + clear 各一只 create op。
    int op_dirs = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(fixture.manager->workspace_dir() / "lifecycle", ec)) {
        (void)entry;
        ++op_dirs;
    }
    CHECK(op_dirs == 2);
}

TEST_CASE("clear 后旧账封链: 再写必拒,新账照常写") {
    BusySessionFixture fixture("sealed", /*with_child=*/false);
    BusySessionFixture::Participant participant;
    participant.owner = &fixture;
    const ClearOutcome outcome = fixture.manager->Clear(ClearRequest{}, &participant);
    REQUIRE(outcome.error_code.empty());

    // 旧账续开(recovery 场景):session.ended 之后一概拒写。
    auto old_recorder =
        TrajectoryRecorder::Continue(fixture.old_dir / "main.jsonl", fixture.old_dir / "artifacts");
    REQUIRE(old_recorder.has_value());
    RecordRequest attempt;
    attempt.kind = EventKind::ControlTitleChanged;
    attempt.scope = old_recorder->base_scope();
    attempt.payload["title"] = "不许写";
    const auto receipt = old_recorder->Record(attempt, Durability::ProcessCrash);
    CHECK(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(receipt.error_code == "state.session_ended");

    // 新账照常收活:链条从 main-0002 续。
    RecordRequest title;
    title.kind = EventKind::ControlTitleChanged;
    title.scope = fixture.manager->active()->main->base_scope();
    title.payload["title"] = "新账第一笔";
    const auto fresh = fixture.manager->active()->main->Record(title, Durability::ProcessCrash);
    CHECK(fresh.status == RecordReceipt::Status::Committed);
    CHECK(fresh.seq == 3);
    CHECK(VerifyJournalFile(fixture.manager->active()->session_dir() / "main.jsonl").ok);
}

TEST_CASE("clear: child 收不口/unknown 不装成功——run.failed + incomplete") {
    BusySessionFixture fixture("unknown");
    BusySessionFixture::Participant participant;
    participant.owner = &fixture;
    participant.close_child = false;  // child 崩着没收口
    const ClearOutcome outcome = fixture.manager->Clear(ClearRequest{}, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.old_run_terminal_kind == "run.failed");
    CHECK(outcome.old_close_quality == "incomplete");
    const auto manifest = ReadSessionJson(fixture.old_dir);
    REQUIRE(manifest.has_value());
    CHECK(manifest->status == "incomplete");  // 不许写 clean closed(§3.3.2)
    const std::vector<std::string> kinds = Kinds(fixture.old_dir / "main.jsonl");
    CHECK(kinds.back() == "session.ended");
    CHECK(VerifyJournalFile(fixture.old_dir / "main.jsonl").ok);
    // 新账照开:旧账 incomplete 不挡换账。
    CHECK(fixture.manager->active() != nullptr);
    CHECK(fixture.manager->active()->session_id() == outcome.new_session_id);
}

TEST_CASE("clear: 掌管期间并发重复请求回 clear_in_progress(§3.3.1)") {
    BusySessionFixture fixture("busy", /*with_child=*/false);

    struct BlockingParticipant : ClearParticipant {
        std::promise<void>* entered;
        std::future<void>* release;
        std::string CancelActiveTurn() override {
            entered->set_value();
            release->wait();
            return {};
        }
        std::vector<ChildClosure> CancelActiveChildren() override { return {}; }
        std::vector<std::string> CancelQueuedItems() override { return {}; }
        std::string ActiveRecordSelectionId() override { return {}; }
        void ResetInMemoryState() override {}
    };

    std::promise<void> entered_promise;
    std::promise<void> release_promise;
    auto entered_future = entered_promise.get_future();
    auto release_future = release_promise.get_future();
    BlockingParticipant participant;
    participant.entered = &entered_promise;
    participant.release = &release_future;

    const std::string old_id = fixture.old_id;
    ClearOutcome worker_outcome;
    std::thread worker([&] {
        worker_outcome = fixture.manager->Clear(ClearRequest{}, &participant);
    });
    REQUIRE(entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    // 第 3 步掌管中:第二次 clear 回 clear_in_progress,close 回 close.busy。
    const ClearOutcome busy = fixture.manager->Clear(ClearRequest{}, nullptr);
    CHECK(busy.error_code == "clear.busy");
    const CloseOutcome close_busy = fixture.manager->Close(CloseRequest{}, nullptr);
    CHECK(close_busy.error_code == "close.busy");

    release_promise.set_value();
    worker.join();
    REQUIRE(worker_outcome.error_code.empty());
    CHECK(fixture.manager->active()->session_id() != old_id);
}

// ---------------------------------------------------------------------------
// /exit 与 EOF 封口(§14.5/§3.3.2 closed 硬门)
// ---------------------------------------------------------------------------

TEST_CASE("close(exit): 干净封口——run.completed + session.ended + closed") {
    BusySessionFixture fixture("exit", /*with_child=*/true);
    BusySessionFixture::Participant participant;
    participant.owner = &fixture;
    participant.close_child = true;
    const CloseOutcome outcome = fixture.manager->Close(CloseRequest{}, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.close_quality == "clean");
    CHECK(outcome.run_terminal_kind == "run.completed");
    CHECK_FALSE(outcome.journal_sha256.empty());

    const std::vector<std::string> kinds = Kinds(fixture.old_dir / "main.jsonl");
    CHECK(kinds.back() == "session.ended");
    const auto manifest = ReadSessionJson(fixture.old_dir);
    REQUIRE(manifest.has_value());
    CHECK(manifest->status == "closed");
    CHECK(VerifyJournalFile(fixture.old_dir / "main.jsonl").ok);

    // 封口的账不许再 close。
    const CloseOutcome again = fixture.manager->Close(CloseRequest{}, nullptr);
    CHECK(again.error_code == "close.no_active_session");
}

TEST_CASE("close 硬门: 盘上漏收的子流记 unknown,标 incomplete") {
    BusySessionFixture fixture("exit-dangling", /*with_child=*/true);
    BusySessionFixture::Participant participant;
    participant.owner = &fixture;
    participant.close_child = false;  // 子流没收口
    const CloseOutcome outcome = fixture.manager->Close(CloseRequest{}, &participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.close_quality == "incomplete");
    CHECK(outcome.run_terminal_kind == "run.failed");
    const auto manifest = ReadSessionJson(fixture.old_dir);
    REQUIRE(manifest.has_value());
    CHECK(manifest->status == "incomplete");
}

// ---------------------------------------------------------------------------
// 单发轨迹断档单:one_shot 一场过账本——run_kind 三处同源、正常收口、
// 索引可标、resume 候选排除、指名 resume 明拒。
// ---------------------------------------------------------------------------

TEST_CASE("one_shot 开场: manifest/信封/run.started 三处 run_kind 同源") {
    const std::filesystem::path root = MakeRoot("oneshot-launch");
    SessionManagerOptions options = Opts(root);
    options.main_run_kind = RunKind::OneShot;
    FakeClock clock;
    SessionManager manager(options, &clock);
    auto* active = manager.LaunchSession().value_or(nullptr);
    REQUIRE(active != nullptr);

    const auto manifest = ReadSessionJson(active->session_dir());
    REQUIRE(manifest.has_value());
    CHECK(manifest->run_kind == "one_shot");
    CHECK(manifest->start_reason == "process_launch");  // 冻结枚举不动,种类归 run_kind

    const std::vector<nlohmann::json> events = Events(active->session_dir() / "main.jsonl");
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().at("kind").get<std::string>() == "run.started");
    CHECK(events.front().at("run_kind").get<std::string>() == "one_shot");
    CHECK(events.front().at("payload").at("run_kind").get<std::string>() == "one_shot");
    CHECK(VerifyJournalFile(active->session_dir() / "main.jsonl").ok);

    // 正常收口:session.ended 在 one_shot main stream 照落(recorder 白名单)。
    CloseRequest close;
    close.reason = "exit";
    NullClearParticipant null_participant;
    const CloseOutcome outcome = manager.Close(close, &null_participant);
    REQUIRE(outcome.error_code.empty());
    CHECK(outcome.run_terminal_kind == "run.completed");
    const std::vector<std::string> kinds = Kinds(active->session_dir() / "main.jsonl");
    CHECK(kinds.back() == "session.ended");
    const auto closed_manifest = ReadSessionJson(active->session_dir());
    REQUIRE(closed_manifest.has_value());
    CHECK(closed_manifest->status == "closed");
}

TEST_CASE("one_shot 场: 索引可标 run_kind,resume 候选排除,指名 resume 明拒") {
    const std::filesystem::path root = MakeRoot("oneshot-resume");
    std::string one_shot_id;
    std::filesystem::path workspaces;
    std::string workspace_key;
    {
        SessionManagerOptions options = Opts(root);
        options.main_run_kind = RunKind::OneShot;
        FakeClock clock;
        SessionManager manager(options, &clock);
        auto* active = manager.LaunchSession().value_or(nullptr);
        REQUIRE(active != nullptr);
        one_shot_id = active->session_id();
        workspace_key = manager.workspace_key();
        CloseRequest close;
        close.reason = "exit";
        NullClearParticipant null_participant;
        REQUIRE(manager.Close(close, &null_participant).error_code.empty());
        workspaces = options.workspaces_root;
    }
    // 索引里认得出 one_shot,exclude_one_shot 滤得掉。
    {
        SessionIndexQuery query;
        query.current_workspace_key = workspace_key;
        const auto page = QueryWorkspaceSessions(workspaces, query);
        REQUIRE(page.total == 1);
        CHECK(page.entries.front().run_kind == "one_shot");
        SessionIndexQuery exclude;
        exclude.current_workspace_key = workspace_key;
        exclude.exclude_one_shot = true;
        CHECK(QueryWorkspaceSessions(workspaces, exclude).total == 0);
    }
    // 新进程的 LatestResumable 不落在 one_shot 上;指名 resume 也明拒。
    {
        SessionManagerOptions options = Opts(root);
        FakeClock clock;
        SessionManager manager(options, &clock);
        CHECK(manager.LatestResumableSessionId().empty());
        ResumeRequest request;
        request.source_session_id = one_shot_id;
        request.interactive = true;
        request.boundary_command.command_id = "cmd-resume-0001";
        request.boundary_command.requested_session_id = one_shot_id;
        request.boundary_command.requested_event_id = "evt";
        request.boundary_command.boundary_operation_id = "op";
        const ResumeOutcome outcome = manager.ResumeAsNew(request);
        CHECK(outcome.error_code == "resume.source_not_resumable");
    }
}

// ---------------------------------------------------------------------------
// 运行中切档即写(单子 §七):UpdateApprovalMode 原子改 session.json 的
// approval_mode;clear 换账继承的是改后的档——用户中途切档,重启/恢复
// 认得出,不再退回起手档。
// ---------------------------------------------------------------------------

TEST_CASE("UpdateApprovalMode: 写盘与内存同拍,clear 继承新档,无活动场明拒") {
    const std::filesystem::path root = MakeRoot("approval-mode-update");
    SessionManagerOptions options = Opts(root);
    options.approval_mode = lubancode::ApprovalMode::Default;
    FakeClock clock;
    SessionManager manager(options, &clock);
    auto* active = manager.LaunchSession().value_or(nullptr);
    REQUIRE(active != nullptr);

    // 起手档按 options 落 manifest。
    {
        const auto manifest = ReadSessionJson(active->session_dir());
        REQUIRE(manifest.has_value());
        REQUIRE(manifest->approval_mode.has_value());
        CHECK(*manifest->approval_mode == lubancode::ApprovalMode::Default);
    }

    // 运行中切到 auto:盘上与内存两本账一起换。
    REQUIRE(manager.UpdateApprovalMode(lubancode::ApprovalMode::Auto).has_value());
    {
        const auto manifest = ReadSessionJson(active->session_dir());
        REQUIRE(manifest.has_value());
        REQUIRE(manifest->approval_mode.has_value());
        CHECK(*manifest->approval_mode == lubancode::ApprovalMode::Auto);
        CHECK(manager.active()->manifest.approval_mode == lubancode::ApprovalMode::Auto);
    }

    // 再切 dont_ask 后 clear:新场继承的是 dont_ask,不是起手 default。
    REQUIRE(manager.UpdateApprovalMode(lubancode::ApprovalMode::DontAsk).has_value());
    ClearRequest clear;
    clear.reason = "user_clear";
    NullClearParticipant null_participant;
    const ClearOutcome outcome = manager.Clear(clear, &null_participant);
    REQUIRE(outcome.error_code.empty());
    {
        const auto new_dir = manager.SessionDirOf(outcome.new_session_id);
        const auto manifest = ReadSessionJson(new_dir);
        REQUIRE(manifest.has_value());
        REQUIRE(manifest->approval_mode.has_value());
        CHECK(*manifest->approval_mode == lubancode::ApprovalMode::DontAsk);
    }

    // 封口后场已不在 running:明拒,不悄悄翻改终态 session.json。
    CloseRequest close;
    close.reason = "exit";
    REQUIRE(manager.Close(close, &null_participant).error_code.empty());
    const auto denied = manager.UpdateApprovalMode(lubancode::ApprovalMode::Yolo);
    REQUIRE_FALSE(denied.has_value());
    const bool refused = denied.error().find("no_active_session") != std::string::npos ||
                         denied.error().find("not_running") != std::string::npos;
    CHECK(refused);
}
