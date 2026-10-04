#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "runtime/async_tool_runtime.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/registry.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

using namespace lubancode;

namespace {
struct Counts { int execute = 0, pre = 0, permission = 0, confirm = 0, retire = 0, post = 0; };

class Probe final : public tools::Tool {
public:
    Probe(std::shared_ptr<Counts> counts, std::string name = "job_probe", bool confirm = true)
        : counts_(std::move(counts)), name_(std::move(name)), confirm_(confirm) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "owned preparation probe"; }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}},
                {"required", nlohmann::json::array({"path"})}, {"additionalProperties", false}};
    }
    bool needs_confirm() const override { return confirm_; }
    tools::ApprovalClass approval_class() const override { return confirm_ ? tools::ApprovalClass::FileEdit : tools::ApprovalClass::None; }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++counts_->execute;
        return {"executed:" + input.at("path").get<std::string>(), false};
    }
private:
    std::shared_ptr<Counts> counts_;
    std::string name_;
    bool confirm_;
};

api::ToolUseBlock Call(std::string id = "call-owned", std::string name = "job_probe") {
    api::ToolUseBlock call;
    call.id = std::move(id); call.name = std::move(name); call.input = {{"path", "original.txt"}};
    return call;
}

struct Trace {
    std::vector<agent::ToolTraceEvent> events;
    void Attach(agent::TurnWiring& wiring) {
        wiring.on_tool_trace = [this](const auto& event) { events.push_back(event); };
    }
    int Count(agent::ToolTraceEventKind kind) const {
        return static_cast<int>(std::count_if(events.begin(), events.end(),
            [kind](const auto& event) { return event.kind == kind; }));
    }
};

class Reply final : public runtime::ScopedApprovalFuture {
public:
    std::optional<runtime::ApprovalResponse> WaitApproval() override {
        return runtime::ApprovalResponse{runtime::InteractionDecision::Accept, {}};
    }
    std::optional<runtime::ApprovalResponse> WaitApproval(const std::atomic<bool>* cancel) override {
        return cancel && cancel->load() ? std::nullopt : WaitApproval();
    }
    std::optional<runtime::QuestionResponse> WaitQuestion() override { return std::nullopt; }
};

class Backend final : public api::Backend {
public:
    std::vector<api::Request> requests;
    bool native = false;
    std::expected<void, api::Error> send_stream(const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& emit, const std::atomic<bool>* = nullptr) override {
        requests.push_back(request);
        emit(api::MessageStart{"message", "test-model"});
        if (requests.size() == 1) {
            for (int i = 0; i != 2; ++i) {
                api::ToolUseStart start;
                start.index = i; start.id = i == 0 ? "call-owned" : "call-inline";
                start.name = i == 0 ? "job_probe" : "inline_probe"; start.async_call = native && i == 0;
                emit(start);
                emit(api::ToolUseInputDelta{i, "{\"path\":\"original.txt\"}"});
                api::ContentBlockDone done; done.index = i; done.tool_use_id = start.id; emit(done);
            }
            emit(api::MessageDone{"tool_use", api::Usage{}});
        } else if (requests.size() == 2) {
            emit(api::TextDelta{"done"}); emit(api::ContentBlockDone{0});
            emit(api::MessageDone{"end_turn", api::Usage{}});
        } else return std::unexpected(api::Error{api::ErrorKind::Api, "unexpected extra request", 0});
        return {};
    }
};

class BatchGate final : public agent::ToolBatchGate {
public:
    agent::JobAdmissionMode selected = agent::JobAdmissionMode::OwnedRequired;
    agent::ToolProtocolMode protocol = agent::ToolProtocolMode::JobHandle;
    int early = 0, take = 0;
    agent::JobAdmissionMode admission_mode() const noexcept override { return selected; }
    bool OnCallItemComplete(const api::ToolUseBlock&, const StreamCallContext&) override { ++early; return true; }
    std::vector<agent::ToolCallAdjudication> AdjudicateBatch(const std::vector<api::ToolUseBlock>& calls) override {
        std::vector<agent::ToolCallAdjudication> result(calls.size());
        if (!result.empty()) result[0].mode = protocol;
        return result;
    }
    std::optional<tools::Tool::Result> TakeJobOrder(const api::ToolUseBlock&, const agent::ToolCallAdjudication&) override {
        ++take; return tools::Tool::Result{"legacy-admission", false};
    }
    void PumpBatchBoundary() override {}
};

std::vector<api::ToolResultBlock> Results(const api::Request& request) {
    std::vector<api::ToolResultBlock> result;
    for (const auto& message : request.messages) for (const auto& block : message.content)
        if (const auto* value = std::get_if<api::ToolResultBlock>(&block)) result.push_back(*value);
    return result;
}
agent::AgentProfile Profile() {
    agent::AgentProfile profile; profile.request.model = "test-model"; profile.system_prompt = "system";
    return profile;
}
void Mark(const char* path) { std::fprintf(stderr, "[owned-job-path] %s\n", path); std::fflush(stderr); }

struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> next{0};
        for (unsigned attempt = 0; attempt != 64; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() / ("lubancode-owned-job-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) { path = std::move(candidate); break; }
            const bool retryable = !error || error == std::errc::file_exists;
            REQUIRE_MESSAGE(retryable, error.message());
        }
        REQUIRE_FALSE(path.empty());
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::string Bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    std::string bytes((std::istreambuf_iterator<char>(input)), {}); REQUIRE_FALSE(input.bad()); return bytes;
}
} // namespace

TEST_CASE("Owned Job preparation snapshots the shared rewrite and permission input without execution") {
    auto counts = std::make_shared<Counts>();
    std::optional<agent::OwnedPreparedToolInput> snapshot;
    {
        tools::ToolRegistry registry;
        tools::ToolRegistration registration;
        registration.tool = std::make_unique<Probe>(counts);
        registration.source_kind = ToolSourceKind::PluginNative;
        registration.source_instance = "owned-plugin"; registration.effect_class = EffectClass::LocalReversible;
        registry.Register(std::move(registration));
        auto call = Call(); agent::TurnWiring wiring; Trace trace; trace.Attach(wiring);
        wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) {
            ++counts->pre; runtime::ToolHookDecision pre; pre.decision = runtime::ToolHookDecision::Decision::Allow;
            pre.updated_input = nlohmann::json{{"path", "rewritten.txt"}}; return pre;
        };
        wiring.on_permission_evaluate = [&](const auto& id, const auto& name, auto kind, const auto& input, const auto&) {
            ++counts->permission; CHECK(id == call.id); CHECK(name == call.name);
            CHECK(kind == tools::ApprovalClass::FileEdit); CHECK(input.at("path").template get<std::string>() == "rewritten.txt");
            runtime::PermissionVerdict verdict; verdict.action = runtime::PermissionVerdict::Action::Allow; return verdict;
        };
        agent::ToolTraceContext context; context.execution_id = "preparation-only";
        auto prepared = agent::PrepareOwnedToolInput(registry, call, wiring, {}, {}, &context);
        REQUIRE(prepared.has_value()); snapshot = std::move(*prepared);
        call.input["path"] = "changed-after-return";
        CHECK(trace.events.empty()); CHECK(counts->execute == 0); CHECK(counts->pre == 1); CHECK(counts->permission == 1);
    }
    REQUIRE(snapshot.has_value()); CHECK(snapshot->call_id == "call-owned"); CHECK(snapshot->tool_name == "job_probe");
    CHECK(snapshot->effective_input.at("path").get<std::string>() == "rewritten.txt");
    CHECK(snapshot->source_kind == ToolSourceKind::PluginNative); CHECK(snapshot->source_instance == "owned-plugin");
    CHECK(snapshot->effect_class == EffectClass::LocalReversible); CHECK(counts->execute == 0);
    Mark("owned-snapshot");
}

TEST_CASE("Owned Job preparation retains the real exposure mode scope hook schema and permission denials") {
    for (int variant = 0; variant != 7; ++variant) {
        auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry;
        registry.Register(std::make_unique<Probe>(counts)); auto call = Call();
        agent::TurnWiring wiring; Trace trace; trace.Attach(wiring);
        wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) {
            ++counts->pre; runtime::ToolHookDecision pre;
            if (variant == 2) { pre.decision = runtime::ToolHookDecision::Decision::Deny; pre.reason = "denied-pre"; }
            if (variant == 3) { pre.decision = runtime::ToolHookDecision::Decision::Allow; pre.updated_input = nlohmann::json{{"path", 9}}; }
            return pre;
        };
        wiring.on_permission_evaluate = [&](const auto&, const auto&, auto, const auto&, const auto&) {
            ++counts->permission; runtime::PermissionVerdict verdict;
            verdict.action = runtime::PermissionVerdict::Action::Deny; return verdict;
        };
        if (variant == 0) wiring.on_mode_policy = [](const auto&, const auto&) { return std::string("mode.denied|denied-mode"); };
        if (variant == 1) wiring.on_scope_gate = [](const auto&, const auto&) { return std::optional<std::string>("denied-scope"); };
        const std::function<bool(const tools::Tool&)> filter = variant == 5
            ? std::function<bool(const tools::Tool&)>([](const auto&) { return false; }) : std::function<bool(const tools::Tool&)>{};
        const std::function<bool(const tools::Tool&)> turn = variant == 6
            ? std::function<bool(const tools::Tool&)>([](const auto&) { return false; }) : std::function<bool(const tools::Tool&)>{};
        agent::ToolTraceContext context; context.execution_id = "rejected-preparation";
        auto result = agent::PrepareOwnedToolInput(registry, call, wiring, filter, {}, &context, nullptr, nullptr, turn);
        REQUIRE_FALSE(result.has_value()); CHECK(result.error().is_error); CHECK_FALSE(result.error().error_code.empty());
        const char* expected[] = {"mode_denied", "scope_gate_pending", "hook_denied", "schema_rejected", "permission_declined", "unavailable", "turn_gate_denied"};
        CHECK(result.error().outcome == expected[variant]); CHECK(counts->execute == 0);
        CHECK(trace.Count(agent::ToolTraceEventKind::ExecutionStarted) == 0);
        CHECK(trace.Count(agent::ToolTraceEventKind::ExecutionFinished) == 1);
        CHECK(counts->pre == ((variant >= 2 && variant <= 4) ? 1 : 0)); CHECK(counts->permission == (variant == 4 ? 1 : 0));
    }
    Mark("shared-denials");
}

TEST_CASE("Owned Job preparation keeps forced ask scoped cancellation and per-ticket retirement") {
    for (int variant = 0; variant != 3; ++variant) {
        auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry; registry.Register(std::make_unique<Probe>(counts));
        std::atomic<bool> cancel{variant == 2}; agent::TurnWiring wiring; auto call = Call();
        const std::set<std::string> allowed{"job_probe"}; runtime::PermissionContext permissions; permissions.always_allowed = &allowed;
        wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) {
            ++counts->pre; runtime::ToolHookDecision pre; pre.decision = runtime::ToolHookDecision::Decision::Ask; return pre;
        };
        wiring.on_permission_evaluate = [&](const auto&, const auto& name, auto kind, const auto& input, const auto& pre) {
            ++counts->permission; const auto verdict = runtime::EvaluatePermission(permissions, pre, kind, name, input);
            CHECK(verdict.action == runtime::PermissionVerdict::Action::Ask); return verdict;
        };
        wiring.on_tool_confirm_scoped = [&](const runtime::ApprovalRequest& request) {
            ++counts->confirm; CHECK(request.tool_use_id == call.id); CHECK(request.tool_name == call.name); CHECK(request.input == call.input);
            if (variant == 1) cancel.store(true);
            return runtime::ApprovalLease::Create(std::make_shared<Reply>(), [counts] { ++counts->retire; });
        };
        auto result = agent::PrepareOwnedToolInput(registry, call, wiring, {}, {}, nullptr, &cancel);
        CHECK(result.has_value() == (variant == 0)); CHECK(counts->execute == 0);
        CHECK(counts->pre == (variant == 2 ? 0 : 1)); CHECK(counts->confirm == (variant == 2 ? 0 : 1));
        CHECK(counts->retire == (variant == 2 ? 0 : 1));
        if (variant != 0) { REQUIRE_FALSE(result.has_value()); CHECK(result.error().outcome == "cancelled_before_start"); }
    }
    Mark("ask-cancel-retire");
}

TEST_CASE("Owned Job preparation rejects incomplete Action Post capability before consumption and preserves unknown") {
    for (int variant = 0; variant != 4; ++variant) {
        auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry; registry.Register(std::make_unique<Probe>(counts));
        agent::TurnWiring wiring; wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) { ++counts->pre; return runtime::ToolHookDecision{}; };
        if (variant == 0) wiring.on_pre_action = [&](const auto&, const auto&, const auto&) { ++counts->post; return agent::TurnWiring::ActionPreDecision{}; };
        if (variant == 1) wiring.on_post_action = [&](const auto&, const auto&, const auto&, const auto& result, const auto&, const auto&, const auto&) { ++counts->post; return result; };
        if (variant == 2) wiring.on_post_tool_use_hook = [&](const auto&, const auto&, const auto&, const auto&) { ++counts->post; return std::vector<std::string>{}; };
        if (variant == 3) wiring.on_post_tool_hook = [&](const auto&, const auto&, const auto&, const auto&) { ++counts->post; };
        const auto result = agent::PrepareOwnedToolInput(registry, Call(), wiring, {});
        REQUIRE_FALSE(result.has_value()); CHECK(result.error().error_code == "job.admission.owned_scope_unavailable");
        CHECK(counts->pre == 0); CHECK(counts->post == 0); CHECK(counts->execute == 0);
    }
    auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry; registry.Register(std::make_unique<Probe>(counts));
    agent::TurnWiring wiring;
    wiring.on_pre_tool_use_hook = [](const auto&, const auto&, const auto&) { runtime::ToolHookDecision pre; pre.decision = runtime::ToolHookDecision::Decision::Deny; return pre; };
    wiring.action_receipt_failure_reason = [] { return std::string("actual-receipt-unconfirmed"); };
    const auto unknown = agent::PrepareOwnedToolInput(registry, Call(), wiring, {});
    REQUIRE_FALSE(unknown.has_value()); CHECK(unknown.error().execution_control == tools::ExecutionControl::StopIndeterminate);
    CHECK(unknown.error().outcome == "hook_denied"); CHECK(unknown.error().content.find("actual-receipt-unconfirmed") != std::string::npos);
    Mark("deferred-capability");
}

TEST_CASE("Owned Job loop pairs missing capability before early registration while legacy and inline remain real") {
    for (int variant = 0; variant != 5; ++variant) {
        auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry;
        registry.Register(std::make_unique<Probe>(counts, "job_probe", false));
        registry.Register(std::make_unique<Probe>(counts, "inline_probe", false));
        Backend backend; BatchGate gate; Trace trace; agent::TurnWiring wiring; trace.Attach(wiring);
        if (variant == 1) { gate.protocol = agent::ToolProtocolMode::NativeDeferred; backend.native = true; }
        if (variant == 2) gate.selected = static_cast<agent::JobAdmissionMode>(99);
        if (variant == 3) gate.protocol = static_cast<agent::ToolProtocolMode>(99);
        if (variant == 4) gate.selected = agent::JobAdmissionMode::Legacy;
        wiring.tool_batch_gate = &gate; wiring.turn_id = "turn-owned";
        wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) { ++counts->pre; return runtime::ToolHookDecision{}; };
        agent::Agent agent(backend, registry, Profile());
        const auto outcome = agent.Run("run the declared pair", wiring); const auto error = outcome.has_value() ? std::string{} : outcome.error();
        REQUIRE_MESSAGE(outcome.has_value(), error); CHECK_FALSE(outcome->side_effect_indeterminate);
        REQUIRE(backend.requests.size() == 2); const auto results = Results(backend.requests[1]); REQUIRE(results.size() == 2);
        CHECK(results[0].tool_use_id == "call-owned"); CHECK(results[1].tool_use_id == "call-inline");
        CHECK_FALSE(results[1].is_error); CHECK(results[1].content == "executed:original.txt");
        CHECK(counts->execute == 1); CHECK(counts->pre == 1);
        CHECK(trace.Count(agent::ToolTraceEventKind::ExecutionStarted) == 1);
        if (variant != 4) {
            CHECK(results[0].is_error); CHECK_FALSE(results[0].job_admission);
            CHECK(results[0].content.find("Owned Job scope") != std::string::npos);
            CHECK(gate.early == 0); CHECK(gate.take == 0);
            CHECK(trace.Count(agent::ToolTraceEventKind::Scheduled) == 2);
            CHECK(trace.Count(agent::ToolTraceEventKind::ExecutionFinished) == 2);
        } else {
            CHECK_FALSE(results[0].is_error); CHECK(results[0].job_admission); CHECK(gate.take == 1); CHECK(gate.early == 2);
            CHECK(trace.Count(agent::ToolTraceEventKind::Scheduled) == 1);
        }
    }
    // Exercise the actual runtime's public-internal legacy entry points too.
    Directory directory; const auto journal = directory.path / "main.jsonl";
    auto writer = trajectory::v3::V3Writer::Start(journal, "20261003-120000-OWNED", "run-000001", "system");
    REQUIRE(writer.has_value()); std::atomic<unsigned> auth{0}, execute{0}, origin{0};
    runtime::AsyncToolRuntime::Hooks hooks; hooks.writer = &*writer;
    hooks.writer_mutex = std::make_shared<std::recursive_mutex>();
    hooks.auth = [&](const auto&, const auto&) { ++auth; return tools::JobAuthDecision{true, false, {}}; };
    hooks.executor = [&](const auto&) { ++execute; return tools::Tool::Result{"unexpected worker", false}; };
    hooks.call_origin_resolver = [&](const auto&) -> std::optional<tools::JobStartRequest> { ++origin; return std::nullopt; };
    runtime::AsyncToolRuntimeOptions options; options.admission_mode = agent::JobAdmissionMode::OwnedRequired;
    options.tools["job_probe"].execution.allow_background = true;
    options.tools["job_probe"].dispatch_point = agent::ToolDispatchPoint::OnCallItemComplete;
    auto async_runtime = runtime::AsyncToolRuntime::Create(std::move(hooks), std::move(options)); REQUIRE(async_runtime != nullptr);
    const auto before = Bytes(journal);
    CHECK_FALSE(async_runtime->gate()->OnCallItemComplete(Call(), {}));
    agent::ToolCallAdjudication adjudication; adjudication.mode = agent::ToolProtocolMode::JobHandle;
    const auto rejected = async_runtime->gate()->TakeJobOrder(Call(), adjudication); REQUIRE(rejected.has_value());
    CHECK(rejected->error_code == "job.admission.owned_scope_unavailable"); CHECK(rejected->is_error);
    CHECK(auth == 0); CHECK(execute == 0); CHECK(origin == 0); CHECK(Bytes(journal) == before);
    CHECK(async_runtime->coordinator()->running_count() == 0); CHECK(async_runtime->coordinator()->queued_count() == 0);
    REQUIRE(async_runtime->Shutdown()); async_runtime.reset(); REQUIRE(writer->Close().has_value());
    Mark("loop-no-fallback");
}

TEST_CASE("Owned Job rejection with a real unconfirmed writer receipt stops same batch inline and next provider") {
    // Exercise every early refusal family through a genuine writer receipt,
    // not a callback string standing in for persistence failure.
    for (int variant = 0; variant != 3; ++variant) {
        Directory directory; bool armed = false; unsigned injected = 0;
        trajectory::v3::V3WriterOptions options;
        options.inject_io_failure = [&]() -> std::optional<std::string> {
            if (!armed) return std::nullopt;
            ++injected; return "owned-preparation-test.io_failed";
        };
        auto writer = trajectory::v3::V3Writer::Start(directory.path / "main.jsonl", "20261003-120000-PREP", "run-000001", "system", nlohmann::json::object(), options);
        REQUIRE(writer.has_value());
        auto action = trajectory::v3::ToolActionSession::Admit(*writer, "turn-owned", "step-owned", "action-owned", "queued", std::nullopt, "call-owned");
        REQUIRE(action.last_event_id().has_value());
        auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry;
        registry.Register(std::make_unique<Probe>(counts, "job_probe", false));
        std::atomic<bool> cancel{variant == 1}; agent::TurnWiring wiring;
        if (variant == 0) wiring.on_pre_action = [&](const auto&, const auto&, const auto&) { ++counts->post; return agent::TurnWiring::ActionPreDecision{}; };
        if (variant == 2) wiring.on_pre_tool_use_hook = [&](const auto&, const auto&, const auto&) {
            ++counts->pre; cancel.store(true); return runtime::ToolHookDecision{};
        };
        std::string receipt_error; unsigned attempted = 0; bool actual_write_unconfirmed = false;
        std::optional<trajectory::v3::WriteReceipt> actual_receipt;
        wiring.on_tool_trace = [&](const agent::ToolTraceEvent& event) {
            if (event.kind != agent::ToolTraceEventKind::ExecutionFinished || event.tool_use_id != "call-owned") return;
            ++attempted; armed = true;
            auto receipt = action.Reject(*writer, "preparation-refused");
            actual_write_unconfirmed = receipt.status != trajectory::v3::WriteReceipt::Status::Committed && writer->broken();
            receipt_error = receipt.error_code;
            actual_receipt.emplace(std::move(receipt));
        };
        wiring.action_receipt_failure_reason = [&] { return receipt_error; };
        agent::ToolTraceContext trace; trace.execution_id = "refusal-owned";
        const auto refused = agent::PrepareOwnedToolInput(registry, Call(), wiring, {}, {}, &trace, &cancel);
        REQUIRE_FALSE(refused.has_value()); CHECK(actual_write_unconfirmed); CHECK(attempted == 1); CHECK(injected == 1);
        REQUIRE(actual_receipt.has_value()); CHECK(actual_receipt->status == trajectory::v3::WriteReceipt::Status::Rejected);
        CHECK(actual_receipt->error_code == "v3writer.injected"); CHECK(writer->broken());
        CHECK(refused.error().execution_control == tools::ExecutionControl::StopIndeterminate);
        CHECK(refused.error().content.find(receipt_error) != std::string::npos);
        CHECK(refused.error().outcome == (variant == 0 ? "unavailable" : "cancelled_before_start"));
        CHECK(refused.error().error_code == (variant == 0 ? "job.admission.owned_scope_unavailable" : "runtime.tool.cancelled_before_start"));
        CHECK(counts->execute == 0); CHECK(counts->post == 0); CHECK(counts->pre == (variant == 2 ? 1 : 0));
        CHECK_FALSE(action.started()); CHECK(action.terminal() == trajectory::v3::ToolActionSession::Terminal::None);
    }
    Directory directory; bool armed = false; unsigned injected = 0;
    trajectory::v3::V3WriterOptions options;
    options.inject_io_failure = [&]() -> std::optional<std::string> {
        if (!armed) return std::nullopt; ++injected; return "owned-job-test.io_failed";
    };
    auto writer = trajectory::v3::V3Writer::Start(directory.path / "main.jsonl", "20261003-120000-FAULT", "run-000001", "system", nlohmann::json::object(), options);
    REQUIRE(writer.has_value());
    auto action = trajectory::v3::ToolActionSession::Admit(*writer, "turn-owned", "step-owned", "action-owned", "queued", std::nullopt, "call-owned");
    REQUIRE(action.last_event_id().has_value());
    auto counts = std::make_shared<Counts>(); tools::ToolRegistry registry;
    registry.Register(std::make_unique<Probe>(counts, "job_probe", false));
    registry.Register(std::make_unique<Probe>(counts, "inline_probe", false));
    Backend backend; BatchGate gate; agent::TurnWiring wiring; wiring.tool_batch_gate = &gate; wiring.turn_id = "turn-owned";
    std::string receipt_error; unsigned attempted = 0; bool actual_write_unconfirmed = false;
    std::optional<trajectory::v3::WriteReceipt> actual_receipt;
    wiring.on_tool_trace = [&](const agent::ToolTraceEvent& event) {
        if (event.kind != agent::ToolTraceEventKind::ExecutionFinished || event.tool_use_id != "call-owned") return;
        ++attempted; armed = true;
        auto receipt = action.Reject(*writer, "missing-owned-capability");
        actual_write_unconfirmed = receipt.status != trajectory::v3::WriteReceipt::Status::Committed && writer->broken();
        receipt_error = receipt.error_code;
        actual_receipt.emplace(std::move(receipt));
    };
    wiring.action_receipt_failure_reason = [&] { return receipt_error; };
    agent::Agent agent(backend, registry, Profile());
    const auto outcome = agent.Run("reject without unsafe continuation", wiring);
    const auto error = outcome.has_value() ? std::string{} : outcome.error(); REQUIRE_MESSAGE(outcome.has_value(), error);
    CHECK(actual_write_unconfirmed); CHECK(attempted == 1); CHECK(injected == 1); CHECK_FALSE(receipt_error.empty());
    REQUIRE(actual_receipt.has_value()); CHECK(actual_receipt->status == trajectory::v3::WriteReceipt::Status::Rejected);
    CHECK(actual_receipt->error_code == "v3writer.injected"); CHECK(writer->broken());
    CHECK(outcome->side_effect_indeterminate); CHECK(outcome->side_effect_error == receipt_error);
    CHECK(backend.requests.size() == 1); CHECK(counts->execute == 0); CHECK(gate.early == 0); CHECK(gate.take == 0);
    CHECK_FALSE(action.started()); CHECK(action.terminal() == trajectory::v3::ToolActionSession::Terminal::None);
    Mark("receipt-unknown-stop");
}
