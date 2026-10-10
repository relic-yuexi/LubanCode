#include <doctest/doctest.h>

#include <limits>
#include <string>
#include <utility>

#include "api/usage_totals.hpp"
#include "runtime/headless_progress.hpp"
#include "runtime/turn_runtime.hpp"

using namespace lubancode;

TEST_CASE("Headless usage retains overflow uncertainty across later valid reports") {
    std::string log;
    runtime::HeadlessProgressReporter reporter(
        [&](const std::string& line) { log += line + "\n"; }, "test", "model");
    const auto maximum = (std::numeric_limits<std::int64_t>::max)();
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::UsageUpdated;
    event.payload = {{"reported_by_provider", true}, {"input_tokens", maximum},
        {"output_tokens", maximum}, {"cache_read_tokens", 0},
        {"cache_creation_tokens", 0}, {"cache_read_reported_by_provider", true},
        {"cache_creation_reported_by_provider", true}};
    reporter.Observe(event);
    CHECK(log.find("输入=" + std::to_string(maximum)) != std::string::npos);
    event.payload["input_tokens"] = 1;
    event.payload["output_tokens"] = 1;
    event.payload["cache_read_tokens"] = maximum;
    event.payload["cache_creation_tokens"] = maximum;
    const auto source = event.payload;
    reporter.Observe(event);
    CHECK(event.payload == source);
    CHECK(log.find("输入=未知（溢出） 输出=1") != std::string::npos);
    CHECK(log.find("命中率=未知（溢出）") != std::string::npos);
    event.payload["input_tokens"] = 0;
    event.payload["output_tokens"] = 0;
    event.payload["cache_read_tokens"] = 1;
    event.payload["cache_creation_tokens"] = 1;
    reporter.Observe(event);
    CHECK(log.find("输入=2 输出=0 缓存读=1 缓存写=1") != std::string::npos);
    event.kind = runtime::ServerEventKind::TurnCompleted;
    reporter.Observe(event);
    CHECK(log.find("累计输入=未知（溢出） 累计输出=未知（溢出）") != std::string::npos);
    CHECK(log.find("缓存读合计=未知（溢出）") != std::string::npos);
    CHECK(log.find("缓存写合计=未知（溢出）") != std::string::npos);
}

TEST_CASE("Checked input projection preserves all five source numbers and intermediate overflow") {
    api::Usage usage;
    usage.input_tokens = 100;
    usage.output_tokens = 4;
    usage.cache_read_tokens = 50;
    usage.cache_creation_tokens = 10;
    usage.output_reasoning_tokens = 9;
    CHECK(api::CheckedTotalInputTokens(usage) == 160);
    CHECK(usage.input_tokens == 100);
    CHECK(usage.output_tokens == 4);
    CHECK(usage.cache_read_tokens == 50);
    CHECK(usage.cache_creation_tokens == 10);
    CHECK(usage.output_reasoning_tokens == 9);
    usage.input_tokens = (std::numeric_limits<std::int64_t>::max)();
    usage.cache_read_tokens = 1;
    usage.cache_creation_tokens = -1;
    CHECK_FALSE(api::CheckedTotalInputTokens(usage).has_value());
    usage.input_tokens = (std::numeric_limits<std::int64_t>::min)();
    usage.cache_read_tokens = -1;
    usage.cache_creation_tokens = 1;
    CHECK_FALSE(api::CheckedTotalInputTokens(usage).has_value());
    usage.input_tokens = 0;
    usage.cache_read_tokens = 0;
    usage.cache_creation_tokens = 0;
    CHECK(api::CheckedTotalInputTokens(usage) == 0);
}

TEST_CASE("Headless ordinal and wide context windows do not wrap into valid counts") {
    std::string log;
    runtime::HeadlessProgressReporter reporter(
        [&](const std::string& line) { log += line + "\n"; }, "test", "model");
    runtime::ServerEvent event;
    event.kind = runtime::ServerEventKind::ModelStepStarted;
    event.payload = {{"step_index", (std::numeric_limits<std::int64_t>::max)()}};
    reporter.Observe(event);
    CHECK(log.find("step=未知（溢出）") != std::string::npos);
    if constexpr (std::numeric_limits<std::size_t>::digits > 63) {
        agent::ContextPressure pressure;
        pressure.phase = agent::ContextPressure::Phase::PreRequest;
        pressure.window_tokens = (std::numeric_limits<std::size_t>::max)();
        pressure.projected_tokens = pressure.window_tokens;
        reporter.Context(pressure);
        CHECK(log.find("占用=未知（溢出）") != std::string::npos);
    }
}

TEST_CASE("Actual PostStep command hooks receive raw five-field facts and explicit overflow uncertainty") {
    for (const std::string mode : {"post-step-normal", "post-step-overflow", "post-step-zero"}) {
        lubancode::hooks::HookDefinition definition;
        definition.id = 1;
        definition.event = lubancode::hooks::HookEvent::PostStep;
        definition.trusted = true;
        definition.handler.command = LUBANCORE_TEST_USAGE_NUMERIC_FAULT_PROBE;
        definition.handler.args = {mode};
        definition.definition_hash = "actual-post-step-usage";
        lubancode::hooks::LoadedHooks loaded;
        loaded.definitions.push_back(std::move(definition));
        auto [trust, error] = lubancode::hooks::HookTrustStore::Load(std::nullopt);
        REQUIRE_FALSE(error.has_value());
        lubancode::hooks::HookDispatcher dispatcher;
        dispatcher.Configure(std::move(loaded), std::move(trust), {});
        api::UsageReport report;
        report.step_id = "step-checked";
        report.turn_id = "turn-checked";
        report.reported_by_provider = true;
        if (mode == "post-step-normal") report.usage = api::Usage{100,7,50,10,9};
        else if (mode == "post-step-overflow")
            report.usage = api::Usage{(std::numeric_limits<std::int64_t>::max)(),7,1,-1,9};
        runtime::EmitPostStep(&dispatcher, report);
        const auto* record = dispatcher.LastRecordFor(1);
        REQUIRE(record);
        INFO(mode); INFO(record->detail);
        CHECK(record->outcome == "ok");
        CHECK(record->exit_code == 0);
        CHECK(report.usage.output_tokens == (mode == "post-step-zero" ? 0 : 7));
        CHECK(report.usage.output_reasoning_tokens == (mode == "post-step-zero" ? 0 : 9));
    }
}
