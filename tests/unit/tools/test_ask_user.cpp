#include <doctest/doctest.h>

#include "tools/ask_user.hpp"

using lubancode::tools::AskUserQuestion;
using lubancode::tools::AskUserResponse;
using lubancode::tools::AskUserTool;

TEST_CASE("ask_user: 顺次调用 handler 并把单选、多选答案回给模型") {
    int calls = 0;
    AskUserTool tool([&](const AskUserQuestion& question)
                         -> std::expected<AskUserResponse, std::string> {
        ++calls;
        if (question.multi_select) {
            return AskUserResponse::Answered({"Windows", "Linux"});
        }
        return AskUserResponse::Answered({question.options.front().label});
    });
    const nlohmann::json input = {
        {"questions",
         nlohmann::json::array({
             {{"header", "模式"},
              {"question", "选哪种模式?"},
              {"options", nlohmann::json::array({{{"label", "自动"}}, {{"label", "手动"}}})}},
             {{"question", "支持哪些平台?"},
              {"multi_select", true},
              {"options", nlohmann::json::array({{{"label", "Windows"}}, {{"label", "Linux"}}})}},
         })},
    };

    const auto result = tool.execute(input);
    REQUIRE_FALSE(result.is_error);
    CHECK(calls == 2);
    const nlohmann::json output = nlohmann::json::parse(result.content);
    CHECK(output["answers"][0]["answers"] == nlohmann::json::array({"自动"}));
    CHECK(output["answers"][1]["answers"] == nlohmann::json::array({"Windows", "Linux"}));
}

TEST_CASE("ask_user: 坏题目在调用 handler 前拦下") {
    int calls = 0;
    AskUserTool tool([&](const AskUserQuestion&) -> std::expected<AskUserResponse, std::string> {
        ++calls;
        return AskUserResponse::Answered({"x"});
    });

    const auto no_questions = tool.execute(nlohmann::json::object());
    CHECK(no_questions.is_error);

    const nlohmann::json too_few_options = {
        {"questions", nlohmann::json::array({{{"question", "选?"},
                                               {"options", nlohmann::json::array({{{"label", "A"}}})}}})},
    };
    CHECK(tool.execute(too_few_options).is_error);
    CHECK(calls == 0);
}

TEST_CASE("ask_user: 前端故障仍返回错误") {
    AskUserTool tool([](const AskUserQuestion&) -> std::expected<AskUserResponse, std::string> {
        return std::unexpected("用户取消了选择");
    });
    const nlohmann::json input = {
        {"questions", nlohmann::json::array({{{"question", "继续?"},
                                               {"options", nlohmann::json::array({{{"label", "是"}},
                                                                                  {{"label", "否"}}})}}})},
    };
    const auto result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("取消") != std::string::npos);
}

TEST_CASE("ask_user: 用户拒答是正常结果,不是工具错误") {
    AskUserTool tool([](const AskUserQuestion&) -> std::expected<AskUserResponse, std::string> {
        return AskUserResponse::Declined();
    });
    const nlohmann::json input = {
        {"questions", nlohmann::json::array({{{"question", "继续?"},
                                               {"options", nlohmann::json::array({{{"label", "是"}},
                                                                                  {{"label", "否"}}})}}})},
    };

    const auto result = tool.execute(input);
    REQUIRE_FALSE(result.is_error);
    CHECK(nlohmann::json::parse(result.content)["status"] == "declined");
}

TEST_CASE("ask_user: 转为讨论把补充原文交回模型") {
    AskUserTool tool([](const AskUserQuestion&) -> std::expected<AskUserResponse, std::string> {
        return AskUserResponse::Discuss("先说说两种方案的风险");
    });
    const nlohmann::json input = {
        {"questions", nlohmann::json::array({{{"question", "继续?"},
                                               {"options", nlohmann::json::array({{{"label", "是"}},
                                                                                  {{"label", "否"}}})}}})},
    };

    const auto result = tool.execute(input);
    REQUIRE_FALSE(result.is_error);
    const nlohmann::json output = nlohmann::json::parse(result.content);
    CHECK(output["status"] == "discussion");
    CHECK(output["message"] == "先说说两种方案的风险");
}

TEST_CASE("ask_user: 转为讨论却没有补充原文时守门") {
    AskUserTool tool([](const AskUserQuestion&) -> std::expected<AskUserResponse, std::string> {
        return AskUserResponse::Discuss({});
    });
    const nlohmann::json input = {
        {"questions", nlohmann::json::array({{{"question", "继续?"},
                                               {"options", nlohmann::json::array({{{"label", "是"}},
                                                                                  {{"label", "否"}}})}}})},
    };

    const auto result = tool.execute(input);
    CHECK(result.is_error);
    CHECK(result.content.find("补充内容") != std::string::npos);
}
