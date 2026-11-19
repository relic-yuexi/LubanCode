#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <iostream>
#include <memory>
#include <new>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include "agent/sample_model.hpp"
#include "api/assembler.hpp"
#include "api/usage_aggregation.hpp"
#include "api/usage_event_projection.hpp"
#include "sdk/adapters.hpp"
#include "sdk/backend_owner_test_hooks.hpp"
#include "sdk/sampling_test_hooks.hpp"
#include "sdk/usage_result.hpp"
#include "runtime/turn_event_adapter.hpp"

namespace {
namespace api = lubancode::api;
namespace sdk = lubancore;
namespace agent = lubancode::agent;
namespace facts = lubancore::usage::v1;
void Mark(const char* path) { std::cout << "[sdk-model-sampling-path] " << path << '\n'; }
struct Capture final : sdk::Backend {
    std::vector<sdk::ModelRequest> requests;
    sdk::ModelReply reply{R"({"summary":"actual sample"})", {}, sdk::Usage{11, 7}};
    enum class Fault { None, Standard, Unknown, Cancel } fault = Fault::None;
    std::atomic<bool>* cancellation = nullptr;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        requests.push_back(request);
        if (fault == Fault::Standard) throw std::runtime_error("sample fixture");
        if (fault == Fault::Unknown) throw 17;
        if (fault == Fault::Cancel) cancellation->store(true);
        return reply;
    }
};
std::vector<std::string> InvalidValues() {
    return {std::string("a\0b",3), std::string(1,static_cast<char>(0xff)),
            std::string(257,'x'), std::string(129,'\xc3')};
}

enum class ReplyBoundaryFault { Standard, Allocation, Unknown };
[[noreturn]] void ThrowReplyBoundaryFault(ReplyBoundaryFault fault) {
    if (fault == ReplyBoundaryFault::Standard) throw std::runtime_error("reply boundary");
    if (fault == ReplyBoundaryFault::Allocation) throw std::bad_alloc{};
    throw 17;
}

TEST_CASE("SDK five-field owner: typed attempt receipt survives later event observer faults without double charge") {
    for (const auto fault : {ReplyBoundaryFault::Standard, ReplyBoundaryFault::Allocation,
                             ReplyBoundaryFault::Unknown}) {
        sdk::OperationUsage owned;
        auto material = api::usage_wire::LegacyBackend(api::Usage{17, 9, 11, 13, 2}); REQUIRE(material);
        lubancode::runtime::IdAuthority ids;
        lubancode::runtime::TurnEventAdapter events("owner-fixture", ids);
        int typed = 0, serialized = 0;
        events.ObserveUsage([&](const api::Usage& usage, const facts::Observation* observation, bool subordinate, bool incomplete,
                                const lubancode::runtime::UsageAttemptContext&) {
            ++typed; sdk::detail::usage_result::CaptureTyped(owned, usage, observation, subordinate, incomplete);
        });
        events.Attach([&](const lubancode::runtime::ServerEvent& event) {
            if (event.kind != lubancode::runtime::ServerEventKind::UsageUpdated) return;
            ++serialized; CHECK(event.usage_observed);
            if (!event.usage_observed) sdk::detail::usage_result::Capture(owned, event.payload);
            ThrowReplyBoundaryFault(fault);
        });
        const bool observed = events.OnUsageFacts(api::usage_wire::Numbers(*material), &material->observation);
        REQUIRE(observed);
        CHECK(owned.direct.coverage.samples == 1); CHECK(serialized == 0);
        api::UsageReport report;
        report.usage = api::usage_wire::Numbers(*material); report.usage_observation = material->observation;
        bool threw = false;
        try { events.OnUsage(report, false, observed); } catch (...) { threw = true; }
        CHECK(threw); CHECK(typed == 1); CHECK(serialized == 1);
        CHECK(owned.direct.coverage.samples == 1); CHECK(owned.subordinate.coverage.samples == 0);
        CHECK(owned.direct.total.input_tokens == 17); CHECK(owned.direct.total.output_tokens == 9);
        CHECK(owned.direct.total.cache_read_tokens == 11); CHECK(owned.direct.total.cache_creation_tokens == 13);
        CHECK(owned.direct.total.output_reasoning_tokens == 2);
        CHECK(api::usage_aggregation::Exact(owned.direct.coverage, facts::Field::Input));
        CHECK_FALSE(api::usage_aggregation::Exact(owned.direct.coverage, facts::Field::CacheRead));
    }
}

TEST_CASE("SDK five-field owner: a forwarded receipt never suppresses the parent subordinate bill") {
    sdk::OperationUsage owned;
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnEventAdapter events("owner-fixture", ids);
    std::optional<lubancode::runtime::ServerEvent> child_event;
    events.ObserveUsage([&](const api::Usage& usage, const facts::Observation* observation, bool subordinate, bool incomplete,
                            const lubancode::runtime::UsageAttemptContext&) {
        sdk::detail::usage_result::CaptureTyped(owned, usage, observation, subordinate, incomplete);
    });
    events.Attach([&](const lubancode::runtime::ServerEvent& event) {
        if (event.kind != lubancode::runtime::ServerEventKind::UsageUpdated) return;
        if (!event.usage_observed) sdk::detail::usage_result::Capture(owned, event.payload);
        else child_event = event;
    });
    api::UsageReport report; report.usage = api::Usage{3, 7, 11, 13, 2};
    events.OnUsage(report);
    REQUIRE(child_event); CHECK(child_event->usage_observed);
    CHECK(owned.direct.coverage.samples == 1); CHECK(owned.subordinate.coverage.samples == 0);
    events.ForwardFromSubordinate(*child_event);
    CHECK(owned.direct.coverage.samples == 1); CHECK(owned.subordinate.coverage.samples == 1);
    CHECK(owned.direct.total.output_tokens == 7); CHECK(owned.subordinate.total.output_tokens == 7);
    CHECK(owned.subordinate.total.output_reasoning_tokens == 2);
}

TEST_CASE("SDK five-field owner: incomplete attempts close precision without counting existing anomalies twice") {
    sdk::OperationUsage owned;
    auto source = api::usage_wire::LegacyBackend(api::Usage{-3, 9, 11, 13, 2}); REQUIRE(source);
    sdk::detail::usage_result::CaptureTyped(owned, api::usage_wire::Numbers(*source), &source->observation, false, true);
    CHECK(owned.direct.coverage.samples == 1); CHECK(owned.direct.total.input_tokens == -3);
    CHECK(owned.direct.total.output_tokens == 9); CHECK(owned.direct.total.output_reasoning_tokens == 2);
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        CHECK(owned.direct.coverage.fields[i].anomalous == 1);
        CHECK_FALSE(api::usage_aggregation::Exact(owned.direct.coverage, static_cast<facts::Field>(i)));
    }
    const auto recovered = sdk::detail::usage_result::Decode(sdk::detail::usage_result::Encode(owned));
    REQUIRE(recovered); CHECK(recovered->direct.coverage.samples == 1);
    CHECK(recovered->direct.coverage.fields[0].anomalous == 1);
    CHECK(recovered->direct.coverage.fields[1].anomalous == 1);
    CHECK(recovered->direct.total.cache_creation_tokens == 13);
}

TEST_CASE("SDK five-field attempts: record capacity stops admission after preserving numbers and closes precision") {
    sdk::OperationUsage owned; owned.attempts_complete = true;
    auto source = api::usage_wire::LegacyBackend(api::Usage{1, 2, 3, 5, 7}); REQUIRE(source);
    lubancode::runtime::UsageAttemptContext context;
    context.purpose = "main_turn"; context.reported_by_provider = true;
    for (std::size_t i = 0; i < sdk::kMaxUsageAttempts; ++i) {
        const auto id = "fixture-request-" + std::to_string(i); context.trajectory_request_id = id;
        sdk::detail::usage_result::CaptureAttempt(owned, api::usage_wire::Numbers(*source), &source->observation, false, false, context);
    }
    CHECK(owned.attempts_complete); CHECK(owned.attempts.size() == sdk::kMaxUsageAttempts);
    context.trajectory_request_id = "fixture-request-over-capacity";
    CHECK_THROWS_AS(sdk::detail::usage_result::CaptureAttempt(owned, api::usage_wire::Numbers(*source), &source->observation,
        false, false, context), std::runtime_error);
    CHECK_FALSE(owned.attempts_complete); CHECK(owned.attempts.size() == sdk::kMaxUsageAttempts);
    CHECK(owned.direct.coverage.samples == sdk::kMaxUsageAttempts + 1);
    CHECK(owned.direct.total.output_reasoning_tokens == 7 * static_cast<std::int64_t>(sdk::kMaxUsageAttempts + 1));
    for (std::size_t i = 0; i < facts::kFieldCount; ++i) {
        CHECK(owned.direct.coverage.fields[i].anomalous == 1);
        CHECK_FALSE(api::usage_aggregation::Exact(owned.direct.coverage, static_cast<facts::Field>(i)));
    }
    const auto restored = sdk::detail::usage_result::Decode(sdk::detail::usage_result::Encode(owned)); REQUIRE(restored);
    CHECK_FALSE(restored->attempts_complete); CHECK(restored->attempts.size() == sdk::kMaxUsageAttempts);
    CHECK(restored->direct.total.input_tokens == static_cast<std::int64_t>(sdk::kMaxUsageAttempts + 1));
}

TEST_CASE("SDK five-field accounting: Generate exceptions retain API attribution after raising cancellation") {
    struct ThrowingBackend final : sdk::Backend {
        std::atomic<bool>& cancel;
        ReplyBoundaryFault fault;
        int calls = 0;
        ThrowingBackend(std::atomic<bool>& flag, ReplyBoundaryFault value) : cancel(flag), fault(value) {}
        sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
            ++calls;
            cancel.store(true);
            ThrowReplyBoundaryFault(fault);
        }
    };
    for (const auto fault : {ReplyBoundaryFault::Standard, ReplyBoundaryFault::Allocation,
                             ReplyBoundaryFault::Unknown}) {
        std::atomic<bool> cancel{false};
        auto backend = std::make_shared<ThrowingBackend>(cancel, fault);
        auto adapter = sdk::detail::AdaptBackend(backend);
        int frames = 0;
        const auto sent = adapter->send_stream({}, [&](const api::StreamEvent&) { ++frames; }, &cancel);
        REQUIRE_FALSE(sent);
        CHECK(sent.error().kind == api::ErrorKind::Api);
        CHECK(sent.error().message.find("sdk.backend.exception") == 0);
        CHECK(cancel.load()); CHECK(backend->calls == 1); CHECK(frames == 0);
    }
}

TEST_CASE("SDK five-field accounting: post-reply observer faults retain owned numbers without success") {
    // The actual consumer owns the snapshot before its observer throws. This
    // does not claim delivery when an observer rejects a frame before Feed.
    for (const auto fault : {ReplyBoundaryFault::Standard, ReplyBoundaryFault::Allocation,
                             ReplyBoundaryFault::Unknown}) {
        for (const bool raise_cancel : {false, true}) {
            for (const int fault_snapshot : {1, 2}) {
                CAPTURE(static_cast<int>(fault));
                CAPTURE(raise_cancel);
                CAPTURE(fault_snapshot);
                std::atomic<bool> cancel{false};
                auto backend = std::make_shared<Capture>();
                backend->reply.text = "must not be delivered";
                backend->reply.usage = sdk::Usage{17, 9, 11, 13, 2};
                auto adapter = sdk::detail::AdaptBackend(backend);
                api::MessageAssembler assembler;
                int snapshots = 0, body_frames = 0, terminals = 0;
                const auto sent = adapter->send_stream({}, [&](const api::StreamEvent& event) {
                    assembler.Feed(event);
                    if (std::holds_alternative<api::UsageSnapshot>(event)) {
                        if (++snapshots == fault_snapshot) {
                            if (raise_cancel) cancel.store(true);
                            ThrowReplyBoundaryFault(fault);
                        }
                    } else if (std::holds_alternative<api::MessageDone>(event)) ++terminals;
                    else ++body_frames;
                }, &cancel);
                REQUIRE_FALSE(sent);
                CHECK(sent.error().kind == (raise_cancel ? api::ErrorKind::Cancelled : api::ErrorKind::Api));
                CHECK(snapshots == fault_snapshot); CHECK(body_frames == 0); CHECK(terminals == 0);
                CHECK(backend->requests.size() == 1);
                CHECK(assembler.usage_seen()); CHECK(assembler.stop_reason().empty());
                CHECK(assembler.BuildMessage().content.empty());
                CHECK(assembler.usage().input_tokens == 17); CHECK(assembler.usage().output_tokens == 9);
                CHECK(assembler.usage().cache_read_tokens == 11); CHECK(assembler.usage().cache_creation_tokens == 13);
                CHECK(assembler.usage().output_reasoning_tokens == 2);
                CHECK(assembler.usage_observation().has_value() == (fault_snapshot == 2));
            }
        }
    }
}

TEST_CASE("SDK five-field accounting: explicit zero and missing stay distinct through actual sampling") {
    Mark("five-field-zero-missing");
    auto backend = std::make_shared<Capture>();
    backend->reply.usage = sdk::Usage{0, 0};
    backend->reply.provider_response_id = "provider-zero";
    facts::Observation observation;
    observation.provider_namespace = "fixture";
    observation.raw_fields.push_back({"usage.input", facts::RawKind::SignedInteger, 0, {}, {}});
    auto& input = observation.fields[0];
    input.presence = facts::Presence::Present;
    input.validity = facts::Validity::ValidInteger;
    input.origin = facts::Origin::Reported;
    input.operand_count = 1;
    backend->reply.usage_observation = observation;
    auto adapter = sdk::detail::AdaptBackend(backend);
    agent::SampleRequest request; request.model = "fixture";
    const auto first = agent::SampleModel(*adapter, request);
    REQUIRE(first.ok); REQUIRE(first.usage_observation);
    CHECK(first.provider_response_id == "provider-zero");
    lubancode::runtime::IdAuthority ids;
    lubancode::runtime::TurnEventAdapter events("fixture", ids);
    nlohmann::json payload;
    events.Attach([&](const lubancode::runtime::ServerEvent& event) {
        if (event.kind == lubancode::runtime::ServerEventKind::UsageUpdated) payload = event.payload;
    });
    events.Start();
    api::UsageReport report;
    report.usage = first.usage; report.usage_observation = first.usage_observation;
    report.provider_response_id = first.provider_response_id;
    events.OnUsage(report);
    REQUIRE(payload.contains("usage_observation"));
    const auto& material = payload.at("usage_observation");
    CHECK(material.at("version") == 1);
    CHECK(material.at("raw_fields").at(0).at("integer") == 0);
    CHECK(material.at("fields").at(0).at("presence") == static_cast<unsigned>(facts::Presence::Present));
    CHECK(material.at("fields").at(1).at("presence") == static_cast<unsigned>(facts::Presence::Missing));
    CHECK_FALSE(material.contains("input_tokens"));
    CHECK(payload.at("provider_response_id") == "provider-zero");
    api::UsageReport restored; restored.usage = first.usage;
    api::usage_json::Restore(restored, payload);
    REQUIRE(restored.usage_observation);
    CHECK(restored.usage_observation->raw_fields[0].integer == 0);
    CHECK(restored.usage_observation->fields[1].presence == facts::Presence::Missing);
    auto bad = payload;
    bad["usage_observation"]["fields"][0]["operands"] = nlohmann::json::array({64});
    api::usage_json::Restore(restored, bad);
    CHECK_FALSE(restored.usage_observation); CHECK_FALSE(restored.usage_observation_error.empty());
    CHECK(restored.usage.input_tokens == 0);
    bad = payload;
    bad["usage_observation"]["raw_fields"][0]["path"] = std::string(facts::kMaxPathBytes + 1, 'x');
    api::usage_json::Restore(restored, bad);
    CHECK_FALSE(restored.usage_observation); CHECK_FALSE(restored.usage_observation_error.empty());
    bad = payload;
    bad["usage_observation"]["version"] = 3;
    api::usage_json::Restore(restored, bad);
    CHECK_FALSE(restored.usage_observation);
    CHECK(restored.usage_observation_error == "usage.json.version");
    agent::BackgroundCallAccounting total;
    agent::AddSampleAccounting(&total, first);
    CHECK(total.usage_coverage.samples == 1);
    CHECK(total.usage_coverage.fields[0].observed == 1);
    CHECK(total.usage_coverage.fields[1].missing == 1);
    CHECK(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Input));
    CHECK_FALSE(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Output));
    backend->reply.usage_observation.reset();
    backend->reply.usage = sdk::Usage{11, 7};
    const auto second = agent::SampleModel(*adapter, request);
    REQUIRE(second.ok); REQUIRE(second.usage_observation);
    CHECK(second.usage_observation->provider_namespace == "lubancore.backend.legacy");
    agent::AddSampleAccounting(&total, second);
    CHECK(total.usage.input_tokens == 11); CHECK(total.usage.output_tokens == 7);
    CHECK(total.usage_reported);
    CHECK(total.usage_coverage.samples == 2);
    CHECK(total.usage_coverage.fields[0].missing == 0);
    CHECK(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Input));
    CHECK_FALSE(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Output));
    CHECK(total.usage_coverage.fields[2].missing == 2);
}

TEST_CASE("SDK five-field accounting: overflow freezes only the affected field and records omissions") {
    Mark("five-field-overflow");
    auto backend = std::make_shared<Capture>();
    auto adapter = sdk::detail::AdaptBackend(backend);
    agent::SampleRequest request; request.model = "fixture";
    agent::BackgroundCallAccounting total;
    backend->reply.usage = sdk::Usage{std::numeric_limits<std::int64_t>::max(), 2};
    const auto first = agent::SampleModel(*adapter, request); REQUIRE(first.ok);
    agent::AddSampleAccounting(&total, first);
    backend->reply.usage = sdk::Usage{1, 3};
    const auto second = agent::SampleModel(*adapter, request); REQUIRE(second.ok);
    agent::AddSampleAccounting(&total, second);
    CHECK(total.usage.input_tokens == std::numeric_limits<std::int64_t>::max());
    CHECK(total.usage.output_tokens == 5);
    CHECK(total.usage_coverage.fields[0].arithmetic_overflow);
    CHECK(total.usage_coverage.fields[0].omitted == 1);
    CHECK_FALSE(total.usage_coverage.fields[1].arithmetic_overflow);
    backend->reply.usage = sdk::Usage{-1, 4};
    const auto third = agent::SampleModel(*adapter, request); REQUIRE(third.ok);
    agent::AddSampleAccounting(&total, third);
    CHECK(total.usage.input_tokens == std::numeric_limits<std::int64_t>::max());
    CHECK(total.usage.output_tokens == 9);
    CHECK(total.usage_coverage.fields[0].included == 1);
    CHECK(total.usage_coverage.fields[0].omitted == 2);
}

TEST_CASE("SDK five-field accounting: legacy appended values stay unknown and negative originals remain visible") {
    auto backend = std::make_shared<Capture>();
    backend->reply.usage = sdk::Usage{-3, 0, 11, 13, 2};
    auto adapter = sdk::detail::AdaptBackend(backend);
    agent::SampleRequest request; request.model = "fixture";
    const auto sample = agent::SampleModel(*adapter, request);
    REQUIRE(sample.ok); REQUIRE(sample.usage_observation);
    CHECK(sample.usage.input_tokens == -3); CHECK(sample.usage.output_tokens == 0);
    CHECK(sample.usage.cache_read_tokens == 11); CHECK(sample.usage.cache_creation_tokens == 13);
    CHECK(sample.usage.output_reasoning_tokens == 2);
    CHECK_FALSE(sample.cache_read_reported); CHECK_FALSE(sample.cache_creation_reported);
    const auto& material = *sample.usage_observation;
    CHECK(material.raw_fields.size() == 2); CHECK(material.raw_fields[0].integer == -3);
    CHECK(material.fields[0].presence == facts::Presence::Present);
    CHECK(material.fields[1].validity == facts::Validity::ValidInteger);
    for (std::size_t i = 2; i < facts::kFieldCount; ++i) {
        CHECK(material.fields[i].presence == facts::Presence::Missing);
        CHECK(material.fields[i].validity == facts::Validity::Unknown);
        CHECK(material.fields[i].origin == facts::Origin::Unknown);
    }
    agent::BackgroundCallAccounting total; agent::AddSampleAccounting(&total, sample);
    CHECK(total.usage.input_tokens == -3);
    CHECK(total.usage_coverage.fields[0].anomalous == 1);
    CHECK_FALSE(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Input));
    CHECK(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::Output));
    CHECK_FALSE(api::usage_aggregation::Exact(total.usage_coverage, facts::Field::CacheRead));
}
}

TEST_CASE("SDK sampling: original aggregate initialization and defaults survive") {
    Mark("defaults");
    sdk::ModelRequest request{"old-model","old-system",{}, {}, 29};
    sdk::ModelReply reply{"old-text",{},sdk::Usage{3,4}};
    CHECK(request.reasoning_effort.empty()); CHECK(reply.stop_reason.empty());
    const auto capture = std::make_shared<Capture>(); auto adapter = sdk::detail::AdaptBackend(capture);
    api::Request native; native.model = request.model; native.max_tokens = request.max_output_tokens;
    std::string reason;
    REQUIRE(adapter->send_stream(native,[&](const api::StreamEvent& event) {
        if (const auto* done=std::get_if<api::MessageDone>(&event)) reason=done->stop_reason;
    }).has_value());
    REQUIRE(capture->requests.size()==1); CHECK(capture->requests[0].reasoning_effort.empty());
    CHECK(reason=="end_turn");
    capture->reply.tool_calls.push_back({"call","inspect","{}"});
    REQUIRE(adapter->send_stream(native,[&](const api::StreamEvent& event) {
        if (const auto* done=std::get_if<api::MessageDone>(&event)) reason=done->stop_reason;
    }).has_value());
    CHECK(reason=="tool_use");
}

TEST_CASE("SDK sampling: actual Generate receives effort without changing body token scope") {
    Mark("effort");
    const auto capture=std::make_shared<Capture>(); auto adapter=sdk::detail::AdaptBackend(capture);
    api::Request request; request.model="sample-model"; request.system="actual material";
    const auto before=adapter->PrepareModelInput(request); REQUIRE(before.has_value()); REQUIRE(before->has_value());
    for (const std::string effort : {std::string("high"),std::string("future-provider-level"),std::string(256,'x'),std::string("\xe4\xb8\xad")}) {
        request.reasoning_effort=effort;
        const auto projected=adapter->PrepareModelInput(request); REQUIRE(projected.has_value()); REQUIRE(projected->has_value());
        CHECK((**projected).input==(**before).input); CHECK((**projected).scope==(**before).scope);
        REQUIRE(adapter->send_stream(request,[](const api::StreamEvent&) {}).has_value());
        CHECK(capture->requests.back().reasoning_effort==effort);
    }
}

TEST_CASE("SDK sampling: provider finish reasons survive the real SampleModel assembler") {
    Mark("finish");
    const auto capture=std::make_shared<Capture>(); auto adapter=sdk::detail::AdaptBackend(capture);
    agent::SampleRequest request; request.model="sample"; request.reasoning_effort="low";
    request.output_schema={{"type","object"},{"required",{"summary"}},
                           {"properties",{{"summary",{{"type","string"}}}}}};
    for (const std::string reason : {"length","max_tokens","max_output_tokens","future-provider-stop"}) {
        capture->reply.stop_reason=reason;
        const auto result=agent::SampleModel(*adapter,request);
        REQUIRE(result.ok); CHECK(result.stop_reason==reason); CHECK(result.schema_ok);
        CHECK(result.usage.input_tokens==11); CHECK(result.usage.output_tokens==7);
        CHECK(capture->requests.back().reasoning_effort=="low"); CHECK(capture->requests.back().tools.empty());
    }
    capture->reply.text=R"({"summary":7})";
    const auto invalid=agent::SampleModel(*adapter,request);
    CHECK(invalid.ok); CHECK_FALSE(invalid.schema_ok); CHECK_FALSE(invalid.schema_error.empty());
}

TEST_CASE("SDK sampling: invalid effort stops projection and Generate") {
    Mark("invalid-request");
    const auto capture=std::make_shared<Capture>(); auto adapter=sdk::detail::AdaptBackend(capture);
    for (const auto& value : InvalidValues()) {
        api::Request request; request.reasoning_effort=value;
        const auto projection=adapter->PrepareModelInput(request); REQUIRE_FALSE(projection);
        CHECK(projection.error()=="sdk.backend.invalid_reasoning_effort");
        int events=0; const auto sent=adapter->send_stream(request,[&](const api::StreamEvent&){++events;});
        REQUIRE_FALSE(sent); CHECK(sent.error().message=="sdk.backend.invalid_reasoning_effort");
        CHECK(events==0); CHECK(capture->requests.empty());
    }
}

TEST_CASE("SDK sampling: invalid finish reason emits no partial reply") {
    Mark("invalid-reply");
    const auto capture=std::make_shared<Capture>(); auto adapter=sdk::detail::AdaptBackend(capture);
    for (const auto& value : InvalidValues()) {
        capture->reply.stop_reason=value;
        int facts=0, content=0;
        const auto sent=adapter->send_stream({},[&](const api::StreamEvent& event){
            if (const auto* snapshot=std::get_if<api::UsageSnapshot>(&event)) {
                ++facts; CHECK(snapshot->usage_reported);
                CHECK(snapshot->usage.input_tokens==11); CHECK(snapshot->usage.output_tokens==7);
            } else ++content;
        });
        REQUIRE_FALSE(sent); CHECK(sent.error().message=="sdk.backend.invalid_stop_reason");
        CHECK(content==0); CHECK(facts==2);
    }
    capture->reply.stop_reason=std::string(256,'x');
    CHECK(adapter->send_stream({},[](const api::StreamEvent&){}).has_value());
}

TEST_CASE("SDK sampling: cancellation and Backend exceptions keep explicit errors") {
    Mark("errors");
    const auto capture=std::make_shared<Capture>(); auto adapter=sdk::detail::AdaptBackend(capture);
    for (const auto fault : {Capture::Fault::Standard,Capture::Fault::Unknown}) {
        capture->fault=fault; int events=0;
        const auto sent=adapter->send_stream({},[&](const api::StreamEvent&){++events;});
        REQUIRE_FALSE(sent); CHECK(sent.error().kind==api::ErrorKind::Api);
        CHECK(sent.error().message.starts_with("sdk.backend.exception")); CHECK(events==0);
    }
    capture->fault=Capture::Fault::None; std::atomic<bool> cancel{true};
    const auto calls=capture->requests.size();
    const auto before=adapter->send_stream({},[](const api::StreamEvent&){},&cancel);
    REQUIRE_FALSE(before); CHECK(before.error().kind==api::ErrorKind::Cancelled); CHECK(capture->requests.size()==calls);
    cancel=false; capture->fault=Capture::Fault::Cancel; capture->cancellation=&cancel;
    int facts=0, content=0;
    const auto during=adapter->send_stream({},[&](const api::StreamEvent& event){
        if (const auto* snapshot=std::get_if<api::UsageSnapshot>(&event)) {
            ++facts; CHECK(snapshot->usage_reported);
            CHECK(snapshot->usage.input_tokens==11); CHECK(snapshot->usage.output_tokens==7);
        } else ++content;
    },&cancel);
    REQUIRE_FALSE(during); CHECK(during.error().kind==api::ErrorKind::Cancelled);
    CHECK(content==0); CHECK(facts==2);
}


namespace {
struct UsageRecorder final : agent::LoopBoundaryRecorder {
    std::vector<std::string> events;
    api::Usage usage;
    bool reported = false;
    std::string response_id;
    std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
        events.push_back("prepared"); return "actual-usage-request";
    }
    bool OnRequestSent(const std::string&) override { events.push_back("sent"); return true; }
    void OnUsageRecorded(const std::string&, const api::Usage& observed, bool was_reported,
        const std::string& id, int, bool, bool, bool, const std::string&) override {
        events.push_back("usage"); usage=observed; reported=was_reported; response_id=id;
    }
    bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                           const std::string&) override { events.push_back("completed"); return true; }
    void OnOutputFailed(const std::string&, const std::string&) override { events.push_back("failed"); }
    void OnOutputCancelled(const std::string&, agent::OutputCancelSource) override { events.push_back("cancelled"); }
};
struct ReturnGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered=false, released=false;
    bool WaitEntered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock,std::chrono::seconds(5),[&]{return entered;});
    }
    void Release() { std::lock_guard lock(mutex); released=true; cv.notify_all(); }
};
struct ReturnGateRelease {
    std::shared_ptr<ReturnGate> gate;
    ~ReturnGateRelease() { gate->Release(); }
};
struct ReturnedReply final : sdk::Backend {
    sdk::ModelReply reply{"actual reply",{},sdk::Usage{17,9}};
    std::shared_ptr<ReturnGate> gate;
    int calls=0;
    enum class Fault { None, Error, Standard, Unknown } fault=Fault::None;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&,sdk::Cancellation) override {
        ++calls;
        if (gate) {
            std::unique_lock lock(gate->mutex); gate->entered=true; gate->cv.notify_all();
            if (!gate->cv.wait_for(lock,std::chrono::seconds(5),[&]{return gate->released;}))
                return std::unexpected(sdk::Error{"fixture.timeout","return gate timed out"});
        }
        if (fault==Fault::Error) return std::unexpected(sdk::Error{"fixture.error","no observable reply"});
        if (fault==Fault::Standard) throw std::runtime_error("returned reply fixture");
        if (fault==Fault::Unknown) throw 27;
        return reply;
    }
};
class UsageFrameTrace final : public api::Backend {
public:
    UsageFrameTrace(api::Backend& inner,std::vector<std::string>& frames) : inner_(inner),frames_(frames) {}
    std::expected<std::optional<api::ModelInputSnapshot>,std::string>
    PrepareModelInput(const api::Request& request) const override { return inner_.PrepareModelInput(request); }
    std::expected<void,api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit,const std::atomic<bool>* cancel) override {
        return inner_.send_stream(request,[&](const api::StreamEvent& event) {
            if (std::holds_alternative<api::UsageSnapshot>(event)) frames_.push_back("usage");
            else if (std::holds_alternative<api::MessageDone>(event)) frames_.push_back("done");
            else if (std::holds_alternative<api::MessageStart>(event)) frames_.push_back("start");
            else frames_.push_back("content");
            emit(event);
        },cancel);
    }
private:
    api::Backend& inner_;
    std::vector<std::string>& frames_;
};
agent::SampleResult SharedSample(const std::shared_ptr<ReturnedReply>& backend,
                                UsageRecorder& recorder,const std::atomic<bool>* cancel=nullptr,
                                std::vector<std::string>* observed_frames=nullptr) {
    auto adapter=sdk::detail::testing::AdaptObservedBackend(backend);
    agent::SampleRequest request; request.model="actual-shared-sdk";
    agent::SampleOptions options; options.cancel=cancel; options.boundary_recorder=&recorder;
    options.cancel_source=agent::OutputCancelSource::Internal;
    std::vector<std::string> unobserved;
    UsageFrameTrace trace(*adapter,observed_frames ? *observed_frames : unobserved);
    return sdk::detail::testing::SampleInsideSharedSdk(trace,request,options);
}
}

TEST_CASE("SDK usage snapshot: returned usage survives cancellation inside the actual shared SDK") {
    Mark("usage-cancelled");
    for (const sdk::Usage usage : {sdk::Usage{17,9},sdk::Usage{0,0}}) {
        auto backend=std::make_shared<ReturnedReply>(); backend->reply.usage=usage;
        backend->gate=std::make_shared<ReturnGate>(); std::atomic<bool> cancel{false}; UsageRecorder recorder;
        std::vector<std::string> frames;
        auto pending=std::async(std::launch::async,[&]{return SharedSample(backend,recorder,&cancel,&frames);});
        ReturnGateRelease release{backend->gate};
        REQUIRE(backend->gate->WaitEntered()); cancel.store(true); backend->gate->Release();
        REQUIRE(pending.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        const auto result=pending.get();
        CHECK_FALSE(result.ok); CHECK(result.error.kind==api::ErrorKind::Cancelled);
        CHECK(result.stop_reason.empty()); CHECK(result.text.empty()); CHECK(result.provider_response_id.empty());
        CHECK(result.usage_reported); CHECK(result.usage.input_tokens==usage.input_tokens);
        CHECK(result.usage.output_tokens==usage.output_tokens); CHECK(backend->calls==1);
        CHECK(recorder.events==std::vector<std::string>{"prepared","sent","usage","cancelled"});
        CHECK(recorder.reported); CHECK(recorder.response_id.empty());
        CHECK(frames==std::vector<std::string>{"usage", "usage"});
        CHECK(recorder.usage.input_tokens==usage.input_tokens); CHECK(recorder.usage.output_tokens==usage.output_tokens);
    }
}

TEST_CASE("SDK usage snapshot: invalid returned output preserves usage without completing") {
    Mark("usage-invalid-output");
    for (int invalid=0;invalid<3;++invalid) {
        auto backend=std::make_shared<ReturnedReply>();
        if (invalid==0) backend->reply.stop_reason=std::string("a\0b",3);
        if (invalid==1) backend->reply.text=std::string(1,static_cast<char>(0xff));
        if (invalid==2) backend->reply.tool_calls.push_back({"id","inspect","[]"});
        UsageRecorder recorder; std::vector<std::string> frames;
        const auto result=SharedSample(backend,recorder,nullptr,&frames);
        CHECK(frames==std::vector<std::string>{"usage", "usage"});
        CHECK_FALSE(result.ok); CHECK(result.error.kind==api::ErrorKind::Parse);
        CHECK(result.error.message==std::vector<std::string>{"sdk.backend.invalid_stop_reason",
            "sdk.backend.invalid_utf8","sdk.backend.invalid_tool_call"}[invalid]);
        CHECK(result.text.empty()); CHECK(result.stop_reason.empty()); CHECK(result.usage_reported);
        CHECK(result.usage.input_tokens==17); CHECK(result.usage.output_tokens==9);
        CHECK(recorder.events==std::vector<std::string>{"prepared","sent","usage","failed"});
        CHECK(recorder.reported); CHECK(backend->calls==1);
    }
}

TEST_CASE("SDK usage snapshot: absent returned usage stays absent on rejected output") {
    Mark("usage-absent");
    auto backend=std::make_shared<ReturnedReply>(); backend->reply.usage.reset();
    backend->reply.stop_reason=std::string("a\0b",3); UsageRecorder recorder;
    const auto result=SharedSample(backend,recorder);
    CHECK_FALSE(result.ok); CHECK(result.error.kind==api::ErrorKind::Parse);
    CHECK_FALSE(result.usage_reported); CHECK_FALSE(recorder.reported);
    CHECK(result.usage.input_tokens==0); CHECK(result.usage.output_tokens==0);
    CHECK(recorder.events==std::vector<std::string>{"prepared","sent","usage","failed"});
    auto adapter=sdk::detail::testing::AdaptObservedBackend(backend); int frames=0;
    REQUIRE_FALSE(adapter->send_stream({},[&](const api::StreamEvent&){++frames;})); CHECK(frames==0);
}

TEST_CASE("SDK usage snapshot: errors and exceptions never invent observable usage") {
    Mark("usage-no-reply");
    for (const auto fault : {ReturnedReply::Fault::Error,ReturnedReply::Fault::Standard,ReturnedReply::Fault::Unknown}) {
        auto backend=std::make_shared<ReturnedReply>(); backend->fault=fault; UsageRecorder recorder;
        const auto result=SharedSample(backend,recorder);
        CHECK_FALSE(result.ok); CHECK(result.error.kind==api::ErrorKind::Api);
        CHECK_FALSE(result.usage_reported); CHECK(result.usage.input_tokens==0); CHECK(result.usage.output_tokens==0);
        CHECK_FALSE(recorder.reported); CHECK(backend->calls==1);
        CHECK(recorder.events==std::vector<std::string>{"prepared","sent","usage","failed"});
        auto adapter=sdk::detail::testing::AdaptObservedBackend(backend); int frames=0;
        REQUIRE_FALSE(adapter->send_stream({},[&](const api::StreamEvent&){++frames;})); CHECK(frames==0);
    }
}

TEST_CASE("SDK usage snapshot: successful terminal does not add the snapshot twice") {
    Mark("usage-success");
    auto backend=std::make_shared<ReturnedReply>(); UsageRecorder first_recorder;
    const auto first=SharedSample(backend,first_recorder);
    REQUIRE(first.ok); CHECK(first.stop_reason=="end_turn"); CHECK(first.text=="actual reply");
    CHECK(first.usage_reported); CHECK(first.usage.input_tokens==17); CHECK(first.usage.output_tokens==9);
    CHECK(first_recorder.events==std::vector<std::string>{"prepared","sent","usage","completed"});
    agent::BackgroundCallAccounting total; agent::AddSampleAccounting(&total,first);
    backend->reply.usage=sdk::Usage{3,4}; UsageRecorder second_recorder;
    const auto second=SharedSample(backend,second_recorder); REQUIRE(second.ok);
    agent::AddSampleAccounting(&total,second);
    CHECK(total.usage.input_tokens==20); CHECK(total.usage.output_tokens==13);
    CHECK(backend->calls==2);
}
