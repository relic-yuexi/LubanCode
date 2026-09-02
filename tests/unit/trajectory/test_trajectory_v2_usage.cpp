// Trajectory v2 usage owner 合同(Token 账本单 §6.1.1 A0 冻结):
//   - v2 加 model.usage.recorded,一 request attempt 一 owner,重复拒;
//   - v2 completed 不带 usage;v1 stream 拒收 v2 事件;
//   - reported_by_provider 与 token 五项的条件硬约束;
//   - 一条 stream 不混 v1/v2(verify 拒);
//   - session manifest 钉 event schema major。
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/canonical_json.hpp"
#include "trajectory/directory.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/schema.hpp"

using namespace lubancode::trajectory;

namespace {

class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

struct Harness {
    FixedClock clock;
    std::filesystem::path dir;
    std::optional<TrajectoryRecorder> recorder;

    explicit Harness(const char* tag, int schema_version) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-traj-v2-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "artifacts", ec);
        RecorderOptions options;
        options.event_schema_version = schema_version;
        auto started = TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts",
                                                 BaseScope(), options, &clock);
        REQUIRE(started.has_value());
        recorder = std::move(*started);
    }

    static EventScope BaseScope() {
        EventScope scope;
        scope.workspace_key = "ws-000000000000";
        scope.session_id = "20260830-031522-7K4M2P";
        scope.run_id = "main-0001";
        scope.run_kind = RunKind::MainSession;
        scope.visibility = {Visibility::HostOnly};
        return scope;
    }

    static EventScope TurnScope(std::string turn, std::optional<std::string> request = std::nullopt) {
        EventScope scope = BaseScope();
        scope.turn_id = std::move(turn);
        scope.request_id = std::move(request);
        scope.actor = Actor::Model;
        scope.origin = Origin::ProviderModel;
        return scope;
    }

    RecordReceipt Put(EventKind kind, EventScope scope, nlohmann::json payload) {
        RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.payload = std::move(payload);
        return recorder->Record(std::move(request), Durability::ProcessCrash);
    }

    // 开一场带一枚完整请求的 v2 流(usage 已记)。
    void OpenTurnWithRequest(const std::string& turn, const std::string& request_id,
                             bool with_usage_owner = true,
                             bool reported = true) {
        REQUIRE(recorder->WriteRunStarted(nlohmann::json::object(), Durability::PowerLoss).status ==
                RecordReceipt::Status::Committed);
        REQUIRE(Put(EventKind::TurnStarted, TurnScope(turn), nlohmann::json{{"trigger", "external_user"}})
                    .status == RecordReceipt::Status::Committed);
        EventScope input_scope = TurnScope(turn);
        input_scope.actor = Actor::User;
        input_scope.origin = Origin::ExternalUser;
        input_scope.visibility = {Visibility::UserVisible, Visibility::ModelInput};
        REQUIRE(Put(EventKind::InputReceived, input_scope,
                    nlohmann::json{{"input_id", "input-0001"},
                                   {"content", nlohmann::json::array({"text"})},
                                   {"channel", "terminal"},
                                   {"sender", nlohmann::json{{"kind", "local_user"}}}})
                    .status == RecordReceipt::Status::Committed);
        const auto prepared =
            Put(EventKind::ModelRequestPrepared, TurnScope(turn, request_id),
                nlohmann::json{{"model", "m"},
                               {"provider", "p"},
                               {"wire", "responses"},
                               {"message_refs", nlohmann::json::array()},
                               {"purpose", "main_turn"}});
        REQUIRE(prepared.status == RecordReceipt::Status::Committed);
        REQUIRE(Put(EventKind::ModelRequestSent, TurnScope(turn, request_id),
                    nlohmann::json{{"prepared_event_id", prepared.event_id}})
                    .status == RecordReceipt::Status::Committed);
        if (with_usage_owner) {
            nlohmann::json usage{{"attempt", std::uint64_t{1}}, {"reported_by_provider", reported}};
            if (reported) {
                usage["input_tokens"] = 100;
                usage["cache_read_tokens"] = 0;
                usage["cache_creation_tokens"] = 0;
                usage["output_tokens"] = 40;
                usage["reasoning_tokens"] = 10;
            }
            REQUIRE(Put(EventKind::ModelUsageRecorded, TurnScope(turn, request_id), std::move(usage))
                        .status == RecordReceipt::Status::Committed);
        }
    }
};

const char* CodeOf(const RecordReceipt& receipt) { return receipt.error_code.c_str(); }

}  // namespace

TEST_CASE("v2 recorder 落 model.usage.recorded,一 attempt 一 owner") {
    Harness h("owner", 2);
    h.OpenTurnWithRequest("turn-0001", "req-0001");
    // 同 request 同 attempt 的第二条 owner:拒。
    nlohmann::json duplicate{{"attempt", std::uint64_t{1}}, {"reported_by_provider", false}};
    auto receipt = h.Put(EventKind::ModelUsageRecorded, Harness::TurnScope("turn-0001", "req-0001"),
                         std::move(duplicate));
    REQUIRE(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.usage_owner_duplicate");
    // 同 request 第二 attempt 的 owner:收(重试各记各的)。
    nlohmann::json attempt2{{"attempt", std::uint64_t{2}}, {"reported_by_provider", false}};
    receipt = h.Put(EventKind::ModelUsageRecorded, Harness::TurnScope("turn-0001", "req-0001"),
                    std::move(attempt2));
    CHECK(receipt.status == RecordReceipt::Status::Committed);
}

TEST_CASE("v2 usage owner 须引用已发送 request") {
    Harness h("notsent", 2);
    h.OpenTurnWithRequest("turn-0001", "req-0001");
    nlohmann::json usage{{"attempt", std::uint64_t{1}}, {"reported_by_provider", false}};
    auto receipt = h.Put(EventKind::ModelUsageRecorded, Harness::TurnScope("turn-0001", "req-9999"),
                         std::move(usage));
    REQUIRE(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "state.request_not_sent");
}

TEST_CASE("v1 stream 拒收 model.usage.recorded;v2 completed 拒 usage 键") {
    Harness v1("v1reject", 1);
    v1.OpenTurnWithRequest("turn-0001", "req-0001", /*with_usage_owner=*/false);
    nlohmann::json usage{{"attempt", std::uint64_t{1}}, {"reported_by_provider", false}};
    auto receipt = v1.Put(EventKind::ModelUsageRecorded, Harness::TurnScope("turn-0001", "req-0001"),
                          std::move(usage));
    REQUIRE(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "schema.kind_not_in_version");

    Harness v2("v2completed", 2);
    v2.OpenTurnWithRequest("turn-0001", "req-0001");
    nlohmann::json completed{{"output_id", "output-1"},
                             {"blocks", nlohmann::json::array()},
                             {"stop_reason", "end_turn"},
                             {"usage", nlohmann::json::object()}};
    receipt = v2.Put(EventKind::ModelOutputCompleted, Harness::TurnScope("turn-0001", "req-0001"),
                     std::move(completed));
    REQUIRE(receipt.status == RecordReceipt::Status::Rejected);
    CHECK(std::string(CodeOf(receipt)) == "schema.payload_unknown_field");
}

TEST_CASE("v2 usage payload 条件硬约束") {
    const nlohmann::json base{{"attempt", std::uint64_t{1}}, {"reported_by_provider", true}};
    // 明报缺 token 字段:拒。
    auto error = ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, base);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_missing_field");
    // 没报却带 token:拒。
    nlohmann::json silent = base;
    silent["reported_by_provider"] = false;
    silent["input_tokens"] = 10;
    error = ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, silent);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_forbidden_field");
    // reasoning > output:拒。
    nlohmann::json bad_reasoning = base;
    bad_reasoning["input_tokens"] = 100;
    bad_reasoning["cache_read_tokens"] = 0;
    bad_reasoning["cache_creation_tokens"] = 0;
    bad_reasoning["output_tokens"] = 10;
    bad_reasoning["reasoning_tokens"] = 20;
    error = ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, bad_reasoning);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.usage_reasoning_exceeds_output");
    // 负 token:拒。
    nlohmann::json negative = bad_reasoning;
    negative["reasoning_tokens"] = 5;
    negative["input_tokens"] = -1;
    error = ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, negative);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.usage_negative_tokens");
    // 全字段的合法 owner:过。
    nlohmann::json good = bad_reasoning;
    good["reasoning_tokens"] = 5;
    CHECK(!ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, good).has_value());
    // 明报全零:合法(provider 真回了全零)。
    nlohmann::json zero = good;
    zero["input_tokens"] = 0;
    zero["output_tokens"] = 0;
    zero["reasoning_tokens"] = 0;
    CHECK(!ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, zero).has_value());
    // 没报且不带 token:合法,这是 unknown owner 的形状。
    nlohmann::json unknown{{"attempt", std::uint64_t{1}}, {"reported_by_provider", false}};
    CHECK(!ValidatePayloadWithVersion(2, EventKind::ModelUsageRecorded, unknown).has_value());
}

TEST_CASE("一条 stream 不混 v1/v2:verify 整本拒") {
    Harness h("mixed", 2);
    h.OpenTurnWithRequest("turn-0001", "req-0001");
    // 手工在 v2 流尾补一枚 v1 信封:kind 用 v1/v2 都合法的 turn.completed,
    // 让"版本混写"成为唯一失败点(用 v2 独有 kind 会在逐行裁里先撞
    // kind_not_in_version,那也是正确拒绝,只是测不到这一层)。
    const auto lines = ReadJournalLines(h.dir / "main.jsonl").value();
    REQUIRE(lines.size() >= 2);
    const auto last = nlohmann::json::parse(lines.back());
    nlohmann::json v1_line = last;
    v1_line["schema_version"] = 1;
    v1_line["kind"] = "turn.completed";
    v1_line["plane"] = "control";
    v1_line["payload"] = nlohmann::json{{"outcome", "passed"}};
    // 重算链以通过机械校验,让"版本混写"成为唯一失败点。
    v1_line["prev_hash"] = last.at("event_hash");
    v1_line["seq"] = last.at("seq").get<std::uint64_t>() + 1;
    v1_line["event_id"] = FormatEventId("main-0001", v1_line["seq"].get<std::uint64_t>());
    v1_line.erase("event_hash");
    const std::string recomputed = ComputeEventHash(
        v1_line.at("prev_hash").get<std::string>(),
        CanonicalJsonDump(v1_line).value());
    v1_line["event_hash"] = recomputed;
    const std::filesystem::path mixed = h.dir / "mixed.jsonl";
    {
        std::FILE* file = std::fopen(mixed.string().c_str(), "wb");
        REQUIRE(file != nullptr);
        for (const auto& line : lines) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fputc('\n', file);
        }
        const std::string dumped = CanonicalJsonDump(v1_line).value();
        std::fwrite(dumped.data(), 1, dumped.size(), file);
        std::fputc('\n', file);
        std::fclose(file);
    }
    const auto report = VerifyJournalFile(mixed);
    CHECK(!report.ok);
    CHECK(report.error_code == "verify.schema_version_mixed");
}

TEST_CASE("session manifest 钉 event schema major") {
    SessionManifest manifest;
    manifest.workspace_key = "ws-000000000000";
    manifest.session_id = "20260830-031522-7K4M2P";
    manifest.main_run_id = "main-0001";
    manifest.event_schema_version = 2;
    const auto json = manifest.ToJson();
    CHECK(json.at("event_schema_version").get<int>() == 2);
    const auto parsed = SessionManifest::FromJson(json);
    REQUIRE(parsed.has_value());
    CHECK(parsed->event_schema_version == 2);
    // 旧 manifest 没这键:按 v1 兜。
    nlohmann::json legacy = json;
    legacy.erase("event_schema_version");
    const auto legacy_parsed = SessionManifest::FromJson(legacy);
    REQUIRE(legacy_parsed.has_value());
    CHECK(legacy_parsed->event_schema_version == 1);
}

TEST_CASE("session manifest 审批档五值稳定，兼容旧值并保守回退") {
    const std::vector<std::pair<lubancode::ApprovalMode, std::string>> cases{
        {lubancode::ApprovalMode::Default, "default"},
        {lubancode::ApprovalMode::AcceptEdits, "accept_edits"},
        {lubancode::ApprovalMode::Yolo, "yolo"},
        {lubancode::ApprovalMode::Auto, "auto"},
        {lubancode::ApprovalMode::DontAsk, "dont_ask"}};
    for (const auto& [mode, name] : cases) {
        SessionManifest manifest;
        manifest.workspace_key = "ws";
        manifest.session_id = "session";
        manifest.main_run_id = "main";
        manifest.status = "running";
        manifest.approval_mode = mode;
        const auto json = manifest.ToJson();
        CHECK(json["approval_mode"] == name);
        const auto parsed = SessionManifest::FromJson(json);
        REQUIRE(parsed.has_value());
        CHECK(parsed->approval_mode == mode);
    }
    SessionManifest base;
    base.workspace_key = "ws";
    base.session_id = "session";
    base.main_run_id = "main";
    base.status = "running";
    nlohmann::json json = base.ToJson();
    CHECK_FALSE(SessionManifest::FromJson(json)->approval_mode.has_value());
    json["approval_mode"] = "confirm";
    CHECK(SessionManifest::FromJson(json)->approval_mode == lubancode::ApprovalMode::Default);
    json["approval_mode"] = "future_unrestricted";
    CHECK(SessionManifest::FromJson(json)->approval_mode == lubancode::ApprovalMode::Default);
}

TEST_CASE("Start 拒不支持的 event_schema_version") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-traj-v2-badver";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "artifacts", ec);
    RecorderOptions options;
    options.event_schema_version = 3;
    auto started = TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts",
                                             Harness::BaseScope(), options);
    REQUIRE(!started.has_value());
    CHECK(started.error().find("schema.unsupported_version") != std::string::npos);
}
