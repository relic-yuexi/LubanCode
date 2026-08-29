#include <doctest/doctest.h>

#include "app/commands/workflow_commands.hpp"

TEST_CASE("workflow 面板:候选案从 JSON 投成可读方案并给导航短述") {
    const std::string raw = R"({
      "name":"最小修复",
      "strategy":"只改 add 的返回式。",
      "scope":["build/wf-smoke/calc.py"],
      "impact":["调用方拿到正确和"],
      "acceptance":["三项测试全绿"],
      "unknowns":[],
      "tradeoffs":["不顺手重构"]
    })";

    const auto formatted = lubancode::app::FormatWorkflowPanelOutput(raw);
    CHECK(formatted.structured);
    CHECK(formatted.summary == "最小修复");
    CHECK(formatted.markdown.find("## 最小修复") != std::string::npos);
    CHECK(formatted.markdown.find("### 验收") != std::string::npos);
    CHECK(formatted.markdown.find("三项测试全绿") != std::string::npos);
    CHECK(formatted.markdown.find("\"acceptance\"") == std::string::npos);
}

TEST_CASE("workflow 面板:中书方案书与门下判词各走人话视图") {
    const auto edict = lubancode::app::FormatWorkflowPanelOutput(
        R"({"summary":"修 add，三案全绿","memorial":"## 需求复述\n修好 add。"})");
    CHECK(edict.summary == "修 add，三案全绿");
    CHECK(edict.markdown == "## 需求复述\n修好 add。");

    const auto review = lubancode::app::FormatWorkflowPanelOutput(
        R"({"approved":true,"reasons":["范围清楚","验收可判"],"question":""})");
    CHECK(review.summary == "门下通过");
    CHECK(review.markdown.find("**准**") != std::string::npos);
    CHECK(review.markdown.find("范围清楚") != std::string::npos);
}

TEST_CASE("workflow 面板:用户交办只摆任务与本路立场，不倾倒下游材料") {
    const nlohmann::json input = {
        {"requirement", "修好 calc.py"},
        {"item", "风险优先"},
        {"candidates", nlohmann::json::array({{{"name", "甲"}}, {{"name", "乙"}}})},
    };
    const std::string formatted = lubancode::app::FormatWorkflowPanelInput(input);
    CHECK(formatted.find("**任务：** 修好 calc.py") != std::string::npos);
    CHECK(formatted.find("**本路立场：** 风险优先") != std::string::npos);
    CHECK(formatted.find("candidates") == std::string::npos);
    CHECK(formatted.find("\"name\"") == std::string::npos);
}

TEST_CASE("workflow 收官:有御史报告便直摆报告，不再倾倒整份结果 JSON") {
    const nlohmann::json result = {
        {"dispatches", nlohmann::json::array({{{"ministry", "工部"}}})},
        {"report", "## 复命\n三项测试全绿。"},
        {"review_history", nlohmann::json::array({{{"iteration", 1}}})},
    };
    const std::string formatted = lubancode::app::FormatWorkflowRunResult(result);
    CHECK(formatted == "## 复命\n三项测试全绿。");
    CHECK(formatted.find("dispatches") == std::string::npos);
    CHECK(formatted.find("review_history") == std::string::npos);

    const nlohmann::json ordinary = {{"echo", R"(/chaoting D:\\lubancode\\src)"}};
    CHECK(lubancode::app::FormatWorkflowRunResult(ordinary) == ordinary.dump(2));
}
