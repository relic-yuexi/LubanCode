#include <doctest/doctest.h>

#include <iostream>
#include <string>
#include <vector>

#include "platform/process.hpp"

TEST_CASE("Production provider material allocation faults preserve the preceding numeric owner") {
    const auto result = lubancode::platform::RunProcess(
        std::vector<std::string>{LUBANCORE_TEST_USAGE_NUMERIC_FAULT_PROBE}, 30000);
    INFO(result.output); INFO(result.spawn_error);
    REQUIRE_FALSE(result.spawn_failed); REQUIRE_FALSE(result.timed_out);
    REQUIRE_FALSE(result.cancelled); REQUIRE_FALSE(result.output_truncated);
    REQUIRE(result.exit_code == 0);
    for (const char* name : {"chat", "responses", "anthropic", "gemini"}) {
        const auto marker = std::string("actual-material-allocation-fault:") + name;
        const auto offset = result.output.find(marker);
        REQUIRE(offset != std::string::npos);
        CHECK(result.output.find(marker, offset + marker.size()) == std::string::npos);
    }
    for (const char* name : {"chat", "responses", "anthropic", "gemini", "responses-nonstream"}) {
        for (const char* stage : {"lexical", "parser"}) {
            const auto marker = std::string("actual-") + stage + "-allocation-fault:" + name + "\n";
            const auto offset = result.output.find(marker);
            REQUIRE(offset != std::string::npos);
            CHECK(result.output.find(marker, offset + marker.size()) == std::string::npos);
        }
    }
    std::cout << result.output;
}
