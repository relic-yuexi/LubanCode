// SessionRuntime 单测(显示系统剥离单第六步:拆 SessionRuntime)。
//
// P0-2(Trajectory 升为唯一 Session):旧 SessionStore 建档/轮末补抄路停用
// ——EnsureBegun 恒 Disabled、PersistNew 恒 Nothing,会话真账在 ctor 里恒开
// 的 TrajectorySessionLedger。旧路的建档语义(首句 slug、标题补行、增量
// 落盘、broken 账)随旧档退役,由 P0-5 迁移器/P0-6 删码收口;这里钉的是
// 新语义 + 与旧 Store 无关的部分:
//   1. ledger 恒开:临时根下出 workspace/session 目录与 main.jsonl;
//   2. EnsureBegun/PersistNew 的退役语义(Disabled/Nothing);
//   3. 权限账与 thread 身份:IdAuthority 发号、always_allowed 直通;
//   4. MakeTurnAdapter:同一 thread_id、同一发号局,事件落 AttachSink。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/session_runtime.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/v3/reader.hpp"
#include "tools/registry.hpp"
#include "workspace/identity.hpp"

namespace rt = lubancode::runtime;
using namespace lubancode;

namespace {

// 临时会话目录 RAII:先关柄再删、remove_all 用 error_code 形态。
class TempSessionsDir {
public:
    TempSessionsDir() {
        path_ = (std::filesystem::temp_directory_path() /
                 ("lubancode-session-runtime-" + std::to_string(counter_++)))
                    .string();
        std::filesystem::create_directories(path_);
    }
    ~TempSessionsDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }

private:
    static inline int counter_ = 0;
    std::string path_;
};

class RecordingSink final : public rt::EventSink {
public:
    void Emit(const rt::ServerEvent& event) override { events.push_back(event); }
    std::vector<rt::ServerEvent> events;
};

rt::TrajectorySessionLedger::Options OpeningOptions(const TempSessionsDir& dir) {
    rt::TrajectorySessionLedger::Options options;
    options.workspaces_root = std::filesystem::path(dir.path()) / "workspaces";
    options.workspace_root = std::filesystem::path(dir.path()) / "repo";
    std::filesystem::create_directories(options.workspace_root);
    options.workspace_identity = workspace::MakeFallbackIdentity(options.workspace_root);
    options.launch_cwd = options.workspace_root.generic_string();
    options.lubancode_version = "opening-test";
    return options;
}

nlohmann::json OpeningBindings() {
    return {{"skills", {{"schemaVersion", 1}, {"sha256", "frozen-plan"}}}};
}

trajectory::V3OpeningParticipant MatchingOpeningParticipant() {
    return [](const trajectory::V3OpeningContext&) -> std::expected<nlohmann::json, std::string> {
        return nlohmann::json{{"hostBindings", OpeningBindings()}};
    };
}

struct OpeningSource {
    std::string session_id;
    std::filesystem::path stream;
    std::string effective_root;
    std::uint64_t revision = 0;
};

OpeningSource MakeOpeningSource(const TempSessionsDir& dir, std::uint64_t settings_version = 7) {
    auto options = OpeningOptions(dir);
    options.v3_system_content = "initial base";
    options.v3_opening_participant = MatchingOpeningParticipant();
    auto source = rt::TrajectorySessionLedger::Open(options);
    REQUIRE(source.has_value());
    auto* writer = source->v3_main_writer();
    REQUIRE(writer != nullptr);
    const auto switched = writer->SwitchSystem("saved full system",
        {{"cause", "system_prompt_changed"}, {"settingsVersion", settings_version}, {"systemChanged", true}});
    REQUIRE(switched.apply_event.status == trajectory::v3::WriteReceipt::Status::Committed);
    OpeningSource saved{source->session_id(), writer->path(),
        writer->context().system_message_ref, writer->context().revision};
    REQUIRE(source->CloseSession("exit").error_code.empty());
    return saved;
}

}  // namespace

TEST_CASE("SessionRuntime:账本恒开,workspace/session 目录在临时根下") {
    TempSessionsDir dir;
    rt::SessionRuntime::Options options;
    options.trajectory_workspaces_root = dir.path() + "/workspaces";
    std::error_code ec;
    std::filesystem::create_directories(dir.path() + "/repo", ec);
    options.trajectory_workspace_identity = workspace::MakeFallbackIdentity(
        std::filesystem::path(dir.path()) / "repo");
    options.lubancode_version = "test";
    rt::SessionRuntime runtime(std::move(options));
    REQUIRE(runtime.trajectory() != nullptr);
    // V3-LEGACY-01 后新建唯一 v3:主账是 <id>.jsonl,v2 文件一枚不长。
    CHECK(std::filesystem::exists(
        runtime.trajectory()->session_dir() /
        std::filesystem::path(runtime.trajectory()->session_id() + ".jsonl")));
    CHECK_FALSE(std::filesystem::exists(runtime.trajectory()->session_dir() / "main.jsonl"));
    CHECK_FALSE(runtime.trajectory()->session_id().empty());
    CHECK(runtime.trajectory_open_error().empty());
}

// (P0-6:旧档建档/轮末补抄的退役语义用例已删——EnsureBegun/PersistNew
// 本体随 SessionStore 删除,新语义是"这些方法不存在"。)

TEST_CASE("SessionRuntime:thread 身份、发号局与权限账") {
    rt::SessionRuntime runtime({"anthropic", "ts"});
    CHECK(runtime.thread_id().rfind("thread-", 0) == 0);
    const std::uint64_t before = runtime.ids().items_issued();
    (void)runtime.ids().NextItemId();
    CHECK(runtime.ids().items_issued() == before + 1);

    runtime.always_allowed().insert("read_file");
    CHECK(runtime.always_allowed().count("read_file") == 1);
}

TEST_CASE("SessionRuntime:MakeTurnAdapter 共用 thread_id 与发号局,事件落挂的 sink") {
    rt::SessionRuntime runtime({"anthropic", "ts"});
    RecordingSink sink;
    runtime.AttachSink(&sink);

    auto adapter = runtime.MakeTurnAdapter();
    adapter.Attach([&](const rt::ServerEvent& event) { sink.Emit(event); });
    const std::string turn_id = adapter.Start();
    CHECK(turn_id.rfind("turn-", 0) == 0);
    // Finish 走 sink:TurnCompleted 一枚,seq 单调。
    adapter.Finish(rt::Outcome::Succeeded);
    REQUIRE(sink.events.size() == 2);
    CHECK(sink.events[0].kind == rt::ServerEventKind::TurnStarted);
    CHECK(sink.events[0].envelope.thread_id == runtime.thread_id());
    CHECK(sink.events[0].turn_id == turn_id);
    CHECK(sink.events[1].kind == rt::ServerEventKind::TurnCompleted);
    CHECK(sink.events[1].envelope.seq > sink.events[0].envelope.seq);
}

TEST_CASE("TrajectorySessionLedger: strict empty resume keeps the effective root") {
    TempSessionsDir dir;
    const auto saved = MakeOpeningSource(dir);
    const auto before = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(before.has_value());
    auto options = OpeningOptions(dir);
    options.resume_at_launch = true;
    options.require_v3_resume = true;
    options.resume_source_session_id = saved.session_id;
    options.v3_opening_participant = MatchingOpeningParticipant();
    auto resumed = rt::TrajectorySessionLedger::Open(options);
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->v3_main_writer() != nullptr);
    CHECK(resumed->session_id() == saved.session_id);
    CHECK(resumed->v3_main_writer()->context().system_message_ref == saved.effective_root);
    CHECK(resumed->v3_main_writer()->context().revision == saved.revision);
    const auto after = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(after.has_value());
    REQUIRE(after->size() == before->size() + 1);
    CHECK(std::equal(before->begin(), before->end(), after->begin()));
    CHECK(nlohmann::json::parse(after->back())["kind"] == "approval.mode.applied");
    const auto ledger = trajectory::v3::ReadV3Ledger(saved.stream);
    REQUIRE(ledger.has_value());
    CHECK(trajectory::v3::ProjectModelContext(*ledger).system_content == "saved full system");
}

TEST_CASE("TrajectorySessionLedger: explicit replacement continues the saved settings version") {
    TempSessionsDir dir;
    std::uint64_t version = 7;
    SUBCASE("ordinary version") { version = 7; }
    SUBCASE("version exceeds the old int-sized metadata conversion") { version = 4294967296ULL; }
    const auto saved = MakeOpeningSource(dir, version);
    const auto before = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(before.has_value());
    auto options = OpeningOptions(dir);
    options.resume_at_launch = true;
    options.require_v3_resume = true;
    options.resume_source_session_id = saved.session_id;
    options.v3_system_content = "explicit replacement";
    options.v3_opening_participant = MatchingOpeningParticipant();
    auto resumed = rt::TrajectorySessionLedger::Open(options);
    REQUIRE(resumed.has_value());
    const auto after = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(after.has_value());
    REQUIRE(after->size() == before->size() + 4);
    CHECK(std::equal(before->begin(), before->end(), after->begin()));
    const auto change = nlohmann::json::parse((*after)[before->size() + 1]);
    const auto system = nlohmann::json::parse((*after)[before->size() + 2]);
    const auto applied = nlohmann::json::parse((*after)[before->size() + 3]);
    CHECK(change["kind"] == "system.change");
    CHECK(change["payload"]["oldSystemMessageRef"] == saved.effective_root);
    CHECK(system["message"]["content"] == "explicit replacement");
    CHECK(system["systemMeta"]["settingsVersion"] == version + 1);
    CHECK(system["systemMeta"]["hostBindings"] == OpeningBindings());
    CHECK(applied["kind"] == "context.system.applied");
    CHECK(resumed->v3_main_writer()->context().revision == saved.revision + 1);
}

TEST_CASE("TrajectorySessionLedger: each explicit system transition failure refuses opening") {
    TempSessionsDir dir;
    const auto saved = MakeOpeningSource(dir);
    const auto before = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(before.has_value());
    int fail_commit = 2;  // approval recompute is commit 1
    SUBCASE("system.change fails") { fail_commit = 2; }
    SUBCASE("new system message fails") { fail_commit = 3; }
    SUBCASE("context.system.applied fails") { fail_commit = 4; }
    int commits = 0;
    auto options = OpeningOptions(dir);
    options.resume_at_launch = true;
    options.require_v3_resume = true;
    options.resume_source_session_id = saved.session_id;
    options.v3_system_content = "must not become a successful opening";
    options.v3_opening_participant = MatchingOpeningParticipant();
    options.v3_main_io_fault = [&]() -> std::optional<std::string> {
        return ++commits == fail_commit ? std::optional<std::string>("test.opening_write_failed") : std::nullopt;
    };
    const auto resumed = rt::TrajectorySessionLedger::Open(options);
    CHECK_FALSE(resumed.has_value());
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().find("resume.system_adoption_failed:") == 0);
    CHECK(commits == fail_commit);
    CHECK_FALSE(trajectory::SessionLock::Inspect(saved.stream.parent_path()).has_value());
    const auto after = trajectory::ReadJournalLines(saved.stream);
    REQUIRE(after.has_value());
    REQUIRE(after->size() == before->size() + static_cast<std::size_t>(fail_commit - 1));
    CHECK(std::equal(before->begin(), before->end(), after->begin()));
    const auto ledger = trajectory::v3::ReadV3Ledger(saved.stream);
    REQUIRE(ledger.has_value());
    CHECK(ledger->context.system_message_ref == saved.effective_root);
    CHECK(ledger->context.revision == saved.revision);
    // The failed writer and lock must be released on every return path.
    auto writer = trajectory::v3::V3Writer::Continue(saved.stream);
    REQUIRE(writer.has_value());
    CHECK(writer->context().system_message_ref == saved.effective_root);
}

TEST_CASE("TrajectorySessionLedger: legacy roots default settingsVersion to one but reject bad values") {
    TempSessionsDir dir;
    auto options = OpeningOptions(dir);
    auto bootstrap = rt::TrajectorySessionLedger::Open(options);
    REQUIRE(bootstrap.has_value());
    const auto sessions = bootstrap->session_dir().parent_path();
    REQUIRE(bootstrap->CloseSession("exit").error_code.empty());
    const std::string source_id = "20260910-120000-LEGACY";
    const auto source_dir = sessions / source_id;
    std::filesystem::create_directories(source_dir);
    const auto stream = source_dir / (source_id + ".jsonl");
    nlohmann::json extra = nlohmann::json::object();
    bool invalid = false;
    SUBCASE("missing version uses one") {}
    SUBCASE("zero version is rejected") { extra["settingsVersion"] = 0; invalid = true; }
    SUBCASE("negative version is rejected") { extra["settingsVersion"] = -1; invalid = true; }
    SUBCASE("string version is rejected") { extra["settingsVersion"] = "1"; invalid = true; }
    SUBCASE("fractional version is rejected") { extra["settingsVersion"] = 1.5; invalid = true; }
    auto writer = trajectory::v3::V3Writer::Start(stream, source_id, "run-000001", "legacy", extra);
    REQUIRE(writer.has_value());
    REQUIRE(writer->Close().has_value());
    const auto before = trajectory::ReadJournalLines(stream);
    REQUIRE(before.has_value());
    int calls = 0;
    options.resume_at_launch = true;
    options.require_v3_resume = true;
    options.resume_source_session_id = source_id;
    options.v3_system_content = "explicit legacy replacement";
    options.v3_opening_participant = [&](const trajectory::V3OpeningContext&)
        -> std::expected<nlohmann::json, std::string> { ++calls; return nlohmann::json::object(); };
    auto resumed = rt::TrajectorySessionLedger::Open(options);
    if (invalid) {
        CHECK_FALSE(resumed.has_value());
        REQUIRE_FALSE(resumed.has_value());
        CHECK(resumed.error().find("opening.invalid_settings_version") != std::string::npos);
        CHECK(calls == 0);
        CHECK(trajectory::ReadJournalLines(stream).value() == *before);
    } else {
        REQUIRE(resumed.has_value());
        CHECK(calls == 1);
        const auto ledger = trajectory::v3::ReadV3Ledger(stream);
        REQUIRE(ledger.has_value());
        const auto* effective = ledger->FindMessage(ledger->context.system_message_ref);
        REQUIRE(effective != nullptr);
        REQUIRE(effective->system_meta.has_value());
        CHECK(effective->system_meta->at("settingsVersion") == 2);
        CHECK_FALSE(effective->system_meta->contains("hostBindings"));
    }
}

TEST_CASE("TrajectorySessionLedger: a verified nontext or nonsystem effective root refuses the gate") {
    TempSessionsDir dir;
    auto options = OpeningOptions(dir);
    auto bootstrap = rt::TrajectorySessionLedger::Open(options);
    REQUIRE(bootstrap.has_value());
    const auto sessions = bootstrap->session_dir().parent_path();
    REQUIRE(bootstrap->CloseSession("exit").error_code.empty());
    const std::string source_id = "20260910-120000-BADROOT";
    const auto source_dir = sessions / source_id;
    std::filesystem::create_directories(source_dir);
    const auto stream = source_dir / (source_id + ".jsonl");
    auto writer = trajectory::v3::V3Writer::Start(stream, source_id, "run-000001", "initial");
    REQUIRE(writer.has_value());
    trajectory::v3::MessageDraft bad_root;
    bad_root.origin = trajectory::v3::MessageOrigin::SessionRuntime;
    bad_root.message = {{"role", "system"}, {"content", "bad root"}};
    bad_root.system_meta = nlohmann::json{{"settingsVersion", 1}};
    SUBCASE("root role is user") {
        bad_root.message["role"] = "user";
        bad_root.turn_id = "turn-000001";
    }
    SUBCASE("root content is an array") { bad_root.message["content"] = nlohmann::json::array(); }
    SUBCASE("root content is absent") { bad_root.message.erase("content"); }
    const auto message = writer->AppendMessage(std::move(bad_root), trajectory::Durability::PowerLoss);
    REQUIRE(message.status == trajectory::v3::WriteReceipt::Status::Committed);
    trajectory::v3::EventDraft apply;
    apply.kind = trajectory::v3::EventKindV3::ContextSystemApplied;
    apply.payload = {{"contextId", "main"}, {"beforeRevision", 1}, {"afterRevision", 2},
        {"rootMessageRef", message.id}, {"contextChain", nlohmann::json::array({
            nlohmann::json{{"messageRef", message.id}, {"prevMessageRef", nullptr}}})}};
    REQUIRE(writer->AppendEvent(std::move(apply), trajectory::Durability::PowerLoss).status ==
        trajectory::v3::WriteReceipt::Status::Committed);
    REQUIRE(writer->Close().has_value());
    REQUIRE(trajectory::v3::VerifyV3File(stream).ok);
    const auto before = trajectory::ReadJournalLines(stream);
    REQUIRE(before.has_value());
    int calls = 0;
    options.resume_at_launch = true;
    options.require_v3_resume = true;
    options.resume_source_session_id = source_id;
    options.v3_opening_participant = [&](const trajectory::V3OpeningContext&)
        -> std::expected<nlohmann::json, std::string> { ++calls; return nlohmann::json::object(); };
    const auto resumed = rt::TrajectorySessionLedger::Open(options);
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().find("opening.effective_system_missing") != std::string::npos);
    CHECK(calls == 0);
    CHECK(trajectory::ReadJournalLines(stream).value() == *before);
    CHECK_FALSE(trajectory::SessionLock::Inspect(source_dir).has_value());
}
