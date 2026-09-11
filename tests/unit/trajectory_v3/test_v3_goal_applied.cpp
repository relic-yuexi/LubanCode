// state.goal.applied 合同测试(轨迹 v3 §4.67 Goal 模式 G0):
//   - kind 枚举稳定("state.goal.applied"),不带 status(与 context.*.applied
//     同族:控制状态提交事实,不是操作生命周期终态);
//   - payload 单行合同:goalId/旧新 stateRevision(+1)/contractRevision/
//     snapshotRef/snapshotSha256(hex64)/lifecycle 枚举 + 可选 causeRef;
//   - writer 落行进哈希链,VerifyV3File 收;
//   - 反例:缺字段、revision 不 +1、坏 hash、坏 lifecycle、带 status、
//     causeRef 不是引用——全部拒,不静默跳。
// 快照本体(不可变文件)与提交事务(CAS/fail-closed)在
// tests/unit/runtime/test_goal_service.cpp 钉;本册只钉账上行的形状。
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

// ctest 钉 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0(v2 老册行为断言);v3 册
// 显式开回 1(守门案"未设=开"的产品缺省)。同款见 test_v3_clear_switch.cpp。
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

// 组一行合法 event JSON 的底座(信封字段齐全)。
nlohmann::json GoalAppliedJson(nlohmann::json payload) {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = 3;
    json["sessionId"] = "20260911-120000-AAAAAA";
    json["runId"] = "run-000001";
    json["seq"] = 2;
    json["timestamp"] = "2026-09-11T04:59:25.314Z";
    json["eventId"] = "evt-000001";
    json["kind"] = "state.goal.applied";
    json["payload"] = std::move(payload);
    json["prevHash"] = std::string(kGenesisHash);
    json["lineHash"] = std::string(kGenesisHash);
    return json;
}

// 一份合法 payload 的底座。
nlohmann::json GoodPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["fromStateRevision"] = 0;
    payload["toStateRevision"] = 1;
    payload["contractRevision"] = 1;
    payload["snapshotRef"] = "state/goals/goal-1/rev-000001.json";
    payload["snapshotSha256"] = std::string(64, 'a');
    payload["lifecycle"] = "preparing";
    return payload;
}

std::optional<Schema3Error> ValidateGoalAppliedLine(const nlohmann::json& payload) {
    std::string ec, msg;
    auto parsed = EventLine::FromJsonStrict(GoalAppliedJson(payload), &ec, &msg);
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
              ("lubancode-v3-goal-applied-" + std::string(tag));
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

TEST_CASE("枚举与名字:state.goal.applied 稳定,全表可往返") {
    CHECK(std::string(EventKindV3Name(EventKindV3::StateGoalApplied)) == "state.goal.applied");
    const auto back = EventKindV3FromName("state.goal.applied");
    REQUIRE(back.has_value());
    CHECK(*back == EventKindV3::StateGoalApplied);
    bool in_table = false;
    for (EventKindV3 kind : AllEventKindsV3()) {
        if (kind == EventKindV3::StateGoalApplied) in_table = true;
    }
    CHECK(in_table);
    // 控制状态提交事实:不携带 status(§2.2 豁免,同 context.*.applied)。
    CHECK_FALSE(RequiredStatusForKind(EventKindV3::StateGoalApplied).has_value());
}

TEST_CASE("payload 合同:合法行通过,缺字段/坏类型拒") {
    REQUIRE(!ValidateGoalAppliedLine(GoodPayload()).has_value());

    SUBCASE("缺 goalId") {
        auto payload = GoodPayload();
        payload.erase("goalId");
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
    SUBCASE("goalId 空串") {
        auto payload = GoodPayload();
        payload["goalId"] = "";
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("toStateRevision 不为 from+1") {
        auto payload = GoodPayload();
        payload["fromStateRevision"] = 3;
        payload["toStateRevision"] = 5;
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("snapshotSha256 不是 hex64") {
        auto payload = GoodPayload();
        payload["snapshotSha256"] = "not-a-hash";
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("lifecycle 枚举不认得") {
        auto payload = GoodPayload();
        payload["lifecycle"] = "running";  // running 是 phase,不是 lifecycle
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_enum");
    }
    SUBCASE("带了 status:statusless kind 拒") {
        std::string ec, msg;
        auto json = GoalAppliedJson(GoodPayload());
        json["status"] = "done";
        auto parsed = EventLine::FromJsonStrict(json, &ec, &msg);
        REQUIRE(parsed.has_value());  // 信封层收下
        auto error = ValidateEventLine(*parsed);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.status_kind_mismatch");
    }
    SUBCASE("causeRef 合法引用可带,坏引用拒") {
        auto payload = GoodPayload();
        payload["causeRef"] = "command-000001";  // 同会话 string 引用
        REQUIRE(!ValidateGoalAppliedLine(payload).has_value());
        auto bad = GoodPayload();
        bad["causeRef"] = "";  // 空串不是合法引用
        auto error = ValidateGoalAppliedLine(bad);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_ref");
    }
    SUBCASE("contractRevision 从 1 起") {
        auto payload = GoodPayload();
        payload["contractRevision"] = 0;
        auto error = ValidateGoalAppliedLine(payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("writer 落行进哈希链,VerifyV3File 收") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("writer-chain");
    EventDraft draft;
    draft.kind = EventKindV3::StateGoalApplied;
    draft.payload = GoodPayload();
    const auto receipt = harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    CHECK(!receipt.id.empty());
    CHECK(receipt.seq >= 2);  // 首行 system(seq=1)+ session.started 之后

    const auto report = VerifyV3File(harness.dir / "s1.jsonl");
    REQUIRE(report.ok);
    CHECK(report.lines >= 3);  // system + session.started + applied
    // 严格解析回读:kind 与 payload 完整。
    const auto ledger = ReadV3Ledger(harness.dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    bool found = false;
    for (const auto& event : ledger->events) {
        if (event.kind != EventKindV3::StateGoalApplied) continue;
        found = true;
        CHECK(event.payload.at("goalId") == "goal-1");
        CHECK(event.payload.at("toStateRevision") == 1);
        CHECK(event.payload.at("snapshotRef") == "state/goals/goal-1/rev-000001.json");
        CHECK(event.payload.at("lifecycle") == "preparing");
    }
    CHECK(found);
}

TEST_CASE("验卷拒坏行:payload 缺字段的 applied 让整卷明报") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    WriterHarness harness("bad-payload");
    EventDraft draft;
    draft.kind = EventKindV3::StateGoalApplied;
    auto payload = GoodPayload();
    payload.erase("snapshotRef");
    draft.payload = std::move(payload);
    const auto receipt = harness.writer->AppendEvent(std::move(draft), Durability::PowerLoss);
    // 写入口就拒(schema 校验先于落盘)。
    CHECK(receipt.status == WriteReceipt::Status::Rejected);
    CHECK(receipt.error_code == "schema3.missing_field");
}
