// FrictionClassifier 的正反例册(Token 账本单 §15.5/A4):每类可判摩擦
// 有正例;单事件可挂多类;判不了的不硬猜;assistant 自称完成不算 passed。
#include <doctest/doctest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "insights/friction_classifier.hpp"

using namespace lubancode;
using namespace lubancode::insights;
using trajectory::EventEnvelope;
using trajectory::EventKind;

namespace {

EventEnvelope Envelope(EventKind kind, const std::string& turn, const std::string& call,
                       nlohmann::json payload) {
    EventEnvelope envelope;
    envelope.workspace_key = "ws";
    envelope.session_id = "s";
    envelope.run_id = "main-0001";
    envelope.kind = kind;
    envelope.turn_id = turn;
    envelope.call_id = call;
    envelope.event_id = "main-0001:evt-00000001";
    envelope.payload = std::move(payload);
    return envelope;
}

bool HasCategory(const std::vector<FrictionOccurrence>& occurrences, const std::string& category) {
    return std::any_of(occurrences.begin(), occurrences.end(),
                       [&](const FrictionOccurrence& o) { return o.category == category; });
}

std::size_t CountCategory(const std::vector<FrictionOccurrence>& occurrences,
                          const std::string& category) {
    std::size_t count = 0;
    for (const auto& o : occurrences) {
        if (o.category == category) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("类名表:§9.3 十六类全在册(在册不等于本版能判)") {
    const std::vector<std::string>& categories = AllFrictionCategories();
    REQUIRE(categories.size() == 16);
    CHECK(std::is_sorted(categories.begin(), categories.end()));
    CHECK(std::find(categories.begin(), categories.end(), "verification.missing") !=
          categories.end());
    CHECK(std::find(categories.begin(), categories.end(), "tool.repeated_retry") !=
          categories.end());
    CHECK(std::find(categories.begin(), categories.end(), "unknown") != categories.end());
}

TEST_CASE("工具失败:普通失败/参数错/权限拒,各自归类;理由文本只截 80 字") {
    std::vector<EventEnvelope> events;
    events.push_back(Envelope(EventKind::ToolExecutionFailed, "turn-1", "call-1",
                              nlohmann::json{{"tool_name", "read_file"},
                                             {"reason", "missing_file"}}));
    events.push_back(Envelope(EventKind::ToolExecutionFailed, "turn-1", "call-2",
                              nlohmann::json{{"tool_name", "edit_file"},
                                             {"reason", "invalid arguments for schema"}}));
    events.push_back(Envelope(EventKind::ToolExecutionFailed, "turn-1", "call-3",
                              nlohmann::json{{"tool_name", "run_command"},
                                             {"reason", "permission denied by policy"}}));
    const std::vector<FrictionOccurrence> occurrences = ClassifyFriction(events);
    CHECK(HasCategory(occurrences, "tool.execution_failure"));
    CHECK(HasCategory(occurrences, "tool.invalid_input"));
    CHECK(HasCategory(occurrences, "permission.denied"));
    // 同 turn 同工具失败两次才挂 repeated_retry;这里三个不同工具,不该有。
    CHECK(!HasCategory(occurrences, "tool.repeated_retry"));
    // 证据回引事件。
    for (const auto& occurrence : occurrences) {
        CHECK(occurrence.evidence.event_id.has_value());
        CHECK(!occurrence.rule_version.empty());
    }
}

TEST_CASE("同 turn 同工具失败两次:重复重试要报,且第二笔同时挂执行失败") {
    std::vector<EventEnvelope> events;
    for (int i = 0; i < 2; ++i) {
        events.push_back(Envelope(EventKind::ToolExecutionFailed, "turn-1", "call-" + std::to_string(i),
                                  nlohmann::json{{"tool_name", "run_command"},
                                                 {"reason", "timeout"}}));
    }
    const std::vector<FrictionOccurrence> occurrences = ClassifyFriction(events);
    CHECK(CountCategory(occurrences, "tool.execution_failure") == 2);
    CHECK(CountCategory(occurrences, "tool.repeated_retry") == 1);
    const auto retry = std::find_if(occurrences.begin(), occurrences.end(),
                                    [](const FrictionOccurrence& o) {
                                        return o.category == "tool.repeated_retry";
                                    });
    REQUIRE(retry != occurrences.end());
    CHECK(retry->evidence.value.at("failures_in_turn") == 2);
}

TEST_CASE("验证:失败报 verification.failure;passed 无验证报 missing;有验证不报") {
    SUBCASE("outcome=passed 且同 turn 无验证") {
        std::vector<EventEnvelope> events;
        events.push_back(Envelope(EventKind::OutcomeAssessed, "turn-1", "",
                                  nlohmann::json{{"outcome", "passed"}}));
        const auto occurrences = ClassifyFriction(events);
        CHECK(HasCategory(occurrences, "verification.missing"));
    }
    SUBCASE("同 turn 有过验证(先于 outcome 记账)不报 missing") {
        std::vector<EventEnvelope> events;
        events.push_back(Envelope(EventKind::VerificationRecorded, "turn-1", "",
                                  nlohmann::json{{"kind", "build"}, {"passed", true}}));
        events.push_back(Envelope(EventKind::OutcomeAssessed, "turn-1", "",
                                  nlohmann::json{{"outcome", "passed"}}));
        const auto occurrences = ClassifyFriction(events);
        CHECK(!HasCategory(occurrences, "verification.missing"));
    }
    SUBCASE("partial 不自称成功,不算 missing") {
        std::vector<EventEnvelope> events;
        events.push_back(Envelope(EventKind::OutcomeAssessed, "turn-1", "",
                                  nlohmann::json{{"outcome", "partial"}}));
        const auto occurrences = ClassifyFriction(events);
        CHECK(!HasCategory(occurrences, "verification.missing"));
    }
    SUBCASE("验证失败单列") {
        std::vector<EventEnvelope> events;
        events.push_back(Envelope(EventKind::VerificationRecorded, "turn-1", "",
                                  nlohmann::json{{"kind", "ctest"}, {"passed", false}}));
        const auto occurrences = ClassifyFriction(events);
        CHECK(HasCategory(occurrences, "verification.failure"));
        CHECK(!HasCategory(occurrences, "verification.missing"));
    }
}

TEST_CASE("provider 失败/取消/审批等待;判不了的类不硬猜") {
    std::vector<EventEnvelope> events;
    events.push_back(Envelope(EventKind::ModelOutputFailed, "turn-1", "req-1",
                              nlohmann::json{{"reason", "provider_error"}}));
    events.push_back(Envelope(EventKind::ModelOutputCancelled, "turn-2", "req-2",
                              nlohmann::json{{"reason", "user_interrupt"}}));
    events.push_back(Envelope(EventKind::ControlApprovalRequested, "turn-2", "call-9",
                              nlohmann::json{{"tool", "run_command"}}));
    const std::vector<FrictionOccurrence> occurrences = ClassifyFriction(events);
    CHECK(HasCategory(occurrences, "provider.failure"));
    CHECK(HasCategory(occurrences, "cancelled"));
    CHECK(HasCategory(occurrences, "approval.wait"));
    // 语义类没证据就不出:不许为了凑表硬报。
    CHECK(!HasCategory(occurrences, "request.ambiguity"));
    CHECK(!HasCategory(occurrences, "user.correction"));
    CHECK(!HasCategory(occurrences, "context.loss"));
    CHECK(!HasCategory(occurrences, "unknown"));
}

TEST_CASE("空事件流:零摩擦,不冒充") {
    const std::vector<FrictionOccurrence> occurrences = ClassifyFriction({});
    CHECK(occurrences.empty());
}
