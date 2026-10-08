#include <doctest/doctest.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include "agent/sample_model.hpp"
#include "sdk/adapters.hpp"

namespace {
namespace api = lubancode::api;
namespace sdk = lubancore;
namespace agent = lubancode::agent;
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
        int events=0; const auto sent=adapter->send_stream({},[&](const api::StreamEvent&){++events;});
        REQUIRE_FALSE(sent); CHECK(sent.error().message=="sdk.backend.invalid_stop_reason"); CHECK(events==0);
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
    int events=0; const auto during=adapter->send_stream({},[&](const api::StreamEvent&){++events;},&cancel);
    REQUIRE_FALSE(during); CHECK(during.error().kind==api::ErrorKind::Cancelled); CHECK(events==0);
}
