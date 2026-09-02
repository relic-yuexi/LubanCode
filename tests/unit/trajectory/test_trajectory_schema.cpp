// schema 强校验测试:信封语义 + payload 按 kind 的必填/未知/类型三拒。
// §五 payload 样例逐个喂过;fail-closed 是合同,不是选项。
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "trajectory/schema.hpp"

using namespace lubancode::trajectory;

namespace {

EventEnvelope BaseEnvelope(EventKind kind) {
    const EventKindInfo& info = EventKindInfoOf(kind);
    EventEnvelope envelope;
    envelope.workspace_key = "ws-key-000000000000";
    envelope.session_id = "20260830-031522-7K4M2P";
    envelope.run_id = "main-0001";
    envelope.run_kind = RunKind::MainSession;
    envelope.seq = 1;
    envelope.event_id = FormatEventId(envelope.run_id, 1);
    envelope.kind = kind;
    envelope.plane = info.plane;
    envelope.actor = Actor::Host;
    envelope.origin = Origin::RecoveryRuntime;
    envelope.visibility = {Visibility::HostOnly};
    envelope.training_policy = TrainingPolicy::Exclude;
    if (info.turn == IdNeed::Required) {
        envelope.turn_id = "turn-0001";
    }
    if (info.request == IdNeed::Required) {
        envelope.request_id = "req-0001";
    }
    if (info.call == IdNeed::Required) {
        envelope.call_id = "call-0001";
    }
    envelope.wall_time_ms = 0;
    envelope.monotonic_ns = 0;
    envelope.payload = nlohmann::json::object();
    envelope.prev_hash = std::string(64, '0');
    envelope.event_hash = std::string(64, '0');
    return envelope;
}

}  // namespace

TEST_CASE("schema: §五 payload 样例全过校验") {
    SUBCASE("input.received") {
        auto envelope = BaseEnvelope(EventKind::InputReceived);
        envelope.actor = Actor::User;
        envelope.origin = Origin::ExternalUser;
        envelope.payload = nlohmann::json::parse(R"({
            "input_id": "input-0007",
            "content": [{"type": "text", "text": "..."}],
            "attachments": [],
            "channel": "terminal",
            "sender": {"kind": "local_user"}})");
        CHECK_FALSE(ValidateEnvelope(envelope).has_value());
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("context.attached") {
        auto envelope = BaseEnvelope(EventKind::ContextAttached);
        envelope.payload = nlohmann::json::parse(R"({
            "context_id": "ctx-0014",
            "context_kind": "memory_recall",
            "scope": "request",
            "content_ref": {"sha256": "ab", "size": 1234},
            "source_refs": ["memory:fact-19"]})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("model.request.prepared") {
        auto envelope = BaseEnvelope(EventKind::ModelRequestPrepared);
        envelope.payload = nlohmann::json::parse(R"({
            "model": "gpt-5.6-sol", "provider": "ccmoon", "wire": "responses",
            "parameters": {"max_output_tokens": 32768, "reasoning_effort": "high",
                           "temperature": null, "seed": null},
            "system_ref": {"sha256": "ab", "size": 0},
            "toolset_ref": {"sha256": "ab", "size": 0},
            "message_refs": ["evt-1", "evt-2"],
            "context_refs": ["ctx-0014"],
            "request_snapshot_ref": {"sha256": "ab", "size": 0},
            "request_snapshot_sha256": "ab",
            "cache_epoch": 2})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("model.output.completed") {
        auto envelope = BaseEnvelope(EventKind::ModelOutputCompleted);
        envelope.actor = Actor::Model;
        envelope.origin = Origin::ProviderModel;
        envelope.payload = nlohmann::json::parse(R"({
            "output_id": "output-0019",
            "blocks": [
                {"type": "text", "text": "我先查文件。"},
                {"type": "tool_call", "call_id": "call-0012",
                 "provider_call_id": "toolu_x", "name": "read_file",
                 "arguments": {"path": "src/a.cpp"}}],
            "stop_reason": "tool_use",
            "usage": {},
            "provider_response_id": "resp_x"})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("tool.input.effective") {
        auto envelope = BaseEnvelope(EventKind::ToolInputEffective);
        envelope.actor = Actor::Tool;
        envelope.origin = Origin::BuiltinTool;
        envelope.payload = nlohmann::json::parse(R"({
            "call_id": "call-0012", "tool_name": "read_file",
            "source_kind": "builtin", "source_instance": "",
            "effect_class": "read_only_local",
            "effective_arguments": {"path": "src/a.cpp"},
            "effective_arguments_sha256": "ab",
            "rewritten_by": ["hook:pre_tool_use"]})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("tool.execution.finished") {
        auto envelope = BaseEnvelope(EventKind::ToolExecutionFinished);
        envelope.payload = nlohmann::json::parse(R"({
            "outcome": "succeeded", "duration_ms": 18, "exit_code": null,
            "stdout_ref": null, "stderr_ref": null,
            "result_ref": {"sha256": "ab", "size": 921},
            "side_effects": [], "undo_ref": null})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("tool.result.committed") {
        auto envelope = BaseEnvelope(EventKind::ToolResultCommitted);
        envelope.payload = nlohmann::json::parse(R"({
            "call_id": "call-0012",
            "content": [{"type": "text", "text_ref": {"sha256": "ab", "size": 1}}],
            "is_error": false,
            "derived_from_event": "main-0001:evt-00000045"})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("verification.recorded") {
        auto envelope = BaseEnvelope(EventKind::VerificationRecorded);
        envelope.actor = Actor::Verifier;
        envelope.origin = Origin::VerifierHost;
        envelope.training_policy = TrainingPolicy::Include;
        envelope.payload = nlohmann::json::parse(R"({
            "verification_id": "verify-0004", "kind": "test_suite",
            "subject": "build/tests.exe",
            "command_ref": {"sha256": "ab"},
            "passed": true,
            "facts": {"passed": 104, "failed": 0, "skipped": 0},
            "artifact_refs": [], "observed_after_seq": 281,
            "producer": "run_command", "fresh": true})");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
    SUBCASE("run.completed 封口四件套") {
        auto envelope = BaseEnvelope(EventKind::RunCompleted);
        envelope.payload = MakeTerminalSealPayload(std::string(64, '0'), 10, 1, "v1");
        CHECK_FALSE(ValidatePayload(envelope.kind, envelope.payload).has_value());
    }
}

TEST_CASE("schema: context.pressure.recorded 三项账闭合且类型严格") {
    const auto valid = nlohmann::json{{"phase", "preflight_exceeded"},
                                      {"estimated_input_tokens", std::uint64_t{16000}},
                                      {"reserved_output_tokens", std::uint64_t{2048}},
                                      {"protocol_headroom_tokens", std::uint64_t{512}},
                                      {"window_tokens", std::uint64_t{32768}},
                                      {"reserve_clamped", true}};
    CHECK_FALSE(ValidatePayload(EventKind::ContextPressureRecorded, valid).has_value());

    auto missing = valid;
    missing.erase("protocol_headroom_tokens");
    const auto missing_error = ValidatePayload(EventKind::ContextPressureRecorded, missing);
    REQUIRE(missing_error.has_value());
    CHECK(missing_error->error_code == "schema.payload_missing_field");

    auto bad_type = valid;
    bad_type["reserved_output_tokens"] = -1;
    const auto type_error = ValidatePayload(EventKind::ContextPressureRecorded, bad_type);
    REQUIRE(type_error.has_value());
    CHECK(type_error->error_code == "schema.payload_bad_type");
}

TEST_CASE("schema: payload 缺必填拒绝") {
    auto envelope = BaseEnvelope(EventKind::TurnStarted);
    envelope.payload = nlohmann::json{{"trigger", "external_user"}};
    CHECK_FALSE(ValidatePayload(EventKind::TurnStarted, envelope.payload).has_value());
    envelope.payload = nlohmann::json::object();  // 缺 trigger
    const auto error = ValidatePayload(EventKind::TurnStarted, envelope.payload);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_missing_field");

    const auto missing_id = ValidatePayload(EventKind::InputReceived, nlohmann::json::object());
    REQUIRE(missing_id.has_value());
    CHECK(missing_id->error_code == "schema.payload_missing_field");
}

TEST_CASE("schema: payload 未知字段拒绝") {
    auto payload = nlohmann::json{{"trigger", "external_user"}, {"mystery", 1}};
    const auto error = ValidatePayload(EventKind::TurnStarted, payload);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_unknown_field");
}

TEST_CASE("schema: payload 类型不合拒绝") {
    auto payload = nlohmann::json{{"trigger", 42}};
    const auto error = ValidatePayload(EventKind::TurnStarted, payload);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_bad_type");

    auto seal = MakeTerminalSealPayload(std::string(64, '0'), 10, 1, "v1");
    seal["event_count_before_terminal"] = -1;  // 负数不是无符号
    const auto bad = ValidatePayload(EventKind::RunCompleted, seal);
    REQUIRE(bad.has_value());
    CHECK(bad->error_code == "schema.payload_bad_type");
}

TEST_CASE("schema: 信封语义——plane、actor/origin、event_id、id 三档") {
    SUBCASE("plane 不合 kind 固定面") {
        auto envelope = BaseEnvelope(EventKind::InputReceived);
        envelope.plane = Plane::Control;
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.plane_mismatch");
    }
    SUBCASE("actor/origin 不合法组合") {
        auto envelope = BaseEnvelope(EventKind::InputReceived);
        envelope.actor = Actor::User;
        envelope.origin = Origin::PeerAgent;
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.bad_actor_origin");
    }
    SUBCASE("event_id 形状") {
        auto envelope = BaseEnvelope(EventKind::TurnStarted);
        envelope.payload = nlohmann::json{{"trigger", "external_user"}};
        envelope.event_id = "not-the-shape";
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.bad_event_id");
    }
    SUBCASE("缺 turn_id 的 input.received") {
        auto envelope = BaseEnvelope(EventKind::InputReceived);
        envelope.turn_id = std::nullopt;
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.missing_field");
    }
    SUBCASE("带了禁止的 call_id") {
        auto envelope = BaseEnvelope(EventKind::TurnStarted);
        envelope.payload = nlohmann::json{{"trigger", "x"}};
        envelope.call_id = "call-0001";  // turn.started 不许带
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.forbidden_field");
    }
    SUBCASE("hash 字段形状") {
        auto envelope = BaseEnvelope(EventKind::TurnStarted);
        envelope.payload = nlohmann::json{{"trigger", "x"}};
        envelope.event_hash = "zz";
        const auto error = ValidateEnvelope(envelope);
        REQUIRE(error.has_value());
        CHECK(error->error_code == "schema.bad_hash");
    }
}

TEST_CASE("schema: ParseAndValidateEventLine 一处过齐") {
    auto envelope = BaseEnvelope(EventKind::TurnStarted);
    envelope.actor = Actor::User;
    envelope.origin = Origin::ExternalUser;
    envelope.payload = nlohmann::json{{"trigger", "external_user"}};
    EventEnvelope parsed;
    CHECK_FALSE(ParseAndValidateEventLine(envelope.ToJson(), &parsed).has_value());
    CHECK(parsed.kind == EventKind::TurnStarted);
    CHECK(parsed.turn_id.has_value());

    nlohmann::json bad = envelope.ToJson();
    bad["extra"] = true;
    const auto error = ParseAndValidateEventLine(bad);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.unknown_field");
}

TEST_CASE("schema: 正文类字段吃 BlobRef 形态(超限 offload 后的落盘形状)") {
    auto envelope = BaseEnvelope(EventKind::ModelRequestPrepared);
    envelope.payload = nlohmann::json{
        {"model", "m"}, {"provider", "p"}, {"wire", "w"}, {"message_refs", nlohmann::json::array()},
        {"system_ref", nlohmann::json{{"sha256", std::string(64, '0')},
                                      {"size", 100u},
                                      {"media_type", "text/plain"},
                                      {"encoding", "utf-8"},
                                      {"compression", "none"}}}};
    CHECK_FALSE(ValidatePayload(EventKind::ModelRequestPrepared, envelope.payload).has_value());
    // 半截 BlobRef(缺 size)不是合法形状。
    envelope.payload["system_ref"] = nlohmann::json{{"sha256", std::string(64, '0')}};
    const auto error = ValidatePayload(EventKind::ModelRequestPrepared, envelope.payload);
    REQUIRE(error.has_value());
    CHECK(error->error_code == "schema.payload_bad_type");
}
