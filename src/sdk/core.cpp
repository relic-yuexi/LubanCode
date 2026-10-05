#include "lubancore/core.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "agent/agent.hpp"
#include "mcp/mcp_tool.hpp"
#include "platform/atomic_write.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/assembly/backend.hpp"
#include "runtime/assembly/builtin_tools.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/scoped_turn_bindings.hpp"
#include "runtime/interaction_broker.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "runtime/session_service.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "runtime/turn_runtime.hpp"
#include "sdk/adapters.hpp"
#include "sdk/approval.hpp"
#include "sdk/callback_scope.hpp"
#include "sdk/extensions.hpp"
#include "sdk/event_queue.hpp"
#include "sdk/action_opening.hpp"
#include "sdk/action_dispatch.hpp"
#include "sdk/results.hpp"
#include "sdk/skills.hpp"
#include "sdk/subagents.hpp"
#include "sdk/memory.hpp"
#include "sdk/memory_blobs.hpp"
#include "sdk/memory_write.hpp"
#include "sdk/lua.hpp"
#include "sdk/operation_ledger.hpp"
#include "tools/path_utils.hpp"
#include "tools/search_ripgrep.hpp"
#include "tools/web_fetch.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace lubancore {
Result<web_fetch::v1::Capabilities> web_fetch::v1::DescribeCapabilities() {
    try { return web_fetch::v1::Capabilities{lubancode::tools::WebFetchSupportsGzip()}; }
    catch (...) { return std::unexpected(Error{"sdk.web_fetch.capabilities_unavailable", "cannot query the built-in transport"}); }
}

namespace detail {
thread_local bool in_session_worker = false;
}
namespace {
namespace rt = lubancode::runtime;
namespace api = lubancode::api;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using detail::in_session_worker;

// Keep shutdown diagnostics after a public session handle is dropped without
// retaining its result text, connection material or execution resources.
struct CloseErrors {
    std::mutex mutex;
    std::optional<Error> first;
    void Remember(const Error& error) {
        std::lock_guard lock(mutex);
        if (!first) first = error;
    }
    Result<void> Read() {
        std::lock_guard lock(mutex);
        return first ? Result<void>(std::unexpected(*first)) : Result<void>{};
    }
};

Error Failure(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
lubancode::trajectory::RecoveryReadLimits InternalRecoveryLimits(const RecoveryReadLimits& value) {
    return {{value.journal.max_bytes, value.journal.max_lines, value.journal.max_line_bytes},
        {value.operations.max_bytes, value.operations.max_lines, value.operations.max_line_bytes},
        value.result_total_bytes, value.view_total_bytes, value.result_directory_entries,
        value.view_directory_entries, value.directory_name_bytes, value.directory_name_total_bytes};
}
bool Terminal(OperationState state) { return state != OperationState::Accepted && state != OperationState::Running; }
bool ValidId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
Result<fs::path> AbsoluteDirectory(const std::string& value, bool create) {
    if (value.empty() || value.find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(value)) return std::unexpected(Failure("sdk.path.invalid", value));
    auto path = lubancode::tools::Utf8ToPath(value);
    if (!path.is_absolute()) return std::unexpected(Failure("sdk.path.absolute_required", value));
    std::error_code ec;
    if (create) fs::create_directories(path, ec);
    if (ec || !fs::is_directory(path, ec)) return std::unexpected(Failure("sdk.path.directory_required", value));
    auto canonical = fs::weakly_canonical(path, ec);
    if (ec) return std::unexpected(Failure("sdk.path.resolve_failed", ec.message()));
    return canonical;
}
lubancode::ApprovalMode Mode(ApprovalMode mode) {
    switch (mode) {
        case ApprovalMode::Confirm: return lubancode::ApprovalMode::Default;
        case ApprovalMode::AcceptEdits: return lubancode::ApprovalMode::AcceptEdits;
        case ApprovalMode::DontAsk: return lubancode::ApprovalMode::DontAsk;
        case ApprovalMode::Yolo: return lubancode::ApprovalMode::Yolo;
    }
    return lubancode::ApprovalMode::Default;
}
rt::InteractionDecision Decision(ApprovalDecision decision) {
    switch (decision) {
        case ApprovalDecision::Accept: return rt::InteractionDecision::Accept;
        case ApprovalDecision::AcceptForSession: return rt::InteractionDecision::AcceptForSession;
        case ApprovalDecision::Decline: return rt::InteractionDecision::Decline;
        case ApprovalDecision::Cancel: return rt::InteractionDecision::Cancel;
    }
    return rt::InteractionDecision::Cancel;
}
std::string Status(OperationState state) {
    switch (state) {
        case OperationState::Succeeded: return "success";
        case OperationState::Failed: return "error";
        case OperationState::Cancelled: return "cancelled";
        default: return "interrupted";
    }
}

} // namespace

struct EventStream::Impl {
    explicit Impl(std::shared_ptr<detail::EventQueueState> value) : queue(std::move(value)) {}
    std::shared_ptr<detail::EventQueueState> queue;
    void Push(const Event& event) { queue->Push(event); }
    Result<void> CloseChecked() { return queue->CloseChecked(); }
};

EventStream::EventStream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
EventStream::~EventStream() { Close(); }
void EventStream::Close() { (void)CloseChecked(); }
Result<void> EventStream::CloseChecked() { auto state = impl_; return state->CloseChecked(); }
Result<std::optional<Event>> EventStream::Next(std::chrono::milliseconds timeout) {
    auto state = impl_;
    return state->queue->Next(timeout);
}

// One writer/Agent per session. Cwd belongs to tool adapters, never the process.
// All borrowed Agent/MCP/trajectory references die before their owners.
struct Session::Impl final : rt::InteractionBroker {
    RuntimeOptions roots;
    SessionOptions options;
    std::shared_ptr<std::atomic<bool>> runtime_stopping;
    std::string session_id;
    fs::path session_dir;
    // RequestClose may race a different caller finishing Close. A short mutex
    // snapshot keeps the service alive while cancellation is signalled.
    std::shared_ptr<rt::SessionService> service;
    // Sole ownership lives with SessionResources, after Agent and before tools.
    // Publish this borrow only after initialization succeeds; queries use cache.
    detail::SessionExtensions* extensions = nullptr;
    std::string extension_plan_json;
    skills::v1::Snapshot skills_snapshot;
    detail::SessionSubagents* subagent_module = nullptr;
    subagents::v1::Snapshot subagent_snapshot;
    std::map<std::string, Result<std::vector<subagents::v1::Report>>> subagent_reports;
    // Worker-only actual receipt values. A restored run does not populate these
    // from child journal claims or the coordinator's business status.
    std::map<std::pair<std::string, std::uint64_t>, subagents::v1::LiveTerminalReceipt> live_child_receipts;
    std::shared_ptr<detail::SessionMemory> memory_module;
    std::shared_ptr<lubancode::trajectory::MemoryCapabilityFactory> memory_blob_factory;
    memory::v1::Snapshot memory_snapshot;
    std::map<std::string, Result<memory::v1::RecallReport>> memory_reports;
    std::shared_ptr<detail::SessionMemoryWrite> memory_write_module;
    memory::v1::WriteSnapshot memory_write_snapshot;
    lua::v1::Snapshot lua_snapshot;
    std::map<std::string, Result<std::vector<memory::v1::SaveReport>>> memory_saves;
    bool memory_write_indeterminate = false; // protected by mutex after publication
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::mutex close_mutex;
    std::map<std::string, Operation> operations;
    results::v1::SessionResultPolicy result_policy;
    // Immutable per-operation identity/ref snapshots. These retain no service,
    // Agent or original output bytes and remain usable after Close.
    std::map<std::string, Result<std::shared_ptr<const detail::OperationToolResultIndex>>> tool_results;
    std::set<std::string> cancelled;
    std::vector<std::weak_ptr<EventStream::Impl>> subscriptions;
    std::shared_ptr<detail::EventDeliveryOwner> event_delivery;
    std::string active_operation;
    std::string active_turn_id; // worker-only; allocated by the durable V3 writer
    bool closing = false;
    bool closed = false;
    std::size_t close_signals_inflight = 0;
    bool broken = false;
    std::optional<Error> close_error;
    std::shared_ptr<CloseErrors> close_errors;
    std::atomic<bool> interrupt{false};
    std::thread worker;
    detail::SessionApprovals approvals;

    ~Impl() { (void)Close(); }

    void Emit(Event event) {
        event.session_id = session_id;
        std::vector<std::shared_ptr<EventStream::Impl>> targets;
        {
            std::lock_guard lock(mutex);
            for (auto it = subscriptions.begin(); it != subscriptions.end();) {
                if (auto stream = it->lock()) { targets.push_back(std::move(stream)); ++it; }
                else it = subscriptions.erase(it);
            }
        }
        for (auto& stream : targets) stream->Push(event);
    }

    std::shared_ptr<rt::InteractionFuture> AskApproval(const rt::ApprovalRequest& request) override {
        Approval approval;
        // Session-local counters start from one. Scope the opaque request ID by
        // the session and durable turn so another session (or a stale reply from
        // before resume) cannot resolve this session's pending approval.
        approval.request_id = session_id + ":" + active_turn_id + ":" +
            service->runtime()->ids().NextPrefixedId("sdk-approval");
        approval.operation_id = active_operation; // written only by this worker
        approval.tool_call_id = request.tool_use_id;
        approval.tool_name = request.tool_name;
        approval.input_json = request.input.dump();
        approval.cwd = options.cwd;
        approval.reason = request.reason;
        bool registered = false;
        auto future = approvals.Register(approval, options.approval_timeout, &interrupt, &registered);
        // Keep the old interrupt check under the shared pending lock. A ticket
        // cancelled concurrently can still publish an obsolete event, just as
        // before; resolution remains stale and cannot revive that ticket.
        if (!registered) return future;
        Event event;
        event.kind = "approval_requested";
        event.operation_id = approval.operation_id;
        event.approval = std::move(approval);
        Emit(std::move(event));
        return future;
    }
    std::shared_ptr<rt::InteractionFuture> AskQuestion(const rt::QuestionRequest&) override {
        return detail::SessionApprovals::CancelledFuture(); // ask_user is not admitted by this SDK version
    }
    bool ResolveApproval(const rt::InteractionRequestId& id, const rt::ApprovalResponse& response) override {
        return approvals.Resolve(id.value, response);
    }
    bool AnswerQuestion(const rt::InteractionRequestId&, const rt::QuestionResponse&) override { return false; }
    void CancelApprovals() {
        approvals.CancelAll();
    }

    Result<void> Initialize() {
        const auto recovery_limits = InternalRecoveryLimits(options.recovery_read_limits);
        if (!lubancode::trajectory::ValidRecoveryReadLimits(recovery_limits))
            return std::unexpected(Failure("sdk.recovery.invalid_limits"));
        auto cwd = AbsoluteDirectory(options.cwd, false);
        if (!cwd) return std::unexpected(cwd.error());
        options.cwd = lubancode::tools::PathToUtf8(*cwd);
        if (options.model.empty() || !lubancode::platform::IsValidUtf8(options.model) ||
            !lubancode::platform::IsValidUtf8(options.system_prompt) || options.approval_timeout.count() <= 0 || options.max_steps_per_turn < 0 ||
            options.context_window_tokens == 0 || (!!options.backend == options.connection.has_value())) {
            return std::unexpected(Failure("sdk.session.invalid_options"));
        }
        if (!options.resume_session_id.empty() && !ValidId(options.resume_session_id)) {
            return std::unexpected(Failure("sdk.resume.invalid_id"));
        }
        std::optional<lubancode::tools::WebFetchOptions> web_fetch_options;
        const bool has_web_fetch = std::find(options.builtin_tools.begin(), options.builtin_tools.end(), "web_fetch") != options.builtin_tools.end();
        if (options.web_fetch && !has_web_fetch)
            return std::unexpected(Failure("sdk.web_fetch.not_selected"));
        if (has_web_fetch) {
            const auto selected = options.web_fetch.value_or(web_fetch::v1::Options{});
            web_fetch_options = lubancode::tools::WebFetchOptions{selected.user_agent, selected.connect_timeout_ms,
                selected.total_timeout_ms, selected.max_header_bytes, selected.max_download_bytes,
                selected.max_output_bytes, selected.max_redirects};
            if (!lubancode::tools::ValidateWebFetchOptions(*web_fetch_options))
                return std::unexpected(Failure("sdk.web_fetch.invalid_options"));
        }
        struct InitCleanupScope {
            bool previous = in_session_worker;
            InitCleanupScope() { in_session_worker = true; }
            ~InitCleanupScope() { in_session_worker = previous; }
        } cleanup_scope;
        // Keep the public backend alive across every initialization rollback.
        // MCP launch can fail before registry_factory consumes prepared_registry;
        // later identity/ledger/Agent failures can likewise release the candidate
        // resources before the SDK-owned inline tool callback sources retire.
        std::shared_ptr<Backend> initialization_backend(std::move(options.backend));
        struct SourceScope {
            SessionOptions& options;
            ~SourceScope() {
                // Other initialization locals retire first, under the same TLS
                // lifecycle guard. Clear the actual sources before the backend
                // anchor declared above, including std::function SBO copies.
                options.custom_tools.clear();
                options.extensions.clear();
            }
        } source_scope{options};
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(*cwd, lubancode::tools::Utf8ToPath(roots.data_root));
        if (!identity) return std::unexpected(Failure("sdk.workspace.failed", identity.error()));
        auto action_opening = detail::SessionActionOpening::Prepare(options.extensions,
            lubancode::tools::Utf8ToPath(roots.data_root), identity->workspace_key, options.resume_session_id);
        if (!action_opening) return std::unexpected(action_opening.error());
        auto child_plan = detail::SessionSubagentPlan::Prepare(options.subagents,
            lubancode::tools::Utf8ToPath(roots.data_root), identity->workspace_key,
            options.resume_session_id, options.cwd, options.model,
            lubancode::ApprovalModeMachineName(Mode(options.approval_mode)), options.max_steps_per_turn);
        if (!child_plan) return std::unexpected(child_plan.error());
        auto skill_module = detail::SessionSkills::Prepare(options.skills,
            lubancode::tools::Utf8ToPath(roots.data_root), identity->workspace_key,
            options.resume_session_id, options.system_prompt);
        if (!skill_module) return std::unexpected(skill_module.error());
        auto memory_candidate = detail::SessionMemory::Prepare(options.memory,
            lubancode::tools::Utf8ToPath(roots.data_root), *identity, options.resume_session_id, *cwd);
        if (!memory_candidate) return std::unexpected(memory_candidate.error());
        memory_module = std::move(*memory_candidate);
        auto write_candidate = detail::SessionMemoryWrite::Prepare(options.memory_write,
            lubancode::tools::Utf8ToPath(roots.data_root), *identity, options.resume_session_id);
        if (!write_candidate) return std::unexpected(write_candidate.error());
        memory_write_module = std::move(*write_candidate);
        auto lua_module = detail::SessionLua::Prepare(options.lua,
            lubancode::tools::Utf8ToPath(roots.data_root), identity->workspace_key,
            options.resume_session_id);
        if (!lua_module) return std::unexpected(lua_module.error());
        auto prepared_registry = std::make_unique<lubancode::tools::ToolRegistry>();
        std::shared_ptr<lubancode::tools::BundledRipgrepRunner> search_runner;
        for (const auto& name : options.builtin_tools) {
            if (name == "search" && !search_runner) {
                auto executable = lubancode::tools::Utf8ToPath(roots.resource_root) / "libexec";
#ifdef _WIN32
                executable /= "rg.exe";
#else
                executable /= "rg";
#endif
                // Nonempty absolute override: SDK search never uses the CLI's
                // exe/home/PATH discovery. Preparation stays lazy until every
                // local tool declaration and server spec has been validated.
                search_runner = std::make_shared<lubancode::tools::BundledRipgrepRunner>(std::move(executable));
            }
            auto tool = rt::assembly::CreateLocalTool(name, search_runner, web_fetch_options ? &*web_fetch_options : nullptr);
            if (!tool || prepared_registry->Find(name)) return std::unexpected(Failure("sdk.tool.unsupported_or_duplicate", name));
            prepared_registry->Register(detail::BindLocalTool(std::move(tool), options.cwd));
        }
        for (auto& tool : options.custom_tools) {
            if (prepared_registry->Find(tool.name)) return std::unexpected(Failure("sdk.tool.duplicate", tool.name));
            auto adapted = detail::AdaptTool(std::move(tool), options.cwd);
            if (!adapted) return std::unexpected(adapted.error());
            prepared_registry->Register(std::move(*adapted));
        }
        if ((*skill_module)->enabled() && prepared_registry->Find("skill"))
            return std::unexpected(Failure("sdk.tool.duplicate", "skill"));
        if (memory_write_module->Describe().enabled && prepared_registry->Find("memory_save"))
            return std::unexpected(Failure("sdk.tool.duplicate", "memory_save"));
        for (const auto& name : (*lua_module)->Names()) {
            if (prepared_registry->Find(name))
                return std::unexpected(Failure("sdk.tool.duplicate", name));
        }
        auto lua_tools = (*lua_module)->TakeTools();
        if (!lua_tools) return std::unexpected(lua_tools.error());
        const auto lua_declaration = (*lua_module)->Describe();
        for (auto& tool : *lua_tools) {
            const auto entry = std::find_if(lua_declaration.entries.begin(), lua_declaration.entries.end(),
                [&](const auto& item) { return item.tool_name == tool->name(); });
            if (entry == lua_declaration.entries.end())
                return std::unexpected(Failure("sdk.lua.plan_invalid", "tool is absent from its declaration"));
            lubancode::tools::ToolRegistration registration;
            registration.source_kind = lubancode::tools::ToolSourceKind::PluginLua;
            registration.source_instance = entry->path;
            registration.version_or_digest = entry->content_sha256;
            registration.tool = std::move(tool);
            prepared_registry->Register(std::move(registration));
        }
        rt::assembly::SessionResourcesRequest resource_request;
        std::set<std::string> server_names;
        for (const auto& spec : options.mcp_servers) {
            if (!ValidId(spec.name) || !server_names.insert(spec.name).second || spec.command.empty() ||
                !lubancode::tools::Utf8ToPath(spec.command).is_absolute() ||
                spec.tools.empty() || spec.startup_timeout_ms <= 0 || spec.call_timeout_ms <= 0) {
                return std::unexpected(Failure("sdk.mcp.invalid_spec", spec.name));
            }
            resource_request.mcp_servers.push_back({
                {spec.name, spec.command, spec.arguments, spec.environment,
                 lubancode::platform::EnvMode::Replace, options.cwd},
                {spec.startup_timeout_ms, spec.call_timeout_ms, &interrupt}, true});
        }
        resource_request.registry_factory = [&](std::span<const rt::assembly::McpServerRuntime> servers)
            -> rt::assembly::SessionRegistryResult {
            // Move the candidate table into this callback before it borrows a
            // Client. Failure/exception destroys these tools before factory rollback.
            auto registry = std::move(prepared_registry);
            for (std::size_t i = 0; i < servers.size(); ++i) {
                const auto& owner = servers[i];
                const auto& spec = options.mcp_servers[i]; // all SDK servers are required
                for (const auto& name : spec.tools) {
                    auto found = std::find_if(owner.tools.begin(), owner.tools.end(),
                        [&](const auto& info) { return info.name == name; });
                    if (found == owner.tools.end()) {
                        return std::unexpected(rt::assembly::SessionResourceFailure{
                            rt::assembly::SessionResourceStage::Registry, "sdk.mcp.tool_missing", name, {}, {}});
                    }
                    auto tool = std::make_unique<lubancode::mcp::McpTool>(*owner.client, spec.name, *found);
                    if (registry->Find(tool->name())) {
                        return std::unexpected(rt::assembly::SessionResourceFailure{
                            rt::assembly::SessionResourceStage::Registry, "sdk.tool.duplicate", tool->name(), {}, {}});
                    }
                    lubancode::tools::ToolRegistration registration;
                    registration.source_kind = lubancode::tools::ToolSourceKind::Mcp;
                    registration.source_instance = spec.name;
                    registration.tool = std::move(tool);
                    registry->Register(std::move(registration));
                }
            }
            std::set<std::string> tool_face;
            for (const auto& tool : registry->All()) tool_face.insert(tool->name());
            if ((*child_plan)->enabled()) {
                if (registry->Find("agent"))
                    return std::unexpected(rt::assembly::SessionResourceFailure{
                        rt::assembly::SessionResourceStage::Registry, "sdk.tool.duplicate", "agent", "subagents", {}});
                tool_face.insert("agent");
            }
            // The actual target is attached after exclusive ledger opening.
            // Include its reserved name in the frozen Skills tool surface.
            if (memory_write_module->Describe().enabled) {
                if (registry->Find("memory_save"))
                    return std::unexpected(rt::assembly::SessionResourceFailure{
                        rt::assembly::SessionResourceStage::Registry, "sdk.tool.duplicate",
                        "memory_save", "memory", {}});
                tool_face.insert("memory_save");
            }
            auto frozen = (*skill_module)->BindToolSurface(std::move(tool_face));
            if (!frozen) return std::unexpected(rt::assembly::SessionResourceFailure{
                rt::assembly::SessionResourceStage::Registry, frozen.error().code,
                frozen.error().message, "skills", {}});
            if ((*skill_module)->enabled()) registry->Register((*skill_module)->BuildTool());
            auto child_tools = (*child_plan)->BindTools(*registry);
            if (!child_tools) return std::unexpected(rt::assembly::SessionResourceFailure{
                rt::assembly::SessionResourceStage::Registry, child_tools.error().code,
                child_tools.error().message, "subagents", {}});
            return registry;
        };
        std::string wire = "sdk_custom";
        std::optional<lubancode::config::Config> backend_config;
        if (!initialization_backend) {
            const auto& source = *options.connection;
            if (source.base_url.empty() || source.connect_timeout_ms <= 0 || source.idle_timeout_seconds <= 0 || source.request_timeout_seconds <= 0) {
                return std::unexpected(Failure("sdk.connection.invalid"));
            }
            lubancode::config::Config config;
            switch (source.wire) {
                case Wire::Anthropic: config.wire = lubancode::config::Wire::Anthropic; wire = "anthropic"; break;
                case Wire::ChatCompletions: config.wire = lubancode::config::Wire::ChatCompletions; wire = "chat_completions"; break;
                case Wire::Responses: config.wire = lubancode::config::Wire::Responses; wire = "responses"; break;
                case Wire::Gemini: config.wire = lubancode::config::Wire::GoogleGenerateContent; wire = "gemini"; break;
            }
            config.base_url = source.base_url;
            config.auth_token = source.api_key;
            config.connect_timeout_ms = source.connect_timeout_ms;
            config.stream_idle_timeout_secs = source.idle_timeout_seconds;
            config.request_hard_timeout_secs = source.request_timeout_seconds;
            backend_config = std::move(config);
        }
        resource_request.backend_factory = [&]() -> std::unique_ptr<api::Backend> {
            if (initialization_backend) return detail::AdaptBackend(initialization_backend);
            return rt::assembly::BuildBackend(*backend_config);
        };
        if (search_runner) {
            const auto prepared = search_runner->Prepare();
            if (!prepared) return std::unexpected(Failure(
                std::string(lubancode::tools::ToString(prepared.error().code)), prepared.error().message));
        }
        auto assembled = rt::assembly::BuildSessionResources(std::move(resource_request));
        if (!assembled) {
            const auto& error = assembled.error();
            if (error.stage == rt::assembly::SessionResourceStage::Mcp)
                return std::unexpected(Failure("sdk.mcp.start_failed", error.message));
            if (error.code == "assembly.mcp.invalid_spec")
                return std::unexpected(Failure("sdk.mcp.invalid_spec", error.component));
            if (error.stage == rt::assembly::SessionResourceStage::Registry)
                return std::unexpected(Failure(error.code, error.message));
            return std::unexpected(Failure("sdk.session.open_failed", error.message));
        }
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = options.cwd;
        launch.workspace_identity = std::move(*identity);
        launch.lubancode_version = Version();
        launch.wire_name = wire;
        launch.approval_mode = Mode(options.approval_mode);
        launch.workspaces_root = lubancode::tools::Utf8ToPath(roots.data_root) / "workspaces";
        launch.v3_system_content = (*skill_module)->EffectiveSystem();
        launch.recovery_capture.limits = recovery_limits;
        launch.recovery_capture.memory_metadata = memory_module->RequiresRecoveryMetadata();
        launch.memory_capability_factory = memory_blob_factory;
        auto skills_opening = (*skill_module)->OpeningParticipant();
        auto memory_opening = memory_module->OpeningParticipant();
        auto write_opening = memory_write_module->OpeningParticipant();
        auto child_opening = (*child_plan)->OpeningParticipant();
        auto lua_opening = (*lua_module)->OpeningParticipant();
        auto action_gate = (*action_opening)->OpeningParticipant();
        launch.v3_opening_participant = [skills_opening = std::move(skills_opening), memory_opening = std::move(memory_opening),
                                       write_opening = std::move(write_opening), child_opening = std::move(child_opening),
                                       lua_opening = std::move(lua_opening), action_gate = std::move(action_gate)]
            (const lubancode::trajectory::V3OpeningContext& context) -> std::expected<Json, std::string> {
                // Recheck the strict Action declaration before another opening
                // participant can publish metadata or transfer an old system.
                auto actions = action_gate(context);
                if (!actions) return std::unexpected(actions.error());
                auto skills = skills_opening(context);
                if (!skills) return std::unexpected(skills.error());
                auto memory = memory_opening(context);
                if (!memory) return std::unexpected(memory.error());
                auto writes = write_opening(context);
                if (!writes) return std::unexpected(writes.error());
                auto children = child_opening(context);
                if (!children) return std::unexpected(children.error());
                auto scripts = lua_opening(context);
                if (!scripts) return std::unexpected(scripts.error());
                for (const auto* part : {&*memory, &*writes, &*children, &*scripts, &*actions}) if (part->contains("hostBindings")) {
                    if (!skills->contains("hostBindings")) (*skills)["hostBindings"] = Json::object();
                    for (auto it = (*part)["hostBindings"].begin(); it != (*part)["hostBindings"].end(); ++it)
                        (*skills)["hostBindings"][it.key()] = it.value();
                }
                return std::move(*skills);
            };
        launch.resume_at_launch = !options.resume_session_id.empty();
        launch.require_v3_resume = launch.resume_at_launch;
        launch.resume_source_session_id = options.resume_session_id;
        service = std::make_shared<rt::SessionService>(std::move(launch));
        if (!service->runtime()) return std::unexpected(Failure(
            service->launch_error().find("sdk.skill.") != std::string::npos ? "sdk.skill.open_failed" :
            service->launch_error().find("sdk.memory.") != std::string::npos ? "sdk.memory.open_failed" :
            service->launch_error().find("sdk.memory_write.") != std::string::npos ? "sdk.memory_write.open_failed" :
            service->launch_error().find("sdk.subagent.") != std::string::npos ? "sdk.subagent.open_failed" :
            service->launch_error().find("sdk.action.") != std::string::npos ? "sdk.action.open_failed" :
            service->launch_error().find("sdk.lua.") != std::string::npos ? "sdk.lua.open_failed" : "sdk.session.open_failed",
            service->launch_error()));
        session_id = service->trajectory()->session_id();
        session_dir = service->trajectory()->session_dir();
        memory_snapshot = memory_module->Describe();
        memory_write_snapshot = memory_write_module->Describe();
        lua_snapshot = (*lua_module)->Describe();
        if (!options.resume_session_id.empty() && session_id != options.resume_session_id) {
            return std::unexpected(Failure("sdk.resume.identity_changed"));
        }
        auto frozen_policy = detail::FreezeResultPolicy(session_dir, session_id, options.result_policy,
            !options.resume_session_id.empty());
        if (!frozen_policy) return std::unexpected(frozen_policy.error());
        result_policy = std::move(*frozen_policy);
        if (memory_write_snapshot.enabled)
            (*assembled)->registry().Register(memory_write_module->BuildTool(*service->trajectory()));
        options.system_prompt = (*skill_module)->EffectiveSystem();
        if (!options.resume_session_id.empty() && options.system_prompt.empty()) {
            auto saved = lubancode::trajectory::v3::ReadV3Ledger(service->trajectory()->v3_main_writer()->path());
            if (!saved) return std::unexpected(Failure("sdk.resume.context_unavailable", saved.error()));
            options.system_prompt = lubancode::trajectory::v3::ProjectModelContext(*saved).system_content;
        }
        const auto plan_path = session_dir / "sdk-extension-plan.json";
        std::optional<std::string> expected_plan;
        if ((*action_opening)->enabled()) expected_plan = (*action_opening)->plan();
        if (!(*action_opening)->enabled() && !options.resume_session_id.empty()) {
            std::error_code inspect_error;
            const bool exists = fs::exists(plan_path, inspect_error);
            if (inspect_error) return std::unexpected(Failure("sdk.extension.resume_mismatch", "cannot inspect saved extension plan"));
            if (exists) {
                std::ifstream saved(plan_path, std::ios::binary);
                if (!saved) return std::unexpected(Failure("sdk.extension.resume_mismatch", "cannot read saved extension plan"));
                auto json = Json::parse(saved, nullptr, false);
                if (!json.is_object() || !json.contains("schemaVersion") || json["schemaVersion"] != 1 ||
                    !json.contains("extensions") || !json["extensions"].is_array()) {
                    return std::unexpected(Failure("sdk.extension.resume_mismatch", "invalid saved extension plan"));
                }
                expected_plan = json.dump();
            } else if (!options.extensions.empty()) {
                // Legacy sessions did not declare an extension identity. Never
                // silently attach new executable callbacks to that old history.
                return std::unexpected(Failure("sdk.extension.resume_mismatch", "legacy session has no extension plan"));
            }
        }
        auto module = detail::SessionExtensions::Build(options.extensions, {session_id, options.cwd}, expected_plan);
        if (!module) return std::unexpected(module.error());
        auto* module_ptr = module->get();
        extension_plan_json = module_ptr->DescribePlan();
        (*assembled)->Attach(std::move(*module));
        if (!(*action_opening)->enabled() && options.resume_session_id.empty()) {
            const auto written = lubancode::platform::AtomicWriteFile(plan_path, extension_plan_json,
                lubancode::platform::WriteDurability::ProcessCrashDurability);
            if (!written) return std::unexpected(Failure("sdk.extension.plan_write_failed", written.error().message));
        }
        lubancode::agent::AgentProfile profile;
        profile.request.model = options.model;
        profile.system_prompt = options.system_prompt;
        profile.runtime.max_steps_per_turn = options.max_steps_per_turn;
        profile.runtime.context_window_tokens = options.context_window_tokens;
        detail::SessionSubagents* child_module_ptr = nullptr;
        if ((*child_plan)->enabled()) {
            auto children = detail::SessionSubagents::Build(*child_plan,
                (*assembled)->backend(), (*assembled)->registry(), profile);
            if (!children) return std::unexpected(children.error());
            child_module_ptr = children->get();
            (*assembled)->registry().Register(child_module_ptr->BuildTool());
            (*assembled)->Attach(std::move(*children));
        }
        subagent_snapshot = (*child_plan)->Describe();
        std::optional<std::vector<api::Message>> restored_history;
        if (!options.resume_session_id.empty()) restored_history = service->trajectory()->LaunchResumeHistory();
        service->InitializeExecution(std::move(*assembled), std::move(profile), std::move(restored_history));
        auto loaded = LoadOperations();
        if (!loaded) return loaded;
        extensions = module_ptr;
        subagent_module = child_module_ptr;
        skills_snapshot = (*skill_module)->Describe();
        return {};
    }

    void Start() {
        std::lock_guard lock(mutex); // Publish the owned thread before Pump enters callbacks.
        worker = std::thread([this] { Pump(); });
    }

    Result<void> LoadOperations() {
        const auto valid = detail::ValidateOperationLedger(session_dir);
        if (!valid) return valid;
        std::map<std::string, std::string> accepted_hashes;
        std::set<std::string> final_ids;
        for (const auto& fact : rt::SessionService::ReadOperationFacts(session_dir)) {
            if (!ValidId(fact.operation_id)) throw std::runtime_error("sdk.operation.invalid_persisted_id");
            auto& operation = operations[fact.operation_id];
            operation.operation_id = fact.operation_id;
            if (fact.kind == "operation.accepted") accepted_hashes[fact.operation_id] = fact.payload_hash;
            if (fact.kind == "operation.dispatched") {
                operation.state = OperationState::Indeterminate;
                operation.error = "sdk.operation.indeterminate: dispatched without a durable final";
            }
            if (fact.kind != "operation.final") continue;
            final_ids.insert(fact.operation_id);
            operation.turn_id = fact.turn_id;
            operation.state = fact.execution_status == "success" ? OperationState::Succeeded :
                fact.execution_status == "error" ? OperationState::Failed :
                fact.execution_status == "cancelled" ? OperationState::Cancelled : OperationState::Indeterminate;
            bool has_owned_result = false;
            std::ifstream input(session_dir / "sdk-results" / (fact.operation_id + ".json"), std::ios::binary);
            if (input) {
                auto json = Json::parse(input, nullptr, false);
                if (json.is_object() && json.value("operationId", "") == fact.operation_id && json.value("turnId", "") == fact.turn_id) {
                    has_owned_result = true;
                    operation.final_text = json.value("finalText", "");
                    operation.error = json.value("error", "");
                    operation.result_persisted = json.value("complete", false) && operation.state != OperationState::Indeterminate;
                }
            }
            if (!operation.result_persisted) {
                // A real interrupted result can deliberately record complete=false.
                // Preserve its owned failure cause; absence/corruption still has
                // the established unavailable diagnostic.
                if (!has_owned_result || operation.state != OperationState::Indeterminate || operation.error.empty())
                    operation.error = "sdk.result.unavailable: final operation exists without SDK result artifact";
                if (memory_snapshot.enabled || memory_write_snapshot.enabled) operation.state = OperationState::Indeterminate;
            }
        }
        // SessionService tolerates an unreadable input artifact by leaving it
        // out of its recovery queue. A runnable SDK session must instead fail
        // explicitly: an Accepted operation without a queued input cannot finish.
        // Check the original payload hash as well, before any worker can dispatch.
        std::set<std::string> queued_ids;
        for (const auto& input : service->PendingInputsSnapshot()) {
            const auto found = operations.find(input.operation_id);
            const auto hash = accepted_hashes.find(input.operation_id);
            if (found == operations.end() || found->second.state != OperationState::Accepted ||
                hash == accepted_hashes.end() || !queued_ids.insert(input.operation_id).second ||
                hash->second != lubancode::platform::Sha256Hex(
                    rt::SessionService::CanonicalInputPayload({{}, input.text, input.images}))) {
                return std::unexpected(Failure("sdk.resume.input_unavailable", input.operation_id));
            }
        }
        for (const auto& [id, operation] : operations) {
            if (Terminal(operation.state)) {
                if (memory_snapshot.enabled) {
                    auto report = memory_module->ReadReport(id);
                    if (report && final_ids.contains(id) && report->turn_id != operation.turn_id)
                        report = std::unexpected(Failure("sdk.memory.report_invalid", "operation turn differs"));
                    if (!report && operation.result_persisted) return std::unexpected(report.error());
                    // A crash after dispatch may leave a valid adopted report
                    // without a final. Preserve Indeterminate and its owned
                    // metadata; a missing report is an explicit query gap.
                    memory_reports.insert_or_assign(id, std::move(report));
                } else memory_reports.insert_or_assign(id, memory_module->EmptyReport(id, operation.turn_id));
            }
            if (operation.state == OperationState::Accepted && !queued_ids.contains(id)) {
                return std::unexpected(Failure("sdk.resume.input_unavailable", id));
            }
        }
        // No worker has started. Read the verified ledger once for all restored
        // terminal operations; a failed read is retained as a query error.
        const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(
            session_dir / (session_id + ".jsonl"));
        if (memory_snapshot.enabled) {
            if (!ledger) return std::unexpected(Failure("sdk.memory.report_invalid", "verified input is unavailable"));
            for (auto& [id, report] : memory_reports) {
                if (!report) continue;
                auto valid_report = memory_module->ValidateReport(*report, *ledger);
                if (!valid_report) {
                    if (operations.at(id).result_persisted) return std::unexpected(valid_report.error());
                    report = std::unexpected(valid_report.error());
                }
            }
        }
        if (memory_write_snapshot.enabled && !ledger)
            return std::unexpected(Failure("sdk.memory_write.report_invalid", "verified input is unavailable"));
        for (auto& [id, operation] : operations) {
            if (!Terminal(operation.state)) continue;
            auto reports = memory_write_module->ReadReports(id);
            // A dispatched turn interrupted before operation.final still has
            // its real identity in the owned, V3-checked per-action reports.
            if (reports && !final_ids.contains(id) && operation.turn_id.empty() && !reports->empty())
                operation.turn_id = reports->front().turn_id;
            if (reports && memory_write_snapshot.enabled) {
                auto valid = memory_write_module->ValidateReports(id, operation.turn_id, *reports, *ledger,
                    operation.result_persisted);
                if (!valid) reports = std::unexpected(valid.error());
            }
            const bool uncertain = reports && std::any_of(reports->begin(), reports->end(),
                [](const auto& item) { return item.state == "indeterminate"; });
            if (operation.result_persisted && (!reports || uncertain))
                return std::unexpected(reports ? Failure("sdk.memory_write.report_invalid",
                    "complete operation claims an indeterminate project write") : reports.error());
            if (memory_write_snapshot.enabled && (uncertain || operation.state == OperationState::Indeterminate))
                memory_write_indeterminate = true;
            memory_saves.insert_or_assign(id, std::move(reports));
        }
        memory_write_indeterminate = memory_write_indeterminate || memory_write_module->HasIndeterminate();
        std::map<std::string, std::size_t> turn_owners;
        for (const auto& [id, operation] : operations) {
            (void)id;
            if (Terminal(operation.state) && !operation.turn_id.empty()) ++turn_owners[operation.turn_id];
        }
        for (const auto& [id, operation] : operations) {
            if (!Terminal(operation.state)) continue;
            if (!operation.turn_id.empty() && turn_owners[operation.turn_id] != 1) {
                tool_results.insert_or_assign(id, std::unexpected(Failure("sdk.result.index_invalid", "turn belongs to multiple operations")));
            } else if (!ledger) {
                tool_results.insert_or_assign(id, std::unexpected(Failure("sdk.result.index_unavailable", "cannot read verified result ledger")));
            } else {
                CacheToolResults(operation, true, &*ledger);
            }
            Result<std::vector<subagents::v1::Report>> children = std::vector<subagents::v1::Report>{};
            if (subagent_snapshot.enabled) {
                if (!operation.turn_id.empty() && turn_owners[operation.turn_id] != 1)
                    return std::unexpected(Failure("sdk.subagent.report_invalid", "turn belongs to multiple accepted operations"));
                if (!ledger) children = std::unexpected(Failure("sdk.subagent.report_unavailable", "verified parent journal unavailable"));
                else if (operation.turn_id.empty() && operation.state == OperationState::Indeterminate)
                    children = std::unexpected(Failure("sdk.subagent.report_unavailable", "interrupted operation has no owned durable turn envelope"));
                else children = detail::ReadSubagentReports(*ledger, session_dir, session_id, id,
                    operation.turn_id, operation.result_persisted,
                    operation.state == OperationState::Cancelled ||
                    (operation.state == OperationState::Failed && operation.error == "sdk.turn.limit_reached"));
                if (!children && operation.result_persisted) return std::unexpected(children.error());
            }
            subagent_reports.insert_or_assign(id, std::move(children));
        }
        return {};
    }

    void CacheToolResults(const Operation& operation, bool ledger_ok,
        const lubancode::trajectory::v3::V3Ledger* restored = nullptr) {
        Result<std::shared_ptr<const detail::OperationToolResultIndex>> indexed =
            std::unexpected(Failure("sdk.result.index_unavailable", "operation has no verified result index"));
        if (ledger_ok && operation.turn_id.empty() && operation.state == OperationState::Cancelled) {
            // Cancelled before dispatching a V3 turn: no tool could have run.
            indexed = std::make_shared<const detail::OperationToolResultIndex>();
        } else if (ledger_ok && !operation.turn_id.empty()) {
            const auto capture = [&](const auto& ledger) {
                auto value = detail::IndexToolResults(ledger, session_id, operation.operation_id, operation.turn_id);
                if (!value) indexed = std::unexpected(value.error());
                else indexed = std::make_shared<const detail::OperationToolResultIndex>(std::move(*value));
            };
            if (restored) capture(*restored);
            else {
                const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
                if (ledger) capture(*ledger);
            }
        }
        std::lock_guard lock(mutex);
        if (!operation.turn_id.empty()) for (const auto& [id, other] : operations) {
            if (id != operation.operation_id && other.turn_id == operation.turn_id) {
                indexed = std::unexpected(Failure("sdk.result.index_invalid", "turn belongs to multiple operations"));
                break;
            }
        }
        tool_results.insert_or_assign(operation.operation_id, std::move(indexed));
    }

    bool SaveMemoryReport(memory::v1::RecallReport report) {
        const auto id = report.operation_id;
        auto saved = memory_module->PersistReport(report);
        std::lock_guard lock(mutex);
        if (!saved) memory_reports.insert_or_assign(id, std::unexpected(saved.error()));
        else memory_reports.insert_or_assign(id, std::move(report));
        return saved.has_value();
    }
    bool EnsureMemoryReport(const Operation& operation) {
        {
            std::lock_guard lock(mutex);
            const auto found = memory_reports.find(operation.operation_id);
            if (found != memory_reports.end()) return found->second.has_value();
        }
        auto report = memory_module->EmptyReport(operation.operation_id, operation.turn_id);
        if (report.enabled) report.state = "not_attempted";
        return SaveMemoryReport(std::move(report));
    }
    Operation Complete(Operation operation, const std::vector<std::string>& refs, bool usage_reported, bool ledger_ok = true) {
        if (!ledger_ok) {
            operation.state = OperationState::Indeterminate;
            operation.error += " sdk.trajectory.persistence_failed";
        }
        // A durable final may only claim completeness after its owned Memory
        // report and the actual adopted input have both been checked.
        bool memory_saved = EnsureMemoryReport(operation);
        std::optional<lubancode::trajectory::v3::V3Ledger> verified_memory;
        if (memory_snapshot.enabled && memory_saved) {
            auto source = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
            auto report = memory_module->ReadReport(operation.operation_id);
            auto valid = source && report ? memory_module->ValidateReport(*report, *source) :
                Result<void>(std::unexpected(Failure("sdk.memory.report_invalid", "owned report or verified input is unavailable")));
            if (valid && report->turn_id != operation.turn_id)
                valid = std::unexpected(Failure("sdk.memory.report_invalid", "operation turn differs"));
            if (valid && operation.state == OperationState::Succeeded && report->state != "no_match" && report->state != "admitted")
                valid = std::unexpected(Failure("sdk.memory.report_invalid", "successful operation has no complete recall result"));
            if (valid) {
                verified_memory = std::move(*source);
                std::lock_guard lock(mutex);
                memory_reports.insert_or_assign(operation.operation_id, std::move(*report));
            } else {
                memory_saved = false;
                std::lock_guard lock(mutex);
                memory_reports.insert_or_assign(operation.operation_id, std::unexpected(valid.error()));
            }
        }
        if (!memory_saved) {
            operation.state = OperationState::Indeterminate;
            operation.error += " sdk.memory.report_persistence_failed";
        }
        auto finalized_writes = memory_write_module->FinalizeOperation(operation.operation_id, operation.turn_id);
        Result<std::vector<memory::v1::SaveReport>> writes = finalized_writes
            ? memory_write_module->ReadReports(operation.operation_id)
            : Result<std::vector<memory::v1::SaveReport>>(std::unexpected(finalized_writes.error()));
        if (writes && memory_write_snapshot.enabled) {
            if (!verified_memory) {
                auto source = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
                if (source) verified_memory = std::move(*source);
            }
            auto valid = verified_memory ? memory_write_module->ValidateReports(
                operation.operation_id, operation.turn_id, *writes, *verified_memory,
                ledger_ok && operation.state != OperationState::Indeterminate && !memory_write_module->HasIndeterminate()) :
                Result<void>(std::unexpected(Failure("sdk.memory_write.report_invalid", "verified input is unavailable")));
            if (!valid) writes = std::unexpected(valid.error());
        }
        const bool writes_saved = writes.has_value();
        const bool write_uncertain = memory_write_module->HasIndeterminate() ||
            (writes && std::any_of(writes->begin(), writes->end(),
                [](const auto& item) { return item.state == "indeterminate"; }));
        if (!writes_saved || write_uncertain) {
            operation.state = OperationState::Indeterminate;
            operation.error += !writes_saved ? " sdk.memory_write.report_persistence_failed" : " sdk.memory_write.indeterminate";
        }
        {
            std::lock_guard lock(mutex);
            memory_saves.insert_or_assign(operation.operation_id, std::move(writes));
            memory_write_indeterminate = memory_write_indeterminate || write_uncertain ||
                (memory_write_snapshot.enabled && !writes_saved);
        }
        Result<std::vector<subagents::v1::Report>> children = std::vector<subagents::v1::Report>{};
        if (subagent_snapshot.enabled && !operation.turn_id.empty()) {
            if (!verified_memory) {
                auto source = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
                if (source) verified_memory = std::move(*source);
            }
            children = verified_memory ? detail::ReadSubagentReports(*verified_memory, session_dir,
                session_id, operation.operation_id, operation.turn_id,
                ledger_ok && operation.state != OperationState::Indeterminate,
                operation.state == OperationState::Cancelled ||
                (operation.state == OperationState::Failed && operation.error == "sdk.turn.limit_reached")) :
                Result<std::vector<subagents::v1::Report>>(std::unexpected(Failure("sdk.subagent.report_unavailable", "verified parent journal unavailable")));
            if (children) for (auto& report : *children) {
                const auto found = live_child_receipts.find({report.parent_action_id, report.parent_attempt});
                if (found != live_child_receipts.end()) report.live_receipt = found->second;
            }
        }
        const bool children_saved = children.has_value();
        if (!children_saved) {
            operation.state = OperationState::Indeterminate;
            operation.error += " sdk.subagent.adoption_unconfirmed";
        }
        {
            std::lock_guard lock(mutex);
            subagent_reports.insert_or_assign(operation.operation_id, std::move(children));
        }
        const bool complete = ledger_ok && memory_saved && writes_saved && children_saved && operation.state != OperationState::Indeterminate;
        const auto directory = session_dir / "sdk-results";
        std::error_code ec;
        fs::create_directories(directory, ec);
        const Json result{{"operationId", operation.operation_id}, {"turnId", operation.turn_id},
                          {"finalText", operation.final_text}, {"error", operation.error}, {"complete", complete}};
        const auto written = lubancode::platform::AtomicWriteFile(directory / (operation.operation_id + ".json"), result.dump(),
            lubancode::platform::WriteDurability::ProcessCrashDurability);
        if (!written) {
            operation.state = OperationState::Indeterminate;
            operation.error += " sdk.result.artifact_failed";
        }
        rt::SessionService::TurnFinalRecord final;
        final.operation_id = operation.operation_id;
        final.turn_id = operation.turn_id;
        final.execution_status = Status(operation.state);
        final.final_message_refs = refs;
        final.usage_reported = usage_reported;
        const bool recorded = service->RecordTurnFinal(final);
        operation.result_persisted = written.has_value() && recorded && complete;
        if (!operation.result_persisted) {
            operation.error += " sdk.result.persistence_failed";
            operation.state = OperationState::Indeterminate;
        }
        // EndTurn and scoped-binding teardown have drained every writer borrower.
        // Freeze identities before making the terminal operation visible. Readers
        // use this snapshot, so another turn may append V3 without a read race.
        CacheToolResults(operation, ledger_ok && recorded, verified_memory ? &*verified_memory : nullptr);
        {
            std::lock_guard lock(mutex);
            operations[operation.operation_id] = operation;
            if (!operation.result_persisted) broken = true;
            active_operation.clear();
            cv.notify_all();
        }
        Event event;
        event.kind = "operation_completed";
        event.operation_id = operation.operation_id;
        event.turn_id = operation.turn_id;
        event.text = operation.final_text;
        event.payload_json = Json{{"status", Status(operation.state)}, {"resultPersisted", operation.result_persisted}, {"error", operation.error}}.dump();
        Emit(std::move(event));
        return operation;
    }

    void Run(const rt::SessionService::QueuedInput& input, bool skip) {
        live_child_receipts.clear();
        Operation operation;
        operation.operation_id = input.operation_id;
        if (skip) { operation.state = OperationState::Cancelled; Complete(std::move(operation), {}, false); return; }
        // Writer seeds this counter from durable V3 facts, including after a
        // process restart. Process-local counters would reuse turn-1 on resume.
        operation.turn_id = service->trajectory()->v3_main_writer()->NewTurnId();
        active_turn_id = operation.turn_id;
        rt::TurnEventAdapter events(session_id, rt::ProcessIdAuthority());
        bool usage_reported = false;
        events.Attach([&](const rt::ServerEvent& source) {
            if (source.kind == rt::ServerEventKind::UsageUpdated && source.payload.contains("reported_by_provider")) {
                usage_reported = usage_reported || source.payload.at("reported_by_provider").get<bool>();
            }
            Event event;
            event.kind = rt::ToString(source.kind);
            event.operation_id = input.operation_id;
            event.turn_id = source.turn_id;
            event.text = source.text;
            event.payload_json = source.to_json().dump();
            Emit(std::move(event));
        });
        auto* dispatcher = extensions ? extensions->dispatcher() : nullptr;
        auto* writer = service->trajectory()->v3_main_writer();
        rt::BindMiddlewareSessionWriter(dispatcher, nullptr, writer);
        if (extensions) extensions->SetOperationScope(input.operation_id);
        const auto middleware_healthy = [dispatcher] {
            if (!dispatcher) return true;
            const auto* sink = dynamic_cast<rt::V3MiddlewareEventSink*>(dispatcher->middleware_sink());
            return sink && sink->recent_errors().empty();
        };
        rt::MiddlewareHookContext hook_context;
        hook_context.turn_id = operation.turn_id;
        hook_context.origin = "human";
        hook_context.purpose = "interactive";
        hook_context.delivery_mode = "direct";
        hook_context.cancel = &interrupt;
        const auto pre = rt::RunPreUserMiddleware(dispatcher, input.text, hook_context);
        if (pre.blocked || !middleware_healthy()) {
            operation.state = interrupt.load() ? OperationState::Cancelled : OperationState::Failed;
            operation.error = pre.blocked ? pre.block_code + ": " + pre.block_reason : "sdk.extension.trajectory_failed";
            // A reserved identity is not a formal admitted turn. No user or
            // TurnStarted event has been committed on a PreUser rejection.
            operation.turn_id.clear();
            active_turn_id.clear();
            Complete(std::move(operation), {}, false, middleware_healthy());
            return;
        }
        events.Start(operation.turn_id);
        lubancode::agent::TurnWiring wiring;
        // This failure belongs only to the accepted current turn. A later Submit
        // receives a new value and cannot inherit another turn's hook failure.
        std::string action_failure;
        std::string action_receipt_error;
        const bool actions_enabled = extensions && extensions->HasActions();
        if (actions_enabled) wiring.action_failure_reason = [&] { return action_failure; };
        if (actions_enabled) wiring.action_receipt_failure_reason = [&, dispatcher] {
            if (!middleware_healthy()) {
                const auto* sink = dynamic_cast<rt::V3MiddlewareEventSink*>(dispatcher->middleware_sink());
                const auto errors = sink ? sink->recent_errors() : std::vector<std::string>{};
                action_receipt_error = "sdk.action.trajectory_failed";
                if (!errors.empty()) action_receipt_error += ": " + errors.front();
            }
            return action_receipt_error;
        };
        wiring.events = &events;
        wiring.turn_id = operation.turn_id;
        wiring.tool_artifact_dir = lubancode::tools::PathToUtf8(session_dir / "artifacts" / "sha256");
        if (rt::HasPreRequestMiddleware(dispatcher) || actions_enabled) {
            wiring.on_pre_request_hooks = [&, dispatcher](const std::string& step_id, const std::string& turn_id,
                const Json& frozen_request, const rt::PreRequestBudget& budget) {
                if (!action_failure.empty()) return action_failure;
                auto context = hook_context;
                context.turn_id = turn_id;
                context.step_id = step_id;
                const auto stages = rt::RunPreRequestMiddleware(dispatcher, frozen_request, budget, context);
                if (!middleware_healthy()) return std::string("sdk.extension.trajectory_failed");
                if (!stages.dispatched || stages.decision == "allow") return std::string();
                return "sdk.extension.request_denied[" + stages.decision + "]: " + stages.reason;
            };
        }
        wiring.on_permission_evaluate = [&](const std::string&, const std::string& name,
            lubancode::tools::ApprovalClass approval_class, const Json& arguments, const rt::ToolHookDecision& pre) {
            auto allowed = approvals.AllowedTools();
            rt::PermissionContext context;
            context.mode = Mode(options.approval_mode);
            context.always_allowed = &allowed;
            return rt::EvaluatePermission(context, pre, approval_class, name, arguments);
        };
        wiring.on_tool_confirm_async = [&](const rt::ApprovalRequest& request) { return AskApproval(request); };
        wiring.on_tool_denial_text = [&](const std::string&, const std::string&) {
            return interrupt.load() ? "sdk.approval.cancelled" : "sdk.approval.declined_or_expired";
        };
        auto bridge = service->trajectory()->NewTurnBridge({"", service->runtime()->wire_name(), "sdk", {}});
        if (!bridge) { operation.state = OperationState::Failed; operation.error = "sdk.trajectory.bridge_unavailable"; Complete(std::move(operation), {}, false); return; }
        rt::ToolTraceHub hub(service->runtime()->ids());
        auto& agent = service->execution()->agent();
        rt::ScopedTurnBindings turn_bindings(agent);
        turn_bindings.Bind(wiring, {.hub = &hub, .trajectory = bridge.get(),
                                   .thread_id = session_id, .turn_id = operation.turn_id});
        // Installed after the scoped snapshot, so Reset revokes this live
        // bridge borrow before the bridge or any writer can close.
        wiring.tool_invocation_identity = [owner = session_id, op = input.operation_id,
            turn = operation.turn_id, actual = bridge.get()](const std::string& provider_call_id)
            -> std::optional<lubancode::tools::ToolInvocationIdentity> {
            const auto identity = actual->V3ExecutingCallIdentity(provider_call_id);
            if (!identity) return std::nullopt;
            return lubancode::tools::ToolInvocationIdentity{owner, op, turn, identity->first, identity->second};
        };
        if (actions_enabled) {
            const auto action_trigger = [&, actual = bridge.get()](const std::string& call, const std::string& name,
                Json arguments, std::optional<std::uint64_t> attempt) -> std::optional<lubancode::hooks::middleware::DispatchTrigger> {
                const auto declared = actual->V3DeclaredCallOrigin(call);
                if (!declared || declared->turn_id != operation.turn_id || declared->action_id.empty() ||
                    session_id.empty() || input.operation_id.empty() || name.empty()) return std::nullopt;
                lubancode::hooks::middleware::DispatchTrigger trigger;
                trigger.input = { {"arguments", std::move(arguments)} };
                trigger.turn_id = declared->turn_id; trigger.step_id = declared->step_id;
                trigger.action_id = declared->action_id;
                trigger.origin = hook_context.origin; trigger.purpose = hook_context.purpose;
                trigger.delivery_mode = hook_context.delivery_mode; trigger.cancel = &interrupt;
                trigger.action_scope = lubancode::hooks::middleware::ActionScope{
                    session_id, input.operation_id, call, name, options.cwd, attempt};
                return trigger;
            };
            wiring.on_pre_action = [&, action_trigger](const std::string& call, const std::string& name, const Json& arguments) {
                lubancode::agent::TurnWiring::ActionPreDecision rejected;
                if (!action_failure.empty()) {
                    rejected.failed = true; rejected.error_code = "sdk.action.previous_failure";
                    rejected.hook.decision = rt::ToolHookDecision::Decision::Deny;
                    rejected.hook.reason = action_failure;
                    return rejected;
                }
                auto trigger = action_trigger(call, name, arguments, std::nullopt);
                if (!trigger) {
                    action_failure = "sdk.action.owner_invalid: actual accepted main declaration is required";
                    rejected.failed = true; rejected.error_code = "sdk.action.owner_invalid";
                    rejected.hook.decision = rt::ToolHookDecision::Decision::Deny;
                    rejected.hook.reason = action_failure;
                    return rejected;
                }
                auto result = detail::RunPreAction(*dispatcher, *trigger);
                if (result.failed) action_failure = result.hook.reason;
                return result;
            };
            wiring.on_post_action = [&, action_trigger, actual = bridge.get()](const std::string& call, const std::string& name,
                const Json& arguments, const lubancode::tools::Tool::Result& original,
                const lubancode::tools::ToolInvocationIdentity& identity,
                const lubancode::agent::ToolTraceEvent& started, const lubancode::agent::ToolTraceEvent& finished) {
                // Finished is no longer an active execution in the bridge. Use
                // the snapshot actually issued by MarkExecutionStarted, not a
                // second active-identity query after its terminal event.
                const auto declared = actual->V3DeclaredCallOrigin(call);
                if (!declared || identity.session_id != session_id || identity.operation_id != input.operation_id ||
                    identity.turn_id != operation.turn_id || identity.action_id != declared->action_id || identity.attempt == 0 ||
                    started.kind != lubancode::agent::ToolTraceEventKind::ExecutionStarted ||
                    finished.kind != lubancode::agent::ToolTraceEventKind::ExecutionFinished ||
                    started.execution_id.empty() || finished.execution_id != started.execution_id ||
                    started.tool_use_id != call || finished.tool_use_id != call ||
                    started.tool_name != name || finished.tool_name != name ||
                    started.turn_id != operation.turn_id || finished.turn_id != operation.turn_id ||
                    (!started.thread_id.empty() && started.thread_id != session_id) ||
                    (!finished.thread_id.empty() && finished.thread_id != session_id)) {
                    action_failure = "sdk.action.owner_invalid: actual finished main execution is required";
                    return original;
                }
                auto trigger = action_trigger(call, name, arguments, identity.attempt);
                if (!trigger || trigger->action_id != identity.action_id) {
                    action_failure = "sdk.action.owner_invalid: declaration and execution differ";
                    return original;
                }
                trigger->input["result"] = {{"text", original.content}, {"isError", original.is_error},
                    {"outcome", lubancode::agent::ToString(finished.outcome)}, {"errorCode", finished.error_code}};
                auto result = detail::RunPostAction(*dispatcher, *trigger, original);
                if (!result) { action_failure = result.error(); return original; }
                return std::move(*result);
            };
        }
        // The public worker supplies an actual main execution identity. Neither
        // a naked tool call nor model input can manufacture this TLS admission.
        lubancode::tools::ScopedDispatchIdentity main_identity(
            lubancode::tools::AgentRunIdentity{0, 0, 0, writer->run_id()});
        std::map<std::string, std::pair<std::string, std::uint64_t>> child_calls;
        struct ChildTurnScope {
            detail::SessionSubagents* owner;
            ~ChildTurnScope() { if (owner) owner->ClearTurn(); }
        } child_turn_scope{subagent_module};
        if (subagent_module) {
            lubancode::tools::AgentTool::Hooks hooks;
            hooks.on_child_permission_evaluate = [this](const rt::ChildApprovalScope& scope,
                const rt::ToolHookDecision& pre, lubancode::tools::ApprovalClass kind,
                const std::string& name, const Json& arguments) {
                rt::PermissionContext context;
                context.mode = Mode(options.approval_mode);
                if (scope.permission_floor != "inherit") {
                    const auto floor = lubancode::ParseApprovalMode(scope.permission_floor);
                    if (!floor) return rt::PermissionVerdict{rt::PermissionVerdict::Action::Deny,
                        rt::PermissionVerdict::Reason::NoPrompt, true};
                    context.mode = lubancode::StricterApprovalMode(context.mode, *floor);
                }
                // Deliberately omit the parent's ordinary AllowedTools. The
                // child-only grant is checked by AgentTool after hook decisions.
                return rt::EvaluatePermission(context, pre, kind, name, arguments);
            };
            hooks.on_child_tool_confirm_scoped = [this](const rt::ChildApprovalRequest& request) {
                Approval approval;
                approval.request_id = session_id + ":" + active_turn_id + ":" +
                    service->runtime()->ids().NextPrefixedId("sdk-child-approval");
                approval.operation_id = active_operation;
                approval.tool_call_id = request.request.tool_use_id;
                approval.tool_name = request.request.tool_name; approval.input_json = request.request.input.dump();
                approval.cwd = request.scope.effective_cwd; approval.reason = request.request.reason;
                const auto& s = request.scope;
                approval.child = subagents::v1::ApprovalScope{s.host_session_id, s.host_run_id, s.host_operation_id,
                    s.parent_session_id, s.parent_run_id, s.child_session_id, s.child_run_id,
                    s.effective_cwd, s.permission_floor, request.child_turn_id,
                    request.child_declared_action_id, request.child_declared_message_id};
                auto admitted = approvals.RegisterChildScoped(request, std::move(approval), options.approval_timeout,
                    [this](const Approval& ticket, const rt::ChildApprovalRequest&) {
                        Event event; event.kind = "approval_requested";
                        event.operation_id = ticket.operation_id; event.approval = ticket;
                        Emit(std::move(event));
                    });
                return admitted ? std::move(*admitted) : rt::ApprovalLease{};
            };
            hooks.on_child_tool_granted = [this](const auto& scope, const auto& name) {
                return approvals.ChildAllowed(scope, name);
            };
            hooks.on_child_approval_scope_closed = [this](const auto& scope) { approvals.CloseChildScope(scope); };
            hooks.on_pre_tool_use_hook = wiring.on_pre_tool_use_hook;
            hooks.on_permission_request = wiring.on_permission_request;
            hooks.on_post_tool_use_hook = wiring.on_post_tool_use_hook;
            hooks.trajectory_spawn = [&, actual = bridge.get()](const std::string& label,
                const std::string& parent_run, rt::SubagentSpawnFailure* failure_out, rt::SubagentDispatchMode mode) {
                const auto refuse = [&](std::string why) -> std::unique_ptr<rt::TrajectorySubagentBridge> {
                    if (failure_out) *failure_out = {"parent_admission", "sdk.subagent.spawn_rejected", std::move(why), {}, false, {}};
                    return {};
                };
                const auto call = hub.current_agent_call_id();
                const auto identity = actual->V3ExecutingCallIdentity(call);
                if (mode != rt::SubagentDispatchMode::Foreground || parent_run != writer->run_id() || call.empty() || !identity)
                    return refuse("actual main started action is required");
                auto child = service->trajectory()->SpawnSubagent(call, label, parent_run);
                if (!child) {
                    if (failure_out) *failure_out = child.error();
                    service->trajectory()->NoteSubagentStartFailed(child.error(), {}, call, operation.turn_id);
                    return std::unique_ptr<rt::TrajectorySubagentBridge>{};
                }
                const auto attached = actual->AttachChildRun(call, (*child)->run_id(), (*child)->ParentSpawn());
                if (!attached) {
                    const auto receipt = (*child)->Finish(rt::SubagentExecutionOutcome::StartupRejected,
                        "sdk.subagent.parent_link_failed");
                    actual->NoteChildTerminal(receipt);
                    rt::SubagentSpawnFailure failure{"parent_link", "sdk.subagent.parent_link_failed",
                        attached.error(), (*child)->run_id(), false, receipt};
                    service->trajectory()->NoteSubagentStartFailed(failure, {}, call, operation.turn_id);
                    if (failure_out) *failure_out = std::move(failure);
                    return std::unique_ptr<rt::TrajectorySubagentBridge>{};
                }
                child_calls.emplace((*child)->run_id(), *identity);
                return std::move(*child);
            };
            hooks.trajectory_child_finished = [&, actual = bridge.get()](const rt::SubagentTerminalReceipt& receipt) {
                actual->NoteChildTerminal(receipt);
                const auto owner = child_calls.find(receipt.run_id);
                if (owner != child_calls.end()) live_child_receipts.insert_or_assign(owner->second, detail::CopyChildReceipt(receipt));
            };
            subagent_module->BindTurn(std::move(hooks), session_id, input.operation_id, operation.turn_id);
        }
        api::Message message;
        message.role = api::Role::User;
        message.content.push_back(api::TextBlock{pre.prompt});
        bridge->BeginTurn(operation.turn_id, "external_user");
        bridge->RecordInput(message);
        const auto history_before = agent.history().size();
        std::expected<lubancode::agent::RunOutcome, std::string> outcome =
            std::unexpected("sdk.extension.input_admission_failed");
        bool context_healthy = true;
        auto recall = memory_module->BuildRecall(pre.prompt, input.operation_id, operation.turn_id);
        if (!recall) {
            // RecordInput already adopted the human message. Retain that same
            // message in the Agent even though this request cannot call a model.
            try { agent.AppendAdmittedMessage(std::move(message)); }
            catch (...) { service->trajectory()->BlockV3Execution("memory.recall.in_memory_admission_failed"); }
            auto failed = memory_module->EmptyReport(input.operation_id, operation.turn_id);
            failed.state = "failed";
            failed.error = recall.error().code + ": " + recall.error().message;
            const auto saved = SaveMemoryReport(std::move(failed));
            outcome = std::unexpected(saved ? recall.error().code + ": " + recall.error().message
                                            : "sdk.memory.report_write_failed");
        } else if (!dispatcher && recall->context.empty()) {
            // Zero registrations preserve the established host path byte for byte.
            const auto saved = SaveMemoryReport(std::move(recall->report));
            if (saved) outcome = agent.Run(std::move(message), wiring, &interrupt);
            else outcome = std::unexpected("sdk.memory.report_write_failed");
        } else if (bridge->recent_errors().empty()) {
            try { agent.AppendAdmittedMessage(std::move(message)); }
            catch (...) { context_healthy = false; }
            const auto append_context = [&](const std::vector<std::string>& appends, const char* point) {
                namespace v3 = lubancode::trajectory::v3;
                for (const auto& text : appends) {
                    const std::string body = "[" + std::string(point) + " 钩子附加上下文,非用户手敲]\n" + text;
                    api::Message admitted;
                    admitted.role = api::Role::User;
                    admitted.content.push_back(api::TextBlock{body});
                    v3::MessageDraft draft;
                    draft.turn_id = operation.turn_id;
                    draft.origin = v3::MessageOrigin::Hook;
                    draft.display = v3::DisplayMode::Hidden;
                    draft.message = Json{{"role", "user"}, {"content", body}};
                    const auto stored = writer->AppendMessage(std::move(draft), v3::Durability::ProcessCrash);
                    if (stored.status != v3::WriteReceipt::Status::Committed) return false;
                    const auto accepted = writer->AdmitMessages({stored.id}, v3::Durability::ProcessCrash);
                    if (accepted.status != v3::WriteReceipt::Status::Committed) return false;
                    try { agent.AppendAdmittedMessage(std::move(admitted)); }
                    catch (...) { return false; }
                }
                return true;
            };
            if (context_healthy && !recall->context.empty()) {
                rt::MemoryLedgerBridge memory_bridge(*service->trajectory());
                auto admitted = memory_bridge.AdmitRecallContext(recall->context, recall->records, operation.turn_id);
                if (!admitted) {
                    recall->report.state = "failed";
                    recall->report.error = admitted.error();
                    context_healthy = false;
                    outcome = std::unexpected(admitted.error());
                } else {
                    recall->report.state = admitted->error.empty() ? "admitted" : "failed";
                    recall->report.context_message_id = admitted->message_id;
                    if (!admitted->error.empty()) {
                        recall->report.error = admitted->error;
                        context_healthy = false;
                        outcome = std::unexpected(admitted->error);
                    }
                    try { if (context_healthy) agent.AppendAdmittedMessage(std::move(admitted->message)); }
                    catch (...) {
                        service->trajectory()->BlockV3Execution("memory.recall.in_memory_admission_failed");
                        recall->report.state = "failed";
                        recall->report.error = "memory.recall.in_memory_admission_failed";
                        context_healthy = false;
                    }
                }
            }
            if (!context_healthy && recall->report.context_message_id.empty() && recall->report.state != "failed") {
                recall->report.state = "failed";
                recall->report.error = "sdk.memory.input_admission_failed";
            }
            if (!SaveMemoryReport(std::move(recall->report))) {
                service->trajectory()->BlockV3Execution("memory.recall.report_write_failed");
                context_healthy = false;
                outcome = std::unexpected("sdk.memory.report_write_failed");
            }
            if (context_healthy) context_healthy = append_context(pre.additional_context, "PreUser");
            if (context_healthy) {
                const auto post = rt::RunPostUserMiddleware(dispatcher, pre.prompt, hook_context);
                if (post.blocked) outcome = std::unexpected(post.block_code + ": " + post.block_reason);
                else if (middleware_healthy()) {
                    context_healthy = append_context(post.context_appends, "PostUser");
                    if (context_healthy) outcome = agent.RunAdmittedHistory(wiring, &interrupt);
                }
            }
        }
        if (!action_failure.empty() && !(outcome && outcome->side_effect_indeterminate))
            outcome = std::unexpected(action_failure);
        const bool turn_cancelled = (outcome && outcome->cancelled) || (!outcome && interrupt.load());
        const bool write_uncertain = memory_write_module->HasIndeterminate();
        const bool effect_uncertain = (outcome && outcome->side_effect_indeterminate) || !action_receipt_error.empty();
        const bool uncertain = write_uncertain || effect_uncertain;
        std::string uncertain_error = write_uncertain ? "sdk.memory_write.indeterminate" : "sdk.side_effect.indeterminate";
        if (effect_uncertain && outcome && !outcome->side_effect_error.empty()) uncertain_error += ": " + outcome->side_effect_error;
        else if (!action_receipt_error.empty()) uncertain_error += ": " + action_receipt_error;
        bridge->EndTurn(outcome.has_value() && !uncertain, turn_cancelled && !uncertain,
            uncertain ? uncertain_error : outcome ? "" : outcome.error());
        if (subagent_module) subagent_module->ClearTurn();
        child_turn_scope.owner = nullptr;
        turn_bindings.Reset();
        operation.state = uncertain ? OperationState::Indeterminate : turn_cancelled ? OperationState::Cancelled :
                          !outcome ? OperationState::Failed : OperationState::Succeeded;
        if (uncertain) operation.error = std::move(uncertain_error);
        else if (!outcome) operation.error = outcome.error();
        else if (outcome->hit_step_limit || outcome->hit_time_budget || outcome->hit_token_budget || outcome->hit_turn_limit) {
            operation.state = OperationState::Failed;
            operation.error = "sdk.turn.limit_reached";
        }
        const auto& history = agent.history();
        if (history.size() > history_before && history.back().role == api::Role::Assistant) {
            for (const auto& block : history.back().content) if (auto* text = std::get_if<api::TextBlock>(&block)) operation.final_text += text->text;
        }
        std::vector<std::string> refs;
        if (!operation.final_text.empty() && !bridge->last_committed_assistant_message_id().empty()) {
            refs.push_back(bridge->last_committed_assistant_message_id());
        }
        // Durable operation final precedes its public completion notification.
        operation = Complete(operation, refs, usage_reported,
                             bridge->recent_errors().empty() && context_healthy && middleware_healthy());
        events.Finish(operation.state == OperationState::Succeeded ? rt::Outcome::Succeeded :
                      operation.state == OperationState::Cancelled ? rt::Outcome::Cancelled : rt::Outcome::Failed,
                      operation.error);
    }

    void Pump() {
        struct WorkerScope {
            WorkerScope() { in_session_worker = true; }
            ~WorkerScope() { in_session_worker = false; }
        } worker_scope;
        for (;;) {
            rt::SessionService::PendingPop pop;
            bool skip = false;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return closing || broken || memory_write_indeterminate || service->pending_input_count() > 0; });
                if (broken || memory_write_indeterminate || (closing && service->pending_input_count() == 0)) break;
                pop = service->PopPendingInput();
                if (pop.status == rt::SessionService::PendingPop::Status::WriteFailed) { broken = true; cv.notify_all(); break; }
                if (pop.status != rt::SessionService::PendingPop::Status::Ok) continue;
                active_operation = pop.input.operation_id;
                const auto* writer = service->trajectory()->v3_main_writer();
                approvals.SetOperationOwner(session_id, active_operation, writer ? writer->run_id() : std::string());
                active_turn_id.clear();
                skip = closing || cancelled.contains(active_operation);
                interrupt.store(skip);
                operations[active_operation].state = OperationState::Running;
            }
            try { Run(pop.input, skip); }
            catch (const std::exception& error) {
                Operation failed{pop.input.operation_id, active_turn_id, OperationState::Failed, {}, error.what(), false};
                Complete(std::move(failed), {}, false);
            } catch (...) {
                Operation failed{pop.input.operation_id, active_turn_id, OperationState::Failed, {}, "sdk.turn.exception", false};
                Complete(std::move(failed), {}, false);
            }
            CancelApprovals();
        }
        std::lock_guard lock(mutex);
        if (broken || memory_write_indeterminate) for (auto& [id, operation] : operations) {
            (void)id;
            if (!Terminal(operation.state)) {
                operation.state = OperationState::Indeterminate;
                operation.error = memory_write_indeterminate ? "sdk.memory_write.indeterminate" : "sdk.storage.broken";
            }
        }
        cv.notify_all();
    }

    void RequestClose() {
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            closing = true;
            interrupt.store(true);
            cv.notify_all();
        }
        approvals.Close();
        if (event_delivery) event_delivery->BeginClose();
    }
    void RequestExecutionShutdown() {
        std::shared_ptr<rt::SessionService> service_to_stop;
        detail::SessionSubagents* children_to_stop = nullptr;
        struct SignalScope {
            Impl& owner;
            std::shared_ptr<rt::SessionService>& service;
            ~SignalScope() {
                if (service == nullptr) return;
                service.reset();
                std::lock_guard lock(owner.mutex);
                --owner.close_signals_inflight;
                owner.cv.notify_all();
            }
        } signal_scope{*this, service_to_stop};
        {
            std::lock_guard lock(mutex);
            if (closed) return;
            service_to_stop = service;
            if (service_to_stop != nullptr) {
                ++close_signals_inflight;
                children_to_stop = subagent_module;
            }
        }
        if (service_to_stop != nullptr) {
            // The service snapshot pins the attachment. No coordinator work or
            // callback retirement occurs under the public API mutex.
            if (children_to_stop) children_to_stop->RequestClose();
            service_to_stop->RequestExecutionShutdown();
        }
    }
    Result<void> Close() {
        if (in_session_worker || detail::InEventProvider() || detail::InMemoryBlobProvider()) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
        std::lock_guard close_lock(close_mutex);
        {
            std::lock_guard lock(mutex);
            if (closed) return close_error ? Result<void>(std::unexpected(*close_error)) : Result<void>{};
        }
        RequestClose();
        RequestExecutionShutdown();
        if (worker.joinable()) worker.join();
        if (service && service->runtime()) {
            const auto outcome = service->Close("sdk_close");
            if (!outcome.error_code.empty()) close_error = Failure(outcome.error_code, outcome.message);
        }
        // SessionService destruction also releases operations.jsonl. Keep query
        // projections, not the live writer, after Close (Windows delete/rename).
        std::shared_ptr<rt::SessionService> closed_service;
        std::vector<std::shared_ptr<EventStream::Impl>> streams;
        {
            std::unique_lock lock(mutex);
            closed = true;
            // A concurrent signal cannot keep a writer alive after Close
            // returns. Close admission first, then wait for its snapshot back.
            cv.wait(lock, [&] { return close_signals_inflight == 0; });
            closed_service = std::move(service);
            extensions = nullptr;
            subagent_module = nullptr;
            for (auto& weak : subscriptions) if (auto stream = weak.lock()) streams.push_back(std::move(stream));
            subscriptions.clear();
            cv.notify_all();
        }
        {
            // Even a moved std::function can retain an inline callable in the
            // SDK-owned SessionOptions. Clear those sources before destroying
            // the Agent/registry/backend they may borrow. Nonblocking queries
            // may reenter here; blocking lifecycle calls must not wait on this
            // Close's serialization mutex from a capture destructor.
            struct CleanupScope {
                bool previous = in_session_worker;
                CleanupScope() { in_session_worker = true; }
                ~CleanupScope() { in_session_worker = previous; }
            } cleanup_scope;
            options.custom_tools.clear();
            options.extensions.clear();
            // Agent -> registry/MCP/backend -> ledger, outside the API mutex.
            closed_service.reset();
            // Preflight/control-block failure may leave the original public
            // backend in options. Retire it under the same lifecycle guard,
            // after every tool/Agent, rather than during Impl member teardown.
            options.backend.reset();
            memory_module.reset();
            memory_write_module.reset();
            memory_blob_factory.reset();
        }
        for (auto& stream : streams) (void)stream->CloseChecked();
        if (event_delivery) {
            auto closed_delivery = event_delivery->CloseAndWait();
            if (!closed_delivery && !close_error) close_error = closed_delivery.error();
        }
        if (close_error && close_errors) close_errors->Remember(*close_error);
        return close_error ? Result<void>(std::unexpected(*close_error)) : Result<void>{};
    }
};

Session::Session(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Session::~Session() { (void)impl_->Close(); }
std::string Session::id() const { return impl_->session_id; }
Result<void> Session::Close() { return impl_->Close(); }
Result<std::string> Session::DescribeExtensions() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->extension_plan_json;
}
Result<skills::v1::Snapshot> Session::DescribeSkills() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->skills_snapshot;
}
Result<subagents::v1::Snapshot> Session::DescribeSubagents() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->subagent_snapshot;
}
Result<std::vector<subagents::v1::Report>> Session::GetSubagentReports(const std::string& operation_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto operation = impl_->operations.find(operation_id);
    if (operation == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    if (!Terminal(operation->second.state)) return std::unexpected(Failure("sdk.subagent.report_not_ready"));
    const auto found = impl_->subagent_reports.find(operation_id);
    if (found == impl_->subagent_reports.end()) return std::unexpected(Failure("sdk.subagent.report_not_ready"));
    return found->second;
}
Result<memory::v1::Snapshot> Session::DescribeMemory() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->memory_snapshot;
}
Result<memory::v1::RecallReport> Session::GetMemoryRecall(const std::string& operation_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto operation = impl_->operations.find(operation_id);
    if (operation == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    if (!Terminal(operation->second.state)) return std::unexpected(Failure("sdk.memory.report_not_ready"));
    const auto found = impl_->memory_reports.find(operation_id);
    if (found == impl_->memory_reports.end()) return std::unexpected(Failure("sdk.memory.report_not_ready"));
    return found->second;
}
Result<memory::v1::WriteSnapshot> Session::DescribeMemoryWrite() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->memory_write_snapshot;
}
Result<lua::v1::Snapshot> Session::DescribeLua() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->lua_snapshot;
}
Result<std::vector<memory::v1::SaveReport>> Session::GetMemorySaves(const std::string& operation_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto operation = impl_->operations.find(operation_id);
    if (operation == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    if (!Terminal(operation->second.state)) return std::unexpected(Failure("sdk.memory_write.report_not_ready"));
    const auto found = impl_->memory_saves.find(operation_id);
    if (found == impl_->memory_saves.end()) return std::unexpected(Failure("sdk.memory_write.report_not_ready"));
    return found->second;
}
Result<Receipt> Session::Submit(std::string key, std::string text) {
    if (key.empty()) return std::unexpected(Failure("sdk.operation.key_required"));
    if (!lubancode::platform::IsValidUtf8(key) || !lubancode::platform::IsValidUtf8(text)) {
        return std::unexpected(Failure("sdk.input.invalid_utf8"));
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->closed || impl_->runtime_stopping->load()) return std::unexpected(Failure("sdk.session.closed"));
    if (impl_->memory_write_indeterminate) return std::unexpected(Failure("sdk.memory_write.indeterminate",
        "A previous project write needs explicit inspection; this Session cannot continue automatically"));
    if (impl_->broken) return std::unexpected(Failure("sdk.storage.broken"));
    auto receipt = impl_->service->SubmitInput({std::move(key), std::move(text), {}});
    if (!receipt.accepted && !receipt.duplicate) return std::unexpected(Failure(receipt.error_code));
    if (receipt.accepted) impl_->operations.emplace(receipt.operation_id, Operation{receipt.operation_id});
    impl_->cv.notify_all();
    return Receipt{receipt.operation_id, receipt.input_id, receipt.duplicate};
}
Result<std::shared_ptr<EventStream>> Session::Subscribe(std::size_t capacity) {
    if (capacity == 0 || capacity > 65536) return std::unexpected(Failure("sdk.events.invalid_capacity"));
    if (detail::InEventProvider()) return std::unexpected(Failure("sdk.events.reentrant"));
    std::shared_ptr<detail::EventDeliveryOwner> owner;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closing || impl_->closed || impl_->runtime_stopping->load()) return std::unexpected(Failure("sdk.session.closed"));
        owner = impl_->event_delivery;
    }
    // Actual factory runs without the API lock. Owner admission pins its sink;
    // both closing gates are checked before publishing a subscription.
    auto created = owner->CreateQueue(capacity);
    if (!created) return std::unexpected(created.error());
    auto state = std::make_shared<EventStream::Impl>(std::move(*created));
    auto stream = std::shared_ptr<EventStream>(new EventStream(state));
    bool late;
    {
        std::lock_guard lock(impl_->mutex);
        late = impl_->closing || impl_->closed || impl_->runtime_stopping->load();
        if (!late) impl_->subscriptions.push_back(state);
    }
    if (late) { (void)state->CloseChecked(); return std::unexpected(Failure("sdk.session.closed")); }
    return stream;
}
std::vector<Approval> Session::PendingApprovals() const {
    return impl_->approvals.Pending();
}
Result<void> Session::ResolveApproval(std::string id, ApprovalDecision decision, std::string reason) {
    if (!impl_->ResolveApproval({std::move(id)}, {Decision(decision), std::move(reason)})) return std::unexpected(Failure("stale_request_id"));
    return {};
}
Result<void> Session::Cancel(std::string id) {
    {
        std::lock_guard lock(impl_->mutex);
        auto it = impl_->operations.find(id);
        if (it == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
        if (Terminal(it->second.state)) return std::unexpected(Failure("sdk.operation.already_terminal"));
        impl_->cancelled.insert(id);
        if (impl_->active_operation != id) return {};
        impl_->interrupt.store(true);
        // Keep the operation gate until its approvals are cancelled, so the
        // next turn cannot register a request in the cancellation window.
        impl_->CancelApprovals();
    }
    return {};
}
Result<Operation> Session::ReadOperation(std::string id) const {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->operations.find(id);
    if (it == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    return it->second;
}
Result<Operation> Session::WaitResult(std::string id, std::chrono::milliseconds timeout) const {
    if (timeout.count() < 0) return std::unexpected(Failure("sdk.timeout.invalid"));
    if (in_session_worker || detail::InEventProvider() || detail::InMemoryBlobProvider()) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->operations.find(id);
    if (it == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    if (!impl_->cv.wait_for(lock, timeout, [&] { return Terminal(it->second.state); })) return std::unexpected(Failure("sdk.wait.timeout"));
    return it->second;
}
Result<std::vector<results::v1::ToolResultSummary>> Session::ListToolResults(std::string id) const {
    std::shared_ptr<const detail::OperationToolResultIndex> index;
    {
        std::lock_guard lock(impl_->mutex);
        const auto operation = impl_->operations.find(id);
        if (operation == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
        if (!Terminal(operation->second.state)) return std::unexpected(Failure("sdk.result.not_ready"));
        const auto saved = impl_->tool_results.find(id);
        if (saved == impl_->tool_results.end()) return std::unexpected(Failure("sdk.result.index_unavailable"));
        if (!saved->second) return std::unexpected(saved->second.error());
        index = *saved->second;
    }
    std::vector<results::v1::ToolResultSummary> summaries;
    summaries.reserve(index->entries.size());
    for (const auto& entry : index->entries) summaries.push_back(entry.summary);
    return summaries;
}
Result<results::v1::SavedSnapshot> Session::ReadToolResult(
    results::v1::ToolResultIdentity identity, results::v1::ToolResultReadOptions options) const {
    std::shared_ptr<const detail::OperationToolResultIndex> index;
    results::v1::SessionResultPolicy policy;
    fs::path directory;
    {
        std::lock_guard lock(impl_->mutex);
        if (identity.session_id != impl_->session_id) return std::unexpected(Failure("sdk.result.identity_mismatch"));
        const auto operation = impl_->operations.find(identity.operation_id);
        if (operation == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
        if (!Terminal(operation->second.state)) return std::unexpected(Failure("sdk.result.not_ready"));
        const auto saved = impl_->tool_results.find(identity.operation_id);
        if (saved == impl_->tool_results.end()) return std::unexpected(Failure("sdk.result.index_unavailable"));
        if (!saved->second) return std::unexpected(saved->second.error());
        index = *saved->second;
        policy = impl_->result_policy;
        directory = impl_->session_dir;
    }
    const auto entry = std::find_if(index->entries.begin(), index->entries.end(),
        [&](const auto& result) { return result.summary.identity == identity; });
    if (entry == index->entries.end()) return std::unexpected(Failure("sdk.result.identity_mismatch"));
    return detail::ReadIndexedToolResult(directory, *entry, policy, options);
}

struct Runtime::Impl {
    RuntimeOptions options;
    std::mutex mutex;
    bool closed = false;
    std::shared_ptr<std::atomic<bool>> stopping = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::weak_ptr<Session::Impl>> sessions;
    std::shared_ptr<CloseErrors> close_errors = std::make_shared<CloseErrors>();
};
Runtime::Runtime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Runtime::~Runtime() { (void)Shutdown(); }
Result<std::unique_ptr<Runtime>> Runtime::Create(RuntimeOptions options) {
    try {
        auto data = AbsoluteDirectory(options.data_root, true);
        if (!data) return std::unexpected(data.error());
        auto resources = AbsoluteDirectory(options.resource_root, false);
        if (!resources) return std::unexpected(resources.error());
        options.data_root = lubancode::tools::PathToUtf8(*data);
        options.resource_root = lubancode::tools::PathToUtf8(*resources);
        auto impl = std::make_unique<Impl>();
        impl->options = std::move(options);
        return std::unique_ptr<Runtime>(new Runtime(std::move(impl)));
    } catch (const std::exception& error) { return std::unexpected(Failure("sdk.runtime.create_failed", error.what())); }
}
Result<std::shared_ptr<Session>> Runtime::OpenSession(SessionOptions options) {
    if (in_session_worker || detail::InEventProvider() || detail::InMemoryBlobProvider()) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
    try {
        // Wrap the provider before any rejected opening can retire its captures.
        // Its actual Open still runs under the original Runtime/session locks.
        auto memory_factory = detail::MakeMemoryBlobFactory(std::move(options.memory_blob_provider));
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return std::unexpected(Failure("sdk.runtime.closed"));
        std::erase_if(impl_->sessions, [](const auto& session) { return session.expired(); });
        auto execution = std::make_shared<Session::Impl>();
        execution->roots = impl_->options;
        execution->runtime_stopping = impl_->stopping;
        execution->event_delivery = std::make_shared<detail::EventDeliveryOwner>(std::move(options.event_sink));
        execution->memory_blob_factory = std::move(memory_factory);
        execution->options = std::move(options);
        auto opened = execution->Initialize();
        if (!opened) return std::unexpected(opened.error());
        execution->close_errors = impl_->close_errors;
        impl_->sessions.push_back(execution);
        execution->Start();
        return std::shared_ptr<Session>(new Session(std::move(execution)));
    } catch (const std::exception& error) { return std::unexpected(Failure("sdk.session.open_failed", error.what())); }
}
Result<void> Runtime::Shutdown() {
    // Also reject cross-session/runtime blocking lifecycle calls from a tool or
    // backend callback: two workers closing each other must not form a join cycle.
    if (in_session_worker || detail::InEventProvider() || detail::InMemoryBlobProvider()) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
    std::vector<std::shared_ptr<Session::Impl>> sessions;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& session : impl_->sessions)
            if (auto live = session.lock()) sessions.push_back(std::move(live));
        impl_->closed = true;
        impl_->stopping->store(true);
    }
    // Stop admission and signal every worker before waiting for any one worker.
    for (const auto& session : sessions) session->RequestClose();
    // Approval futures across all sessions must be awake before acquiring any
    // coordinator's publication/jobs mutex to signal its background workers.
    for (const auto& session : sessions) session->RequestExecutionShutdown();
    for (const auto& session : sessions) (void)session->Close();
    // Keep the registry until every join finishes. A concurrent Shutdown must
    // take the same live snapshot, rather than return while workers still run.
    { std::lock_guard lock(impl_->mutex); impl_->sessions.clear(); }
    return impl_->close_errors->Read();
}
std::string Version() { return LUBANCORE_VERSION; }
} // namespace lubancore
