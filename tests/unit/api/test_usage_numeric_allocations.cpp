#include <doctest/doctest.h>

#include <charconv>
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
    const auto check_sweep = [&](const char* stage, const char* name) {
        const auto marker = std::string("actual-") + stage + "-allocation-sweep:" + name + ":allocations=";
        const auto offset = result.output.find(marker);
        REQUIRE(offset != std::string::npos);
        CHECK(result.output.find(marker, offset + marker.size()) == std::string::npos);
        const auto start = offset + marker.size();
        const auto split = result.output.find(":faults=", start);
        const auto end = result.output.find('\n', start);
        REQUIRE(split != std::string::npos);
        REQUIRE(end != std::string::npos);
        REQUIRE(split < end);
        std::size_t allocations = 0, faults = 0;
        const auto allocated = std::from_chars(result.output.data() + start, result.output.data() + split, allocations);
        const auto failed = std::from_chars(result.output.data() + split + 8, result.output.data() + end, faults);
        REQUIRE(allocated.ec == std::errc{});
        REQUIRE(allocated.ptr == result.output.data() + split);
        REQUIRE(failed.ec == std::errc{});
        REQUIRE(failed.ptr == result.output.data() + end);
        CHECK(allocations > 0);
        CHECK(allocations <= 10000);
        CHECK(faults == allocations);
    };
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
        check_sweep("parser", name);
        check_sweep("malformed-parser", name);
    }
    check_sweep("typed-owner", "direct");
    check_sweep("typed-owner", "subordinate");
    std::cout << result.output;
}
