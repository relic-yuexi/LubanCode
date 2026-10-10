#pragma once
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include "agent/sample_model.hpp"
#include "agent/sample_model_test_hooks.hpp"
namespace sample_lifetime_fixture {
namespace api = lubancode::api;
namespace agent = lubancode::agent;
using Handle = agent::testing::WatchdogHooksHandle;
using Replace = std::function<Handle(Handle)>;
using Sample = std::function<agent::SampleResult(api::Backend&, const agent::SampleRequest&, const agent::SampleOptions&)>;
struct Port { Sample sample; Replace replace; const char* module; };
struct HooksScope {
    Replace replace; Handle previous;
    HooksScope(const Port& port, Handle hooks) : replace(port.replace), previous(replace(std::move(hooks))) {}
    ~HooksScope() { replace(std::move(previous)); }
};
struct Observation {
    std::atomic<int> starts{0}, exits{0}, joins{0};
    std::atomic<bool> joined_after_exit{true};
    Handle Hooks(int fault = 0) {
        auto hooks = std::make_shared<agent::testing::WatchdogHooks>();
        hooks->before_start = [this,fault] {
            ++starts;
            if (fault == 1) throw std::runtime_error("actual watchdog boundary");
            if (fault == 2) throw 17;
        };
        hooks->on_thread_exit = [this] { ++exits; };
        hooks->after_join = [this] { if (exits.load() != joins.load()+1) joined_after_exit=false; ++joins; };
        return hooks;
    }
};
struct Recorder : agent::LoopBoundaryRecorder {
    bool allow_prepared=true, allow_sent=true;
    int prepared=0,sent=0,owners=0,completed=0,failed=0,cancelled=0;
    bool reported=false; api::Usage usage; std::string failure;
    std::vector<std::string> order;
    std::string OnRequestPrepared(const api::Request&,const agent::RequestPreparedContext&) override {
        ++prepared; order.push_back("prepared"); return allow_prepared ? "real-sample" : "";
    }
    bool OnRequestSent(const std::string& id) override {
        CHECK(id=="real-sample"); ++sent; order.push_back("sent"); return allow_sent;
    }
    void OnUsageRecorded(const std::string& id,const api::Usage& value,bool is_reported,
                         const std::string&,int,bool,bool,bool,const std::string&) override {
        CHECK(id=="real-sample"); ++owners; usage=value; reported=is_reported; order.push_back("usage");
    }
    bool OnOutputCompleted(const std::string& id,const api::Message&,const std::string&,const std::string&) override {
        CHECK(id=="real-sample"); ++completed; order.push_back("completed"); return true;
    }
    void OnOutputFailed(const std::string& id,const std::string& reason) override {
        CHECK(id=="real-sample"); ++failed; failure=reason; order.push_back("failed");
    }
    void OnOutputCancelled(const std::string& id,agent::OutputCancelSource) override {
        CHECK(id=="real-sample"); ++cancelled; order.push_back("cancelled");
    }
};
struct Backend final : api::Backend {
    int fault=0, cap_fault=0, calls=0; bool partial=false, complete=false, cancel_after_done=false;
    std::expected<void,api::Error> send_stream(const api::Request&,
        const std::function<void(const api::StreamEvent&)>& event,const std::atomic<bool>*) override {
        ++calls;
        if (partial || complete) {
            api::MessageStart start; start.id="provider-real"; event(start);
            event(api::TextDelta{"retained partial"});
            api::MessageDone done; done.usage.input_tokens=11; done.usage.output_tokens=7;
            if (complete) done.stop_reason="end_turn";
            event(done);
        }
        if (fault==1) throw std::runtime_error("actual send_stream exception");
        if (fault==2) throw 19;
        if (cancel_after_done) return std::unexpected(api::Error{api::ErrorKind::Cancelled,"cancelled",0});
        return {};
    }
    void ForceMaxOutputTokensOverride(api::Request& request,int tokens) const override {
        if (cap_fault==1) throw std::runtime_error("actual output override exception");
        if (cap_fault==2) throw 23;
        api::Backend::ForceMaxOutputTokensOverride(request,tokens);
    }
    EffectiveOutputLimit GetEffectiveOutputLimit(const api::Request& request) const override {
        if (cap_fault==3) throw std::runtime_error("actual effective cap exception");
        if (cap_fault==4) throw 29;
        return api::Backend::GetEffectiveOutputLimit(request);
    }
};
inline void Mark(const Port& port,const char* path) {
    std::cout << "[sample-lifetime-path] " << port.module << ' ' << path << '\n';
}
inline void Startup(const Port& port) {
    for (bool dual : {false,true}) for (int fault : {1,2}) {
        Observation observed; HooksScope scope(port,observed.Hooks(fault));
        std::atomic<bool> external{false};
        for (int repeat=0;repeat!=2;++repeat) {
            Backend backend; Recorder recorder; agent::SampleOptions options; options.timeout_secs=1;
            options.cancel=dual ? &external : nullptr; options.boundary_recorder=&recorder;
            const auto result=port.sample(backend,{},options);
            CHECK_FALSE(result.ok); CHECK(result.error.api_code=="sample.watchdog_start_failed");
            CHECK(backend.calls==0); CHECK_FALSE(result.usage_reported); CHECK_FALSE(recorder.reported);
            CHECK(recorder.order==std::vector<std::string>{"prepared","sent","usage","failed"});
            CHECK(recorder.failed==1); CHECK(recorder.completed==0); CHECK(recorder.cancelled==0);
            CHECK(result.text.empty()); CHECK_FALSE(external.load());
        }
        CHECK(observed.starts==2); CHECK(observed.exits==0); CHECK(observed.joins==0);
    }
    Observation observed; HooksScope scope(port,observed.Hooks()); Backend backend;
    agent::SampleOptions options; options.timeout_secs=1;
    CHECK(port.sample(backend,{},options).ok);
    CHECK(observed.starts==1); CHECK(observed.exits==1); CHECK(observed.joins==1);
    CHECK(observed.joined_after_exit.load()); Mark(port,"startup");
}
inline void Exceptions(const Port& port) {
    for (bool dual : {false,true}) for (int fault : {1,2}) for (int phase : {0,1,2}) {
        Observation observed; HooksScope scope(port,observed.Hooks());
        Backend backend; backend.fault=fault; backend.partial=phase==1; backend.complete=phase==2;
        Recorder recorder; std::atomic<bool> external{false}; agent::SampleOptions options;
        options.timeout_secs=1; options.cancel=dual ? &external : nullptr; options.boundary_recorder=&recorder;
        const auto result=port.sample(backend,{},options);
        CHECK_FALSE(result.ok);
        CHECK(result.error.api_code==(fault==1 ? "sample.backend_exception" : "sample.backend_unknown_exception"));
        CHECK(backend.calls==1); CHECK(recorder.failed==1); CHECK(recorder.completed==0); CHECK(recorder.cancelled==0);
        CHECK(recorder.order==std::vector<std::string>{"prepared","sent","usage","failed"});
        CHECK(result.usage_reported==(phase!=0)); CHECK(recorder.reported==(phase!=0));
        CHECK(result.text==(phase==0 ? "" : "retained partial"));
        CHECK(result.usage.input_tokens==(phase==0 ? 0 : 11));
        CHECK(result.usage.output_tokens==(phase==0 ? 0 : 7));
        CHECK(result.provider_response_id==(phase==0 ? "" : "provider-real"));
        CHECK(result.stop_reason==(phase==2 ? "end_turn" : ""));
        CHECK(observed.exits==1); CHECK(observed.joins==1); CHECK(observed.joined_after_exit.load());
        CHECK_FALSE(external.load());
    }
    Mark(port,"exceptions");
}
inline void Preflight(const Port& port) {
    for (int fault : {1,2,3,4}) {
        Observation observed; HooksScope scope(port,observed.Hooks()); Backend backend; backend.cap_fault=fault;
        Recorder recorder; agent::SampleOptions options; options.timeout_secs=1; options.boundary_recorder=&recorder;
        agent::SampleRequest request; request.enforce_output_limit=true; request.max_tokens=37;
        const auto result=port.sample(backend,request,options);
        CHECK_FALSE(result.ok);
        CHECK(result.error.api_code==((fault==1 || fault==3) ? "sample.backend_exception" : "sample.backend_unknown_exception"));
        CHECK(backend.calls==0); CHECK(recorder.prepared==0); CHECK(recorder.sent==0); CHECK(recorder.owners==0);
        CHECK(observed.starts==0); CHECK(observed.exits==0); CHECK(observed.joins==0);
        CHECK_FALSE(result.usage_reported);
    }
    Mark(port,"preflight");
}
inline void Gates(const Port& port) {
    for (bool prepared : {false,true}) {
        Observation observed; HooksScope scope(port,observed.Hooks(1)); Backend backend; Recorder recorder;
        recorder.allow_prepared=prepared; recorder.allow_sent=false;
        agent::SampleOptions options; options.timeout_secs=1; options.boundary_recorder=&recorder;
        CHECK_FALSE(port.sample(backend,{},options).ok);
        CHECK(backend.calls==0); CHECK(observed.starts==0); CHECK(recorder.prepared==1);
        CHECK(recorder.sent==(prepared ? 1 : 0)); CHECK(recorder.owners==0); CHECK(recorder.failed==0);
    }
    // The owner also retires on an exception outside the Backend catch boundary.
    Observation observed; HooksScope scope(port,observed.Hooks()); Backend backend;
    struct ThrowingRecorder final : Recorder {
        void OnUsageRecorded(const std::string&,const api::Usage&,bool,const std::string&,
                             int,bool,bool,bool,const std::string&) override {
            throw std::runtime_error("actual recorder exception after join");
        }
    } recorder;
    agent::SampleOptions options; options.timeout_secs=1; options.boundary_recorder=&recorder;
    CHECK_THROWS_AS(port.sample(backend,{},options),std::runtime_error);
    CHECK(observed.exits==1); CHECK(observed.joins==1); CHECK(observed.joined_after_exit.load());
    Mark(port,"gates");
}
inline void Isolation(const Port& port) {
    Observation observed; HooksScope scope(port,observed.Hooks(1));
    agent::SampleResult other; int other_calls=0;
    std::jthread caller([&] {
        Backend backend; agent::SampleOptions options; options.timeout_secs=1;
        other=port.sample(backend,{},options); other_calls=backend.calls;
    });
    caller.join(); CHECK(other.ok); CHECK(other_calls==1); CHECK(observed.starts==0);
    Backend backend; agent::SampleOptions options; options.timeout_secs=1;
    CHECK(port.sample(backend,{},options).error.api_code=="sample.watchdog_start_failed");
    CHECK(backend.calls==0); CHECK(observed.starts==1); Mark(port,"isolation");
}
inline void Cancellation(const Port& port) {
    for (bool complete : {false,true}) {
        Observation observed; HooksScope scope(port,observed.Hooks());
        Backend backend; backend.complete=complete; backend.partial=!complete; backend.cancel_after_done=true;
        Recorder recorder; std::atomic<bool> external{true}; agent::SampleOptions options;
        options.timeout_secs=1; options.cancel=&external; options.boundary_recorder=&recorder;
        const auto result=port.sample(backend,{},options);
        CHECK(result.ok==complete); CHECK(recorder.completed==(complete ? 1 : 0));
        CHECK(recorder.cancelled==(complete ? 0 : 1)); CHECK(recorder.failed==0);
        CHECK(observed.exits==1); CHECK(observed.joins==1); CHECK(observed.joined_after_exit.load());
    }
    // Without a budget the original external flag is forwarded; no hook/thread runs.
    Observation observed; HooksScope scope(port,observed.Hooks(1)); Backend backend;
    std::atomic<bool> external{false}; agent::SampleOptions options; options.cancel=&external;
    CHECK(port.sample(backend,{},options).ok); CHECK(observed.starts==0); CHECK(observed.exits==0);
    Mark(port,"cancellation");
}
}
