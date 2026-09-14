// assistant 子命令的解析册(常驻助理 Web 主界面单 W1,§四):
// 形状对错当场分岔,不静默当普通位置参数走单发问句。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app/cli_options.hpp"

using namespace lubancode::app;

namespace {

ParsedCliArgs Parse(std::vector<std::string> args) {
    args.insert(args.begin(), "lubancode");
    return ParseCliArgs(args);
}

}  // namespace

TEST_CASE("assistant 解析:裸词 + 三枚旗标") {
    SUBCASE("裸 assistant") {
        const ParsedCliArgs parsed = Parse({"assistant"});
        REQUIRE(parsed.action == CliAction::RunAssistant);
        CHECK_FALSE(parsed.assistant.no_open);
        CHECK(parsed.assistant.port == 0);
        CHECK_FALSE(parsed.assistant.port_given);
        CHECK(parsed.assistant.profile.empty());
    }
    SUBCASE("--no-open") {
        const ParsedCliArgs parsed = Parse({"assistant", "--no-open"});
        REQUIRE(parsed.action == CliAction::RunAssistant);
        CHECK(parsed.assistant.no_open);
    }
    SUBCASE("--port 8765 --profile default") {
        const ParsedCliArgs parsed = Parse({"assistant", "--port", "8765", "--profile", "default"});
        REQUIRE(parsed.action == CliAction::RunAssistant);
        CHECK(parsed.assistant.port == 8765);
        CHECK(parsed.assistant.port_given);
        CHECK(parsed.assistant.profile == "default");
    }
    SUBCASE("旗标次序不敏感") {
        const ParsedCliArgs parsed =
            Parse({"assistant", "--profile", "work", "--no-open", "--port", "9001"});
        REQUIRE(parsed.action == CliAction::RunAssistant);
        CHECK(parsed.assistant.profile == "work");
        CHECK(parsed.assistant.no_open);
        CHECK(parsed.assistant.port == 9001);
    }
}

TEST_CASE("assistant 解析:缺值/坏值/认不得当场退用法") {
    SUBCASE("--port 缺值") {
        const ParsedCliArgs parsed = Parse({"assistant", "--port"});
        REQUIRE(parsed.action == CliAction::BadAssistant);
        CHECK(parsed.error_text.find("--port") != std::string::npos);
    }
    SUBCASE("--port 非数字/越界") {
        for (const std::string& bad : {"abc", "0", "65536", "99999"}) {
            const ParsedCliArgs parsed = Parse({"assistant", "--port", bad});
            REQUIRE(parsed.action == CliAction::BadAssistant);
        }
    }
    SUBCASE("--profile 缺值") {
        const ParsedCliArgs parsed = Parse({"assistant", "--profile"});
        REQUIRE(parsed.action == CliAction::BadAssistant);
    }
    SUBCASE("认不得的旗标") {
        const ParsedCliArgs parsed = Parse({"assistant", "--host", "0.0.0.0"});
        REQUIRE(parsed.action == CliAction::BadAssistant);
        CHECK(parsed.error_text.find("--host") != std::string::npos);
        CHECK(parsed.error_text.find("只认 --no-open") != std::string::npos);
    }
}

TEST_CASE("assistant 解析:前面有位置参数时不抢道") {
    // "问一句 assistant" 是单发问句,不是子命令(与 app-server 同规矩)。
    const ParsedCliArgs parsed = Parse({"一句话", "assistant"});
    CHECK(parsed.action == CliAction::Proceed);
    CHECK(parsed.options.positional.find("assistant") != std::string::npos);
}

TEST_CASE("assistant 解析:--version 出现在前早退(扫描次序老规矩)") {
    const ParsedCliArgs parsed = Parse({"--version", "assistant"});
    CHECK(parsed.action == CliAction::PrintVersion);
}
