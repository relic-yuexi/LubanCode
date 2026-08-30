// Trajectory 冒烟(P0-1 验收口):一段纯合成事件流从目录到封口全程落盘,
// verify 过、重读 round-trip 字节一致、故意截断尾行明报;外加 v1 golden
// fixture 旧读新写与确定性 hash,以及 workspace/session 目录制。
//
// fixture 再生:设 LUBANCODE_TRAJECTORY_REGEN_FIXTURE=1 跑本册,会用当前
// writer 重写 tests/fixtures/trajectory/v1/ 下的两份文件。平时只读比对。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/canonical_json.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/recorder.hpp"

using namespace lubancode::trajectory;

#ifndef LUBANCODE_SOURCE_DIR
#define LUBANCODE_SOURCE_DIR "."
#endif

namespace {

class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

EventScope MainScope() {
    EventScope scope;
    scope.workspace_key = "demo-000000000000";
    scope.session_id = "20260830-031522-7K4M2P";
    scope.run_id = "main-0001";
    scope.run_kind = RunKind::MainSession;
    scope.visibility = {Visibility::HostOnly};
    return scope;
}

RecordReceipt Put(TrajectoryRecorder& recorder, EventKind kind, EventScope scope,
                  nlohmann::json payload, Durability durability = Durability::ProcessCrash) {
    RecordRequest request;
    request.kind = kind;
    request.scope = std::move(scope);
    request.payload = std::move(payload);
    return recorder.Record(std::move(request), durability);
}

EventScope ScopeOf(std::optional<std::string> turn = std::nullopt,
                   std::optional<std::string> request = std::nullopt,
                   std::optional<std::string> call = std::nullopt) {
    EventScope scope = MainScope();
    scope.turn_id = std::move(turn);
    scope.request_id = std::move(request);
    scope.call_id = std::move(call);
    return scope;
}

// 合成事件流:run -> turn -> input -> 回合一(带一道工具) -> 回合二(纯文本)
// -> verification -> outcome -> turn/run/session 三重封口。
// 时间走 FixedClock,内容纯合成,不含真实路径与密钥。
void WriteGoldenFlow(TrajectoryRecorder& recorder, std::string* last_output_event_id) {
    REQUIRE(recorder
                .WriteRunStarted(nlohmann::json{{"run_kind", "main_session"},
                                                {"start_reason", "process_launch"},
                                                {"writer_version", "trajectory-recorder-v1"},
                                                {"min_reader_version", 1u}},
                         Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);

    EventScope user_turn = ScopeOf("turn-0001");
    user_turn.actor = Actor::User;
    user_turn.origin = Origin::ExternalUser;
    user_turn.visibility = {Visibility::UserVisible, Visibility::ModelInput};
    user_turn.training_policy = TrainingPolicy::Include;
    REQUIRE(Put(recorder, EventKind::TurnStarted, user_turn,
                nlohmann::json{{"trigger", "external_user"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::InputReceived, user_turn,
                nlohmann::json{{"input_id", "input-0001"},
                               {"content", nlohmann::json::array({nlohmann::json{
                                   {"type", "text"}, {"text", "读一下 README 并数行数"}}})},
                               {"channel", "terminal"},
                               {"sender", nlohmann::json{{"kind", "local_user"}}}})
                .status == RecordReceipt::Status::Committed);

    EventScope model_scope = ScopeOf("turn-0001", "req-0001");
    model_scope.actor = Actor::Model;
    model_scope.origin = Origin::ProviderModel;
    model_scope.visibility = {Visibility::ModelOutput, Visibility::UserVisible};
    model_scope.training_policy = TrainingPolicy::Include;
    const auto prepared1 =
        Put(recorder, EventKind::ModelRequestPrepared, model_scope,
            nlohmann::json{{"model", "demo-model"},
                           {"provider", "demo"},
                           {"wire", "responses"},
                           {"message_refs", nlohmann::json::array({"evt-00000003"})},
                           {"parameters", nlohmann::json{{"max_output_tokens", 4096}}},
                           {"cache_epoch", 1u}});
    REQUIRE(prepared1.status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ModelRequestSent,
                ScopeOf("turn-0001", "req-0001"),
                nlohmann::json{{"prepared_event_id", prepared1.event_id}})
                .status == RecordReceipt::Status::Committed);
    const auto output1 = Put(
        recorder, EventKind::ModelOutputCompleted, ScopeOf("turn-0001", "req-0001"),
        nlohmann::json{
            {"output_id", "output-0001"},
            {"blocks",
             nlohmann::json::array(
                 {nlohmann::json{{"type", "text"}, {"text", "我先读文件。"}},
                  nlohmann::json{{"type", "tool_call"},
                                 {"call_id", "call-0001"},
                                 {"provider_call_id", "toolu_demo"},
                                 {"name", "read_file"},
                                 {"arguments", nlohmann::json{{"path", "README.md"}}}}})},
            {"stop_reason", "tool_use"},
            {"usage", nlohmann::json{{"input_tokens", 128}, {"output_tokens", 64}}}});
    REQUIRE(output1.status == RecordReceipt::Status::Committed);
    *last_output_event_id = output1.event_id;

    EventScope tool_scope = ScopeOf("turn-0001", "req-0001", "call-0001");
    tool_scope.actor = Actor::Tool;
    tool_scope.origin = Origin::BuiltinTool;
    tool_scope.visibility = {Visibility::ToolInput, Visibility::ToolOutput};
    tool_scope.training_policy = TrainingPolicy::Include;
    REQUIRE(Put(recorder, EventKind::ToolExecutionPlanned, tool_scope,
                nlohmann::json{{"call_id", "call-0001"},
                               {"tool_name", "read_file"},
                               {"provider_call_id", "toolu_demo"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ToolInputEffective, tool_scope,
                nlohmann::json{{"call_id", "call-0001"},
                               {"tool_name", "read_file"},
                               {"source_kind", "builtin"},
                               {"effect_class", "read_only_local"},
                               {"effective_arguments", nlohmann::json{{"path", "README.md"}}},
                               {"effective_arguments_sha256", std::string(64, '0')},
                               {"rewritten_by", nlohmann::json::array()}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ToolExecutionStarted, tool_scope,
                nlohmann::json{{"call_id", "call-0001"}}, Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    const auto finished =
        Put(recorder, EventKind::ToolExecutionFinished, tool_scope,
            nlohmann::json{{"outcome", "succeeded"},
                           {"duration_ms", 18},
                           {"exit_code", nullptr},
                           {"side_effects", nlohmann::json::array()}},
            Durability::PowerLoss);
    REQUIRE(finished.status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ToolResultCommitted, tool_scope,
                nlohmann::json{{"call_id", "call-0001"},
                               {"content", nlohmann::json::array({nlohmann::json{
                                   {"type", "text"}, {"text", "共 42 行。"}}})},
                               {"is_error", false},
                               {"derived_from_event", finished.event_id}})
                .status == RecordReceipt::Status::Committed);

    const auto prepared2 =
        Put(recorder, EventKind::ModelRequestPrepared,
            ScopeOf("turn-0001", "req-0002"),
            nlohmann::json{{"model", "demo-model"},
                           {"provider", "demo"},
                           {"wire", "responses"},
                           {"message_refs", nlohmann::json::array({"evt-00000003"})},
                           {"cache_epoch", 1u}});
    REQUIRE(prepared2.status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ModelRequestSent, ScopeOf("turn-0001", "req-0002"),
                nlohmann::json{{"prepared_event_id", prepared2.event_id}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::ModelOutputCompleted, ScopeOf("turn-0001", "req-0002"),
                nlohmann::json{{"output_id", "output-0002"},
                               {"blocks", nlohmann::json::array({nlohmann::json{
                                   {"type", "text"}, {"text", "README 共 42 行。"}}})},
                               {"stop_reason", "end_turn"}})
                .status == RecordReceipt::Status::Committed);

    EventScope verifier_scope = ScopeOf("turn-0001");
    verifier_scope.actor = Actor::Verifier;
    verifier_scope.origin = Origin::VerifierHost;
    verifier_scope.visibility = {Visibility::HostOnly, Visibility::UserVisible};
    verifier_scope.training_policy = TrainingPolicy::Metadata;
    REQUIRE(Put(recorder, EventKind::VerificationRecorded, verifier_scope,
                nlohmann::json{{"verification_id", "verify-0001"},
                               {"kind", "manual_review"},
                               {"passed", true},
                               {"producer", "host"},
                               {"observed_after_seq", 14u}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::OutcomeAssessed, ScopeOf("turn-0001"),
                nlohmann::json{{"outcome", "succeeded"},
                               {"evidence_refs", nlohmann::json::array({"verify-0001"})}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(Put(recorder, EventKind::TurnCompleted, ScopeOf("turn-0001"),
                nlohmann::json{{"outcome", "succeeded"}})
                .status == RecordReceipt::Status::Committed);
    REQUIRE(recorder.FinishRun(EventKind::RunCompleted, "done", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    REQUIRE(recorder.EndSession("exit", std::nullopt, "clean", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
}

std::filesystem::path FixtureDir() {
    return std::filesystem::path(LUBANCODE_SOURCE_DIR) / "tests" / "fixtures" / "trajectory" /
           "v1";
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("冒烟: 合成全流可写可验,round-trip 字节一致,截断明报") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-smoke";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    FixedClock clock;
    auto recorder =
        TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts", MainScope(),
                                  RecorderOptions{}, &clock);
    REQUIRE(recorder.has_value());
    std::string last_output_event_id;
    WriteGoldenFlow(*recorder, &last_output_event_id);
    CHECK_FALSE(last_output_event_id.empty());

    // verify:hash chain 全过。
    const auto report = VerifyJournalFile(dir / "main.jsonl");
    REQUIRE(report.ok);
    CHECK(report.error_code.empty());
    CHECK(report.events == 19);
    CHECK(IsHex64(report.first_event_hash));
    CHECK(IsHex64(report.last_event_hash));

    // 重读 round-trip:每行 canonical 重 dump 与原行字节一致。
    const auto lines = ReadJournalLines(dir / "main.jsonl");
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() == 19);
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(parsed.is_discarded());
        const auto canonical = CanonicalJsonDump(parsed);
        REQUIRE(canonical.has_value());
        CHECK(*canonical == line);
        // 旧读:严格解析 + 全量校验过。
        CHECK_FALSE(ParseAndValidateEventLine(parsed).has_value());
    }

    // 关柄后 journal_sha256 可算且非空。
    const auto journal_sha = recorder->Close();
    REQUIRE(journal_sha.has_value());
    CHECK(IsHex64(*journal_sha));

    // 截断明报(§16.3),两种截法各验一遍。
    const auto text = ReadFileText(dir / "main.jsonl");
    REQUIRE(text.has_value());
    REQUIRE(text->size() > 31);
    SUBCASE("只缺尾换行:verify.truncated_tail") {
        const auto truncated = dir / "truncated_nonl.jsonl";
        {
            std::ofstream out(truncated, std::ios::binary | std::ios::trunc);
            out.write(text->data(), static_cast<std::streamsize>(text->size() - 1));
        }
        const auto report = VerifyJournalFile(truncated);
        CHECK_FALSE(report.ok);
        CHECK(report.truncated_tail);
        CHECK(report.error_code == "verify.truncated_tail");
        // 尾行事件本身完整(只缺 '\n'),照常校验过;判 incomplete 靠标记。
        CHECK(report.events == 19);
    }
    SUBCASE("半行截断:verify 明报坏行") {
        const auto truncated = dir / "truncated_half.jsonl";
        {
            std::ofstream out(truncated, std::ios::binary | std::ios::trunc);
            out.write(text->data(), static_cast<std::streamsize>(text->size() - 30));
        }
        const auto report = VerifyJournalFile(truncated);
        CHECK_FALSE(report.ok);
        CHECK(report.error_code == "verify.bad_json");
        // 前面完整事件照常 replay 得到(§16.3)。
        CHECK(report.events == 18);
    }
}

TEST_CASE("golden fixture: 新写字节与 v1 fixture 逐字一致,确定性 hash 不变") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-golden";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    FixedClock clock;
    auto recorder =
        TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts", MainScope(),
                                  RecorderOptions{}, &clock);
    REQUIRE(recorder.has_value());
    std::string last_output_event_id;
    WriteGoldenFlow(*recorder, &last_output_event_id);
    const auto journal_sha = recorder->Close();
    REQUIRE(journal_sha.has_value());

    const auto golden_path = FixtureDir() / "golden_main.jsonl";
    const auto meta_path = FixtureDir() / "golden_main.meta.json";
    const auto written = ReadFileText(dir / "main.jsonl");
    REQUIRE(written.has_value());

    if (std::getenv("LUBANCODE_TRAJECTORY_REGEN_FIXTURE") != nullptr) {
        // 再生模式:用当前 writer 重写 fixture(改 reader/writer 后人工核对再提交)。
        std::filesystem::create_directories(FixtureDir(), ec);
        {
            std::ofstream out(golden_path, std::ios::binary | std::ios::trunc);
            out.write(written->data(), static_cast<std::streamsize>(written->size()));
        }
        nlohmann::json meta = nlohmann::json::object();
        meta["schema"] = "lubancode.trajectory.fixture";
        meta["schema_version"] = 1;
        meta["journal_sha256"] = *journal_sha;
        meta["event_count"] = 19;
        const auto report = VerifyJournalFile(dir / "main.jsonl");
        meta["last_event_hash"] = report.last_event_hash;
        {
            std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
            out << meta.dump();
        }
        return;
    }

    // 旧读:fixture 每行 parse -> canonical 重 dump 与原行一致(§8.4 旧读新写)。
    const auto golden = ReadFileText(golden_path);
    REQUIRE(golden.has_value());
    const auto golden_lines = ReadJournalLines(golden_path);
    REQUIRE(golden_lines.has_value());
    REQUIRE(golden_lines->size() == 19);
    for (const std::string& line : *golden_lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE_FALSE(parsed.is_discarded());
        const auto canonical = CanonicalJsonDump(parsed);
        REQUIRE(canonical.has_value());
        CHECK(*canonical == line);
        // 未知关键事件 fail-closed:严格解析必须过或给出明确错误码,绝不静默。
        CHECK_FALSE(ParseAndValidateEventLine(parsed).has_value());
    }

    // 新写:当前 writer 产出的字节与 fixture 逐字一致(确定性 hash,§8.4)。
    CHECK(*written == *golden);
    // meta:journal_sha256 与末事件 hash 不变。
    const auto meta_text = ReadFileText(meta_path);
    REQUIRE(meta_text.has_value());
    const auto meta = nlohmann::json::parse(*meta_text, nullptr, false);
    REQUIRE_FALSE(meta.is_discarded());
    CHECK(meta.at("journal_sha256").get<std::string>() == *journal_sha);
    const auto report = VerifyJournalFile(golden_path);
    REQUIRE(report.ok);
    CHECK(report.last_event_hash == meta.at("last_event_hash").get<std::string>());
    CHECK(report.events == 19);
}

TEST_CASE("fail-closed: 未知关键事件与坏链都在 verify 里明报") {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode-traj-failclosed";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "bad.jsonl";

    const auto write_line = [&](const nlohmann::json& event) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << event.dump() << "\n";
    };

    SUBCASE("未知事件 kind") {
        auto event = nlohmann::json{{"schema", "lubancode.trajectory.event"},
                                    {"schema_version", 1},
                                    {"workspace_key", "w"},
                                    {"session_id", "s"},
                                    {"run_id", "r"},
                                    {"run_kind", "main_session"},
                                    {"event_id", "r:evt-00000001"},
                                    {"seq", 1},
                                    {"kind", "totally.unknown"},
                                    {"plane", "control"},
                                    {"actor", "host"},
                                    {"origin", "recovery_runtime"},
                                    {"visibility", nlohmann::json::array({"host_only"})},
                                    {"training_policy", "exclude"},
                                    {"wall_time_ms", 0},
                                    {"monotonic_ns", 0},
                                    {"payload", nlohmann::json::object()},
                                    {"prev_hash", std::string(64, '0')},
                                    {"event_hash", std::string(64, '0')}};
        write_line(event);
        const auto report = VerifyJournalFile(path);
        CHECK_FALSE(report.ok);
        CHECK(report.error_code == "schema.unknown_event_kind");
    }
    SUBCASE("seq 跳号") {
        auto event = nlohmann::json{{"schema", "lubancode.trajectory.event"},
                                    {"schema_version", 1},
                                    {"workspace_key", "w"},
                                    {"session_id", "s"},
                                    {"run_id", "r"},
                                    {"run_kind", "main_session"},
                                    {"event_id", "r:evt-00000002"},
                                    {"seq", 2},
                                    {"kind", "run.started"},
                                    {"plane", "control"},
                                    {"actor", "host"},
                                    {"origin", "recovery_runtime"},
                                    {"visibility", nlohmann::json::array({"host_only"})},
                                    {"training_policy", "exclude"},
                                    {"wall_time_ms", 0},
                                    {"monotonic_ns", 0},
                                    {"payload", nlohmann::json{{"run_kind", "main_session"}}},
                                    {"prev_hash", std::string(64, '0')},
                                    {"event_hash", std::string(64, '0')}};
        write_line(event);
        const auto report = VerifyJournalFile(path);
        CHECK_FALSE(report.ok);
        CHECK(report.error_code == "verify.seq_gap");
    }
    SUBCASE("链断:prev_hash 接不上") {
        auto event = nlohmann::json{{"schema", "lubancode.trajectory.event"},
                                    {"schema_version", 1},
                                    {"workspace_key", "w"},
                                    {"session_id", "s"},
                                    {"run_id", "r"},
                                    {"run_kind", "main_session"},
                                    {"event_id", "r:evt-00000001"},
                                    {"seq", 1},
                                    {"kind", "turn.started"},
                                    {"plane", "control"},
                                    {"actor", "user"},
                                    {"origin", "external_user"},
                                    {"visibility", nlohmann::json::array({"host_only"})},
                                    {"training_policy", "exclude"},
                                    {"turn_id", "turn-0001"},
                                    {"wall_time_ms", 0},
                                    {"monotonic_ns", 0},
                                    {"payload", nlohmann::json{{"trigger", "external_user"}}},
                                    {"prev_hash", std::string(64, '1')},  // 不是链头
                                    {"event_hash", std::string(64, '0')}};
        write_line(event);
        const auto report = VerifyJournalFile(path);
        CHECK_FALSE(report.ok);
        CHECK(report.error_code == "verify.chain_broken");
    }
}

TEST_CASE("目录制: workspace_key、session 树、session.json 原子写、占位流") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-traj-dir";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    SUBCASE("workspace_key 规则(§3.2)") {
        const auto key = ComputeWorkspaceKey(std::filesystem::path("D:/work/demo"));
        // <basename>-<12 hex>。
        CHECK(key.substr(0, 5) == "demo-");
        CHECK(key.size() == 5 + 12);
        // 同一路径两种写法归一(正斜杠/反斜杠、尾斜杠)。
        CHECK(ComputeWorkspaceKey(std::filesystem::path("D:\\work\\demo\\")) == key);
#ifdef _WIN32
        // Windows 大小写不敏感:整串折叠小写后同一仓库同一 key。
        CHECK(ComputeWorkspaceKey(std::filesystem::path("d:/Work/demo")) == key);
#else
        // POSIX 大小写敏感:Work 与 work 是两个不同根,各立各的。
        CHECK(ComputeWorkspaceKey(std::filesystem::path("d:/Work/demo")) != key);
#endif
        CHECK(ComputeWorkspaceKey(std::filesystem::path("D:/work/other")) != key);
#ifdef _WIN32
        CHECK(NormalizeRootPathText(std::filesystem::path("D:\\a\\/b\\\\")) == "d:/a/b");
#endif
    }
    SUBCASE("FindWorkspaceRoot 向上找 .git") {
        std::filesystem::create_directories(root / "proj" / "sub" / "deep", ec);
        std::filesystem::create_directories(root / "proj" / ".git", ec);
        const auto found = FindWorkspaceRoot(root / "proj" / "sub" / "deep");
        REQUIRE(found.has_value());
        CHECK(*found == std::filesystem::absolute(root / "proj"));
        // 无 .git 的分支:找到的必是含 .git 的祖先(temp 祖先链罕见地带着
        // .git 时,这一条仍须成立)。
        const auto missing = FindWorkspaceRoot(root / "nowhere");
        if (missing.has_value()) {
            CHECK(std::filesystem::exists(*missing / ".git"));
        }
    }
    SUBCASE("session_id 形状") {
        const auto id = GenerateSessionId(2026, 8, 30, 3, 15, 22, "7K4M2P");
        CHECK(id == "20260830-031522-7K4M2P");
    }

    SUBCASE("workspace 与 session 目录树(§3.1)") {
        auto workspace = TrajectoryDirectory::CreateWorkspace(root, root / "proj", "demo", 1000);
        REQUIRE(workspace.has_value());
        const auto key = ComputeWorkspaceKey(root / "proj");
        CHECK(std::filesystem::exists(root / "workspaces" / key / "workspace.json"));
        CHECK(std::filesystem::exists(root / "workspaces" / key / "sessions"));
        CHECK(std::filesystem::exists(root / "workspaces" / key / "lifecycle"));
        CHECK(std::filesystem::exists(root / "workspaces" / key / "tombstones"));
        // 二次创建不覆盖(首次创建时间等材料以旧账为准)。
        auto again = TrajectoryDirectory::CreateWorkspace(root, root / "proj", "demo-2", 2000);
        REQUIRE(again.has_value());

        SessionManifest manifest;
        manifest.workspace_key = key;
        manifest.session_id = "20260830-031522-7K4M2P";
        manifest.launch_cwd = "D:/proj";
        manifest.main_run_id = "main-0001";
        manifest.status = "preparing";
        manifest.created_at_ms = 1759000000000LL;
        manifest.lubancode_version = "0.0.0-test";
        auto session = TrajectoryDirectory::CreateSession(root / "workspaces", key, manifest);
        REQUIRE(session.has_value());
        const auto session_dir = session->session_dir();
        // §3.1 全目录树。
        for (const char* leaf : {"main.jsonl"}) {
            CHECK_FALSE(std::filesystem::exists(session_dir / leaf));  // 只占目录,不建文件
        }
        for (const char* sub :
             {"subagents", "workflows", "goals", "loops", "checkpoints", "indexes",
              "artifacts/sha256", "derived/records", "exports/training-v1"}) {
            CHECK(std::filesystem::is_directory(session_dir / sub));
        }
        // session.json 读回。
        const auto back = ReadSessionJson(session_dir);
        REQUIRE(back.has_value());
        CHECK(back->session_id == manifest.session_id);
        CHECK(back->status == "preparing");
        CHECK(back->workspace_key == key);
        CHECK(back->previous_session_id == std::nullopt);
        // 状态推进:preparing -> running 原子改写。
        SessionManifest running = *back;
        running.status = "running";
        REQUIRE(WriteSessionJsonAtomic(session_dir, running));
        const auto reread = ReadSessionJson(session_dir);
        REQUIRE(reread.has_value());
        CHECK(reread->status == "running");
        // 同 session_id 再建即失败(绝不复用)。
        auto duplicate = TrajectoryDirectory::CreateSession(root / "workspaces", key, manifest);
        CHECK_FALSE(duplicate.has_value());

        // 占位流:main / subagent / workflow / node。
        auto main_path = session->ReserveMainStream();
        REQUIRE(main_path.has_value());
        CHECK(*main_path == session_dir / "main.jsonl");
        auto sub_path = session->ReserveSubagentStream("agent-0002");
        REQUIRE(sub_path.has_value());
        CHECK(*sub_path == session_dir / "subagents" / "agent-0002.jsonl");
        auto wf_path = session->ReserveWorkflowRun("wf-0001");
        REQUIRE(wf_path.has_value());
        CHECK(*wf_path == session_dir / "workflows" / "wf-0001" / "workflow.jsonl");
        CHECK(std::filesystem::is_directory(session_dir / "workflows" / "wf-0001" / "nodes"));
        auto node_path = session->ReserveWorkflowNodeStream("wf-0001", "node-a1");
        REQUIRE(node_path.has_value());
        CHECK(*node_path ==
              session_dir / "workflows" / "wf-0001" / "nodes" / "node-a1.jsonl");
        // 非法单段名拒绝(路径逃逸材料)。
        CHECK_FALSE(session->ReserveSubagentStream("../escape").has_value());
        CHECK_FALSE(session->ReserveSubagentStream("a/b").has_value());
    }
}

TEST_CASE("占位流上起 recorder,子流独立收口") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-traj-sub";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto key = "demo-000000000000";
    SessionManifest manifest;
    manifest.workspace_key = key;
    manifest.session_id = "20260830-031522-7K4M2P";
    manifest.main_run_id = "main-0001";
    manifest.status = "running";
    auto session = TrajectoryDirectory::CreateSession(root / "workspaces", key, manifest);
    REQUIRE(session.has_value());

    // 子代理流:Reserve 之后 recorder 才进来(create-new,§3.10)。
    auto sub_path = session->ReserveSubagentStream("agent-0002");
    REQUIRE(sub_path.has_value());
    EventScope sub_scope;
    sub_scope.workspace_key = key;
    sub_scope.session_id = manifest.session_id;
    sub_scope.run_id = "agent-0002";
    sub_scope.run_kind = RunKind::Subagent;
    sub_scope.visibility = {Visibility::HostOnly};
    FixedClock clock;
    auto sub = TrajectoryRecorder::Start(*sub_path, session->artifacts_root(), sub_scope,
                                         RecorderOptions{}, &clock);
    REQUIRE(sub.has_value());
    REQUIRE(sub->WriteRunStarted(nlohmann::json{{"run_kind", "subagent"},
                                                {"agent_run_id", "agent-0002"},
                                                {"owner_run_id", "main-0001"},
                                                {"spawn_event_id", "main-0001:evt-00000001"}},
                                 Durability::PowerLoss)
                .status == RecordReceipt::Status::Committed);
    REQUIRE(sub->FinishRun(EventKind::RunCompleted, "task done", Durability::PowerLoss).status ==
            RecordReceipt::Status::Committed);
    const auto report = VerifyJournalFile(*sub_path);
    REQUIRE(report.ok);
    CHECK(report.events == 2);
    // 同一文件二次 CreateNew 占位必须失败:单写者从目录层就是排他的。
    auto again = JournalWriter::Open(*sub_path, JournalWriter::OpenMode::CreateNew);
    CHECK_FALSE(again.has_value());
}
