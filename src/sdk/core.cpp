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
#include "sdk/callback_scope.hpp"
#include "sdk/extensions.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace lubancore {
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
bool Terminal(OperationState state) { return state != OperationState::Accepted && state != OperationState::Running; }
bool ValidId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
Result<void> ValidateOperationLedger(const fs::path& session_dir) {
    // The shared live-query reader deliberately skips incomplete lines. Recovery
    // cannot do that: losing an accepted row also loses its dedupe key and the
    // operation counter, permitting both repeated effects and reused IDs.
    const auto path = session_dir / "operations.jsonl";
    const auto invalid = [](std::string reason) -> Result<void> {
        return std::unexpected(Failure("sdk.resume.operation_ledger_invalid", std::move(reason)));
    };
    enum class Stage { Accepted, Dispatched, Final };
    std::map<std::string, Stage> stages;
    const auto check_result_coverage = [&]() -> Result<void> {
        std::error_code result_error;
        const auto results = session_dir / "sdk-results";
        const bool exists = fs::exists(results, result_error);
        if (result_error) return invalid("cannot inspect SDK result directory");
        if (!exists) return {};
        fs::directory_iterator entries(results, result_error);
        if (result_error) return invalid("cannot read SDK result directory");
        for (; entries != fs::directory_iterator{}; entries.increment(result_error)) {
            if (result_error) return invalid("cannot read SDK result directory");
            if (entries->path().extension() != ".json") continue;
            const auto id = lubancode::tools::PathToUtf8(entries->path().stem());
            const auto fact = stages.find(id);
            // Result bytes are written before operation.final, so Dispatched is
            // valid during crash recovery. Missing acceptance/dispatch is not.
            if (fact == stages.end() || fact->second == Stage::Accepted) {
                return invalid("SDK result has no preceding operation dispatch");
            }
        }
        if (result_error) return invalid("cannot read SDK result directory");
        return {};
    };
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) return invalid("cannot inspect operation ledger");
    if (!exists) return check_result_coverage(); // new/empty sessions have a lazy ledger
    if (!fs::is_regular_file(path, ec) || ec) return invalid("operation ledger is not a regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input) return invalid("cannot read operation ledger");
    std::set<std::string> keys;
    const auto is_string = [](const Json& row, const char* key) {
        return row.contains(key) && row.at(key).is_string();
    };
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || input.eof()) return invalid("incomplete operation ledger line");
        const auto row = Json::parse(line, nullptr, false);
        if (!row.is_object() || !row.contains("schemaVersion") || !row.at("schemaVersion").is_number_integer() ||
            (row.at("schemaVersion") != 1 && row.at("schemaVersion") != 2) ||
            !is_string(row, "kind") || !is_string(row, "operationId")) {
            return invalid("invalid operation fact shape");
        }
        const auto id = row.at("operationId").get<std::string>();
        const auto kind = row.at("kind").get<std::string>();
        if (!ValidId(id)) return invalid("invalid operation ID");
        const auto found = stages.find(id);
        if (kind == "operation.accepted") {
            if (found != stages.end() || !is_string(row, "inputId") || row.at("inputId") == "" ||
                !is_string(row, "clientOperationId") || !is_string(row, "payloadHash")) {
                return invalid("invalid or duplicate operation acceptance");
            }
            const auto key = row.at("clientOperationId").get<std::string>();
            const auto hash = row.at("payloadHash").get<std::string>();
            if ((!key.empty() && !keys.insert(key).second) || hash.size() != 64 ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
                return invalid("conflicting operation acceptance");
            }
            stages.emplace(id, Stage::Accepted);
        } else if (kind == "operation.dispatched") {
            if (found == stages.end() || found->second != Stage::Accepted) {
                return invalid("dispatch without one preceding acceptance");
            }
            found->second = Stage::Dispatched;
        } else if (kind == "operation.final") {
            if (found == stages.end() || found->second != Stage::Dispatched ||
                !is_string(row, "turnId") || !is_string(row, "executionStatus") ||
                row.at("executionStatus") == "" || !row.contains("finalMessageRefs") ||
                !row.at("finalMessageRefs").is_array() || !row.contains("usageReported") ||
                !row.at("usageReported").is_boolean()) {
                return invalid("invalid final or missing preceding dispatch");
            }
            for (const auto& ref : row.at("finalMessageRefs")) {
                if (!ref.is_string()) return invalid("invalid final message reference");
            }
            found->second = Stage::Final;
        } else {
            return invalid("unknown operation fact kind");
        }
    }
    if (input.bad()) return invalid("failed while reading operation ledger");
    return check_result_coverage();
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

class ApprovalFuture final : public rt::InteractionFuture {
public:
    explicit ApprovalFuture(std::chrono::milliseconds timeout) : timeout_(timeout) {}
    std::optional<rt::ApprovalResponse> WaitApproval() override {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout_, [&] { return done_; })) done_ = true;
        return response_;
    }
    std::optional<rt::QuestionResponse> WaitQuestion() override { return std::nullopt; }
    bool Resolve(std::optional<rt::ApprovalResponse> response) {
        std::lock_guard lock(mutex_);
        if (done_) return false;
        response_ = std::move(response);
        done_ = true;
        cv_.notify_all();
        return true;
    }
    bool pending() const { std::lock_guard lock(mutex_); return !done_; }
private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::chrono::milliseconds timeout_;
    bool done_ = false;
    std::optional<rt::ApprovalResponse> response_;
};
} // namespace

struct EventStream::Impl {
    explicit Impl(std::size_t limit) : capacity(limit) {}
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Event> events;
    std::size_t capacity;
    std::size_t active_reads = 0;
    std::size_t bytes = 0;
    bool closed = false;
    std::string error = "sdk.events.closed";
    void Push(const Event& event) {
        std::lock_guard lock(mutex);
        if (closed) return;
        const auto cost = event.text.size() + event.payload_json.size() +
            (event.approval ? event.approval->input_json.size() : 0);
        if (events.size() >= capacity || cost > 16 * 1024 * 1024 || bytes > 16 * 1024 * 1024 - cost) {
            error = "sdk.events.overflow";
            closed = true;
            events.clear();
            bytes = 0;
        } else { events.push_back(event); bytes += cost; }
        cv.notify_all();
    }
    void Close() {
        std::unique_lock lock(mutex);
        closed = true;
        cv.notify_all();
        cv.wait(lock, [&] { return active_reads == 0; });
        events.clear();
        bytes = 0;
    }
};

EventStream::EventStream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
EventStream::~EventStream() { Close(); }
void EventStream::Close() { impl_->Close(); }
Result<std::optional<Event>> EventStream::Next(std::chrono::milliseconds timeout) {
    auto state = impl_;
    if (timeout.count() < 0) return std::unexpected(Failure("sdk.timeout.invalid"));
    std::unique_lock lock(state->mutex);
    ++state->active_reads;
    state->cv.wait_for(lock, timeout, [&] { return state->closed || !state->events.empty(); });
    --state->active_reads;
    state->cv.notify_all();
    if (state->closed) return std::unexpected(Failure(state->error));
    if (state->events.empty()) return std::optional<Event>{};
    Event event = std::move(state->events.front());
    state->events.pop_front();
    state->bytes -= event.text.size() + event.payload_json.size() + (event.approval ? event.approval->input_json.size() : 0);
    return std::optional<Event>{std::move(event)};
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
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    std::mutex close_mutex;
    std::map<std::string, Operation> operations;
    std::set<std::string> cancelled;
    std::vector<std::weak_ptr<EventStream::Impl>> subscriptions;
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
    mutable std::mutex approval_mutex;
    struct Pending { Approval approval; std::shared_ptr<ApprovalFuture> future; };
    std::map<std::string, Pending> pending;
    std::set<std::string> allowed;

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
        auto future = std::make_shared<ApprovalFuture>(options.approval_timeout);
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
        {
            std::lock_guard lock(approval_mutex);
            if (interrupt.load()) { future->Resolve(std::nullopt); return future; }
            pending.emplace(approval.request_id, Pending{approval, future});
        }
        Event event;
        event.kind = "approval_requested";
        event.operation_id = approval.operation_id;
        event.approval = std::move(approval);
        Emit(std::move(event));
        return future;
    }
    std::shared_ptr<rt::InteractionFuture> AskQuestion(const rt::QuestionRequest&) override {
        auto future = std::make_shared<ApprovalFuture>(std::chrono::milliseconds(0));
        future->Resolve(std::nullopt);
        return future; // ask_user is not admitted by this SDK version
    }
    bool ResolveApproval(const rt::InteractionRequestId& id, const rt::ApprovalResponse& response) override {
        std::lock_guard lock(approval_mutex);
        auto it = pending.find(id.value);
        if (it == pending.end()) return false;
        const bool resolved = it->second.future->Resolve(response);
        if (resolved && response.decision == rt::InteractionDecision::AcceptForSession) allowed.insert(it->second.approval.tool_name);
        pending.erase(it);
        return resolved;
    }
    bool AnswerQuestion(const rt::InteractionRequestId&, const rt::QuestionResponse&) override { return false; }
    void CancelApprovals() {
        std::lock_guard lock(approval_mutex);
        for (auto& [id, entry] : pending) { (void)id; entry.future->Resolve(std::nullopt); }
        pending.clear();
    }

    Result<void> Initialize() {
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
        auto prepared_registry = std::make_unique<lubancode::tools::ToolRegistry>();
        for (const auto& name : options.builtin_tools) {
            auto tool = rt::assembly::CreateLocalTool(name);
            if (!tool || prepared_registry->Find(name)) return std::unexpected(Failure("sdk.tool.unsupported_or_duplicate", name));
            prepared_registry->Register(detail::BindLocalTool(std::move(tool), options.cwd));
        }
        for (auto& tool : options.custom_tools) {
            if (prepared_registry->Find(tool.name)) return std::unexpected(Failure("sdk.tool.duplicate", tool.name));
            auto adapted = detail::AdaptTool(std::move(tool), options.cwd);
            if (!adapted) return std::unexpected(adapted.error());
            prepared_registry->Register(std::move(*adapted));
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
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(*cwd, lubancode::tools::Utf8ToPath(roots.data_root));
        if (!identity) return std::unexpected(Failure("sdk.workspace.failed", identity.error()));
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = options.cwd;
        launch.workspace_identity = std::move(*identity);
        launch.lubancode_version = Version();
        launch.wire_name = wire;
        launch.approval_mode = Mode(options.approval_mode);
        launch.workspaces_root = lubancode::tools::Utf8ToPath(roots.data_root) / "workspaces";
        launch.v3_system_content = options.system_prompt;
        launch.resume_at_launch = !options.resume_session_id.empty();
        launch.require_v3_resume = launch.resume_at_launch;
        launch.resume_source_session_id = options.resume_session_id;
        service = std::make_shared<rt::SessionService>(std::move(launch));
        if (!service->runtime()) return std::unexpected(Failure("sdk.session.open_failed", service->launch_error()));
        session_id = service->trajectory()->session_id();
        session_dir = service->trajectory()->session_dir();
        if (!options.resume_session_id.empty() && session_id != options.resume_session_id) {
            return std::unexpected(Failure("sdk.resume.identity_changed"));
        }
        if (!options.resume_session_id.empty() && options.system_prompt.empty()) {
            auto saved = lubancode::trajectory::v3::ReadV3Ledger(service->trajectory()->v3_main_writer()->path());
            if (!saved) return std::unexpected(Failure("sdk.resume.context_unavailable", saved.error()));
            options.system_prompt = lubancode::trajectory::v3::ProjectModelContext(*saved).system_content;
        }
        const auto plan_path = session_dir / "sdk-extension-plan.json";
        std::optional<std::string> expected_plan;
        if (!options.resume_session_id.empty()) {
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
        if (options.resume_session_id.empty()) {
            const auto written = lubancode::platform::AtomicWriteFile(plan_path, extension_plan_json,
                lubancode::platform::WriteDurability::ProcessCrashDurability);
            if (!written) return std::unexpected(Failure("sdk.extension.plan_write_failed", written.error().message));
        }
        lubancode::agent::AgentProfile profile;
        profile.request.model = options.model;
        profile.system_prompt = options.system_prompt;
        profile.runtime.max_steps_per_turn = options.max_steps_per_turn;
        profile.runtime.context_window_tokens = options.context_window_tokens;
        std::optional<std::vector<api::Message>> restored_history;
        if (!options.resume_session_id.empty()) restored_history = service->trajectory()->LaunchResumeHistory();
        service->InitializeExecution(std::move(*assembled), std::move(profile), std::move(restored_history));
        auto loaded = LoadOperations();
        if (!loaded) return loaded;
        extensions = module_ptr;
        return {};
    }

    void Start() {
        std::lock_guard lock(mutex); // Publish the owned thread before Pump enters callbacks.
        worker = std::thread([this] { Pump(); });
    }

    Result<void> LoadOperations() {
        const auto valid = ValidateOperationLedger(session_dir);
        if (!valid) return valid;
        std::map<std::string, std::string> accepted_hashes;
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
            operation.turn_id = fact.turn_id;
            operation.state = fact.execution_status == "success" ? OperationState::Succeeded :
                fact.execution_status == "error" ? OperationState::Failed :
                fact.execution_status == "cancelled" ? OperationState::Cancelled : OperationState::Indeterminate;
            std::ifstream input(session_dir / "sdk-results" / (fact.operation_id + ".json"), std::ios::binary);
            if (input) {
                auto json = Json::parse(input, nullptr, false);
                if (json.is_object() && json.value("operationId", "") == fact.operation_id && json.value("turnId", "") == fact.turn_id) {
                    operation.final_text = json.value("finalText", "");
                    operation.error = json.value("error", "");
                    operation.result_persisted = json.value("complete", false) && operation.state != OperationState::Indeterminate;
                }
            }
            if (!operation.result_persisted) operation.error = "sdk.result.unavailable: final operation exists without SDK result artifact";
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
            if (operation.state == OperationState::Accepted && !queued_ids.contains(id)) {
                return std::unexpected(Failure("sdk.resume.input_unavailable", id));
            }
        }
        return {};
    }

    Operation Complete(Operation operation, const std::vector<std::string>& refs, bool usage_reported, bool ledger_ok = true) {
        if (!ledger_ok) {
            operation.state = OperationState::Indeterminate;
            operation.error += " sdk.trajectory.persistence_failed";
        }
        const auto directory = session_dir / "sdk-results";
        std::error_code ec;
        fs::create_directories(directory, ec);
        const Json result{{"operationId", operation.operation_id}, {"turnId", operation.turn_id},
                          {"finalText", operation.final_text}, {"error", operation.error}, {"complete", ledger_ok}};
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
        operation.result_persisted = written.has_value() && recorded && ledger_ok;
        if (!operation.result_persisted) {
            operation.error += " sdk.result.persistence_failed";
            operation.state = OperationState::Indeterminate;
        }
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
        wiring.events = &events;
        wiring.turn_id = operation.turn_id;
        wiring.tool_artifact_dir = lubancode::tools::PathToUtf8(session_dir / "artifacts" / "sha256");
        if (rt::HasPreRequestMiddleware(dispatcher)) {
            wiring.on_pre_request_hooks = [&, dispatcher](const std::string& step_id, const std::string& turn_id,
                const Json& frozen_request, const rt::PreRequestBudget& budget) {
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
            std::lock_guard lock(approval_mutex);
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
        api::Message message;
        message.role = api::Role::User;
        message.content.push_back(api::TextBlock{pre.prompt});
        bridge->BeginTurn(operation.turn_id, "external_user");
        bridge->RecordInput(message);
        const auto history_before = agent.history().size();
        std::expected<lubancode::agent::RunOutcome, std::string> outcome =
            std::unexpected("sdk.extension.input_admission_failed");
        bool context_healthy = true;
        if (!dispatcher) {
            // Zero registrations preserve the established host path byte for byte.
            outcome = agent.Run(std::move(message), wiring, &interrupt);
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
        bridge->EndTurn(outcome.has_value(), outcome && outcome->cancelled, outcome ? "" : outcome.error());
        turn_bindings.Reset();
        operation.state = !outcome ? (dispatcher && interrupt.load() ? OperationState::Cancelled : OperationState::Failed) :
                          outcome->cancelled ? OperationState::Cancelled : OperationState::Succeeded;
        if (!outcome) operation.error = outcome.error();
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
                cv.wait(lock, [&] { return closing || broken || service->pending_input_count() > 0; });
                if (broken || (closing && service->pending_input_count() == 0)) break;
                pop = service->PopPendingInput();
                if (pop.status == rt::SessionService::PendingPop::Status::WriteFailed) { broken = true; cv.notify_all(); break; }
                if (pop.status != rt::SessionService::PendingPop::Status::Ok) continue;
                active_operation = pop.input.operation_id;
                skip = closing || cancelled.contains(active_operation);
                interrupt.store(skip);
                operations[active_operation].state = OperationState::Running;
            }
            try { Run(pop.input, skip); }
            catch (const std::exception& error) {
                Operation failed{pop.input.operation_id, {}, OperationState::Failed, {}, error.what(), false};
                Complete(std::move(failed), {}, false);
            } catch (...) {
                Operation failed{pop.input.operation_id, {}, OperationState::Failed, {}, "sdk.turn.exception", false};
                Complete(std::move(failed), {}, false);
            }
            CancelApprovals();
        }
        std::lock_guard lock(mutex);
        if (broken) for (auto& [id, operation] : operations) {
            (void)id;
            if (!Terminal(operation.state)) { operation.state = OperationState::Indeterminate; operation.error = "sdk.storage.broken"; }
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
        CancelApprovals();
    }
    void RequestExecutionShutdown() {
        std::shared_ptr<rt::SessionService> service_to_stop;
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
            if (service_to_stop != nullptr) ++close_signals_inflight;
        }
        if (service_to_stop != nullptr) {
            service_to_stop->RequestExecutionShutdown();
        }
    }
    Result<void> Close() {
        if (in_session_worker) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
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
        if (close_error && close_errors) close_errors->Remember(*close_error);
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
        }
        for (auto& stream : streams) stream->Close();
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
Result<Receipt> Session::Submit(std::string key, std::string text) {
    if (key.empty()) return std::unexpected(Failure("sdk.operation.key_required"));
    if (!lubancode::platform::IsValidUtf8(key) || !lubancode::platform::IsValidUtf8(text)) {
        return std::unexpected(Failure("sdk.input.invalid_utf8"));
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->closed || impl_->runtime_stopping->load()) return std::unexpected(Failure("sdk.session.closed"));
    if (impl_->broken) return std::unexpected(Failure("sdk.storage.broken"));
    auto receipt = impl_->service->SubmitInput({std::move(key), std::move(text), {}});
    if (!receipt.accepted && !receipt.duplicate) return std::unexpected(Failure(receipt.error_code));
    if (receipt.accepted) impl_->operations.emplace(receipt.operation_id, Operation{receipt.operation_id});
    impl_->cv.notify_all();
    return Receipt{receipt.operation_id, receipt.input_id, receipt.duplicate};
}
Result<std::shared_ptr<EventStream>> Session::Subscribe(std::size_t capacity) {
    if (capacity == 0 || capacity > 65536) return std::unexpected(Failure("sdk.events.invalid_capacity"));
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->closed || impl_->runtime_stopping->load()) return std::unexpected(Failure("sdk.session.closed"));
    auto state = std::make_shared<EventStream::Impl>(capacity);
    impl_->subscriptions.push_back(state);
    return std::shared_ptr<EventStream>(new EventStream(std::move(state)));
}
std::vector<Approval> Session::PendingApprovals() const {
    std::lock_guard lock(impl_->approval_mutex);
    std::vector<Approval> result;
    for (const auto& [id, entry] : impl_->pending) { (void)id; if (entry.future->pending()) result.push_back(entry.approval); }
    return result;
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
    if (in_session_worker) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
    std::unique_lock lock(impl_->mutex);
    auto it = impl_->operations.find(id);
    if (it == impl_->operations.end()) return std::unexpected(Failure("sdk.operation.not_found"));
    if (!impl_->cv.wait_for(lock, timeout, [&] { return Terminal(it->second.state); })) return std::unexpected(Failure("sdk.wait.timeout"));
    return it->second;
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
    if (in_session_worker) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
    std::lock_guard lock(impl_->mutex);
    if (impl_->closed) return std::unexpected(Failure("sdk.runtime.closed"));
    try {
        std::erase_if(impl_->sessions, [](const auto& session) { return session.expired(); });
        auto execution = std::make_shared<Session::Impl>();
        execution->roots = impl_->options;
        execution->runtime_stopping = impl_->stopping;
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
    if (in_session_worker) return std::unexpected(Failure("sdk.lifecycle.reentrant"));
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
