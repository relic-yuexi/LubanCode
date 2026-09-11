#include <doctest/doctest.h>

#include "agent/tool_batch_budget.hpp"
#include "api/model_input_snapshot.hpp"
#include "hooks/middleware_builtins.hpp"

using namespace lubancode;

namespace {
api::Message Batch(std::initializer_list<std::size_t> sizes) {
    api::Message message;
    message.role = api::Role::User;
    for (auto size : sizes) {
        api::ToolResultBlock result;
        result.tool_use_id = "call-" + std::to_string(message.content.size());
        result.content.assign(size, 'x');
        message.content.push_back(std::move(result));
    }
    return message;
}
}

TEST_CASE("new tool batch shares the remaining 20K without changing source") {
    const auto source = Batch({16000, 16000, 16000, 16000, 16000,
                               16000, 16000, 16000, 16000, 16000});
    const auto plan = agent::PlanToolBatchBudget(source, 20000);
    REQUIRE(plan.error.empty());
    CHECK(plan.reduced);
    CHECK(plan.total_preview_bytes == 20000);
    for (auto bytes : plan.preview_bytes) CHECK(bytes == 2000);
    CHECK(std::get<api::ToolResultBlock>(source.content.front()).content.size() == 16000);
}

TEST_CASE("batch allocator keeps small results whole and redistributes space") {
    const auto plan = agent::PlanToolBatchBudget(Batch({10, 30000, 30000}), 12010);
    REQUIRE(plan.error.empty());
    CHECK(plan.preview_bytes == std::vector<std::size_t>{10, 6000, 6000});
    CHECK(plan.total_preview_bytes == 12010);
}

TEST_CASE("batch allocation has an explicit minimum failure") {
    const auto source = Batch({32768, 32768});
    const auto plan = agent::PlanToolBatchBudget(source, 4096);
    REQUIRE(plan.error.empty());
    CHECK(plan.preview_bytes == std::vector<std::size_t>{2048, 2048});
    CHECK_FALSE(agent::PlanToolBatchBudget(source, 2047).error.empty());
}

TEST_CASE("closed tool group rejects duplicate missing and foreign results") {
    api::Message calls;
    calls.content.push_back(api::ToolUseBlock{"call-0", "read", nlohmann::json::object()});
    calls.content.push_back(api::ToolUseBlock{"call-1", "read", nlohmann::json::object()});
    auto results = Batch({10, 10});
    CHECK(agent::ToolBatchPairingMatches(calls, results));
    std::get<api::ToolResultBlock>(results.content.back()).tool_use_id = "call-0";
    CHECK_FALSE(agent::ToolBatchPairingMatches(calls, results));
    CHECK_FALSE(agent::PlanToolBatchBudget(results, 100).error.empty());
    results.content.pop_back();
    CHECK_FALSE(agent::ToolBatchPairingMatches(calls, results));
}

TEST_CASE("adapter input snapshot counts one escaped UTF-8 total and omits output controls") {
    const nlohmann::json wire = {
        {"model", "not-input"}, {"max_tokens", 8192}, {"stream", true},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", "中文\n\"x\""}}})},
        {"tools", nlohmann::json::array()}};
    const auto snapshot = api::ModelInputSnapshotFromWire(wire.dump());
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot->contains("max_tokens"));
    CHECK_FALSE(snapshot->contains("model"));
    const auto estimate = hooks::middleware::ComputeUtf8BytesDiv4Estimate(*snapshot);
    const auto bytes = snapshot->dump().size();
    CHECK(estimate.at("inputUtf8Bytes").get<std::size_t>() == bytes);
    CHECK(estimate.at("estimatedInputTokens").get<std::size_t>() == bytes / 4 + (bytes % 4 != 0));
}

TEST_CASE("all adapter input families retain their selected input fields") {
    for (const char* key : {"messages", "input", "contents"}) {
        const auto snapshot = api::ModelInputSnapshotFromWire(nlohmann::json{{key, "input"}, {"instructions", "sys"}}.dump());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->contains(key));
        CHECK(snapshot->at("instructions") == "sys");
    }
    CHECK_FALSE(api::ModelInputSnapshotFromWire("").has_value());
    CHECK_FALSE(api::ModelInputSnapshotFromWire("[]").has_value());
}

TEST_CASE("media and opaque reasoning require a separate capacity policy") {
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"type", "image_url"}, {"image_url", "data:image/png;base64,AAAA"}}));
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"inlineData", {{"data", "AAAA"}}}}));
    CHECK(api::HasUnestimatedInput(nlohmann::json{{"type", "thinking"}, {"signature", "opaque"}}));
    CHECK_FALSE(api::HasUnestimatedInput(nlohmann::json{{"type", "tool_use"}, {"input", {{"type", "image"}}}}));
    CHECK_FALSE(api::HasUnestimatedInput(nlohmann::json{{"tools", {{{"type", "image"}}}}}));
}
