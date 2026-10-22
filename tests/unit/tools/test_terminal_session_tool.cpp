// TerminalSessionTool 的合同面单测(不起进程):名字、审批档、schema、
// 入参校验;Windows 上一律回"仅 POSIX"。真 PTY 用例另在
// tests/integration/process/test_terminal_session_process.cpp。
#include <doctest/doctest.h>

#include <algorithm>
#include <string>

#include <nlohmann/json.hpp>

#include "tools/terminal_session_tool.hpp"

using lubancode::tools::ApprovalClass;
using lubancode::tools::EffectClass;
using lubancode::tools::TerminalSessionTool;

TEST_CASE("TerminalSessionTool: 名字与审批档同 run_command") {
    TerminalSessionTool tool;
    CHECK(tool.name() == "terminal");
    CHECK(tool.needs_confirm());
    CHECK(tool.approval_class() == ApprovalClass::Command);
    CHECK(tool.effect_class() == EffectClass::LocalProcessUnknown);
    CHECK_FALSE(tool.description().empty());
}

TEST_CASE("TerminalSessionTool: schema 以 action 分派且 action 必填") {
    TerminalSessionTool tool;
    const nlohmann::json schema = tool.input_schema();
    REQUIRE(schema.contains("properties"));
    const auto& props = schema.at("properties");
    REQUIRE(props.contains("action"));
    REQUIRE(props.at("action").contains("enum"));
    const auto& actions = props.at("action").at("enum");
    for (const char* a : {"open", "send", "read", "close", "list"}) {
        CAPTURE(a);
        CHECK(std::find(actions.begin(), actions.end(), a) != actions.end());
    }
    for (const char* p : {"session_id", "keys", "literal", "wait_ms", "cwd", "rows", "cols", "history_lines"}) {
        CAPTURE(p);
        CHECK(props.contains(p));
    }
    REQUIRE(schema.contains("required"));
    CHECK(schema.at("required") == nlohmann::json::array({"action"}));
}

#ifdef _WIN32

TEST_CASE("TerminalSessionTool: Windows 上回仅 POSIX") {
    TerminalSessionTool tool;
    const auto result = tool.execute(nlohmann::json{{"action", "open"}});
    CHECK(result.is_error);
    CHECK(result.content.find("POSIX") != std::string::npos);
}

#else

TEST_CASE("TerminalSessionTool: 入参校验不起进程") {
    TerminalSessionTool tool;
    CHECK(tool.execute(nlohmann::json::object()).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "bogus"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "send"}, {"keys", "x"}}).is_error);  // 缺 session_id

    const auto missing = tool.execute(nlohmann::json{{"action", "read"}, {"session_id", 987654}});
    CHECK(missing.is_error);
    CHECK(missing.content.find("987654") != std::string::npos);

    CHECK(tool.execute(nlohmann::json{{"action", "open"}, {"rows", "many"}}).is_error);
    CHECK(tool.execute(nlohmann::json{{"action", "open"}, {"cwd", "/definitely/not/a/dir/xyz"}}).is_error);
}

#endif
