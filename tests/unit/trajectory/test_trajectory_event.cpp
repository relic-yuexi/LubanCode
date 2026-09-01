// EventEnvelope 合同测试:枚举往返、event_id 形状、信封严格解析、
// actor/origin 组合(§4.3)、id 三档要求(§4.1)。
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/event.hpp"

using namespace lubancode::trajectory;

namespace {

EventEnvelope MakeValidEnvelope() {
    EventEnvelope envelope;
    envelope.workspace_key = "lubancode-7d4c9a18f205";
    envelope.session_id = "20260830-031522-7K4M2P";
    envelope.run_id = "agent-0002";
    envelope.run_kind = RunKind::Subagent;
    envelope.seq = 42;
    envelope.event_id = FormatEventId(envelope.run_id, 42);
    envelope.kind = EventKind::ToolExecutionFinished;
    envelope.plane = Plane::Execution;
    envelope.actor = Actor::Tool;
    envelope.origin = Origin::BuiltinTool;
    envelope.visibility = {Visibility::HostOnly};
    envelope.training_policy = TrainingPolicy::Metadata;
    envelope.turn_id = "turn-0007";
    envelope.request_id = "req-0019";
    envelope.call_id = "call-0012";
    envelope.causation_id = "agent-0002:evt-00000039";
    envelope.correlation_id = "call-0012";
    envelope.wall_time_ms = 1000;
    envelope.monotonic_ns = 2000;
    envelope.payload = nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 18}};
    envelope.prev_hash = std::string(64, '0');
    envelope.event_hash = std::string(64, 'a');
    return envelope;
}

}  // namespace

TEST_CASE("event: 全部 68 种 kind 名字往返") {
    int count = 0;
    for (const EventKind kind : AllEventKinds()) {
        const char* name = EventKindName(kind);
        CHECK(name != nullptr);
        CHECK(*name != '\0');
        const auto back = EventKindFromName(name);
        REQUIRE(back.has_value());
        CHECK(*back == kind);
        ++count;
    }
    CHECK(count == 72);
    // v2 新事件(Token 账本单 §6.1.1):usage 的 canonical owner。
    CHECK(EventKindFromName("model.usage.recorded").has_value());
    // 存储 v2 P0-3:召回快照与 Memory 写入因果边。
    CHECK(EventKindFromName("context.injected").has_value());
    CHECK(EventKindFromName("memory.save.requested").has_value());
    CHECK(EventKindFromName("memory.save.committed").has_value());
    CHECK(EventKindFromName("memory.save.failed").has_value());
    CHECK_FALSE(EventKindFromName("no.such.kind").has_value());
    CHECK(EventKindName(static_cast<EventKind>(999))[0] == '\0');
}

TEST_CASE("event: 枚举名与 §4.2-4.5 线上名一致") {
    CHECK(std::string(RunKindName(RunKind::MainSession)) == "main_session");
    CHECK(std::string(PlaneName(Plane::Conversation)) == "conversation");
    CHECK(std::string(OriginName(Origin::QueuedUser)) == "queued_user");
    CHECK(std::string(VisibilityName(Visibility::ModelInput)) == "model_input");
    CHECK(std::string(TrainingPolicyName(TrainingPolicy::Review)) == "review");
    CHECK(std::string(DurabilityName(Durability::PowerLoss)) == "power_loss");
    CHECK(RunKindFromName("workflow_node") == RunKind::WorkflowNode);
    CHECK_FALSE(OriginFromName("nope").has_value());
}

TEST_CASE("event: kind 固定 plane(§4.2 归面)") {
    CHECK(EventKindInfoOf(EventKind::InputReceived).plane == Plane::Conversation);
    CHECK(EventKindInfoOf(EventKind::ToolResultCommitted).plane == Plane::Conversation);
    CHECK(EventKindInfoOf(EventKind::ModelRequestPrepared).plane == Plane::Execution);
    CHECK(EventKindInfoOf(EventKind::VerificationRecorded).plane == Plane::Evidence);
    CHECK(EventKindInfoOf(EventKind::OutcomeAssessed).plane == Plane::Evidence);
    CHECK(EventKindInfoOf(EventKind::ControlTitleChanged).plane == Plane::Control);
    CHECK(EventKindInfoOf(EventKind::SessionEnded).main_stream_only);
    CHECK(EventKindInfoOf(EventKind::SessionClearRequested).main_stream_only);
    CHECK_FALSE(EventKindInfoOf(EventKind::TurnStarted).main_stream_only);
}

TEST_CASE("event: actor/origin 组合(§4.3 表)") {
    CHECK(IsValidActorOrigin(Actor::User, Origin::ExternalUser));
    CHECK(IsValidActorOrigin(Actor::User, Origin::QueuedUser));
    CHECK(IsValidActorOrigin(Actor::Host, Origin::ScheduledHost));
    CHECK(IsValidActorOrigin(Actor::Model, Origin::ProviderModel));
    CHECK(IsValidActorOrigin(Actor::Tool, Origin::McpTool));
    CHECK(IsValidActorOrigin(Actor::Host, Origin::MemoryRecall));
    CHECK(IsValidActorOrigin(Actor::Verifier, Origin::VerifierHost));
    // peer 消息不冒充真人 user(§2.7)。
    CHECK_FALSE(IsValidActorOrigin(Actor::User, Origin::PeerAgent));
    CHECK(IsValidActorOrigin(Actor::Host, Origin::PeerAgent));
    CHECK_FALSE(IsValidActorOrigin(Actor::Model, Origin::ExternalUser));
    CHECK_FALSE(IsValidActorOrigin(Actor::Tool, Origin::ProviderModel));
    CHECK_FALSE(IsValidActorOrigin(Actor::Verifier, Origin::BuiltinTool));
}

TEST_CASE("event: FormatEventId 与 IsHex64") {
    CHECK(FormatEventId("agent-0002", 42) == "agent-0002:evt-00000042");
    CHECK(FormatEventId("main-0001", 1) == "main-0001:evt-00000001");
    CHECK(FormatEventId("wf-0001", 123456789) == "wf-0001:evt-123456789");
    CHECK(IsHex64(std::string(64, '0')));
    CHECK(IsHex64("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    CHECK_FALSE(IsHex64("short"));
    CHECK_FALSE(IsHex64(std::string(64, 'g')));
    CHECK_FALSE(IsHex64(std::string(65, '0')));
}

TEST_CASE("event: ToJson/FromJsonStrict 往返") {
    const EventEnvelope envelope = MakeValidEnvelope();
    const nlohmann::json json = envelope.ToJson();
    std::string error_code;
    std::string message;
    const auto back = EventEnvelope::FromJsonStrict(json, &error_code, &message);
    REQUIRE(back.has_value());
    CHECK(back->workspace_key == envelope.workspace_key);
    CHECK(back->session_id == envelope.session_id);
    CHECK(back->run_id == envelope.run_id);
    CHECK(back->run_kind == envelope.run_kind);
    CHECK(back->seq == envelope.seq);
    CHECK(back->event_id == envelope.event_id);
    CHECK(back->kind == envelope.kind);
    CHECK(back->plane == envelope.plane);
    CHECK(back->actor == envelope.actor);
    CHECK(back->origin == envelope.origin);
    CHECK(back->visibility == envelope.visibility);
    CHECK(back->training_policy == envelope.training_policy);
    CHECK(back->turn_id == envelope.turn_id);
    CHECK(back->request_id == envelope.request_id);
    CHECK(back->call_id == envelope.call_id);
    CHECK(back->causation_id == envelope.causation_id);
    CHECK(back->correlation_id == envelope.correlation_id);
    CHECK(back->wall_time_ms == envelope.wall_time_ms);
    CHECK(back->monotonic_ns == envelope.monotonic_ns);
    CHECK(back->payload == envelope.payload);
    CHECK(back->prev_hash == envelope.prev_hash);
    CHECK(back->event_hash == envelope.event_hash);
    CHECK(back->schema_version == 1);
}

TEST_CASE("event: FromJsonStrict 拒未知信封字段") {
    nlohmann::json json = MakeValidEnvelope().ToJson();
    json["surprise"] = 1;
    std::string error_code;
    std::string message;
    CHECK_FALSE(EventEnvelope::FromJsonStrict(json, &error_code, &message).has_value());
    CHECK(error_code == "schema.unknown_field");
}

TEST_CASE("event: FromJsonStrict 只认已实现 schema_version(1=v1,2=v2)") {
    std::string error_code;
    std::string message;
    for (const int supported : {1, 2}) {
        nlohmann::json json = MakeValidEnvelope().ToJson();
        json["schema_version"] = supported;
        // v2 事件的 completed payload 带 usage 会被版本裁拒,这里只验信封
        // 版本位本身:1/2 都放行(usage 一类差异归 ValidatePayloadWithVersion)。
        json["kind"] = "run.started";
        json["payload"] = nlohmann::json{{"run_kind", "main_session"}};
        auto envelope = EventEnvelope::FromJsonStrict(json, &error_code, &message);
        INFO(supported);
        CHECK(envelope.has_value());
        if (envelope.has_value()) {
            CHECK(envelope->schema_version == supported);
        }
    }
    for (const int unsupported : {0, 3, 99}) {
        nlohmann::json json = MakeValidEnvelope().ToJson();
        json["schema_version"] = unsupported;
        CHECK_FALSE(EventEnvelope::FromJsonStrict(json, &error_code, &message).has_value());
        CHECK(error_code == "schema.unsupported_version");
    }
}

TEST_CASE("event: FromJsonStrict 拒坏 schema 名与缺字段") {
    std::string error_code;
    std::string message;
    nlohmann::json json = MakeValidEnvelope().ToJson();
    json["schema"] = "something.else";
    CHECK_FALSE(EventEnvelope::FromJsonStrict(json, &error_code, &message).has_value());
    CHECK(error_code == "schema.bad_schema_name");

    nlohmann::json missing = MakeValidEnvelope().ToJson();
    missing.erase("workspace_key");
    CHECK_FALSE(EventEnvelope::FromJsonStrict(missing, &error_code, &message).has_value());
    CHECK(error_code == "schema.missing_field");
}

TEST_CASE("event: relations 键集封闭(§6.3)") {
    EventEnvelope envelope = MakeValidEnvelope();
    envelope.relations = nlohmann::json{{"retry_of", "call-0011"}, {"blocked_by", {"call-0009"}}};
    const nlohmann::json json = envelope.ToJson();
    CHECK(json.contains("relations"));
    std::string error_code;
    std::string message;
    const auto back = EventEnvelope::FromJsonStrict(json, &error_code, &message);
    REQUIRE(back.has_value());
    CHECK(back->relations == envelope.relations);

    nlohmann::json bad = envelope.ToJson();
    bad["relations"]["mystery"] = 1;
    CHECK_FALSE(EventEnvelope::FromJsonStrict(bad, &error_code, &message).has_value());
    CHECK(error_code == "schema.unknown_field");
}

TEST_CASE("event: 终态封口四件套(§8.3)") {
    const auto payload = MakeTerminalSealPayload(std::string(64, '1'), 41, 1, "trajectory-v1");
    CHECK(payload.at("first_event_hash") == std::string(64, '1'));
    CHECK(payload.at("event_count_before_terminal") == 41);
    CHECK(payload.at("schema_version") == 1);
    CHECK(payload.at("recorder_version") == "trajectory-v1");
}
