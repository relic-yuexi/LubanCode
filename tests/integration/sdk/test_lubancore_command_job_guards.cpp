#include <doctest/doctest.h>
#include <lubancore/core.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/loop.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "sdk/adapters.hpp"
#include "sdk/approval.hpp"
#include "sdk/command_jobs.hpp"
#include "sdk/job_operations.hpp"
#include "sdk/operation_ledger.hpp"
#include "tools/run_command.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace lubancore_consumer {
void InspectCommandJobs(const std::filesystem::path&, const std::filesystem::path&,
    const std::function<void(const std::filesystem::path&, const lubancore::jobs::v1::Identity&)>&);
}
namespace {
namespace sdk = lubancore;
namespace detail = sdk::detail;
namespace rt = lubancode::runtime;
namespace agent = lubancode::agent;
namespace tools = lubancode::tools;
namespace traj = lubancode::trajectory;
namespace v3 = traj::v3;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace std::chrono_literals;
void Mark(const char* value) { std::cout << "[sdk-command-job-guards-path] " << value << '\n'; }
std::string Utf8(const fs::path& path) { return lubancode::platform::PathToUtf8(path); }
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(input.bad()); return bytes;
}
std::vector<Json> Rows(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    std::vector<Json> rows; std::string line;
    while (std::getline(input, line)) { REQUIRE_FALSE(line.empty()); rows.push_back(Json::parse(line)); }
    REQUIRE_FALSE(input.bad()); return rows;
}
struct Directory {
    fs::path root, cwd, probe;
    Directory() {
        static std::atomic<unsigned> next{0};
        root = fs::temp_directory_path() / ("sdk-command-job-guards-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
        cwd = root / "project"; fs::create_directories(cwd); root = fs::canonical(root); cwd = root / "project";
        const auto original = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
        REQUIRE(fs::is_regular_file(original)); probe = root / original.filename();
        REQUIRE(fs::copy_file(original, probe)); fs::permissions(probe, fs::status(original).permissions());
    }
    ~Directory() { std::error_code error; fs::remove_all(root, error); }
};
std::string Quote(const std::string& text) {
#ifdef _WIN32
    REQUIRE(text.find_first_of("\"%\r\n") == std::string::npos); return '"' + text + '"';
#else
    std::string out = "'";
    for (const char ch : text) out += ch == '\'' ? "'\\''" : std::string(1, ch);
    return out + "'";
#endif
}
Json Command(const Directory& directory, bool wait) {
    std::string command = Quote(Utf8(directory.probe));
    for (const auto& arg : std::vector<std::string>{"guard.started", "guard.done", "64", "0", "guard", "G", wait ? "guard.release" : "-"})
        command += " " + Quote(arg);
#ifdef _WIN32
    const char* shell = "cmd";
#else
    const char* shell = "sh";
#endif
    return {{"command", command}, {"shell", shell}, {"cwd", Utf8(directory.cwd)}, {"execution_mode", "session_job"}};
}
class NativeProbe final : public traj::JournalNativeIoProbe {
public:
    std::atomic<unsigned> remaining{0}, hits{0};
    traj::JournalNativeIoResult observed;
    bool After(const traj::JournalNativeIoResult& actual) noexcept override {
        if (actual.stage != traj::JournalNativeStage::FileSync) return false;
        auto count = remaining.load(std::memory_order_acquire);
        while (count && !remaining.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel)) {}
        if (count != 1) return false;
        observed = actual; hits.fetch_add(1, std::memory_order_release);
        return true; // real native sync already happened; never replace its result
    }
};
struct Rig {
    Directory directory;
    std::shared_ptr<NativeProbe> native = std::make_shared<NativeProbe>();
    detail::SessionApprovals approvals;
    std::unique_ptr<rt::SessionService> service;
    std::shared_ptr<detail::SessionCommandJobs> jobs;
    std::shared_ptr<tools::ToolJobCoordinator> coordinator;
    std::unique_ptr<rt::TrajectoryTurnBridge> bridge;
    tools::ToolRegistry registry;
    agent::TurnWiring wiring;
    agent::ToolTraceContext trace;
    detail::MainOperationTurnStart parent;
    detail::JobOperations table;
    lubancode::api::ToolUseBlock call;
    tools::PreparedJobOwner owner;
    std::shared_ptr<std::atomic<bool>> scope_allowed = std::make_shared<std::atomic<bool>>(true);
    explicit Rig(bool wait = false) {
        const auto identity = lubancode::workspace::MakeFallbackIdentity(directory.cwd);
        auto plan = detail::SessionCommandJobPlan::Prepare(sdk::jobs::v1::CommandOptions{30000, 20000, 8192, 1, 8},
            directory.root / "state", identity.workspace_key, {}, Utf8(directory.cwd));
        REQUIRE_MESSAGE(plan.has_value(), (plan ? std::string() : plan.error().code));
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = Utf8(directory.cwd); launch.workspace_identity = identity;
        launch.workspaces_root = directory.root / "state" / "workspaces";
        launch.v3_system_content = "controlled native Job guard"; launch.wire_name = "chat";
        launch.approval_mode = lubancode::ApprovalMode::Yolo;
        launch.v3_opening_participant = (*plan)->OpeningParticipant();
        launch.journal_native_io_probe = native;
        service = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error());
        auto module = detail::SessionCommandJobs::Build(*plan, *service, approvals, sdk::ApprovalMode::Yolo, 5s, {});
        REQUIRE_MESSAGE(module.has_value(), (module ? std::string() : module.error().code));
        jobs = *module; coordinator = service->runtime()->async_tool_runtime()->coordinator();
        auto actual_owner = coordinator->PreparedOwner(); REQUIRE(actual_owner.has_value()); owner = *actual_owner;
        const auto submitted = service->SubmitInput({"guard-parent", "actual parent input", {}});
        REQUIRE(submitted.accepted);
        parent = detail::BeginMainOperationTurn(*service); REQUIRE(parent.ready()); REQUIRE(parent.input.has_value());
        approvals.SetOperationOwner(owner.session_id, parent.input->operation_id, owner.run_id);
        jobs->SetParent(parent.input->operation_id, parent.turn_id);
        tools::ToolRegistration registration;
        registration.source_kind = tools::ToolSourceKind::Builtin;
        registration.source_instance = "sdk.builtin.run_command"; registration.version_or_digest = "native-command-job-guard-v1";
        registration.effect_class = lubancode::EffectClass::LocalProcessUnknown;
        registration.tool = detail::BindLocalTool(std::make_unique<tools::RunCommandTool>(), Utf8(directory.cwd), true);
        registry.Register(std::move(registration));
        bridge = service->trajectory()->NewTurnBridge({"fixture", "chat", "sdk", {}});
        REQUIRE(bridge != nullptr); bridge->BeginTurn(parent.turn_id, "external_user");
        lubancode::api::Message user; user.role = lubancode::api::Role::User;
        user.content.push_back(lubancode::api::TextBlock{"actual parent input"}); bridge->RecordInput(user);
        lubancode::api::Request request; request.model = "fixture"; request.system = "controlled native Job guard";
        request.messages.push_back(user);
        const auto request_id = bridge->OnRequestPrepared(request, {}); REQUIRE_FALSE(request_id.empty());
        REQUIRE(bridge->OnRequestSent(request_id));
        call = {"guard-call", "run_command", Command(directory, wait)};
        lubancode::api::Message assistant; assistant.role = lubancode::api::Role::Assistant;
        assistant.content.push_back(call);
        REQUIRE(bridge->OnOutputCompleted(request_id, assistant, "tool_use", "fixture"));
        agent::ToolTraceEvent scheduled; scheduled.kind = agent::ToolTraceEventKind::Scheduled;
        scheduled.tool_use_id = call.id; scheduled.tool_name = call.name; scheduled.turn_id = parent.turn_id;
        scheduled.execution_id = "guard-execution"; scheduled.batch_id = "guard-batch";
        bridge->OnToolTrace(scheduled);
        trace.execution_id = scheduled.execution_id; trace.batch_id = scheduled.batch_id; trace.turn_id = parent.turn_id;
        wiring.turn_id = parent.turn_id;
        wiring.on_tool_trace = [this](const auto& event) { bridge->OnToolTrace(event); };
        service->runtime()->async_tool_runtime()->InstallTurnBridge(bridge.get());
        REQUIRE(bridge->recent_errors().empty());
    }
    ~Rig() {
        native->remaining.store(0);
        if (jobs) { (void)jobs->RetireBindings(); (void)jobs->Finalize(); }
        (void)table.Close();
        bridge.reset();
        if (service) (void)service->Close("fixture_close");
    }
    v3::V3Writer& Writer() { return *service->trajectory()->v3_main_writer(); }
    fs::path Path() { return Writer().path(); }
    agent::OwnedJobAdmissionReceipt Admit() {
        const std::function<bool(const tools::Tool&)> filter; const std::string denial;
        const agent::OwnedToolAdmissionContext context{registry, wiring, &trace, nullptr, filter, denial, filter, denial};
        return service->runtime()->async_tool_runtime()->gate()->TakeOwnedJobOrder(call, context);
    }
    void Started() {
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < until) {
            std::ifstream input(directory.cwd / "guard.started", std::ios::binary);
            if (input.is_open()) {
                std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                const auto end = bytes.find('\n'); std::error_code error;
                if (end != std::string::npos && bytes.substr(0, end) == "guard" && end + 1 < bytes.size() &&
                    fs::equivalent(fs::u8path(bytes.substr(end + 1)), directory.cwd, error) && !error) return;
            }
            std::this_thread::sleep_for(5ms);
        }
        FAIL("actual command probe never entered");
    }
    tools::PreparedJobRegistration Register() {
        wiring.on_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        auto prepared = agent::PrepareOwnedToolInput(registry, call, wiring, {}, {}, &trace);
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().error_code));
        const auto source = bridge->V3DeclaredCallOrigin(call.id); REQUIRE(source.has_value());
        tools::PreparedJobRequest request;
        request.owner = owner; request.provider_tool_call_id = call.id; request.assistant_message_ref = source->message_id;
        request.parent_action_id = source->action_id; request.turn_id = source->turn_id; request.step_id = source->step_id;
        request.tool_name = call.name; request.original_input = call.input; request.effective_input = prepared->effective_input;
        const auto* registered_tool = registry.RegistrationOf(call.name); REQUIRE(registered_tool != nullptr);
        request.tool_identity = {call.name, registered_tool->source_instance, registered_tool->version_or_digest, Utf8(directory.cwd)};
        request.policy.allow_background = true; request.policy.deadline_ms = 30000; request.policy.max_output_bytes = 8192;
        request.policy.side_effect_class = "external";
        const auto registered = coordinator->RegisterPreparedJob(request);
        REQUIRE(registered.state == tools::PreparedJobRegistrationState::Registered); REQUIRE(registered.facts);
        return registered;
    }
    std::pair<tools::OwnedJobAdoption, v3::JobOperationBindingFacts> Manual(bool bind = true) {
        const auto registered = Register();
        tools::OwnedJobCapability capability; capability.command = std::make_shared<tools::RunCommandTool>();
        capability.command_limits = {20000, 8192};
        capability.scope_gate = [facts = registered.facts, allowed = scope_allowed](const auto& scope, const Json& input, const auto& identity, const auto& policy) {
            const bool matches = allowed->load() && scope.owner == facts->owner && scope.job_id == facts->job_id && scope.action_id == facts->action_id &&
                scope.attempt == facts->attempt && input == facts->effective_input && identity.ToJson() == facts->tool_identity.ToJson() &&
                policy.ToJson() == facts->policy.ToJson();
            return tools::JobAuthDecision{matches, false, {}};
        };
        capability.post = [](const auto&) { v3::WriteReceipt result; result.error_code = "fixture.no_dispatch_expected"; return result; };
        const auto adopted = coordinator->AdoptPreparedJob(owner, registered.facts->job_id, std::move(capability));
        REQUIRE(adopted.state == tools::OwnedJobAdoptionState::Adopted);
        if (!bind) return {adopted, {}};
        const auto bound = table.Bind(*service, *coordinator, registered.facts->job_id);
        REQUIRE(bound.bound()); REQUIRE(bound.facts != nullptr); return {adopted, *bound.facts};
    }
};
sdk::jobs::v1::ApprovalScope Scope() {
    return {"session", "run", "parent-op", "turn", "parent-action", "provider-call", "assistant-message",
        Utf8(fs::temp_directory_path()), lubancode::platform::Sha256Hex("{}")};
}
sdk::jobs::v1::Identity JobIdentity() { return {"session", "run", "job", "job-operation", "parent-op", "turn", "business-action", 1}; }
sdk::Approval Ticket(const sdk::jobs::v1::ApprovalScope& scope, const std::string& id) {
    sdk::Approval value; value.request_id = id; value.operation_id = scope.parent_operation_id;
    value.tool_call_id = scope.provider_call_id; value.tool_name = "run_command"; value.input_json = "{}";
    value.cwd = scope.cwd; value.job = scope; return value;
}
struct OpeningCalls {
    std::atomic<unsigned> models{0}, tools{0}, live_backends{0};
};
class OpeningBackend final : public sdk::Backend {
public:
    explicit OpeningBackend(std::shared_ptr<OpeningCalls> calls) : calls_(std::move(calls)) { ++calls_->live_backends; }
    ~OpeningBackend() override { --calls_->live_backends; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++calls_->models;
        return sdk::ModelReply{};
    }
private:
    std::shared_ptr<OpeningCalls> calls_;
};
std::map<std::string, std::string> OpeningTree(const fs::path& root) {
    std::map<std::string, std::string> snapshot;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto name = Utf8(entry.path().lexically_relative(root));
        if (entry.is_directory()) snapshot.emplace("directory:" + name, std::string{});
        else {
            REQUIRE(entry.is_regular_file());
            snapshot.emplace("file:" + name, Bytes(entry.path()));
        }
    }
    return snapshot;
}
void OpeningWrite(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close(); REQUIRE_FALSE(output.fail());
}
void CheckMissingResumeBoundary() {
    Directory directory;
    const auto data = directory.root / "opening-data", resources = directory.root / "opening-resources";
    REQUIRE(fs::create_directory(data)); REQUIRE(fs::create_directory(resources));
    auto runtime = sdk::Runtime::Create({Utf8(data), Utf8(resources)});
    REQUIRE_MESSAGE(runtime.has_value(), (runtime ? std::string{} : runtime.error().message));
    auto calls = std::make_shared<OpeningCalls>();
    const auto options = [&](const std::string& resume, bool jobs) {
        sdk::SessionOptions value;
        value.cwd = Utf8(directory.cwd); value.model = "opening-boundary";
        value.system_prompt = "No model or tool work during explicit resume admission.";
        value.backend = std::make_unique<OpeningBackend>(calls);
        value.resume_session_id = resume; value.builtin_tools = {"run_command"};
        if (jobs) value.command_jobs = sdk::jobs::v1::CommandOptions{30000, 20000, 4096, 1, 4};
        sdk::Tool tool; tool.name = "opening_probe"; tool.description = "Must not execute during admission.";
        tool.input_schema_json = R"({"type":"object","properties":{}})";
        tool.requires_approval = false;
        tool.execute = [calls](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            ++calls->tools; return sdk::ToolResult{};
        };
        value.custom_tools.push_back(std::move(tool));
        return value;
    };
    const auto rejected = [&](const std::string& id, const std::string& code) {
        for (const bool jobs : {false, true}) {
            INFO("resume_id=", id, ", command_jobs=", jobs, ", expected=", code);
            const auto before = OpeningTree(data);
            const auto opened = (*runtime)->OpenSession(options(id, jobs));
            REQUIRE_FALSE(opened.has_value());
            REQUIRE_MESSAGE(opened.error().code == code, (opened.error().code + ": " + opened.error().message));
            REQUIRE(OpeningTree(data) == before);
            REQUIRE(calls->models.load() == 0); REQUIRE(calls->tools.load() == 0);
            REQUIRE(calls->live_backends.load() == 0);
        }
    };
    REQUIRE_FALSE(fs::exists(data / "workspaces"));
    rejected("missing-before-workspace", "sdk.session.open_failed");
    REQUIRE_FALSE(fs::exists(data / "workspaces"));

    auto seed = (*runtime)->OpenSession(options({}, false)); REQUIRE(seed.has_value());
    const auto id = (*seed)->id();
    REQUIRE(calls->live_backends.load() == 1);
    REQUIRE((*seed)->Close().has_value()); seed->reset();
    REQUIRE(calls->live_backends.load() == 0);
    std::vector<fs::path> sessions;
    for (const auto& entry : fs::recursive_directory_iterator(data / "workspaces"))
        if (entry.is_regular_file() && entry.path().filename() == id + ".jsonl" &&
            entry.path().parent_path().filename() == id) sessions.push_back(entry.path().parent_path());
    REQUIRE(sessions.size() == 1);
    const auto& session = sessions.front();
    const auto missing = session.parent_path() / "missing-in-existing-workspace";
    REQUIRE_FALSE(fs::exists(missing));
    rejected("missing-in-existing-workspace", "sdk.session.open_failed");
    REQUIRE_FALSE(fs::exists(missing));

    const auto not_directory = session.parent_path() / "not-a-session-directory";
    OpeningWrite(not_directory, "existing foreign bytes\n");
    rejected("not-a-session-directory", "sdk.job.plan_invalid");
    REQUIRE(Bytes(not_directory) == "existing foreign bytes\n");
    const auto plan = session / "sdk-command-jobs-plan.json";
    const auto saved_plan = Bytes(plan);
    OpeningWrite(plan, "{bad frozen plan");
    rejected(id, "sdk.job.plan_invalid");
    REQUIRE(Bytes(plan) == "{bad frozen plan");
    OpeningWrite(plan, saved_plan);
    auto resumed = (*runtime)->OpenSession(options(id, false)); REQUIRE(resumed.has_value());
    REQUIRE((*resumed)->id() == id);
    REQUIRE((*resumed)->Close().has_value()); resumed->reset();
    REQUIRE(Bytes(plan) == saved_plan);
    REQUIRE(calls->models.load() == 0); REQUIRE(calls->tools.load() == 0);
    REQUIRE(calls->live_backends.load() == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
}
}

TEST_CASE("SDK Command Job guards: retired candidates and frozen scope cannot reopen or share binding") {
    detail::SessionApprovals approvals;
    approvals.SetOperationOwner("session", "parent-op", "run");
    auto scope = Scope(); auto identity = JobIdentity();
    approvals.CloseJobScope(scope);
    REQUIRE_FALSE(approvals.BindJobScope(scope, identity));
    REQUIRE_FALSE(approvals.JobAllowed(scope, identity));
    REQUIRE_FALSE(approvals.RegisterJobScoped(scope, Ticket(scope, "retired"), 1s, [](const auto&) {}).has_value());
    scope.parent_action_id = "another-parent"; scope.provider_call_id = "another-provider";
    auto ticket = approvals.RegisterJobScoped(scope, Ticket(scope, "actual"), 1s, [](const auto&) {});
    REQUIRE(ticket.has_value());
    REQUIRE(approvals.Resolve("actual", {rt::InteractionDecision::AcceptForSession, "Job only"}));
    REQUIRE(ticket->Future()->WaitApproval().has_value());
    auto changed_input_scope = scope;
    changed_input_scope.effective_input_sha256 = lubancode::platform::Sha256Hex("{\"command\":\"changed\"}");
    auto changed_input_ticket = Ticket(changed_input_scope, "changed-input");
    changed_input_ticket.input_json = "{\"command\":\"changed\"}";
    REQUIRE_FALSE(approvals.RegisterJobScoped(changed_input_scope, changed_input_ticket, 1s, [](const auto&) {}).has_value());
    REQUIRE(approvals.BindJobScope(scope, identity));
    REQUIRE(approvals.JobAllowed(scope, identity));
    REQUIRE(approvals.AllowedTools().empty());
    auto drift = scope; drift.effective_input_sha256 = std::string(64, 'f');
    REQUIRE_FALSE(approvals.BindJobScope(drift, identity));
    REQUIRE_FALSE(approvals.JobAllowed(drift, identity));
    drift = scope; drift.cwd += "/other";
    REQUIRE_FALSE(approvals.BindJobScope(drift, identity));
    auto sibling = scope; sibling.parent_action_id = "sibling"; sibling.provider_call_id = "sibling-provider";
    REQUIRE_FALSE(approvals.BindJobScope(sibling, identity));
    auto changed = identity; changed.job_id = "another-job";
    REQUIRE_FALSE(approvals.BindJobScope(sibling, changed)); // same operation is still bound
    changed.operation_id = "another-operation";
    REQUIRE_FALSE(approvals.BindJobScope(sibling, changed)); // same action is still bound
    approvals.CloseJobScope(scope);
    REQUIRE_FALSE(approvals.BindJobScope(scope, identity));
    REQUIRE_FALSE(approvals.JobAllowed(scope, identity));
    REQUIRE_FALSE(approvals.Resolve("actual", {rt::InteractionDecision::Accept, "late"}));
    Mark("approval-retirement");
}

TEST_CASE("SDK Command Job guards: bridge verifies complete source before first write and cached receipt") {
    Rig rig;
    const auto [adopted, binding] = rig.Manual();
    const auto original = Bytes(rig.Path());
    for (unsigned field = 0; field != 18; ++field) {
        auto wrong = binding;
        switch (field) {
        case 0: wrong.session_id += "x"; break; case 1: wrong.run_id += "x"; break;
        case 2: wrong.turn_id += "x"; break; case 3: wrong.step_id += "x"; break;
        case 4: wrong.action_id += "x"; break; case 5: wrong.job_id += "x"; break;
        case 6: ++wrong.attempt; break; case 7: ++wrong.seq; break;
        case 8: wrong.operation_id += "x"; break; case 9: wrong.parent_operation_id += "x"; break;
        case 10: wrong.parent_input_id += "x"; break; case 11: wrong.parent_payload_hash += "x"; break;
        case 12: wrong.event_id += "x"; break; case 13: wrong.line_hash += "x"; break;
        case 14: wrong.parent_operation_event_id += "x"; break; case 15: wrong.adopted_event_id += "x"; break;
        case 16: wrong.original_input_sha256 += "x"; break; case 17: wrong.effective_input_sha256 += "x"; break;
        }
        REQUIRE_FALSE(rig.bridge->CommitOwnedJobAdmission(rig.call.id, adopted, wrong).committed);
        REQUIRE(Bytes(rig.Path()) == original);
    }
    auto fake = adopted;
    auto fake_facts = std::make_shared<tools::PreparedJobFacts>(*adopted.facts);
    fake_facts->effective_input["command"] = "fabricated";
    fake.facts = fake_facts;
    REQUIRE_FALSE(rig.bridge->CommitOwnedJobAdmission(rig.call.id, fake, binding).committed);
    REQUIRE(Bytes(rig.Path()) == original);
    const auto committed = rig.bridge->CommitOwnedJobAdmission(rig.call.id, adopted, binding);
    REQUIRE_MESSAGE(committed.committed, committed.error);
    REQUIRE(committed.receipts.size() == 6);
    for (const auto& receipt : committed.receipts) {
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        REQUIRE(receipt.journal_append.has_value());
        REQUIRE(receipt.journal_append->status == traj::JournalAppendStatus::Committed);
    }
    const auto bytes = Bytes(rig.Path());
    auto wrong = binding; wrong.run_id += "forged";
    REQUIRE_FALSE(rig.bridge->CommitOwnedJobAdmission(rig.call.id, adopted, wrong).committed);
    REQUIRE_FALSE(rig.bridge->CommitOwnedJobAdmission(rig.call.id, fake, binding).committed);
    const auto duplicate = rig.bridge->CommitOwnedJobAdmission(rig.call.id, adopted, binding);
    REQUIRE(duplicate.committed);
    REQUIRE(duplicate.refs.admission_event_id == committed.refs.admission_event_id);
    REQUIRE(Bytes(rig.Path()) == bytes);
    REQUIRE_FALSE(fs::exists(rig.directory.cwd / "guard.started"));
    Mark("bridge-provenance");
}

TEST_CASE("SDK Command Job guards: actual Service native faults preserve first uncertainty and never dispatch again") {
    // These are counts of actual PowerLoss sync boundaries after the original
    // declaration/Pending. Each case also checks the on-disk row kind/action;
    // a changed order fails rather than silently testing the wrong boundary.
    for (const auto& [sync, kind] : std::vector<std::pair<unsigned, std::string>>{
        {1, "tool.job.registered"}, {3, "sdk.job.operation.bound"}, {6, "tool.result.persisted"}, {14, "hook.completed"}}) {
        Rig rig;
        rig.native->remaining.store(sync);
        const auto first = rig.Admit();
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (!rig.native->hits.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) {
            rig.jobs->PumpAndPublish(); std::this_thread::sleep_for(5ms);
        }
        REQUIRE(rig.native->hits.load(std::memory_order_acquire) == 1);
        REQUIRE(rig.native->observed.attempted); REQUIRE(rig.native->observed.succeeded);
        REQUIRE(rig.Writer().broken());
        const auto witness = rig.Writer().first_unconfirmed_journal_append();
        REQUIRE(witness.has_value()); REQUIRE(witness->status == traj::JournalAppendStatus::Unconfirmed);
        REQUIRE(witness->failure.has_value()); REQUIRE(witness->failure->injected_unconfirmed);
        REQUIRE(witness->failure->stage == traj::JournalNativeStage::FileSync);
        REQUIRE(witness->failure->succeeded);
#ifdef _WIN32
        REQUIRE(witness->failure->native_return != 0); // actual FlushFileBuffers BOOL
        const char* native_platform = "windows";
#else
        REQUIRE(witness->failure->native_return == 0); // actual fsync return
        const char* native_platform = "posix";
#endif
        REQUIRE(witness->failure->native_return == rig.native->observed.native_return);
        REQUIRE(witness->failure->error_domain == traj::JournalNativeErrorDomain::None);
        const auto rows = Rows(rig.Path()); REQUIRE_FALSE(rows.empty());
        REQUIRE(rows.back().at("kind") == kind);
        const auto values = rig.jobs->List({}); REQUIRE(values.has_value()); REQUIRE(values->size() == 1);
        const auto parent_origin = rig.bridge->V3DeclaredCallOrigin(rig.call.id); REQUIRE(parent_origin.has_value());
        REQUIRE(rows.back().at("actionId") == (sync == 6 ? parent_origin->action_id : values->front().identity.action_id));
        if (sync < 14) {
            REQUIRE(first.state == agent::OwnedJobAdmissionState::Unconfirmed);
            REQUIRE_FALSE(fs::exists(rig.directory.cwd / "guard.started"));
        } else {
            REQUIRE(first.state == agent::OwnedJobAdmissionState::Accepted);
            REQUIRE(Bytes(rig.directory.cwd / "guard.done") == "guard");
        }
        const auto before = Bytes(rig.Path());
        const auto duplicate = rig.Admit();
        REQUIRE(duplicate.state == first.state);
        REQUIRE(duplicate.result.content == first.result.content);
        rig.jobs->PumpAndPublish();
        REQUIRE(Bytes(rig.Path()) == before);
        const auto retained = rig.Writer().first_unconfirmed_journal_append();
        REQUIRE(retained.has_value()); REQUIRE(retained->line_count == witness->line_count);
        REQUIRE(retained->failure->injected_unconfirmed);
        REQUIRE(rig.jobs->indeterminate());
        REQUIRE(rig.coordinator->prepared_count() == 1);
        REQUIRE_FALSE(rig.jobs->Finalize().has_value());
        REQUIRE(rig.coordinator->shutdown_complete()); REQUIRE(rig.coordinator->running_count() == 0);
        std::cout << "[sdk-command-job-native-fault] " << Json{
            {"sync", sync}, {"row_kind", kind}, {"row_seq", rows.back().at("seq")},
            {"journal_status", "Unconfirmed"}, {"native_stage", "FileSync"}, {"platform", native_platform},
            {"native_succeeded", witness->failure->succeeded}, {"native_return", witness->failure->native_return},
            {"injected_unconfirmed", witness->failure->injected_unconfirmed},
            {"first_line_count", witness->line_count}, {"retained_line_count", retained->line_count},
            {"command_completed", fs::is_regular_file(rig.directory.cwd / "guard.done")},
            {"running_after_close", rig.coordinator->running_count()}}.dump() << '\n';
    }
    Mark("native-faults");
}

TEST_CASE("SDK Command Job guards: only a real cancel latch can confirm delivered refs without execution permission") {
    Rig rig;
    const auto [adopted, binding] = rig.Manual();
    const auto parent = rig.bridge->CommitOwnedJobAdmission(rig.call.id, adopted, binding);
    REQUIRE(parent.committed);
    rig.scope_allowed->store(false);
    const auto before = Bytes(rig.Path());
    REQUIRE_FALSE(rig.coordinator->ConfirmParentAdmission(rig.owner, adopted.facts->job_id, parent.refs, true).confirmed);
    REQUIRE(Bytes(rig.Path()) == before); // false policy is not an invented cancellation
    REQUIRE(rig.coordinator->RequestOwnedCancellation(rig.owner, adopted.facts->job_id, "first-real-cancel").ok);
    REQUIRE(rig.coordinator->RequestOwnedCancellation(rig.owner, adopted.facts->job_id, "later-reason").ok);
    REQUIRE(Bytes(rig.Path()) == before); // non-writing cancellation really performs no append
    const auto delivery = rig.coordinator->ConfirmParentAdmission(rig.owner, adopted.facts->job_id, parent.refs, true);
    REQUIRE_MESSAGE(delivery.confirmed, delivery.error);
    const auto status = rig.coordinator->SnapshotOwnedJob(rig.owner, adopted.facts->job_id);
    REQUIRE(status.state == "cancelled"); REQUIRE(status.cancel_requested); REQUIRE(status.gap.empty());
    REQUIRE_FALSE(status.dispatched_receipt.has_value()); REQUIRE_FALSE(status.started_receipt.has_value());
    REQUIRE_FALSE(fs::exists(rig.directory.cwd / "guard.started"));
    unsigned cancels = 0;
    for (const auto& row : Rows(rig.Path())) {
        if (row.value("actionId", Json()) != adopted.facts->action_id) continue;
        REQUIRE(row.value("kind", std::string()) != "tool.job.dispatched");
        REQUIRE(row.value("kind", std::string()) != "tool.execution.started");
        if (row.value("kind", std::string()) == "tool.job.cancel_requested") {
            ++cancels; REQUIRE(row.at("payload").at("reason") == "first-real-cancel");
        }
    }
    REQUIRE(cancels == 1);
    const auto stable = Bytes(rig.Path());
    REQUIRE(rig.coordinator->ConfirmParentAdmission(rig.owner, adopted.facts->job_id, parent.refs, true).confirmed);
    REQUIRE(Bytes(rig.Path()) == stable);
    Mark("cancel-confirm");
}

TEST_CASE("SDK Command Job guards: non-writing cancellation reaches a real process after writer closes") {
    Rig rig(true);
    const auto admitted = rig.Admit(); REQUIRE(admitted.state == agent::OwnedJobAdmissionState::Accepted);
    rig.Started();
    const auto jobs = rig.jobs->List({}); REQUIRE(jobs.has_value()); REQUIRE(jobs->size() == 1);
    const auto id = jobs->front().identity;
    REQUIRE(rig.Writer().Close().has_value());
    REQUIRE_FALSE(rig.coordinator->CancelOwnedJob(rig.owner, id.job_id, "old-writing-entry").ok);
    const auto cancelled = rig.coordinator->RequestOwnedCancellation(rig.owner, id.job_id, "stop-even-without-writer");
    REQUIRE(cancelled.ok);
    const auto again = rig.coordinator->RequestOwnedCancellation(rig.owner, id.job_id, "later-reason");
    REQUIRE(again.ok);
    REQUIRE_FALSE(rig.jobs->Finalize().has_value()); // no fabricated durable completion
    REQUIRE(rig.coordinator->shutdown_complete()); REQUIRE(rig.coordinator->running_count() == 0);
    REQUIRE_FALSE(fs::exists(rig.directory.cwd / "guard.done"));
    REQUIRE(fs::remove(rig.directory.probe));
    Mark("writer-cancel");
}

TEST_CASE("SDK Command Job guards: public completion has one actual binding parent delivery and builtin Post") {
    const auto probe = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
    unsigned observations = 0;
    lubancore_consumer::InspectCommandJobs(fs::temp_directory_path(), probe, [&](const fs::path& root, const sdk::jobs::v1::Identity& id) {
        std::vector<fs::path> streams;
        for (const auto& entry : fs::recursive_directory_iterator(root / "workspaces"))
            if (entry.is_regular_file() && entry.path().filename() == id.session_id + ".jsonl") streams.push_back(entry.path());
        REQUIRE(streams.size() == 1);
        const auto ledger = v3::ReadV3Ledger(streams.front());
        REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
        const auto bindings = detail::ReadJobOperations(streams.front().parent_path(), *ledger);
        REQUIRE(bindings.state == detail::JobOperationRecoveryState::PassiveHold);
        REQUIRE(bindings.facts.size() == 1);
        const auto& binding = bindings.facts.front();
        REQUIRE(binding.operation_id == id.operation_id); REQUIRE(binding.parent_operation_id == id.parent_operation_id);
        REQUIRE(binding.job_id == id.job_id); REQUIRE(binding.action_id == id.action_id); REQUIRE(binding.attempt == id.attempt);
        const auto adoptions = v3::ReadOwnedJobAdoptions(*ledger); REQUIRE(adoptions.has_value()); REQUIRE(adoptions->size() == 1);
        const auto& adoption = adoptions->front();
        REQUIRE(adoption.job_id == id.job_id);
        unsigned parent_tools = 0, started = 0, posts = 0, observed = 0;
        for (const auto& message : ledger->messages)
            if (message.action_id == adoption.parent_action_id && message.message.value("role", std::string()) == "tool") ++parent_tools;
        for (const auto& event : ledger->events) {
            if (event.action_id != id.action_id) continue;
            if (event.kind == v3::EventKindV3::ToolExecutionStarted) { ++started; REQUIRE(event.seq > binding.seq); }
            if (event.kind == v3::EventKindV3::HookStarted && event.payload.value("hookId", std::string()) == "sdk.command_job.verify.v1") ++posts;
            if (event.kind == v3::EventKindV3::ToolJobObserved) { ++observed; REQUIRE(event.payload.at("observedStatus") == "succeeded"); }
        }
        REQUIRE(parent_tools == 1); REQUIRE(started == 1); REQUIRE(posts == 1); REQUIRE(observed == 1);
        ++observations;
    });
    REQUIRE(observations == 1);
    CheckMissingResumeBoundary();
    Mark("actual-public-source");
}

TEST_CASE("SDK Command Job guards: same-ID Hold retains original registration without reviving an old owner") {
    for (const bool adopt : {false, true}) {
        Rig rig;
        std::shared_ptr<const tools::PreparedJobFacts> facts;
        if (adopt) facts = rig.Manual(false).first.facts;
        else facts = rig.Register().facts;
        REQUIRE(facts != nullptr);
        const auto id = facts->job_id; const auto old_owner = facts->owner;
        const auto path = rig.Path(); const auto prefix = Bytes(path);
        const auto own_rows = [&](const std::vector<Json>& rows) {
            std::vector<Json> selected;
            for (const auto& row : rows)
                if (row.value("actionId", Json()) == facts->action_id ||
                    (row.contains("payload") && row.at("payload").value("jobId", std::string()) == id))
                    selected.push_back(row);
            return selected;
        };
        const auto original = own_rows(Rows(path));
        REQUIRE_FALSE(original.empty());
        REQUIRE(rig.Writer().Close().has_value()); // a real lost writer; never rewrite a crash-shaped fixture
        const auto stopped = rig.jobs->Finalize();
        if (adopt) REQUIRE_FALSE(stopped.has_value());
        REQUIRE(rig.coordinator->shutdown_complete());
        rig.bridge.reset(); rig.jobs.reset(); rig.service.reset(); // release the actual SessionLock
        REQUIRE(Bytes(path) == prefix);
        const auto identity = lubancode::workspace::MakeFallbackIdentity(rig.directory.cwd);
        auto plan = detail::SessionCommandJobPlan::Prepare(std::nullopt, rig.directory.root / "state",
            identity.workspace_key, old_owner.session_id, Utf8(rig.directory.cwd));
        REQUIRE_MESSAGE(plan.has_value(), (plan ? std::string() : plan.error().code));
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = Utf8(rig.directory.cwd); launch.workspace_identity = identity;
        launch.workspaces_root = rig.directory.root / "state" / "workspaces";
        launch.resume_at_launch = true; launch.require_v3_resume = true; launch.resume_source_session_id = old_owner.session_id;
        launch.v3_opening_participant = (*plan)->OpeningParticipant();
        auto resumed = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE_MESSAGE(resumed->runtime() != nullptr, resumed->launch_error());
        detail::SessionApprovals approvals;
        auto module = detail::SessionCommandJobs::Build(*plan, *resumed, approvals, sdk::ApprovalMode::Yolo, 5s, {});
        REQUIRE_MESSAGE(module.has_value(), (module ? std::string() : module.error().code));
        auto coordinator = resumed->runtime()->async_tool_runtime()->coordinator();
        const auto owner = coordinator->PreparedOwner(); REQUIRE(owner.has_value());
        REQUIRE(*owner != old_owner); REQUIRE(owner->session_id == old_owner.session_id); REQUIRE(owner->run_id == old_owner.run_id);
        const auto held = (*module)->List({}); REQUIRE(held.has_value()); REQUIRE(held->size() == 1);
        REQUIRE(held->front().identity.session_id == facts->owner.session_id);
        REQUIRE(held->front().identity.run_id == facts->owner.run_id);
        REQUIRE(held->front().identity.action_id == facts->action_id); REQUIRE(held->front().identity.attempt == facts->attempt);
        REQUIRE(held->front().identity.operation_id.empty()); REQUIRE_FALSE(held->front().owner_available);
        const auto waited = (*module)->Wait(held->front().identity, 100ms);
        REQUIRE_FALSE(waited.has_value()); REQUIRE(waited.error().code == "sdk.job.owner_unavailable");
        REQUIRE_FALSE((*module)->Cancel(held->front().identity).has_value());
        (*module)->PumpAndPublish();
        REQUIRE(coordinator->prepared_count() == 0); REQUIRE(coordinator->running_count() == 0); REQUIRE(coordinator->queued_count() == 0);
        REQUIRE_FALSE(coordinator->GetPreparedJob(old_owner, id).has_value());
        REQUIRE(own_rows(Rows(path)) == original); REQUIRE(Bytes(path).starts_with(prefix));
        REQUIRE_FALSE(fs::exists(rig.directory.cwd / "guard.started"));
        REQUIRE((*module)->Finalize().has_value());
        (void)resumed->Close("hold_fixture_close");
    }
    Mark("passive-hold");
}
