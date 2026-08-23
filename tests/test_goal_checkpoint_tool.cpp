// /goal 单第 2 期:goal_checkpoint 工具(schema 门槛、evidence 白名单、
// blocked/needs_user 配对、普通轮旁路拒)。

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/goal_checkpoint_tool.hpp"

using lubancode::tools::GoalCheckpointState;
using lubancode::tools::GoalCheckpointStatus;
using lubancode::tools::GoalCheckpointTool;

namespace {

struct Fixture {
    std::shared_ptr<GoalCheckpointState> state = std::make_shared<GoalCheckpointState>();
    GoalCheckpointTool tool{state};

    Fixture() {
        state->goal_id = "goal-1";
        state->iteration_id = "goal-1/iter-1";
        state->valid_evidence_ids = {"ev-1", "ev-2"};
    }

    lubancode::tools::Tool::Result Run(const nlohmann::json& input) { return tool.execute(input); }
};

nlohmann::json ValidInput() {
    return nlohmann::json{
        {"status", "progress"},
        {"summary", "契约测试跑通"},
        {"completed", nlohmann::json::array({"修迁移代码"})},
        {"remaining", nlohmann::json::array({"重跑 e2e"})},
        {"next_action", "重跑 e2e"},
        {"evidence_ids", nlohmann::json::array({"ev-1"})},
    };
}

}  // namespace

TEST_CASE("合法 checkpoint:记账、候选取最后一枚") {
    Fixture fx;
    const auto r = fx.Run(ValidInput());
    REQUIRE_FALSE(r.is_error);
    CHECK(fx.state->HasCheckpoint());
    const auto candidate = fx.state->Candidate();
    REQUIRE(candidate.has_value());
    CHECK(candidate->status == GoalCheckpointStatus::Progress);
    CHECK(candidate->summary == "契约测试跑通");
    const std::vector<std::string> expect_completed{"修迁移代码"};
    CHECK(candidate->completed == expect_completed);
    const std::vector<std::string> expect_evidence{"ev-1"};
    CHECK(candidate->evidence_ids == expect_evidence);

    // 一轮多次:最后一枚为候选。
    auto second = ValidInput();
    second["status"] = "ready_for_evaluation";
    second["remaining"] = nlohmann::json::array();
    REQUIRE_FALSE(fx.Run(second).is_error);
    const auto last = fx.state->Candidate();
    REQUIRE(last.has_value());
    CHECK(last->status == GoalCheckpointStatus::ReadyForEvaluation);
    CHECK(last->remaining.empty());
    CHECK(fx.state->entries.size() == 2);
}

TEST_CASE("status 四枚与坏枚举") {
    Fixture fx;
    for (const char* status : {"progress", "ready_for_evaluation", "blocked", "needs_user"}) {
        auto input = ValidInput();
        input["status"] = status;
        if (std::string(status) == "blocked") input["blocker_key"] = "missing_credential:X";
        if (std::string(status) == "needs_user") input["question"] = "选哪条路?";
        const auto r = fx.Run(input);
        CHECK_FALSE(r.is_error);
    }
    auto bad = ValidInput();
    bad["status"] = "done";
    const auto r = fx.Run(bad);
    CHECK(r.is_error);
    CHECK(r.error_code == "schema.rejected");
}

TEST_CASE("blocked 缺 blocker_key / needs_user 缺 question 拒") {
    Fixture fx;
    auto blocked = ValidInput();
    blocked["status"] = "blocked";
    const auto r1 = fx.Run(blocked);
    CHECK(r1.is_error);
    CHECK(r1.error_code == "goal.checkpoint_blocker_missing");
    CHECK_FALSE(fx.state->HasCheckpoint());  // 拒的不入账

    auto needs_user = ValidInput();
    needs_user["status"] = "needs_user";
    const auto r2 = fx.Run(needs_user);
    CHECK(r2.is_error);
    CHECK(r2.error_code == "goal.checkpoint_question_missing");
}

TEST_CASE("evidence 白名单:清单外的 id 拒") {
    Fixture fx;
    auto foreign = ValidInput();
    foreign["evidence_ids"] = nlohmann::json::array({"ev-999"});
    const auto r = fx.Run(foreign);
    CHECK(r.is_error);
    CHECK(r.error_code == "goal.evidence_unknown");
    CHECK(r.content.find("ev-999") != std::string::npos);

    // 清单内多枚全过。
    auto ok = ValidInput();
    ok["evidence_ids"] = nlohmann::json::array({"ev-1", "ev-2"});
    CHECK_FALSE(fx.Run(ok).is_error);
}

TEST_CASE("schema:缺必填、类型不对、上限") {
    Fixture fx;
    for (const char* drop : {"status", "summary", "completed", "remaining", "next_action", "evidence_ids"}) {
        auto input = ValidInput();
        input.erase(drop);
        const auto r = fx.Run(input);
        CHECK(r.is_error);
    }
    auto wrong_type = ValidInput();
    wrong_type["completed"] = "not-array";
    CHECK(fx.Run(wrong_type).is_error);
    auto over = ValidInput();
    over["summary"] = std::string(1201, 'x');
    const auto r = fx.Run(over);
    CHECK(r.is_error);
    CHECK(r.error_code == "schema.rejected");
}

TEST_CASE("普通轮旁路:goal_id 空 → 明确拒,不装成功") {
    GoalCheckpointState empty;
    GoalCheckpointTool tool{std::make_shared<GoalCheckpointState>()};
    const auto r = tool.execute(ValidInput());
    CHECK(r.is_error);
    CHECK(r.error_code == "goal.not_in_goal_turn");
}

TEST_CASE("schema()/name/description:工具合同") {
    Fixture fx;
    CHECK(fx.tool.name() == "goal_checkpoint");
    CHECK_FALSE(fx.tool.description().empty());
    const nlohmann::json schema = fx.tool.input_schema();
    CHECK(schema.at("additionalProperties") == false);
    CHECK(schema.at("required").size() == 6);
    CHECK(schema.at("properties").at("status").at("enum").size() == 4);
    // 无项目副作用:只读本地档。
    CHECK(fx.tool.needs_confirm() == false);
}
