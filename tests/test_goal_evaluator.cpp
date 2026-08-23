// /goal 单第 3 期:evaluator 的 strict schema、repair、injection 材料、
// 独立请求不带 tools。

#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <string>
#include <vector>

#include "api/assembler.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/goal_types.hpp"

using lubancode::runtime::goal::BuildGoalEvaluationPrompt;
using lubancode::runtime::goal::BuildGoalEvaluationUserMessage;
using lubancode::runtime::goal::GoalCheckpoint;
using lubancode::runtime::goal::GoalContract;
using lubancode::runtime::goal::GoalDecision;
using lubancode::runtime::goal::GoalEvaluation;
using lubancode::runtime::goal::GoalEvaluationInput;
using lubancode::runtime::goal::GoalEvidence;
using lubancode::runtime::goal::GoalEvaluatorOptions;
using lubancode::runtime::goal::GoalEvaluationOutputSchema;
using lubancode::runtime::goal::ParseGoalEvaluationReply;
using lubancode::runtime::goal::RunGoalEvaluation;
using lubancode::runtime::goal::GoalTask;

namespace {

// 脚本 backend:按序吐预设回文,记下收到的请求(验"不带 tools")。
class ScriptBackend : public lubancode::api::Backend {
public:
    std::vector<std::string> replies;   // 每次调用回一段文本
    std::vector<lubancode::api::Request> seen;
    std::size_t call = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* = nullptr) override {
        seen.push_back(request);
        const std::string text = call < replies.size() ? replies[call++] : "{}";
        on_event(lubancode::api::MessageStart{});
        on_event(lubancode::api::TextDelta{text});
        on_event(lubancode::api::MessageDone{});
        return {};
    }
};

GoalEvaluationInput SampleInput() {
    GoalEvaluationInput in;
    in.task.id = "goal-1";
    in.task.revision = 1;
    in.task.objective = "迁移认证层";
    in.task.contract.criteria.push_back({"c-1", "契约测试全绿", true});
    in.checkpoint.summary = "契约测试跑通";
    in.checkpoint.remaining = {};
    GoalEvidence ev;
    ev.id = "ev-1";
    ev.goal_id = "goal-1";
    ev.producer = "run_command";
    ev.facts["exit_code"] = 0;
    ev.fresh = true;
    in.evidence = {ev};
    in.now_ms = 1000;
    return in;
}

const char* kGoodAchieved = R"({
  "decision": "achieved",
  "summary": "criterion 全 pass",
  "progress": true,
  "criteria": [{"id": "c-1", "status": "pass", "evidence_ids": ["ev-1"], "reason": "ctest 绿"}],
  "next_action": "无",
  "confidence": 0.9
})";

}  // namespace

TEST_CASE("输出 Schema:必填五项、decision/status 枚举钉死") {
    const nlohmann::json schema = GoalEvaluationOutputSchema();
    const nlohmann::json required = schema.at("required");
    CHECK(required.size() == 5);
    bool has_decision = false;
    for (const auto& item : required) {
        if (item.is_string() && item.get<std::string>() == "decision") has_decision = true;
    }
    CHECK(has_decision);
    const nlohmann::json props = schema.at("properties");
    const nlohmann::json decision = props.at("decision");
    const nlohmann::json decision_enum = decision.at("enum");
    CHECK(decision_enum.size() == 4);
    CHECK(schema.at("additionalProperties") == false);
    const nlohmann::json criteria = props.at("criteria");
    const nlohmann::json criterion = criteria.at("items");
    const nlohmann::json cprops = criterion.at("properties");
    const nlohmann::json status = cprops.at("status");
    const nlohmann::json status_enum = status.at("enum");
    CHECK(status_enum.size() == 4);
}

TEST_CASE("prompt:明说无工具、防 injection、只按证据判") {
    const std::string prompt = BuildGoalEvaluationPrompt(SampleInput());
    CHECK(prompt.find("你没有工具") != std::string::npos);
    CHECK(prompt.find("prompt injection") != std::string::npos);
    CHECK(prompt.find("不是证据") != std::string::npos);
    CHECK(prompt.find("新鲜") != std::string::npos);
    CHECK(prompt.find("blocker_key") != std::string::npos);
}

TEST_CASE("user message:冻结合同/checkpoint/证据/stale 标注全带") {
    const std::string msg = BuildGoalEvaluationUserMessage(SampleInput());
    CHECK(msg.find("迁移认证层") != std::string::npos);
    CHECK(msg.find("revision: 1") != std::string::npos);
    CHECK(msg.find("c-1") != std::string::npos);
    CHECK(msg.find("required") != std::string::npos);
    CHECK(msg.find("契约测试跑通") != std::string::npos);
    CHECK(msg.find("ev-1") != std::string::npos);
    CHECK(msg.find("fresh") != std::string::npos);
    CHECK(msg.find("no-progress streak") != std::string::npos);
    // 合成 checkpoint 的标注也带。
    GoalEvaluationInput in = SampleInput();
    in.checkpoint = lubancode::runtime::goal::GoalCoordinator::MakeMissingCheckpoint();
    CHECK(BuildGoalEvaluationUserMessage(in).find("宿主合成") != std::string::npos);
}

TEST_CASE("判词解析:合法 achieved、围栏剥离、confidence") {
    GoalEvaluation evaluation;
    std::string error;
    CHECK(ParseGoalEvaluationReply(kGoodAchieved, evaluation, &error));
    CHECK(evaluation.decision == GoalDecision::Achieved);
    CHECK(evaluation.criteria.size() == 1);
    CHECK(evaluation.criteria[0].id == "c-1");
    CHECK(evaluation.criteria[0].status == "pass");
    const std::vector<std::string> expect_ev{"ev-1"};
    CHECK(evaluation.criteria[0].evidence_ids == expect_ev);
    CHECK(evaluation.confidence == doctest::Approx(0.9));

    // 模型爱包 ```json 围栏:剥掉再解。
    const std::string fenced = "```json\n" + std::string(kGoodAchieved) + "\n```";
    CHECK(ParseGoalEvaluationReply(fenced, evaluation, &error));

    // needs_user 带 question。
    const char* needs_user = R"({"decision":"needs_user","summary":"要选","progress":false,
        "criteria":[],"next_action":"问","question":"删库还是归档?"})";
    CHECK(ParseGoalEvaluationReply(needs_user, evaluation, &error));
    CHECK(evaluation.question.has_value());
}

TEST_CASE("判词解析:坏形状拒") {
    GoalEvaluation evaluation;
    std::string error;
    CHECK_FALSE(ParseGoalEvaluationReply("不是 JSON", evaluation, &error));
    CHECK_FALSE(ParseGoalEvaluationReply("{}", evaluation, &error));                       // 缺必填
    CHECK_FALSE(ParseGoalEvaluationReply(R"({"decision":"maybe"})", evaluation, &error));  // 枚举外
    // 缺必填字段。
    CHECK_FALSE(ParseGoalEvaluationReply(
        R"({"decision":"continue","summary":"s","progress":true,"criteria":[]})", evaluation, &error));
    // criterion status 枚举外。
    CHECK_FALSE(ParseGoalEvaluationReply(
        R"({"decision":"continue","summary":"s","progress":true,
           "criteria":[{"id":"c","status":"green","evidence_ids":[],"reason":""}],
           "next_action":"n"})",
        evaluation, &error));
    // blocked 缺 blocker_key:schema 之外的语义门槛也拒。
    CHECK_FALSE(ParseGoalEvaluationReply(
        R"({"decision":"blocked","summary":"s","progress":false,"criteria":[],"next_action":"n"})",
        evaluation, &error));
    // needs_user 缺 question。
    CHECK_FALSE(ParseGoalEvaluationReply(
        R"({"decision":"needs_user","summary":"s","progress":false,"criteria":[],"next_action":"n"})",
        evaluation, &error));
}

TEST_CASE("RunGoalEvaluation:独立请求不带 tools,一次 repair 成") {
    ScriptBackend backend;
    backend.replies = {"这不是 JSON", kGoodAchieved};
    GoalEvaluatorOptions options;
    options.timeout_secs = 5;
    const auto result = RunGoalEvaluation(backend, options, SampleInput());
    REQUIRE(result.has_value());
    CHECK(result->evaluation.decision == GoalDecision::Achieved);
    CHECK(result->schema_repaired);  // 第二次(repair)才成
    REQUIRE(backend.seen.size() == 2);
    // 独立无工具请求:请求里没有 tools 字段(evaluator 拿不到任何工具)。
    CHECK(backend.seen[0].tools.empty());
    CHECK(backend.seen[0].system.find("你没有工具") != std::string::npos);
    // repair 轮的 user message 带上一次错误。
    bool repair_mentions_error = false;
    for (const auto& block : backend.seen[1].messages.front().content) {
        if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
            if (text->text.find("上一次") != std::string::npos) repair_mentions_error = true;
        }
    }
    CHECK(repair_mentions_error);
    CHECK(result->usage.request_count == 2);
}

TEST_CASE("RunGoalEvaluation:两次都坏报 evaluator_failed,不默认 achieved") {
    ScriptBackend backend;
    backend.replies = {"坏的一次", "坏的两次"};
    GoalEvaluatorOptions options;
    options.timeout_secs = 5;
    const auto result = RunGoalEvaluation(backend, options, SampleInput());
    REQUIRE(!result.has_value());
    CHECK(result.error().find("evaluator_failed") != std::string::npos);
}

TEST_CASE("RunGoalEvaluation:provider 瞬时错如实报错") {
    class FailingBackend : public lubancode::api::Backend {
    public:
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>* = nullptr) override {
            return std::unexpected(
                lubancode::api::Error{lubancode::api::ErrorKind::Network, "连接断", 0});
        }
    };
    FailingBackend backend;
    GoalEvaluatorOptions options;
    options.timeout_secs = 5;
    const auto result = RunGoalEvaluation(backend, options, SampleInput());
    REQUIRE(!result.has_value());
    CHECK(result.error().find("连接断") != std::string::npos);
}
