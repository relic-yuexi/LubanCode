// goal 验收账面合同(轨迹 v3 §4.67 G2):
//   - purpose=goal_evaluation:枚举稳定,system 白名单放行(验收专用
//     system turnId 恒 null,不触发主 system 切换);
//   - 五枚事实行 kind(goal.checkpoint.recorded / goal.evidence.recorded /
//     goal.evaluation.requested / completed / rejected):名字稳定、
//     statusless(与 state.goal.applied 同族)、全表可往返;
//   - payload 单行合同:requested 的 evidenceSetHash 必须 hex64、
//     completed 的 decision 四枚 + requestRefs 引用、rejected 必带 reason;
//   - writer 落行进哈希链,VerifyV3File 收;
//   - 反例:缺字段、坏 hash、坏枚举、未知 purpose——写入口就拒。
// 状态提交事务与判词采用归 runtime 册(test_goal_service_g2.cpp);本册
// 只钉账上行的形状。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/schema3.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode::trajectory::v3;

namespace {

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0;v3 册显式开回 1(同款见
// test_v3_goal_applied.cpp)。
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

nlohmann::json EventJson(const char* kind, nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260911-120000-AAAAAA";
    json["runId"] = "run-000001";
    json["seq"] = 2;
    json["timestamp"] = "2026-09-11T04:59:25.314Z";
    json["eventId"] = "evt-000001";
    json["kind"] = kind;
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

nlohmann::json GoodCheckpointPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["iterationId"] = "goal-1/iter-1";
    payload["checkpoint"] = nlohmann::json::object({{"summary", "跑了 ctest"}});
    payload["synthesized"] = false;
    return payload;
}

nlohmann::json GoodEvidencePayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["iterationId"] = "goal-1/iter-1";
    payload["evidenceId"] = "ev-1";
    payload["evidence"] = nlohmann::json::object({{"evidenceId", "ev-1"}});
    return payload;
}

nlohmann::json GoodRequestedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["iterationId"] = "goal-1/iter-1";
    payload["evaluationId"] = "eval-goal-1/iter-1";
    payload["contractRevision"] = 1;
    payload["evidenceSetHash"] = std::string(64, 'a');
    return payload;
}

nlohmann::json GoodCompletedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["evaluationId"] = "eval-goal-1/iter-1";
    payload["decision"] = "continue";
    payload["evaluationMessageRef"] = "msg-000003";
    payload["requestRefs"] = nlohmann::json::array({"request-000001"});
    return payload;
}

nlohmann::json GoodRejectedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["evaluationId"] = "eval-goal-1/iter-1";
    payload["reason"] = "判词两坏: criterion c-1 缺判词";
    return payload;
}

std::optional<Schema3Error> Validate(const char* kind, const nlohmann::json& payload) {
    std::string ec, msg;
    auto parsed = EventLine::FromJsonStrict(EventJson(kind, payload), &ec, &msg);
    if (!parsed.has_value()) {
        return Schema3Error{ec, msg};
    }
    return ValidateEventLine(*parsed);
}

struct WriterHarness {
    std::filesystem::path dir;
    std::optional<V3Writer> writer;

    explicit WriterHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-v3-goal-eval-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-120000-AAAAAA",
                                       "run-000001", "system prompt");
        REQUIRE(started.has_value());
        writer = std::move(*started);
    }
};

}  // namespace

TEST_CASE("purpose=goal_evaluation:枚举稳定,验收专用 system 放行") {
    CHECK(std::string(MessagePurposeName(MessagePurpose::GoalEvaluation)) == "goal_evaluation");
    const auto back = MessagePurposeFromName("goal_evaluation");
    REQUIRE(back.has_value());
    CHECK(*back == MessagePurpose::GoalEvaluation);
    CHECK_FALSE(MessagePurposeFromName("goal_eval").has_value());

    // system 消息 purpose=goal_evaluation 合法(turnId 恒 null)。
    MessageLine system;
    system.message_id = "msg-1";
    system.turn_id = std::nullopt;
    system.purpose = MessagePurpose::GoalEvaluation;
    system.origin = MessageOrigin::SessionRuntime;
    system.display = DisplayMode::Hidden;
    system.system_meta = nlohmann::json::object(
        {{"cause", "goal_evaluation"}, {"changeEventRef", nullptr}, {"systemChanged", false}});
    system.message = nlohmann::json::object({{"role", "system"}, {"content", "判"}});
    CHECK(!ValidateMessageLine(system).has_value());
    // turnId 有值的验收 system 仍拒(system 恒 null 的通用规矩)。
    MessageLine bad = system;
    bad.turn_id = "goaleval-turn-000001";
    CHECK(ValidateMessageLine(bad).has_value());
}

TEST_CASE("五枚 kind:名字稳定、statusless、全表可往返") {
    struct Row {
        EventKindV3 kind;
        const char* name;
    };
    const Row rows[] = {
        {EventKindV3::GoalCheckpointRecorded, "goal.checkpoint.recorded"},
        {EventKindV3::GoalEvidenceRecorded, "goal.evidence.recorded"},
        {EventKindV3::GoalEvaluationRequested, "goal.evaluation.requested"},
        {EventKindV3::GoalEvaluationCompleted, "goal.evaluation.completed"},
        {EventKindV3::GoalEvaluationRejected, "goal.evaluation.rejected"},
    };
    for (const auto& row : rows) {
        CHECK(std::string(EventKindV3Name(row.kind)) == row.name);
        const auto back = EventKindV3FromName(row.name);
        REQUIRE(back.has_value());
        CHECK(*back == row.kind);
        // 事实行:不携带 status(与 state.goal.applied 同族)。
        CHECK_FALSE(RequiredStatusForKind(row.kind).has_value());
        bool in_table = false;
        for (EventKindV3 kind : AllEventKindsV3()) {
            if (kind == row.kind) in_table = true;
        }
        CHECK(in_table);
    }
}

TEST_CASE("goal.checkpoint.recorded:合法行过,缺字段/坏类型拒") {
    REQUIRE(!Validate("goal.checkpoint.recorded", GoodCheckpointPayload()).has_value());

    SUBCASE("缺 synthesized") {
        auto payload = GoodCheckpointPayload();
        payload.erase("synthesized");
        auto error = Validate("goal.checkpoint.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
    SUBCASE("checkpoint 不是 object") {
        auto payload = GoodCheckpointPayload();
        payload["checkpoint"] = "跑了 ctest";
        auto error = Validate("goal.checkpoint.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("synthesized 不是 boolean") {
        auto payload = GoodCheckpointPayload();
        payload["synthesized"] = "no";
        auto error = Validate("goal.checkpoint.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("goal.evidence.recorded:合法行过,坏项拒") {
    REQUIRE(!Validate("goal.evidence.recorded", GoodEvidencePayload()).has_value());

    SUBCASE("缺 evidenceId") {
        auto payload = GoodEvidencePayload();
        payload.erase("evidenceId");
        auto error = Validate("goal.evidence.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
    SUBCASE("goalId 空串") {
        auto payload = GoodEvidencePayload();
        payload["goalId"] = "";
        auto error = Validate("goal.evidence.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("goal.evaluation.requested:evidenceSetHash 必须 hex64") {
    REQUIRE(!Validate("goal.evaluation.requested", GoodRequestedPayload()).has_value());

    SUBCASE("坏 hash") {
        auto payload = GoodRequestedPayload();
        payload["evidenceSetHash"] = "not-a-hash";
        auto error = Validate("goal.evaluation.requested", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("contractRevision 为 0") {
        auto payload = GoodRequestedPayload();
        payload["contractRevision"] = 0;
        auto error = Validate("goal.evaluation.requested", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("缺 evaluationId") {
        auto payload = GoodRequestedPayload();
        payload.erase("evaluationId");
        auto error = Validate("goal.evaluation.requested", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
}

TEST_CASE("goal.evaluation.completed:decision 四枚、引用校验") {
    REQUIRE(!Validate("goal.evaluation.completed", GoodCompletedPayload()).has_value());

    SUBCASE("decision 枚举外") {
        auto payload = GoodCompletedPayload();
        payload["decision"] = "maybe";
        auto error = Validate("goal.evaluation.completed", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_enum");
    }
    SUBCASE("evaluationMessageRef 空串不是引用") {
        auto payload = GoodCompletedPayload();
        payload["evaluationMessageRef"] = "";
        auto error = Validate("goal.evaluation.completed", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_ref");
    }
    SUBCASE("requestRefs 不是数组") {
        auto payload = GoodCompletedPayload();
        payload["requestRefs"] = "request-1";
        auto error = Validate("goal.evaluation.completed", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("goal.evaluation.rejected:reason 必带非空") {
    REQUIRE(!Validate("goal.evaluation.rejected", GoodRejectedPayload()).has_value());

    SUBCASE("reason 空串") {
        auto payload = GoodRejectedPayload();
        payload["reason"] = "";
        auto error = Validate("goal.evaluation.rejected", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("缺 reason") {
        auto payload = GoodRejectedPayload();
        payload.erase("reason");
        auto error = Validate("goal.evaluation.rejected", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
}

TEST_CASE("writer 落五枚事实行进哈希链,VerifyV3File 收") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("writer-chain");
    const nlohmann::json payloads[] = {
        GoodCheckpointPayload(), GoodEvidencePayload(), GoodRequestedPayload(),
        GoodCompletedPayload(), GoodRejectedPayload(),
    };
    const char* kinds[] = {
        "goal.checkpoint.recorded", "goal.evidence.recorded", "goal.evaluation.requested",
        "goal.evaluation.completed", "goal.evaluation.rejected",
    };
    for (std::size_t i = 0; i < 5; ++i) {
        EventDraft draft;
        draft.kind = *EventKindV3FromName(kinds[i]);
        draft.payload = payloads[i];
        const auto receipt = harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    }
    const auto report = VerifyV3File(harness.dir / "s1.jsonl");
    REQUIRE(report.ok);

    const auto ledger = ReadV3Ledger(harness.dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    int found = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::GoalEvaluationRequested) {
            ++found;
            CHECK(event.payload.at("evaluationId") == "eval-goal-1/iter-1");
            CHECK(event.payload.at("contractRevision") == 1);
        }
        if (event.kind == EventKindV3::GoalEvaluationCompleted) {
            ++found;
            CHECK(event.payload.at("decision") == "continue");
        }
    }
    CHECK(found == 2);
}

TEST_CASE("验卷拒坏行:requested 缺 evidenceSetHash 写入口就拒") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("bad-payload");
    EventDraft draft;
    draft.kind = EventKindV3::GoalEvaluationRequested;
    auto payload = GoodRequestedPayload();
    payload.erase("evidenceSetHash");
    draft.payload = std::move(payload);
    const auto receipt = harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    CHECK(receipt.status == WriteReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema3.missing_field");
}
