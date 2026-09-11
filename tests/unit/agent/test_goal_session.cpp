// goal 事件行读侧钉子(写侧已随 §4.67.6 收敛删除):
//   - ParseGoalEvent 认旧档 goal_v1 五族 type、整收字段、坏行跳过不废
//     整场(resume/evolution 只读观察不断粮);
//   - IsGoalEventLine 粗筛与尾行截断语义不破。
// 写侧(SerializeGoalEvent/SerializeGoalEvidence)与 GoalEvidenceRecord
// 已删:v3 起 goal 持久账走 state.goal.applied + 不可变快照
// (runtime/goal_service.hpp,tests/unit/runtime/test_goal_service.cpp)。

#include <doctest/doctest.h>

#include <string>

#include "sessions/goal_session.hpp"

namespace {

// 一行旧档 goal_v1 事件(ledger sink 年代落盘形状:snake_case 键)。
std::string Line(const char* type, const char* event, const char* goal_id,
                 const char* extra = "") {
    std::string line = R"({"type":")" + std::string(type) + R"(","event":")" +
                       std::string(event) + R"(","goal_id":")" + std::string(goal_id) +
                       R"(")";
    line += extra;
    line += "}";
    return line;
}

}  // namespace

TEST_CASE("goal 事件行读侧:五种 type 全认,iteration 类带 iteration_id") {
    const char* types[] = {"goal_v1", "goal_iteration_v1", "goal_evidence_v1",
                           "goal_checkpoint_v1", "goal_evaluation_v1"};
    int index = 0;
    for (const char* type : types) {
        const bool is_iteration_type = index > 0;  // 除 goal_v1 外都属迭代域
        const std::string extra =
            is_iteration_type ? R"(,"iteration_id":"goal-1/iter-2","revision":2,
                                 "payload":{"dedupe_key":"goal-1:r2:i2"})"
                              : R"(,"revision":1,"payload":{"objective":"迁移认证层"},
                                 "timestamp_ms":1720000000000)";
        const auto parsed = lubancode::sessions::ParseGoalEvent(Line(type, "scheduled", "goal-1", extra.c_str()));
        REQUIRE(parsed.has_value());
        CHECK(parsed->type == type);
        CHECK(parsed->goal_id == "goal-1");
        if (is_iteration_type) {
            CHECK(parsed->iteration_id == "goal-1/iter-2");
            CHECK(parsed->revision == 2);
        } else {
            CHECK(parsed->iteration_id.empty());  // goal 级事件不带
            CHECK(parsed->payload.at("objective") == "迁移认证层");
            CHECK(parsed->timestamp_ms == 1720000000000);
        }
        ++index;
    }
}

TEST_CASE("goal 事件行读侧:证据行顶层字段镜像进 payload") {
    // 旧档 goal_evidence_v1 的领域字段在顶层(evidence_id/kind/facts/…),
    // ParseGoalEvent 镜像进 payload,消费方不必二次解析原文。
    const std::string line =
        R"({"type":"goal_evidence_v1","event":"observed","goal_id":"goal-3",)"
        R"("iteration_id":"goal-3/iter-1","evidence_id":"ev-7","kind":"command_exit",)"
        R"("tool_use_id":"toolu-9","producer":"run_command","sha256":"sha-xyz",)"
        R"("facts":{"exit_code":0},"observed_at_ms":1720000001000,"fresh":true,)"
        R"("truncated":false})";
    const auto parsed = lubancode::sessions::ParseGoalEvent(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->payload.at("evidence_id") == "ev-7");
    CHECK(parsed->payload.at("kind") == "command_exit");
    CHECK(parsed->payload.at("facts").at("exit_code") == 0);
    CHECK(parsed->payload.at("fresh") == true);
}

TEST_CASE("goal 事件行读侧:坏行跳过,不废整场") {
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent("not json").has_value());
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent("{}").has_value());
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(R"({"type":"compact"})").has_value());       // 非 goal 族
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(R"({"type":"goal_v1"})").has_value());       // 缺 event/goal_id
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(R"({"type":"goal_v1","event":"x"})").has_value());  // 缺 goal_id
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(
                    R"({"type":"goal_v9","event":"x","goal_id":"g"})")
                    .has_value());
}

TEST_CASE("IsGoalEventLine 粗筛与尾行截断") {
    CHECK(lubancode::sessions::IsGoalEventLine(R"({"type":"goal_v1","event":"created"})"));
    CHECK(lubancode::sessions::IsGoalEventLine(R"({"type": "goal_iteration_v1"})"));
    CHECK_FALSE(lubancode::sessions::IsGoalEventLine(R"({"type":"compact"})"));
    CHECK_FALSE(lubancode::sessions::IsGoalEventLine(R"({"type":"title"})"));

    // 尾行截断:半截 JSON 的 goal 行跳过,前面的完整事件为准。
    const std::string full =
        Line("goal_v1", "created", "goal-3",
             R"(,"revision":1,"payload":{"objective":"迁移认证层","note":"...."})");
    const std::string truncated = full.substr(0, full.size() - 10);
    CHECK_FALSE(lubancode::sessions::ParseGoalEvent(truncated).has_value());
}
