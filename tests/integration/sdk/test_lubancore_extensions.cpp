#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <lubancore/core.hpp>
#include <lubancore/extensions.hpp>

#include "platform/paths.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

namespace {
namespace sdk = lubancore;
namespace ext = lubancore::extensions::v1;
namespace v3 = lubancode::trajectory::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-extensions-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::platform::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
    fs::path SessionFile(const std::string& id) const {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(directory.has_value());
        return *directory / "sessions" / id / (id + ".jsonl");
    }
};

using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class Backend final : public sdk::Backend {
public:
    explicit Backend(GenerateFunction generate, std::shared_ptr<std::atomic<bool>> alive = {})
        : generate_(std::move(generate)), alive_(std::move(alive)) {
        if (alive_) alive_->store(true);
    }
    ~Backend() override { if (alive_) alive_->store(false); }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& input, sdk::Cancellation cancellation) override {
        return generate_(input, cancellation);
    }
private:
    GenerateFunction generate_;
    std::shared_ptr<std::atomic<bool>> alive_;
};

sdk::SessionOptions Options(const Fixture& fixture, GenerateFunction generate,
                            std::shared_ptr<std::atomic<bool>> alive = {}) {
    sdk::SessionOptions options;
    options.cwd = fixture.Utf8(fixture.root / "cwd");
    options.model = "extension-fixture";
    options.system_prompt = "Run the explicit extension fixture.";
    options.max_steps_per_turn = 4;
    options.backend = std::make_unique<Backend>(std::move(generate), std::move(alive));
    return options;
}
sdk::ModelReply Answer(std::string text = "extension-answer") {
    return {std::move(text), {}, sdk::Usage{4, 3}};
}
bool Has(const sdk::ModelRequest& request, const std::string& text) {
    for (const auto& message : request.messages) if (message.text.find(text) != std::string::npos) return true;
    return false;
}
using InvokeFunction = std::function<sdk::Result<ext::HandlerReturn>(const ext::Context&, const ext::Input&, ext::Next)>;
class Instance final : public ext::Instance {
public:
    explicit Instance(InvokeFunction invoke, std::function<void()> on_destroy = {})
        : invoke_(std::move(invoke)), on_destroy_(std::move(on_destroy)) {}
    ~Instance() override { if (on_destroy_) on_destroy_(); }
    sdk::Result<ext::HandlerReturn> Invoke(const ext::Context& context, const ext::Input& input, ext::Next next) override {
        return invoke_(context, input, std::move(next));
    }
private:
    InvokeFunction invoke_;
    std::function<void()> on_destroy_;
};

ext::HandlerDefinition Definition(std::string name, ext::Point point = ext::Point::PreUser,
                                  ext::Stage stage = ext::Stage::Default) {
    ext::HandlerDefinition definition;
    definition.name = std::move(name);
    definition.point = point;
    definition.stage = stage;
    definition.definition_hash = "fixture-native-v1";
    return definition;
}
ext::Registration Registration(std::string id, std::vector<ext::HandlerDefinition> definitions,
                               InvokeFunction invoke, std::function<void()> on_destroy = {}) {
    ext::Registration registration;
    registration.manifest.id = std::move(id);
    registration.manifest.version = "1.0.0";
    registration.manifest.handlers = std::move(definitions);
    registration.source_label = "SDK regression host";
    registration.factory = [invoke = std::move(invoke), on_destroy = std::move(on_destroy)](const ext::SessionContext&)
        -> sdk::Result<std::unique_ptr<ext::Instance>> {
        return std::make_unique<Instance>(invoke, on_destroy);
    };
    return registration;
}
sdk::Result<ext::HandlerReturn> Continue(ext::Next next, std::optional<std::string> candidate = std::nullopt) {
    auto downstream = next.Call(std::move(candidate));
    if (!downstream) return std::unexpected(downstream.error());
    if (downstream->kind == ext::DownstreamOutcome::Kind::Denied)
        return ext::HandlerReturn::Denied(downstream->code, downstream->message);
    if (downstream->kind == ext::DownstreamOutcome::Kind::Failed)
        return std::unexpected(sdk::Error{downstream->code, downstream->message});
    return ext::HandlerReturn{};
}
sdk::Operation Run(const std::shared_ptr<sdk::Session>& session, const std::string& key,
                   const std::string& text) {
    auto receipt = session->Submit(key, text);
    REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 15s);
    REQUIRE(operation.has_value());
    REQUIRE(operation->state != sdk::OperationState::Accepted);
    REQUIRE(operation->state != sdk::OperationState::Running);
    CHECK(operation->result_persisted);
    return *operation;
}
std::string MessageText(const Json& message) {
    std::string text;
    if (!message.contains("content")) return text;
    if (message["content"].is_string()) return message["content"].get<std::string>();
    for (const auto& block : message["content"])
        if (block.is_object() && block.value("type", "") == "text") text += block.value("text", "");
    return text;
}
bool FormalUserContains(const v3::V3Ledger& ledger, const std::string& text) {
    const auto context = v3::ProjectModelContext(ledger);
    for (const auto& input : context.inputs)
        if (input.message.value("role", "") == "user" && MessageText(input.message).find(text) != std::string::npos)
            return true;
    return false;
}
} // namespace

TEST_CASE("SDK extensions: real chain adopts rewrite and records appended request material in V3") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto order = std::make_shared<std::vector<std::string>>();
    auto contexts = std::make_shared<std::vector<ext::Context>>();
    auto seen_request = std::make_shared<sdk::ModelRequest>();
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto options = Options(fixture, [=](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        *seen_request = request;
        return Answer();
    });
    auto first = Definition("prompt.first");
    first.priority = 900;
    first.before = {"prompt.second"};
    auto second = Definition("prompt.second");
    second.priority = 1;
    auto post = Definition("memory.post", ext::Point::PostUser);
    auto estimate = Definition("request.read", ext::Point::PreRequest, ext::Stage::Estimate);
    estimate.after = {"context.token_estimate"};
    options.extensions.push_back(Registration("chain", {second, post, estimate, first},
        [=](const ext::Context& context, const ext::Input& input, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
            CHECK(input.schema_version == 1);
            CHECK_FALSE(context.session_id.empty());
            CHECK_FALSE(context.operation_id.empty());
            CHECK_FALSE(context.dispatch_id.empty());
            CHECK_FALSE(context.invocation_id.empty());
            REQUIRE(context.turn_id.has_value());
            contexts->push_back(context);
            order->push_back(context.hook_name);
            if (context.hook_name == "prompt.first")
                return Continue(next, Json{{"prompt", "adopted-input"}}.dump());
            if (context.hook_name == "prompt.second") {
                CHECK(Json::parse(input.json).at("prompt") == "adopted-input");
                auto result = Continue(next);
                if (result) result->effects.push_back({ext::EffectType::ContextAppend, R"({"text":"pre-material"})"});
                return result;
            }
            if (context.point == ext::Point::PostUser) {
                CHECK(Json::parse(input.json).at("prompt") == "adopted-input");
                auto result = Continue(next);
                if (result) result->effects.push_back({ext::EffectType::ContextAppend, R"({"text":"post-material"})"});
                return result;
            }
            CHECK(context.stage == ext::Stage::Estimate);
            REQUIRE(context.step_id.has_value());
            // This gate runs before the physical request identity is issued.
            CHECK_FALSE(context.request_id.has_value());
            CHECK(input.json.find("adopted-input") != std::string::npos);
            CHECK(input.json.find("pre-material") != std::string::npos);
            CHECK(input.json.find("post-material") != std::string::npos);
            return Continue(next);
        }));
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    const auto session = *opened;
    const auto operation = Run(session, "chain-key", "original-input");
    CHECK(operation.state == sdk::OperationState::Succeeded);
    CHECK(calls->load() == 1);
    CHECK(Has(*seen_request, "adopted-input"));
    CHECK_FALSE(Has(*seen_request, "original-input"));
    CHECK(Has(*seen_request, "pre-material"));
    CHECK(Has(*seen_request, "post-material"));
    REQUIRE(order->size() == 4);
    CHECK((*order)[0] == "prompt.first");
    CHECK((*order)[1] == "prompt.second");
    CHECK((*order)[2] == "memory.post");
    CHECK((*order)[3] == "request.read");
    for (const auto& context : *contexts) {
        CHECK(context.session_id == session->id());
        CHECK(context.operation_id == operation.operation_id);
        CHECK(context.turn_id == operation.turn_id);
    }
    REQUIRE(session->Close().has_value());
    auto ledger = v3::ReadV3Ledger(fixture.SessionFile(session->id()));
    REQUIRE(ledger.has_value());
    CHECK(FormalUserContains(*ledger, "adopted-input"));
    CHECK_FALSE(FormalUserContains(*ledger, "original-input"));
    CHECK(FormalUserContains(*ledger, "pre-material"));
    CHECK(FormalUserContains(*ledger, "post-material"));
    bool rewrite = false, prepared = false;
    for (const auto& event : ledger->events) {
        if (event.kind == v3::EventKindV3::HookEffectsApplied && event.payload.value("effectType", "") == "input.rewrite")
            rewrite = event.payload.at("appliedValueRef").at("prompt") == "adopted-input";
        if (event.kind == v3::EventKindV3::ModelRequestPrepared) {
            prepared = true;
            CHECK(v3::CheckPreparedAgainstChain(*ledger, event.event_id).empty());
            const auto& refs = event.payload.at("inputMessageRefs");
            REQUIRE(refs.size() == seen_request->messages.size());
            for (std::size_t index = 0; index < refs.size(); ++index) {
                const auto* message = ledger->FindMessage(refs[index].get<std::string>());
                REQUIRE(message != nullptr);
                CHECK(message->message.at("role") == seen_request->messages[index].role);
                CHECK(MessageText(message->message) == seen_request->messages[index].text);
            }
        }
    }
    Json original;
    std::ifstream original_file(fixture.SessionFile(session->id()).parent_path() /
        "operations-inputs" / (operation.operation_id + ".json"));
    REQUIRE(original_file.is_open());
    original_file >> original;
    CHECK(rewrite);
    CHECK(prepared);
    CHECK(original.at("text") == "original-input");
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: admission boundaries keep only the formal user already committed") {
    for (const auto point : {ext::Point::PreUser, ext::Point::PostUser, ext::Point::PreRequest}) {
        CAPTURE(static_cast<int>(point));
        Fixture fixture;
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            ++*calls; return Answer();
        });
        auto definition = Definition("guard.reject", point,
            point == ext::Point::PreRequest ? ext::Stage::Capacity : ext::Stage::Default);
        if (point == ext::Point::PreRequest) definition.after = {"context.capacity_check"};
        options.extensions.push_back(Registration("admission", {definition},
            [](const ext::Context&, const ext::Input&, ext::Next) -> sdk::Result<ext::HandlerReturn> {
                return ext::HandlerReturn::Denied("fixture.denied", "extension rejected fixture input");
            }));
        auto opened = (*runtime)->OpenSession(std::move(options));
        REQUIRE(opened.has_value());
        const auto operation = Run(*opened, "denied-key", "denied-formal-user");
        CHECK(operation.state == sdk::OperationState::Failed);
        CHECK_FALSE(operation.error.empty());
        CHECK(calls->load() == 0);
        REQUIRE((*opened)->Close().has_value());
        auto ledger = v3::ReadV3Ledger(fixture.SessionFile((*opened)->id()));
        REQUIRE(ledger.has_value());
        CHECK(FormalUserContains(*ledger, "denied-formal-user") == (point != ext::Point::PreUser));
        bool denied = false, sent = false;
        for (const auto& event : ledger->events) {
            if (event.kind == v3::EventKindV3::HookCompleted && event.payload.value("decision", "") == "deny") denied = true;
            if (event.kind == v3::EventKindV3::ModelRequestSent) sent = true;
        }
        CHECK(denied);
        CHECK_FALSE(sent);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK extensions: required failure cannot fall back and optional failure continues exactly once") {
    for (const auto mode : {0, 1, 2}) {
        CAPTURE(mode);
        Fixture fixture;
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        auto later = std::make_shared<std::atomic<unsigned>>(0);
        auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            ++*calls; return Answer();
        });
        auto fail = Definition("prompt.failure");
        fail.required = mode == 0;
        fail.failure_policy = ext::FailurePolicy::KeepOriginal;
        fail.before = {"prompt.after"};
        options.extensions.push_back(Registration("failure-policy", {fail, Definition("prompt.after")},
            [=](const ext::Context& context, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                if (context.hook_name == "prompt.after") { ++*later; return Continue(next); }
                if (mode == 2) {
                    auto result = next.Call();
                    REQUIRE(result.has_value());
                }
                return std::unexpected(sdk::Error{"fixture.handler.failed", "deliberate fixture failure"});
            }));
        auto opened = (*runtime)->OpenSession(std::move(options));
        REQUIRE(opened.has_value());
        const auto operation = Run(*opened, "failure-key", "retained-original");
        CHECK(operation.state == (mode == 0 ? sdk::OperationState::Failed : sdk::OperationState::Succeeded));
        CHECK(calls->load() == (mode == 0 ? 0 : 1));
        CHECK(later->load() == (mode == 0 ? 0 : 1));
        REQUIRE((*opened)->Close().has_value());
        auto ledger = v3::ReadV3Ledger(fixture.SessionFile((*opened)->id()));
        REQUIRE(ledger.has_value());
        bool failed = false;
        for (const auto& event : ledger->events)
            if (event.kind == v3::EventKindV3::HookFailed && event.payload.value("error_code", "") == "fixture.handler.failed") failed = true;
        CHECK(failed);
        CHECK(FormalUserContains(*ledger, "retained-original") == (mode != 0));
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK extensions: read-only stages and strict prompt candidates cannot alter model input") {
    for (const auto mode : {0, 1, 2}) {
        CAPTURE(mode);
        Fixture fixture;
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            ++*calls; return Answer();
        });
        auto definition = Definition("immutable.guard", mode == 0 ? ext::Point::PreUser :
            mode == 1 ? ext::Point::PostUser : ext::Point::PreRequest,
            mode == 2 ? ext::Stage::Estimate : ext::Stage::Default);
        definition.required = true;
        options.extensions.push_back(Registration("immutable", {definition},
            [=](const ext::Context&, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                const auto result = next.Call(mode == 0 ? R"({"prompt":"tampered","extra":true})" : R"({"prompt":"tampered"})");
                REQUIRE_FALSE(result.has_value());
                CHECK(result.error().code == "hook.next.bad_candidate");
                return std::unexpected(result.error());
            }));
        auto opened = (*runtime)->OpenSession(std::move(options));
        REQUIRE(opened.has_value());
        CHECK(Run(*opened, "immutable-key", "unchanged-input").state == sdk::OperationState::Failed);
        CHECK(calls->load() == 0);
        REQUIRE((*opened)->Close().has_value());
        auto ledger = v3::ReadV3Ledger(fixture.SessionFile((*opened)->id()));
        REQUIRE(ledger.has_value());
        CHECK_FALSE(FormalUserContains(*ledger, "tampered"));
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK extensions: invalid plans fail before factories and do not poison the runtime") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    for (const auto mode : {0, 1, 2}) {
        CAPTURE(mode);
        auto factory_calls = std::make_shared<std::atomic<unsigned>>(0);
        auto options = Options(fixture, [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Answer(); });
        auto first = Definition("bad.first");
        auto second = Definition("bad.second");
        if (mode == 0) first.after = {"not.registered"};
        if (mode == 1) { first.after = {"bad.second"}; second.after = {"bad.first"}; }
        if (mode == 2) second = first;
        auto registration = Registration("bad-plan", {first, second},
            [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); });
        auto factory = registration.factory;
        registration.factory = [factory_calls, factory](const ext::SessionContext& context) {
            ++*factory_calls; return factory(context);
        };
        options.extensions.push_back(std::move(registration));
        auto opened = (*runtime)->OpenSession(std::move(options));
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == (mode == 0 ? "hook.plan.missing_dependency" :
            mode == 1 ? "hook.plan.dependency_cycle" : "hook.plan.same_key_conflict"));
        CHECK(factory_calls->load() == 0);
    }
    auto healthy = (*runtime)->OpenSession(Options(fixture,
        [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Answer(); }));
    REQUIRE(healthy.has_value());
    CHECK(Run(*healthy, "healthy-key", "still healthy").state == sdk::OperationState::Succeeded);
    REQUIRE((*healthy)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: a failed factory drains staged instances before the backend") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto alive = std::make_shared<std::atomic<bool>>(false);
    auto destroyed = std::make_shared<std::atomic<unsigned>>(0);
    auto alive_at_destroy = std::make_shared<std::atomic<bool>>(false);
    auto options = Options(fixture,
        [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Answer(); }, alive);
    options.extensions.push_back(Registration("staged", {Definition("staged.first")},
        [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); },
        [=] { ++*destroyed; alive_at_destroy->store(alive->load()); }));
    auto failed = Registration("failing", {Definition("staged.failed")},
        [](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); });
    failed.factory = [](const ext::SessionContext&) -> sdk::Result<std::unique_ptr<ext::Instance>> {
        return std::unexpected(sdk::Error{"fixture.factory.failed", "factory rollback fixture"});
    };
    options.extensions.push_back(std::move(failed));
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == "sdk.extension.factory_failed");
    CHECK(opened.error().message.find("fixture.factory.failed") != std::string::npos);
    CHECK(destroyed->load() == 1);
    CHECK(alive_at_destroy->load());
    CHECK_FALSE(alive->load());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: Next grants reject wrong threads duplicate calls and saved invocations") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto saved = std::make_shared<ext::Next>();
    auto reached = std::make_shared<std::atomic<unsigned>>(0);
    auto holder = std::make_shared<std::weak_ptr<sdk::Session>>();
    auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*reached; return Answer();
    });
    options.extensions.push_back(Registration("next-grants", {Definition("prompt.next")},
        [saved, holder, raw_runtime = runtime->get()](const ext::Context& context, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
            auto current = holder->lock();
            REQUIRE(current != nullptr);
            auto close = current->Close();
            REQUIRE_FALSE(close.has_value());
            CHECK(close.error().code == "sdk.lifecycle.reentrant");
            auto wait = current->WaitResult(context.operation_id, 1ms);
            REQUIRE_FALSE(wait.has_value());
            CHECK(wait.error().code == "sdk.lifecycle.reentrant");
            auto shutdown = raw_runtime->Shutdown();
            REQUIRE_FALSE(shutdown.has_value());
            CHECK(shutdown.error().code == "sdk.lifecycle.reentrant");
            CHECK(current->ReadOperation(context.operation_id).has_value());
            *saved = next;
            auto foreign = std::async(std::launch::async, [next] { return next.Call(); }).get();
            REQUIRE_FALSE(foreign.has_value());
            CHECK(foreign.error().code == "hook.next.wrong_thread");
            auto first = next.Call();
            REQUIRE(first.has_value());
            CHECK(first->kind == ext::DownstreamOutcome::Kind::Value);
            auto repeated = saved->Call();
            REQUIRE_FALSE(repeated.has_value());
            CHECK(repeated.error().code == "hook.next.already_consumed");
            return ext::HandlerReturn{};
        }));
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    *holder = *opened;
    CHECK(Run(*opened, "next-key", "next input").state == sdk::OperationState::Succeeded);
    CHECK(reached->load() == 1);
    auto expired = saved->Call();
    REQUIRE_FALSE(expired.has_value());
    CHECK(expired.error().code == "hook.next.expired");
    const auto description = (*opened)->DescribeExtensions();
    REQUIRE(description.has_value());
    CHECK(description->find("next-grants") != std::string::npos);
    REQUIRE((*opened)->Close().has_value());
    auto closed_description = (*opened)->DescribeExtensions();
    REQUIRE(closed_description.has_value());
    CHECK(*closed_description == *description);
    expired = saved->Call();
    REQUIRE_FALSE(expired.has_value());
    CHECK(expired.error().code == "hook.next.expired");
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: overriding a required request slot cannot bypass EST1 or effect validation") {
    for (const auto stage : {ext::Stage::Estimate, ext::Stage::Capacity}) {
        CAPTURE(static_cast<int>(stage));
        Fixture fixture;
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        auto invoked = std::make_shared<std::atomic<unsigned>>(0);
        auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            ++*calls; return Answer();
        });
        auto definition = Definition(stage == ext::Stage::Estimate ? "context.token_estimate" :
            "context.capacity_check", ext::Point::PreRequest, stage);
        definition.failure_policy = ext::FailurePolicy::KeepOriginal;
        definition.required = false; // The underlying builtin slot still owns this requirement.
        options.extensions.push_back(Registration("bad-request-slot", {definition},
            [=](const ext::Context&, const ext::Input&, ext::Next) -> sdk::Result<ext::HandlerReturn> {
                ++*invoked;
                ext::HandlerReturn result;
                if (stage == ext::Stage::Estimate) result.output_json = R"({"estimatedInputTokens":1})";
                else result.effects.push_back({ext::EffectType::ContextAppend, R"({"text":"illegal-request-context"})"});
                return result;
            }));
        auto opened = (*runtime)->OpenSession(std::move(options));
        REQUIRE(opened.has_value());
        CHECK(Run(*opened, "invalid-slot-key", "guarded-request-input").state == sdk::OperationState::Failed);
        CHECK(invoked->load() == 1);
        CHECK(calls->load() == 0);
        REQUIRE((*opened)->Close().has_value());
        auto ledger = v3::ReadV3Ledger(fixture.SessionFile((*opened)->id()));
        REQUIRE(ledger.has_value());
        bool failed = false;
        for (const auto& event : ledger->events)
            if (event.kind == v3::EventKindV3::HookFailed && event.payload.value("error_code", "") == "hook.result.invalid") failed = true;
        CHECK(failed);
        CHECK(FormalUserContains(*ledger, "guarded-request-input"));
        CHECK_FALSE(FormalUserContains(*ledger, "illegal-request-context"));
        REQUIRE((*runtime)->Shutdown().has_value());
    }
}

TEST_CASE("SDK extensions: close cancels a real handler and waits for its actual exit before destruction") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        std::atomic<bool> entered{false}, cancelled{false}, returned{false}, destroyed{false}, timed_out{false};
    };
    auto gate = std::make_shared<Gate>();
    auto alive = std::make_shared<std::atomic<bool>>(false);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto options = Options(fixture, [=](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls; return Answer();
    }, alive);
    options.extensions.push_back(Registration("close-gate", {Definition("handler.hold")},
        [gate](const ext::Context& context, const ext::Input&, ext::Next) -> sdk::Result<ext::HandlerReturn> {
            std::unique_lock lock(gate->mutex);
            gate->entered.store(true);
            gate->cv.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!context.cancellation.requested() && std::chrono::steady_clock::now() < deadline)
                gate->cv.wait_for(lock, 2ms);
            gate->cancelled.store(context.cancellation.requested());
            gate->cv.notify_all();
            if (!gate->cv.wait_until(lock, deadline, [&] { return gate->release; })) gate->timed_out.store(true);
            gate->returned.store(true);
            return ext::HandlerReturn{};
        }, [=] { CHECK(gate->returned.load()); CHECK(alive->load()); gate->destroyed.store(true); }));
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    auto receipt = (*opened)->Submit("held-key", "held input");
    REQUIRE(receipt.has_value());
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 5s, [&] { return gate->entered.load(); }));
    }
    auto closing = std::async(std::launch::async, [session = *opened] { return session->Close(); });
    {
        std::unique_lock lock(gate->mutex);
        CHECK(gate->cv.wait_for(lock, 5s, [&] { return gate->cancelled.load(); }));
        CHECK(closing.wait_for(0ms) == std::future_status::timeout);
        CHECK_FALSE(gate->destroyed.load());
        CHECK(alive->load());
        CHECK(calls->load() == 0);
        gate->release = true;
        gate->cv.notify_all();
    }
    REQUIRE(closing.wait_for(5s) == std::future_status::ready);
    REQUIRE(closing.get().has_value());
    CHECK(gate->returned.load());
    CHECK(gate->destroyed.load());
    CHECK_FALSE(gate->timed_out.load());
    CHECK_FALSE(alive->load());
    auto result = (*opened)->ReadOperation(receipt->operation_id);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Cancelled);
    CHECK(result->result_persisted);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: same-cwd factories isolate state and compatible resume re-creates the instance") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto factory_calls = std::make_shared<std::atomic<unsigned>>(0);
    auto destroyed = std::make_shared<std::atomic<unsigned>>(0);
    const auto make_options = [&](std::string resume = {}, bool changed = false, bool absent = false) {
        auto options = Options(fixture,
            [](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
                std::string input;
                for (const auto& message : request.messages) if (message.role == "user") input = message.text;
                return Answer(input);
            });
        options.resume_session_id = std::move(resume);
        if (absent) return options;
        auto definition = Definition("private.counter");
        if (changed) definition.definition_hash = "fixture-native-v2";
        auto registration = Registration("counter", {definition}, {});
        registration.factory = [=](const ext::SessionContext& context) -> sdk::Result<std::unique_ptr<ext::Instance>> {
            ++*factory_calls;
            auto count = std::make_shared<unsigned>(0);
            return std::make_unique<Instance>([count, id = context.session_id](const ext::Context& invocation,
                const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
                CHECK(invocation.session_id == id);
                return Continue(next, Json{{"prompt", id + "/" + std::to_string(++*count)}}.dump());
            }, [destroyed] { ++*destroyed; });
        };
        options.extensions.push_back(std::move(registration));
        return options;
    };
    auto first = (*runtime)->OpenSession(make_options());
    auto second = (*runtime)->OpenSession(make_options());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK((*first)->id() != (*second)->id());
    auto a = std::async(std::launch::async, [session = *first] { return Run(session, "a1", "a input"); });
    auto b = std::async(std::launch::async, [session = *second] { return Run(session, "b1", "b input"); });
    CHECK(a.get().final_text == (*first)->id() + "/1");
    CHECK(b.get().final_text == (*second)->id() + "/1");
    CHECK(Run(*first, "a2", "a second").final_text == (*first)->id() + "/2");
    const auto id = (*first)->id();
    REQUIRE((*first)->Close().has_value());
    CHECK(destroyed->load() == 1);
    CHECK(Run(*second, "b2", "b second").final_text == (*second)->id() + "/2");
    auto mismatched = (*runtime)->OpenSession(make_options(id, true));
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().code == "sdk.extension.resume_mismatch");
    auto missing = (*runtime)->OpenSession(make_options(id, false, true));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == "sdk.extension.resume_mismatch");
    auto resumed = (*runtime)->OpenSession(make_options(id));
    REQUIRE(resumed.has_value());
    CHECK((*resumed)->id() == id);
    CHECK(Run(*resumed, "a3", "a resumed").final_text == id + "/1");
    CHECK(factory_calls->load() == 3);
    REQUIRE((*resumed)->Close().has_value());
    REQUIRE((*second)->Close().has_value());
    CHECK(destroyed->load() == 3);
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: observers have no continuation or write effects and all threads settle") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto observed = std::make_shared<std::atomic<unsigned>>(0);
    auto invalid_next = std::make_shared<std::atomic<unsigned>>(0);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto first = Definition("audit.first");
    first.observer = true;
    auto second = Definition("audit.second");
    second.observer = true;
    auto options = Options(fixture, [=](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        CHECK_FALSE(Has(request, "observer-injected-material"));
        return Answer();
    });
    options.extensions.push_back(Registration("observers", {first, second},
        [=](const ext::Context&, const ext::Input&, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
            ++*observed;
            auto forbidden = next.Call();
            if (!forbidden && forbidden.error().code == "hook.next.not_allowed") ++*invalid_next;
            ext::HandlerReturn result;
            result.effects.push_back({ext::EffectType::ContextAppend, R"({"text":"observer-injected-material"})"});
            return result;
        }));
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    CHECK(Run(*opened, "observer-key", "observer input").state == sdk::OperationState::Succeeded);
    CHECK(observed->load() == 2);
    CHECK(invalid_next->load() == 2);
    CHECK(calls->load() == 1);
    REQUIRE((*opened)->Close().has_value());
    auto ledger = v3::ReadV3Ledger(fixture.SessionFile((*opened)->id()));
    REQUIRE(ledger.has_value());
    unsigned failures = 0;
    for (const auto& event : ledger->events)
        if (event.kind == v3::EventKindV3::HookFailed && event.payload.value("error_code", "") == "hook.result.invalid") ++failures;
    CHECK(failures == 2);
    CHECK_FALSE(FormalUserContains(*ledger, "observer-injected-material"));
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK extensions: inline factory captures retire inside the callback guard before their backend") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    struct Audit {
        sdk::Runtime* runtime = nullptr;
        sdk::Backend* backend = nullptr;
        std::shared_ptr<std::atomic<bool>> alive;
        std::atomic<bool> armed{false};
        std::atomic<unsigned> finals{0}, probes{0}, rejected_lifecycle{0}, errors{0};
    };
    struct Capture {
        std::shared_ptr<Audit> audit;
        ~Capture() {
            if (!audit->armed.load()) return;
            ++audit->finals;
            if (!audit->alive->load()) { ++audit->errors; return; }
            sdk::ModelRequest request;
            request.model = "destruction-probe";
            auto result = audit->backend->Generate(request, {});
            if (!result || result->text != "backend-alive") ++audit->errors;
            auto shutdown = audit->runtime->Shutdown();
            if (!shutdown && shutdown.error().code == "sdk.lifecycle.reentrant") ++audit->rejected_lifecycle;
            else ++audit->errors;
        }
    };
    struct Factory {
        std::shared_ptr<Capture> capture;
        explicit Factory(std::shared_ptr<Capture> value) : capture(std::move(value)) {}
        Factory(const Factory&) noexcept = default;
        sdk::Result<std::unique_ptr<ext::Instance>> operator()(const ext::SessionContext&) const {
            return std::make_unique<Instance>(
                [owned = capture](const ext::Context&, const ext::Input&, ext::Next next) { return Continue(next); });
        }
    };
    static_assert(sizeof(Factory) == sizeof(std::shared_ptr<Capture>));
    for (const bool failure : {false, true}) {
        CAPTURE(failure);
        auto audit = std::make_shared<Audit>();
        audit->runtime = runtime->get();
        audit->alive = std::make_shared<std::atomic<bool>>(false);
        auto options = Options(fixture,
            [audit](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
                if (request.model == "destruction-probe") { ++audit->probes; return Answer("backend-alive"); }
                return Answer();
            }, audit->alive);
        audit->backend = options.backend.get();
        auto capture = std::make_shared<Capture>();
        capture->audit = audit;
        std::weak_ptr<Capture> weak = capture;
        auto registration = Registration("inline-factory", {Definition("inline.pass")}, {});
        registration.factory = Factory(capture);
        options.extensions.push_back(std::move(registration));
        registration.factory = nullptr;
        capture.reset();
        if (failure) {
            auto failing = Registration("rollback", {Definition("rollback.pass")}, {});
            failing.factory = [](const ext::SessionContext&) -> sdk::Result<std::unique_ptr<ext::Instance>> {
                return std::unexpected(sdk::Error{"fixture.rollback", "second factory fails"});
            };
            options.extensions.push_back(std::move(failing));
            audit->armed.store(true);
        }
        auto opened = (*runtime)->OpenSession(std::move(options));
        if (failure) {
            REQUIRE_FALSE(opened.has_value());
            CHECK(opened.error().code == "sdk.extension.factory_failed");
        } else {
            REQUIRE(opened.has_value());
            CHECK(Run(*opened, "inline-key", "inline capture input").state == sdk::OperationState::Succeeded);
            audit->armed.store(true);
            REQUIRE((*opened)->Close().has_value());
        }
        CHECK(weak.expired());
        CHECK(audit->finals.load() == 1);
        CHECK(audit->probes.load() == 1);
        CHECK(audit->rejected_lifecycle.load() == 1);
        CHECK(audit->errors.load() == 0);
        CHECK_FALSE(audit->alive->load());
    }
    REQUIRE((*runtime)->Shutdown().has_value());
}
