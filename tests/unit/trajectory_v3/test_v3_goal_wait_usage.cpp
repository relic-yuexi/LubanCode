// goal 等待/usage 账面合同(轨迹 v3 §4.67 G3):
//   - 三枚事实行 kind(goal.wait.registered / goal.wait.resolved /
//     goal.usage.recorded):名字稳定、statusless(与 state.goal.applied
//     同族)、全表可往返;
//   - payload 单行合同:registered 的 taskRefs 非空 + inspectionPlan 三键
//     (pollsDone>=0/maxPolls>=1/nextDueMs>=0)、resolved 三键非空、
//     usage 的 requestId/source 非空 + usage 计量对象非负 + usageReported
//     boolean;
//   - writer 落行进哈希链,VerifyV3File 收;
//   - 反例:空 taskRefs、坏巡检计划、负计量、缺 usageReported——写入口
//     就拒。
// 服务面(登记/解除/去重计费)归 runtime 册(test_goal_service_g3.cpp);
// 本册只钉账上行的形状。
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

nlohmann::json GoodWaitRegisteredPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["taskRefs"] = nlohmann::json::array({"subagent-3", "subagent-7"});
    payload["notifyDedupeKey"] = "dedupe-abc123";
    payload["inspectionPlan"] = nlohmann::json{{"pollsDone", 0},
                                               {"maxPolls", 3},
                                               {"nextDueMs", 1700001800000}};
    return payload;
}

nlohmann::json GoodWaitResolvedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["deliveryKey"] = "subagent-3";
    payload["reason"] = "background_task_finished";
    return payload;
}

nlohmann::json UsageObject() {
    return nlohmann::json{{"inputTokens", 100},
                          {"outputTokens", 40},
                          {"cacheReadTokens", 0},
                          {"cacheCreationTokens", 0},
                          {"reasoningTokens", 0},
                          {"requestCount", 2},
                          {"durationMs", 1500},
                          {"usageReported", true}};
}

nlohmann::json GoodUsageRecordedPayload() {
    nlohmann::json payload = nlohmann::json::object();
    payload["goalId"] = "goal-1";
    payload["requestId"] = "subagent-5";
    payload["source"] = "subagent";
    payload["usage"] = UsageObject();
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

}  // namespace

TEST_CASE("三枚 kind:名字稳定、statusless、全表可往返") {
    struct Row {
        EventKindV3 kind;
        const char* name;
    };
    const Row rows[] = {
        {EventKindV3::GoalWaitRegistered, "goal.wait.registered"},
        {EventKindV3::GoalWaitResolved, "goal.wait.resolved"},
        {EventKindV3::GoalUsageRecorded, "goal.usage.recorded"},
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

TEST_CASE("goal.wait.registered:合法行过,空 taskRefs/坏巡检计划拒") {
    REQUIRE(!Validate("goal.wait.registered", GoodWaitRegisteredPayload()).has_value());

    SUBCASE("taskRefs 空数组拒(无关进程不进等待账)") {
        auto payload = GoodWaitRegisteredPayload();
        payload["taskRefs"] = nlohmann::json::array();
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("taskRefs 带空串项拒") {
        auto payload = GoodWaitRegisteredPayload();
        payload["taskRefs"] = nlohmann::json::array({"subagent-3", ""});
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("缺 notifyDedupeKey") {
        auto payload = GoodWaitRegisteredPayload();
        payload.erase("notifyDedupeKey");
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
    SUBCASE("inspectionPlan.maxPolls 为 0 拒") {
        auto payload = GoodWaitRegisteredPayload();
        payload["inspectionPlan"]["maxPolls"] = 0;
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("inspectionPlan.pollsDone 为负拒") {
        auto payload = GoodWaitRegisteredPayload();
        payload["inspectionPlan"]["pollsDone"] = -1;
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("inspectionPlan 缺 nextDueMs 拒") {
        auto payload = GoodWaitRegisteredPayload();
        payload["inspectionPlan"].erase("nextDueMs");
        auto error = Validate("goal.wait.registered", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("goal.wait.resolved:三键非空;空 deliveryKey 拒") {
    REQUIRE(!Validate("goal.wait.resolved", GoodWaitResolvedPayload()).has_value());

    SUBCASE("deliveryKey 空串拒(去重键不许空)") {
        auto payload = GoodWaitResolvedPayload();
        payload["deliveryKey"] = "";
        auto error = Validate("goal.wait.resolved", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("缺 reason") {
        auto payload = GoodWaitResolvedPayload();
        payload.erase("reason");
        auto error = Validate("goal.wait.resolved", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.missing_field");
    }
}

TEST_CASE("goal.usage.recorded:归属非空、计量非负、usageReported 必带") {
    REQUIRE(!Validate("goal.usage.recorded", GoodUsageRecordedPayload()).has_value());

    SUBCASE("requestId 空串拒") {
        auto payload = GoodUsageRecordedPayload();
        payload["requestId"] = "";
        auto error = Validate("goal.usage.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("负计量拒") {
        auto payload = GoodUsageRecordedPayload();
        payload["usage"]["outputTokens"] = -1;
        auto error = Validate("goal.usage.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("缺 usageReported 拒(unknown 不冒充 0)") {
        auto payload = GoodUsageRecordedPayload();
        payload["usage"].erase("usageReported");
        auto error = Validate("goal.usage.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
    SUBCASE("usage 不是 object 拒") {
        auto payload = GoodUsageRecordedPayload();
        payload["usage"] = nlohmann::json::array();
        auto error = Validate("goal.usage.recorded", payload);
        REQUIRE(error.has_value());
        CHECK(error->code == "schema3.bad_type");
    }
}

TEST_CASE("writer 落三枚事实行进哈希链,VerifyV3File 收") {
    EnvGuard guard("LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS", "1");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "lubancode-v3-goal-wait-usage-writer";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    auto started = V3Writer::Start(dir / "s1.jsonl", "20260911-120000-AAAAAA", "run-000001",
                                   "system prompt");
    REQUIRE(started.has_value());
    V3Writer& writer = *started;

    {
        EventDraft registered;
        registered.kind = EventKindV3::GoalWaitRegistered;
        registered.payload = GoodWaitRegisteredPayload();
        auto receipt = writer.AppendEvent(std::move(registered), Durability::ProcessCrash);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    }
    {
        EventDraft resolved;
        resolved.kind = EventKindV3::GoalWaitResolved;
        resolved.payload = GoodWaitResolvedPayload();
        auto receipt = writer.AppendEvent(std::move(resolved), Durability::ProcessCrash);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    }
    {
        EventDraft usage;
        usage.kind = EventKindV3::GoalUsageRecorded;
        usage.payload = GoodUsageRecordedPayload();
        auto receipt = writer.AppendEvent(std::move(usage), Durability::ProcessCrash);
        REQUIRE(receipt.status == WriteReceipt::Status::Committed);
    }
    const auto report = VerifyV3File(dir / "s1.jsonl");
    REQUIRE(report.ok);
    const auto ledger = ReadV3Ledger(dir / "s1.jsonl");
    REQUIRE(ledger.has_value());
    int found = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == EventKindV3::GoalWaitRegistered) {
            ++found;
            CHECK(event.payload.at("goalId") == "goal-1");
            REQUIRE(event.payload.at("taskRefs").size() == 2);
        }
        if (event.kind == EventKindV3::GoalWaitResolved) {
            ++found;
            CHECK(event.payload.at("deliveryKey") == "subagent-3");
        }
        if (event.kind == EventKindV3::GoalUsageRecorded) {
            ++found;
            CHECK(event.payload.at("requestId") == "subagent-5");
            CHECK(event.payload.at("usage").at("inputTokens") == 100);
        }
    }
    CHECK(found == 3);
}
