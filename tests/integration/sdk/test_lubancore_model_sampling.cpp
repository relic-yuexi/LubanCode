#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include "agent/sample_model.hpp"
#include "sdk/adapters.hpp"
#include "sdk/backend_owner_test_hooks.hpp"
#include "sdk/sampling_test_hooks.hpp"

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
        int facts=0, content=0;
        const auto sent=adapter->send_stream({},[&](const api::StreamEvent& event){
            if (const auto* snapshot=std::get_if<api::UsageSnapshot>(&event)) {
                ++facts; CHECK(snapshot->usage_reported);
                CHECK(snapshot->usage.input_tokens==11); CHECK(snapshot->usage.output_tokens==7);
            } else ++content;
        });
        REQUIRE_FALSE(sent); CHECK(sent.error().message=="sdk.backend.invalid_stop_reason");
        CHECK(content==0); CHECK(facts==1);
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
    CHECK(content==0); CHECK(facts==1);
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
        CHECK(frames==std::vector<std::string>{"usage"});
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
        CHECK(frames==std::vector<std::string>{"usage"});
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
