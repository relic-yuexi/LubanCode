#include <lubancore/events.hpp>
#include <lubancore/core.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace sdk = lubancore;
namespace ev = lubancore::events::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
std::string Utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
template<class T> T Take(sdk::Result<T> value) {
    const std::string error = value ? std::string{} : value.error().code + ": " + value.error().message;
    REQUIRE_MESSAGE(value.has_value(), error);
    return std::move(*value);
}
void Take(sdk::Result<void> value) {
    const std::string error = value ? std::string{} : value.error().code + ": " + value.error().message;
    REQUIRE_MESSAGE(value.has_value(), error);
}
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-events-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "project"); fs::create_directories(root / "resources");
        root = fs::canonical(root);
    }
    ~Directory() { std::error_code error; fs::remove_all(root, error); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
enum class Fault { None, FactoryError, FactoryThrow, FactoryNull, PushError, PushThrow, NextError, NextThrow, CloseError, CloseThrow };
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<sdk::Event> delivered;
    std::atomic<unsigned> factories{0}, pushes{0}, nexts{0}, closes{0}, queues_destroyed{0}, sinks_destroyed{0}, models{0}, tools{0};
    std::array<std::atomic<unsigned>, 4> reentry{};
    std::atomic<bool> reentry_ok{true};
    Fault fault = Fault::None; // frozen before real calls
    bool hold_factory = false, hold_next = false, hold_close = false, hold_destructor = false;
    bool factory_entered = false, next_entered = false, close_entered = false, destructor_entered = false, released = false;
    std::atomic<bool> gate_timeout{false};
    std::function<void(unsigned)> on_call;
    std::weak_ptr<sdk::Session> session;
    std::weak_ptr<sdk::EventStream> stream;
    sdk::Runtime* runtime = nullptr; // fixture owns it through all calls
    std::string answer = "owned-answer";
    void Enter(bool& entered, bool held) {
        std::unique_lock lock(mutex); entered = true; cv.notify_all();
        if (held && !cv.wait_for(lock, 20s, [&] { return released; })) gate_timeout = true;
    }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    bool Await(bool State::*field) {
        std::unique_lock lock(mutex); return cv.wait_for(lock, 10s, [&] { return this->*field; });
    }
    std::vector<sdk::Event> Copy() { std::lock_guard lock(mutex); return delivered; }
};
struct Release {
    std::shared_ptr<State> state;
    ~Release() { state->Release(); }
};
class Queue final : public ev::EventQueue {
public:
    Queue(std::shared_ptr<State> state, std::size_t capacity) : state_(std::move(state)), capacity_(capacity) {}
    ~Queue() override { state_->Enter(state_->destructor_entered, state_->hold_destructor); ++state_->queues_destroyed; }
    sdk::Result<void> Push(sdk::Event event) override {
        ++state_->pushes; if (state_->on_call) state_->on_call(1);
        if (state_->fault == Fault::PushThrow) throw std::runtime_error("actual Push throw");
        if (state_->fault == Fault::PushError) return std::unexpected(sdk::Error{"fixture.push", "actual Push error"});
        std::lock_guard lock(mutex_);
        if (closed_) return {};
        { std::lock_guard state_lock(state_->mutex); state_->delivered.push_back(event); }
        const auto cost = event.text.size() + event.payload_json.size() + (event.approval ? event.approval->input_json.size() : 0);
        if (events_.size() >= capacity_ || cost > 16u * 1024u * 1024u || bytes_ > 16u * 1024u * 1024u - cost) {
            closed_ = true; events_.clear(); bytes_ = 0; error_ = "fixture.overflow"; cv_.notify_all();
            return std::unexpected(sdk::Error{error_, {}});
        }
        bytes_ += cost; events_.push_back(std::move(event)); cv_.notify_all(); return {};
    }
    sdk::Result<std::optional<sdk::Event>> Next(std::chrono::milliseconds timeout) override {
        ++state_->nexts; if (state_->on_call) state_->on_call(2);
        state_->Enter(state_->next_entered, state_->hold_next);
        if (state_->fault == Fault::NextThrow) throw std::runtime_error("actual Next throw");
        if (state_->fault == Fault::NextError) return std::unexpected(sdk::Error{"fixture.next", "actual Next error"});
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return closed_ || !events_.empty(); });
        if (closed_) return std::unexpected(sdk::Error{error_, {}});
        if (events_.empty()) return std::optional<sdk::Event>{};
        auto event = std::move(events_.front()); events_.pop_front();
        bytes_ -= event.text.size() + event.payload_json.size() + (event.approval ? event.approval->input_json.size() : 0);
        return std::optional<sdk::Event>{std::move(event)};
    }
    sdk::Result<void> Close() override {
        ++state_->closes; if (state_->on_call) state_->on_call(3);
        { std::lock_guard lock(mutex_); closed_ = true; events_.clear(); bytes_ = 0; cv_.notify_all(); }
        state_->Enter(state_->close_entered, state_->hold_close);
        if (state_->fault == Fault::CloseThrow) throw std::runtime_error("actual Close throw");
        if (state_->fault == Fault::CloseError) return std::unexpected(sdk::Error{"fixture.close", "actual Close error"});
        return {};
    }
private:
    std::shared_ptr<State> state_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<sdk::Event> events_;
    std::size_t capacity_, bytes_ = 0;
    bool closed_ = false;
    std::string error_ = "fixture.closed";
};
class Sink final : public ev::EventSink {
public:
    explicit Sink(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Sink() override { ++state_->sinks_destroyed; }
    sdk::Result<std::unique_ptr<ev::EventQueue>> CreateQueue(std::size_t capacity) override {
        ++state_->factories; if (state_->on_call) state_->on_call(0);
        state_->Enter(state_->factory_entered, state_->hold_factory);
        if (state_->fault == Fault::FactoryThrow) throw std::runtime_error("actual factory throw");
        if (state_->fault == Fault::FactoryError) return std::unexpected(sdk::Error{"fixture.factory", "actual factory error"});
        if (state_->fault == Fault::FactoryNull) return std::unique_ptr<ev::EventQueue>{};
        return std::unique_ptr<ev::EventQueue>(new Queue(state_, capacity));
    }
private:
    std::shared_ptr<State> state_;
};
class Backend final : public sdk::Backend {
public:
    Backend(std::shared_ptr<State> state, bool approval) : state_(std::move(state)), approval_(approval) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++state_->models;
        bool replied = false;
        for (const auto& message : request.messages) if (!message.tool_replies.empty()) replied = true;
        if (approval_ && !replied) return sdk::ModelReply{"before", {{"event-call", "event_tool", R"({"value":"owned"})"}}, {}};
        return sdk::ModelReply{state_->answer, {}, sdk::Usage{3, 4}};
    }
private:
    std::shared_ptr<State> state_;
    bool approval_;
};
sdk::SessionOptions Options(const Directory& directory, std::shared_ptr<State> state, bool host, bool approval = false, fs::path cwd = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd.empty() ? directory.root / "project" : fs::canonical(cwd));
    options.model = "fixture"; options.system_prompt = "Events fixture"; options.max_steps_per_turn = 4;
    options.backend = std::make_unique<Backend>(state, approval); options.approval_timeout = 10s;
    if (host) options.event_sink = std::make_unique<Sink>(state);
    if (approval) {
        sdk::Tool tool; tool.name = "event_tool"; tool.description = "Actual approved tool";
        tool.input_schema_json = R"({"type":"object","properties":{"value":{"type":"string"}},"required":["value"]})";
        tool.execute = [state](const std::string& input, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            ++state->tools; if (input != R"({"value":"owned"})" || context.cwd.empty()) return std::unexpected(sdk::Error{"fixture.tool", "wrong owned input"});
            return sdk::ToolResult{"native-result", false};
        };
        options.custom_tools.push_back(std::move(tool));
    }
    return options;
}
std::vector<sdk::Event> ReadCompletion(const std::shared_ptr<sdk::EventStream>& stream,
    const std::shared_ptr<sdk::Session>& session, const std::string& operation, bool approve = false) {
    std::vector<sdk::Event> values; bool operation_completed = false;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (values.size() < 512 && std::chrono::steady_clock::now() < deadline) {
        auto event = Take(stream->Next(1s)); if (!event) continue;
        REQUIRE(event->session_id == session->id());
        REQUIRE(event->operation_id == operation);
        if (event->approval && approve) Take(session->ResolveApproval(event->approval->request_id, sdk::ApprovalDecision::Accept));
        operation_completed = operation_completed || event->kind == "operation_completed";
        const bool turn_completed = event->kind == "turn.completed";
        values.push_back(std::move(*event)); if (operation_completed && turn_completed) return values;
    }
    FAIL("actual operation and turn completion events missing"); return values;
}
const sdk::Event& Completion(const std::vector<sdk::Event>& values) {
    const auto found = std::find_if(values.begin(), values.end(), [](const auto& value) { return value.kind == "operation_completed"; });
    REQUIRE(found != values.end()); return *found;
}
void SameEvent(const sdk::Event& left, const sdk::Event& right) {
    CHECK(left.kind == right.kind); CHECK(left.session_id == right.session_id); CHECK(left.operation_id == right.operation_id);
    CHECK(left.turn_id == right.turn_id); CHECK(left.text == right.text); CHECK(left.payload_json == right.payload_json);
    REQUIRE(left.approval.has_value() == right.approval.has_value());
    if (left.approval) {
        CHECK(left.approval->request_id == right.approval->request_id); CHECK(left.approval->operation_id == right.approval->operation_id);
        CHECK(left.approval->tool_call_id == right.approval->tool_call_id); CHECK(left.approval->tool_name == right.approval->tool_name);
        CHECK(left.approval->input_json == right.approval->input_json); CHECK(left.approval->cwd == right.approval->cwd);
        CHECK(left.approval->reason == right.approval->reason); CHECK_FALSE(left.approval->child.has_value()); CHECK_FALSE(right.approval->child.has_value());
    }
}
void CheckSucceeded(const std::shared_ptr<sdk::Session>& session, const std::string& operation) {
    const auto result = Take(session->WaitResult(operation, 20s));
    CHECK(result.state == sdk::OperationState::Succeeded); CHECK(result.result_persisted); CHECK(result.error.empty());
}
void InstallReentry(const std::shared_ptr<State>& state) {
    std::weak_ptr<State> weak = state;
    state->on_call = [weak](unsigned phase) {
        auto state = weak.lock(); if (!state) return;
        auto session = state->session.lock(); if (!session) { state->reentry_ok = false; return; }
        const auto subscribe = session->Subscribe();
        const auto close = session->Close();
        const auto wait = session->WaitResult("not-yet-accepted", 0ms);
        const auto runtime = state->runtime->Shutdown();
        bool okay = !subscribe && subscribe.error().code == "sdk.events.reentrant" &&
            !close && close.error().code == "sdk.lifecycle.reentrant" &&
            !wait && wait.error().code == "sdk.lifecycle.reentrant" &&
            !runtime && runtime.error().code == "sdk.lifecycle.reentrant";
        if (auto stream = state->stream.lock()) {
            auto next = stream->Next(0ms); auto closed = stream->CloseChecked();
            okay = okay && !next && next.error().code == "sdk.events.reentrant" && !closed && closed.error().code == "sdk.events.reentrant";
        }
        if (!okay) state->reentry_ok = false;
        ++state->reentry[phase];
    };
}
}

TEST_CASE("SDK EventSink default queue retains owned values and original errors" * doctest::test_suite("sdk_event_sink")) {
    Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots()));
    auto state = std::make_shared<State>(); auto session = Take(runtime->OpenSession(Options(directory, state, false, true)));
    auto first = Take(session->Subscribe()); auto second = Take(session->Subscribe());
    CHECK_FALSE(Take(first->Next(0ms)).has_value());
    CHECK_FALSE(session->Subscribe(0).has_value()); CHECK_FALSE(session->Subscribe(65537).has_value());
    const auto negative = first->Next(-1ms); REQUIRE_FALSE(negative.has_value()); CHECK(negative.error().code == "sdk.timeout.invalid");
    const auto receipt = Take(session->Submit("default", "question"));
    const auto left = ReadCompletion(first, session, receipt.operation_id, true);
    const auto right = ReadCompletion(second, session, receipt.operation_id);
    REQUIRE(left.size() == right.size()); REQUIRE(left.size() > 2);
    for (std::size_t index = 0; index < left.size(); ++index) SameEvent(left[index], right[index]);
    CHECK(left.back().kind == "turn.completed"); CHECK(Completion(left).text == state->answer);
    CheckSucceeded(session, receipt.operation_id); CHECK(state->models.load() == 2); CHECK(state->tools.load() == 1);
    Take(first->CloseChecked()); Take(first->CloseChecked()); Take(session->Close()); Take(runtime->Shutdown());
    const auto closed = first->Next(0ms); REQUIRE_FALSE(closed.has_value()); CHECK(closed.error().code == "sdk.events.closed");
    std::cout << "[sdk-event-sink-path] default\n";
}

TEST_CASE("SDK EventSink host queue receives actual delta approval and completion" * doctest::test_suite("sdk_event_sink")) {
    Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots()));
    auto state = std::make_shared<State>(); state->runtime = runtime.get();
    auto session = Take(runtime->OpenSession(Options(directory, state, true, true))); state->session = session;
    InstallReentry(state); auto stream = Take(session->Subscribe()); state->stream = stream;
    auto peer = Take(session->Subscribe());
    const auto receipt = Take(session->Submit("actual", "question"));
    const auto events = ReadCompletion(stream, session, receipt.operation_id, true);
    const auto other = ReadCompletion(peer, session, receipt.operation_id);
    REQUIRE(events.size() == other.size()); bool approval = false, text = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        SameEvent(events[i], other[i]); text = text || events[i].text == "before" || events[i].text == state->answer;
        if (events[i].approval) { approval = true; CHECK(events[i].kind == "approval_requested"); CHECK(events[i].approval->tool_name == "event_tool"); CHECK(events[i].approval->cwd == Utf8(directory.root / "project")); }
    }
    CHECK(approval); CHECK(text); CheckSucceeded(session, receipt.operation_id);
    Take(stream->CloseChecked()); Take(peer->CloseChecked()); Take(session->Close()); Take(runtime->Shutdown());
    CHECK(state->factories.load() == 2); CHECK(state->closes.load() == 2); CHECK(state->queues_destroyed.load() == 2); CHECK(state->sinks_destroyed.load() == 1);
    CHECK(state->models.load() == 2); CHECK(state->tools.load() == 1); CHECK(state->reentry_ok.load());
    for (const auto& phase : state->reentry) CHECK(phase.load() > 0);
    std::cout << "[sdk-event-sink-path] actual\n";
}

TEST_CASE("SDK EventSink overflow stays local and does not poison checked Close" * doctest::test_suite("sdk_event_sink")) {
    for (bool host : {false, true}) {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>();
        auto session = Take(runtime->OpenSession(Options(directory, state, host)));
        auto tiny = Take(session->Subscribe(1)); auto healthy = Take(session->Subscribe());
        const auto receipt = Take(session->Submit("overflow", "answer"));
        ReadCompletion(healthy, session, receipt.operation_id); CheckSucceeded(session, receipt.operation_id);
        // Close before reading: original default overflow must survive this route.
        Take(tiny->CloseChecked()); const auto error = tiny->Next(0ms); REQUIRE_FALSE(error.has_value());
        CHECK(error.error().code == (host ? "sdk.events.provider_failed" : "sdk.events.overflow"));
        if (host) CHECK(error.error().message.find("fixture.overflow") != std::string::npos);
        Take(session->Close()); Take(runtime->Shutdown()); CHECK(state->models.load() == 1);
    }
    std::cout << "[sdk-event-sink-path] overflow\n";
}

TEST_CASE("SDK EventSink real provider failures retain notification and Close receipts" * doctest::test_suite("sdk_event_sink")) {
    for (const auto fault : {Fault::FactoryError, Fault::FactoryThrow, Fault::FactoryNull, Fault::PushError, Fault::PushThrow, Fault::NextError, Fault::NextThrow, Fault::CloseError, Fault::CloseThrow}) {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>(); state->fault = fault;
        auto session = Take(runtime->OpenSession(Options(directory, state, true)));
        auto subscribed = session->Subscribe();
        if (fault == Fault::FactoryError || fault == Fault::FactoryThrow || fault == Fault::FactoryNull) {
            REQUIRE_FALSE(subscribed.has_value());
            CHECK(subscribed.error().code == (fault == Fault::FactoryNull ? "sdk.events.provider_invalid" : "sdk.events.provider_failed"));
            CHECK(state->queues_destroyed.load() == 0); CHECK(state->models.load() == 0); CHECK(state->tools.load() == 0);
        } else {
            auto stream = Take(std::move(subscribed));
            if (fault == Fault::CloseError || fault == Fault::CloseThrow) {
                stream->Close(); auto checked = stream->CloseChecked(); REQUIRE_FALSE(checked.has_value());
                CHECK(checked.error().code == "sdk.events.provider_close_failed");
                stream.reset(); // weak subscription gone; Session/Runtime must retain true Close failure
            } else {
                const auto receipt = Take(session->Submit("error", "actual work")); CheckSucceeded(session, receipt.operation_id);
                auto next = stream->Next(0ms); REQUIRE_FALSE(next.has_value()); CHECK(next.error().code == "sdk.events.provider_failed");
                Take(stream->CloseChecked()); CHECK(state->models.load() == 1);
            }
            CHECK(state->closes.load() == 1); CHECK(state->queues_destroyed.load() == 1);
        }
        const auto closed = session->Close(); const auto repeated = session->Close();
        session.reset(); // expired Runtime weak entry must not discard the real close receipt
        const auto shutdown = runtime->Shutdown();
        if (fault == Fault::CloseError || fault == Fault::CloseThrow) {
            REQUIRE_FALSE(closed.has_value()); REQUIRE_FALSE(repeated.has_value()); REQUIRE_FALSE(shutdown.has_value());
            CHECK(closed.error().code == "sdk.events.provider_close_failed"); CHECK(repeated.error().message == closed.error().message);
            CHECK(shutdown.error().message == closed.error().message);
        } else { Take(closed); Take(repeated); Take(shutdown); }
        CHECK(state->sinks_destroyed.load() == 1);
    }
    std::cout << "[sdk-event-sink-path] error\n";
}

TEST_CASE("SDK EventSink Close waits for actual borrows and reclaims late factory" * doctest::test_suite("sdk_event_sink")) {
    {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>(); state->hold_next = true;
        auto session = Take(runtime->OpenSession(Options(directory, state, true))); auto stream = Take(session->Subscribe());
        std::future<sdk::Result<std::optional<sdk::Event>>> next; std::future<sdk::Result<void>> close;
        Release release{state};
        next = std::async(std::launch::async, [stream] { return stream->Next(20s); }); REQUIRE(state->Await(&State::next_entered));
        close = std::async(std::launch::async, [session] { return session->Close(); }); REQUIRE(state->Await(&State::close_entered));
        CHECK(close.wait_for(20ms) == std::future_status::timeout); CHECK(state->queues_destroyed.load() == 0); CHECK(state->sinks_destroyed.load() == 0);
        state->Release(); REQUIRE(next.wait_for(10s) == std::future_status::ready); REQUIRE(close.wait_for(10s) == std::future_status::ready);
        const auto read_closed = next.get(); REQUIRE_FALSE(read_closed.has_value()); CHECK(read_closed.error().code == "sdk.events.closed");
        Take(close.get()); Take(stream->CloseChecked());
        CHECK(state->queues_destroyed.load() == 1); CHECK(state->sinks_destroyed.load() == 1); CHECK_FALSE(state->gate_timeout.load());
    }
    {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>(); state->hold_factory = true;
        auto session = Take(runtime->OpenSession(Options(directory, state, true)));
        std::future<sdk::Result<std::shared_ptr<sdk::EventStream>>> subscribe; std::future<sdk::Result<void>> close;
        Release release{state};
        subscribe = std::async(std::launch::async, [session] { return session->Subscribe(); }); REQUIRE(state->Await(&State::factory_entered));
        close = std::async(std::launch::async, [session] { return session->Close(); });
        // Observe the real closing gate, rather than guess from thread launch.
        bool closing = false; const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!closing && std::chrono::steady_clock::now() < deadline) {
            auto result = session->Submit("closing", "never dispatch after close");
            closing = !result && result.error().code == "sdk.session.closed";
            if (!closing) std::this_thread::yield();
        }
        REQUIRE(closing); CHECK(close.wait_for(20ms) == std::future_status::timeout); CHECK(state->sinks_destroyed.load() == 0);
        state->Release(); REQUIRE(subscribe.wait_for(10s) == std::future_status::ready); REQUIRE(close.wait_for(10s) == std::future_status::ready);
        const auto late = subscribe.get(); REQUIRE_FALSE(late.has_value()); CHECK(late.error().code == "sdk.session.closed"); Take(close.get());
        CHECK(state->factories.load() == 1); CHECK(state->closes.load() == 1); CHECK(state->queues_destroyed.load() == 1); CHECK(state->sinks_destroyed.load() == 1); CHECK_FALSE(state->gate_timeout.load());
    }
    {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>(); state->hold_close = true;
        auto session = Take(runtime->OpenSession(Options(directory, state, true))); auto stream = Take(session->Subscribe());
        std::future<sdk::Result<void>> stream_close; std::future<sdk::Result<void>> session_close;
        Release release{state};
        stream_close = std::async(std::launch::async, [stream] { return stream->CloseChecked(); }); REQUIRE(state->Await(&State::close_entered));
        session_close = std::async(std::launch::async, [session] { return session->Close(); });
        CHECK(session_close.wait_for(20ms) == std::future_status::timeout); CHECK(stream_close.wait_for(20ms) == std::future_status::timeout);
        CHECK(state->queues_destroyed.load() == 0); CHECK(state->sinks_destroyed.load() == 0);
        state->Release(); REQUIRE(stream_close.wait_for(10s) == std::future_status::ready); REQUIRE(session_close.wait_for(10s) == std::future_status::ready);
        Take(stream_close.get()); Take(session_close.get()); CHECK(state->closes.load() == 1); CHECK(state->queues_destroyed.load() == 1); CHECK(state->sinks_destroyed.load() == 1); CHECK_FALSE(state->gate_timeout.load());
    }
    {
        Directory directory; auto runtime = Take(sdk::Runtime::Create(directory.Roots())); auto state = std::make_shared<State>(); state->hold_destructor = true;
        auto session = Take(runtime->OpenSession(Options(directory, state, true))); auto stream = Take(session->Subscribe());
        std::future<sdk::Result<void>> stream_close; std::future<sdk::Result<void>> session_close;
        Release release{state};
        stream_close = std::async(std::launch::async, [stream] { return stream->CloseChecked(); }); REQUIRE(state->Await(&State::destructor_entered));
        session_close = std::async(std::launch::async, [session] { return session->Close(); });
        CHECK(stream_close.wait_for(20ms) == std::future_status::timeout); CHECK(session_close.wait_for(20ms) == std::future_status::timeout);
        CHECK(state->queues_destroyed.load() == 0); CHECK(state->sinks_destroyed.load() == 0);
        state->Release(); REQUIRE(stream_close.wait_for(10s) == std::future_status::ready); REQUIRE(session_close.wait_for(10s) == std::future_status::ready);
        Take(stream_close.get()); Take(session_close.get()); CHECK(state->closes.load() == 1); CHECK(state->queues_destroyed.load() == 1); CHECK(state->sinks_destroyed.load() == 1); CHECK_FALSE(state->gate_timeout.load());
    }
    std::cout << "[sdk-event-sink-path] drain\n";
}

TEST_CASE("SDK EventSink four real Sessions keep independent providers and values" * doctest::test_suite("sdk_event_sink")) {
    Directory directory; fs::create_directories(directory.root / "other-project"); fs::create_directories(directory.root / "third-project");
    auto runtime = Take(sdk::Runtime::Create(directory.Roots()));
    std::array<std::shared_ptr<State>, 4> states; std::array<std::shared_ptr<sdk::Session>, 4> sessions;
    std::array<std::shared_ptr<sdk::EventStream>, 4> streams; std::array<sdk::Receipt, 4> receipts;
    for (unsigned i = 0; i < 4; ++i) {
        states[i] = std::make_shared<State>(); states[i]->answer = "scene-" + std::to_string(i);
        const auto cwd = i < 2 ? directory.root / "project" : directory.root / (i == 2 ? "other-project" : "third-project");
        sessions[i] = Take(runtime->OpenSession(Options(directory, states[i], true, false, cwd)));
        streams[i] = Take(sessions[i]->Subscribe()); receipts[i] = Take(sessions[i]->Submit("same-key", "scene input"));
    }
    for (unsigned i = 0; i < 4; ++i) {
        const auto values = ReadCompletion(streams[i], sessions[i], receipts[i].operation_id); REQUIRE_FALSE(values.empty());
        CHECK(Completion(values).text == states[i]->answer); CheckSucceeded(sessions[i], receipts[i].operation_id);
        const auto delivered = states[i]->Copy(); REQUIRE(values.size() == delivered.size());
        for (std::size_t j = 0; j < values.size(); ++j) SameEvent(values[j], delivered[j]);
        Take(sessions[i]->Close()); CHECK(states[i]->sinks_destroyed.load() == 1); CHECK(states[i]->queues_destroyed.load() == 1);
        CHECK(states[i]->models.load() == 1); CHECK(states[i]->factories.load() == 1); CHECK(states[i]->closes.load() == 1);
        for (unsigned later = i + 1; later < 4; ++later) CHECK(states[later]->sinks_destroyed.load() == 0);
    }
    Take(runtime->Shutdown());
    std::cout << "[sdk-event-sink-path] isolation\n";
}
