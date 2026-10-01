#include "host.hpp"
#include "result_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <lubancore/core.hpp>

namespace lubancode::worker_host {
namespace {
using Json = nlohmann::json;
namespace sdk = lubancore;
namespace saved = sdk::results::v1;
namespace support = result_support;
std::string BootNonce() {
    std::random_device random;
    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (int i = 0; i != 4; ++i) value << std::setw(8) << static_cast<std::uint32_t>(random());
    return value.str();
}
struct Rejected { std::string code; std::string sdk_code; };
[[noreturn]] void Reject(std::string code) { throw Rejected{std::move(code), {}}; }
void Fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object()) Reject("worker.invalid_object");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) Reject("worker.unsupported_field");
    }
}
std::string Text(const Json& object, const char* key, std::size_t max = 65536, bool required = true) {
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) Reject("worker.missing_field");
        return {};
    }
    if (!it->is_string()) Reject("worker.invalid_string");
    auto text = it->get<std::string>();
    if (text.size() > max || text.find('\0') != std::string::npos || (required && text.empty())) Reject("worker.invalid_string");
    return text;
}
std::string Id(const Json& object, const char* key) {
    auto id = Text(object, key, 200);
    if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:") != std::string::npos)
        Reject("worker.invalid_id");
    return id;
}
std::size_t Number(const Json& object, const char* key, std::size_t fallback, std::size_t maximum) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_number_unsigned() && (!it->is_number_integer() || it->get<std::int64_t>() < 0)) Reject("worker.invalid_number");
    auto value = it->get<std::uint64_t>();
    if (value > maximum) Reject("worker.invalid_number");
    return static_cast<std::size_t>(value);
}
template<class T> T Take(sdk::Result<T> result) {
    if (!result) throw Rejected{"worker.sdk_failure", result.error().code};
    return std::move(*result);
}
void Check(sdk::Result<void> result) {
    if (!result) throw Rejected{"worker.sdk_failure", result.error().code};
}
template<class T> T QueryTake(sdk::Result<T> result) {
    if (!result) throw support::Failure("worker.sdk_failure", result.error().code);
    return std::move(*result);
}
bool Boolean(const Json& object, const char* key, bool fallback) {
    if (!object.contains(key)) return fallback;
    if (!object[key].is_boolean()) Reject("worker.invalid_boolean");
    return object[key].get<bool>();
}
std::string SecretReference(const Json& object, const char* field) {
    const auto name = Text(object, field, 128);
    if (name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
        Reject("worker.invalid_secret_reference");
    const char* value = std::getenv(name.c_str());
    if (!value) Reject("worker.secret_unavailable");
    std::string resolved(value);
    if (resolved.size() > 65536) Reject("worker.secret_too_large");
    return resolved;
}
saved::ToolResultIdentity ResultIdentity(const Json& value) {
    Fields(value, {"session_id", "operation_id", "turn_id", "tool_call_id", "persisted_event_id", "result_id"});
    return {Id(value, "session_id"), Id(value, "operation_id"), Id(value, "turn_id"),
            Id(value, "tool_call_id"), Id(value, "persisted_event_id"), Id(value, "result_id")};
}
void ResultIdsSafe(const saved::ToolResultIdentity& id, const std::vector<std::string>& secrets) {
    const auto fields = support::Identity(id);
    for (const auto& secret : secrets) for (const auto& value : fields)
        if (!secret.empty() && value.get_ref<const std::string&>().find(secret) != std::string::npos)
            throw support::Failure("worker.result_identity_rejected");
}
std::string State(sdk::OperationState state) {
    switch (state) {
    case sdk::OperationState::Accepted: return "accepted";
    case sdk::OperationState::Running: return "running";
    case sdk::OperationState::Succeeded: return "succeeded";
    case sdk::OperationState::Failed: return "failed";
    case sdk::OperationState::Cancelled: return "cancelled";
    case sdk::OperationState::Indeterminate: return "indeterminate";
    }
    return "indeterminate";
}
sdk::ApprovalMode Mode(const Json& params) {
    const auto value = Text(params, "approval_mode", 32, false);
    if (value.empty() || value == "confirm") return sdk::ApprovalMode::Confirm;
    if (value == "accept_edits") return sdk::ApprovalMode::AcceptEdits;
    if (value == "dont_ask") return sdk::ApprovalMode::DontAsk;
    if (value == "yolo") return sdk::ApprovalMode::Yolo;
    Reject("worker.unsupported_approval_mode");
}
Json Page(const std::string& text, const Json& params) {
    const auto offset = Number(params, "text_offset", 0, text.size());
    const auto limit = Number(params, "text_limit", 32768, 65536);
    if (!limit) Reject("worker.invalid_number");
    const auto continuation = [&](std::size_t i) { return (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80; };
    if (offset < text.size() && continuation(offset)) Reject("worker.invalid_text_offset");
    auto end = std::min(text.size(), offset + limit);
    while (end < text.size() && continuation(end)) --end;
    if (end == offset && end < text.size()) Reject("worker.text_limit_too_small");
    return {{"content", text.substr(offset, end - offset)}, {"offset", offset},
            {"next_offset", end}, {"total_bytes", text.size()}, {"complete", end == text.size()}};
}
} // namespace

struct Host::Impl {
    struct Session {
        Json specification;
        std::string client_id;
        std::shared_ptr<sdk::Session> handle;
        std::shared_ptr<sdk::EventStream> events;
        saved::SessionResultPolicy result_policy;
        std::shared_ptr<support::Store> result_store;
        std::vector<std::string> result_secrets;
        std::set<std::string> operations;
        std::uint64_t sequence = 0;
        bool closed = false;
    };
    std::unique_ptr<sdk::Runtime> runtime;
    Json roots;
    std::map<std::string, Session> sessions;
    std::map<std::string, std::string> client_sessions;
    std::vector<std::string> secrets;
    const std::string instance_id = BootNonce();
    std::unique_ptr<support::Queries> queries;
    saved::NodeResultPolicy node_policy{false, 4096, "v1"};
    std::vector<std::string> node_secrets;
    std::string attachment;
    std::uint64_t generation = 0;
    bool stop = false;

    std::string PublicText(std::string text) const {
        // Only mask resolved model credentials on this display boundary. This
        // is separate from the public SDK's persisted tool-result projection.
        for (const auto& secret : secrets) {
            if (secret.empty()) continue;
            std::size_t at = 0;
            while ((at = text.find(secret, at)) != std::string::npos) {
                text.replace(at, secret.size(), "[credential]");
                at += sizeof("[credential]") - 1;
            }
        }
        return text;
    }
    void Detach() {
        for (auto& [id, session] : sessions) {
            (void)id;
            if (session.events) session.events->Close();
            session.events.reset();
        }
        attachment.clear();
    }
    Session& Find(const Json& params) {
        auto found = sessions.find(Id(params, "session_id"));
        if (found == sessions.end()) Reject("worker.session_not_found");
        return found->second;
    }
    Json SessionInfo(const std::string& id, const Session& session) const {
        auto ids = Json::array();
        for (const auto& operation : session.operations) {
            if (ids.size() == 128) break;
            ids.push_back(operation);
        }
        return {{"session_id", id}, {"client_session_id", session.client_id}, {"closed", session.closed},
                {"known_operation_ids", std::move(ids)}, {"known_operation_count", session.operations.size()},
                {"operation_ids_complete", session.operations.size() <= 128}, {"events_ephemeral", true},
                {"tool_result_sync", support::ModeName(session.result_policy.mode)},
                {"result_policy_version", session.result_policy.version}};
    }
    Json Execute(const std::string& method, const Json& params, const Json& request) {
        if (method == "worker.status") {
            Fields(params, {});
            return {{"protocol_version", 1}, {"worker_instance_id", instance_id}, {"sdk_version", sdk::Version()}, {"initialized", !!runtime},
                    {"attached", !attachment.empty()}, {"stopping", stop}, {"session_count", sessions.size()},
                    {"capabilities", {{"transport", "private-parent-stdio"}, {"assistant_text", "complete-paged"},
                     {"tool_results", "persisted-selected-pull"}, {"tool_result_default", "preview"},
                     {"result_query_queue", 8}, {"events", "ephemeral-pull"}, {"v3_resume", true},
                     {"mcp_configuration", false}, {"jobs", false}, {"network", false}}}};
        }
        if (stop) Reject("worker.stopping");
        if (method == "worker.initialize") {
            Fields(params, {"data_root", "resource_root", "result_policy"});
            if (runtime) {
                if (roots != params) Reject("worker.initialize_conflict");
                return {{"initialized", true}, {"duplicate", true}};
            }
            saved::NodeResultPolicy policy{false, 4096, "v1"};
            std::vector<std::string> resolved;
            if (params.contains("result_policy")) {
                const auto& configured = params["result_policy"];
                Fields(configured, {"allow_full_tool_results", "preview_max_bytes", "version", "secret_envs"});
                policy.allow_full_tool_results = Boolean(configured, "allow_full_tool_results", false);
                policy.preview_max_bytes = Number(configured, "preview_max_bytes", 4096, 65536);
                if (!policy.preview_max_bytes) Reject("worker.invalid_number");
                if (configured.contains("version")) policy.version = Id(configured, "version");
                if (configured.contains("secret_envs")) {
                    if (!configured["secret_envs"].is_array() || configured["secret_envs"].size() > 64) Reject("worker.invalid_secret_reference");
                    std::size_t secret_bytes = 0;
                    for (const auto& name : configured["secret_envs"]) {
                        resolved.push_back(SecretReference(Json{{"name", name}}, "name"));
                        secret_bytes += resolved.back().size();
                        if (secret_bytes > 65536) Reject("worker.secret_too_large");
                    }
                }
            }
            std::sort(resolved.begin(), resolved.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
            auto opened = Take(sdk::Runtime::Create({Text(params, "data_root"), Text(params, "resource_root")}));
            auto executor = std::make_unique<support::Queries>(instance_id);
            roots = params;
            node_policy = std::move(policy);
            node_secrets = std::move(resolved);
            secrets = node_secrets;
            runtime = std::move(opened);
            queries = std::move(executor);
            return {{"initialized", true}, {"duplicate", false}};
        }
        if (method == "worker.shutdown") {
            Fields(params, {});
            stop = true;
            if (queries) queries->Stop();
            struct JoinQueries {
                support::Queries* executor;
                ~JoinQueries() { if (executor) executor->Join(); }
            } join_queries{queries.get()};
            Detach();
            auto closed = runtime ? runtime->Shutdown() : sdk::Result<void>{};
            if (queries) queries->Join();
            Check(std::move(closed));
            return {{"stopped", true}};
        }
        if (!runtime) Reject("worker.not_initialized");
        if (method == "client.attach") {
            Fields(params, {"client_id"});
            (void)Id(params, "client_id");
            Detach();
            attachment = instance_id + ":" + std::to_string(++generation);
            for (auto& [id, session] : sessions) {
                (void)id;
                if (!session.closed) session.events = Take(session.handle->Subscribe());
                session.sequence = 0;
            }
            return {{"attachment", attachment}, {"events_ephemeral", true}, {"query_operations_after_attach", true}};
        }
        if (attachment.empty() || Text(request, "attachment", 200, false) != attachment) Reject("worker.stale_attachment");
        if (method == "client.detach") {
            Fields(params, {});
            Detach();
            return {{"detached", true}, {"sessions_retained", true}};
        }
        if (method == "session.open") {
            Fields(params, {"client_session_id", "cwd", "model", "system_prompt", "connection", "resume_session_id",
                            "builtin_tools", "approval_mode", "approval_timeout_ms", "max_steps_per_turn", "context_window_tokens",
                            "tool_result_sync", "result_policy_version"});
            auto client = Id(params, "client_session_id");
            auto duplicate = client_sessions.find(client);
            if (duplicate != client_sessions.end()) {
                auto& existing = sessions.at(duplicate->second);
                if (existing.specification != params) Reject("worker.session_open_conflict");
                auto result = SessionInfo(duplicate->second, existing);
                result["duplicate"] = true;
                return result;
            }
            if (sessions.size() >= 64) Reject("worker.session_limit");
            sdk::SessionOptions options;
            options.cwd = Text(params, "cwd");
            options.model = Text(params, "model", 1024);
            options.system_prompt = Text(params, "system_prompt", 262144, false);
            options.resume_session_id = Text(params, "resume_session_id", 200, false);
            if (!options.resume_session_id.empty() && sessions.contains(options.resume_session_id)) Reject("worker.session_already_hosted");
            const bool resume = !options.resume_session_id.empty();
            saved::SessionResultOptions result_options;
            if (resume) {
                const auto manifest = support::ReadManifest(support::Path(Text(roots, "data_root")), options.resume_session_id);
                result_options.mode = manifest["mode"] == "full" ? saved::Mode::Full : saved::Mode::Preview;
                result_options.version = manifest["session_policy_version"].get<std::uint64_t>();
            }
            if (params.contains("tool_result_sync")) {
                const auto choice = Text(params, "tool_result_sync", 16);
                if (choice != "preview" && choice != "full") Reject("worker.invalid_result_mode");
                const auto requested = choice == "full" ? saved::Mode::Full : saved::Mode::Preview;
                if (resume && requested != result_options.mode) Reject("worker.result_policy_conflict");
                result_options.mode = requested;
            }
            if (params.contains("result_policy_version")) {
                const auto version = Number(params, "result_policy_version", 1, 1000000000);
                if (!version) Reject("worker.invalid_number");
                if (resume && version != result_options.version) Reject("worker.result_policy_conflict");
                result_options.version = version;
            }
            if (result_options.mode == saved::Mode::Full && !node_policy.allow_full_tool_results)
                Reject("worker.full_result_sync_disabled");
            options.result_policy = result_options;
            options.approval_mode = Mode(params);
            options.approval_timeout = std::chrono::milliseconds(Number(params, "approval_timeout_ms", 300000, 3600000));
            options.max_steps_per_turn = static_cast<int>(Number(params, "max_steps_per_turn", 0, 10000));
            options.context_window_tokens = Number(params, "context_window_tokens", 128000, 10000000);
            if (params.contains("builtin_tools")) {
                const auto& names = params.at("builtin_tools");
                if (!names.is_array() || names.size() > 4) Reject("worker.invalid_tools");
                for (const auto& name : names) {
                    if (!name.is_string()) Reject("worker.invalid_tools");
                    options.builtin_tools.push_back(name.get<std::string>());
                }
            }
            if (!params.contains("connection")) Reject("worker.missing_field");
            const auto& connection = params.at("connection");
            Fields(connection, {"wire", "base_url", "api_key_env", "connect_timeout_ms", "idle_timeout_seconds", "request_timeout_seconds"});
            sdk::Connection config;
            auto wire = Text(connection, "wire", 32);
            if (wire == "chat_completions") config.wire = sdk::Wire::ChatCompletions;
            else if (wire == "anthropic") config.wire = sdk::Wire::Anthropic;
            else if (wire == "responses") config.wire = sdk::Wire::Responses;
            else if (wire == "gemini") config.wire = sdk::Wire::Gemini;
            else Reject("worker.unsupported_wire");
            config.base_url = Text(connection, "base_url", 8192);
            config.api_key = SecretReference(connection, "api_key_env");
            if (!config.api_key.empty() && std::find(secrets.begin(), secrets.end(), config.api_key) == secrets.end()) {
                secrets.push_back(config.api_key);
                std::sort(secrets.begin(), secrets.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
            }
            config.connect_timeout_ms = static_cast<int>(Number(connection, "connect_timeout_ms", 10000, 300000));
            config.idle_timeout_seconds = static_cast<int>(Number(connection, "idle_timeout_seconds", 60, 3600));
            config.request_timeout_seconds = static_cast<int>(Number(connection, "request_timeout_seconds", 300, 86400));
            options.connection = std::move(config);
            Session session;
            session.client_id = client;
            session.specification = params; // references secret environment names, never values
            session.result_secrets = node_secrets;
            session.result_secrets.push_back(options.connection->api_key);
            std::shared_ptr<saved::ResultProjector> projector;
            if (resume) {
                saved::SessionResultPolicy policy{options.resume_session_id, result_options.mode, result_options.version};
                projector = Take(saved::ResultProjector::Create(node_policy, policy, session.result_secrets));
                const auto manifest = support::ReadManifest(support::Path(Text(roots, "data_root")), policy.session_id);
                if (manifest["binding"] != projector->BindingFingerprint()) Reject("worker.result_policy_conflict");
            }
            session.handle = Take(runtime->OpenSession(std::move(options)));
            const auto id = session.handle->id();
            session.result_policy = {id, result_options.mode, result_options.version};
            try {
                if (!projector) projector = Take(saved::ResultProjector::Create(node_policy, session.result_policy, session.result_secrets));
                session.result_store = std::make_shared<support::Store>(support::Path(Text(roots, "data_root")), session.result_policy, projector, resume);
                session.events = Take(session.handle->Subscribe());
            } catch (...) { (void)session.handle->Close(); throw; }
            sessions.emplace(id, std::move(session));
            client_sessions.emplace(std::move(client), id);
            auto result = SessionInfo(id, sessions.at(id));
            result["duplicate"] = false;
            return result;
        }
        if (method == "session.list") {
            Fields(params, {});
            auto entries = Json::array();
            for (const auto& [id, session] : sessions) entries.push_back(SessionInfo(id, session));
            return {{"sessions", std::move(entries)}};
        }
        if (method == "session.get" || method == "session.close") {
            Fields(params, {"session_id"});
            auto& session = Find(params);
            if (method == "session.close") {
                auto closed = session.handle->Close();
                session.closed = true;
                if (session.events) session.events->Close();
                session.events.reset();
                Check(std::move(closed));
            }
            return SessionInfo(session.handle->id(), session);
        }
        if (method == "operation.submit") {
            Fields(params, {"session_id", "client_operation_id", "text"});
            auto& session = Find(params);
            const auto receipt = Take(session.handle->Submit(Id(params, "client_operation_id"), Text(params, "text", 262144)));
            session.operations.insert(receipt.operation_id);
            return {{"operation_id", receipt.operation_id}, {"input_id", receipt.input_id}, {"duplicate", receipt.duplicate}};
        }
        if (method == "operation.get") {
            Fields(params, {"session_id", "operation_id", "text_offset", "text_limit"});
            const auto operation = Take(Find(params).handle->ReadOperation(Id(params, "operation_id")));
            return {{"operation_id", operation.operation_id}, {"turn_id", operation.turn_id}, {"state", State(operation.state)},
                    {"result_persisted", operation.result_persisted}, {"assistant_text", Page(PublicText(operation.final_text), params)},
                    {"has_error", !operation.error.empty()}};
        }
        if (method == "operation.cancel") {
            Fields(params, {"session_id", "operation_id"});
            Check(Find(params).handle->Cancel(Id(params, "operation_id")));
            return {{"cancellation_requested", true}};
        }
        if (method == "result.query.start") {
            Fields(params, {"session_id", "operation_id", "client_query_id", "identity"});
            auto& session = Find(params);
            const auto operation = Id(params, "operation_id");
            const auto client_key = Id(params, "client_query_id");
            std::optional<saved::ToolResultIdentity> identity;
            if (params.contains("identity")) {
                identity = ResultIdentity(params["identity"]);
                if (identity->session_id != session.handle->id() || identity->operation_id != operation)
                    Reject("worker.result_identity_mismatch");
            }
            auto handle = session.handle;
            const auto session_id = handle->id();
            auto store = session.result_store;
            auto known_secrets = session.result_secrets;
            return queries->Start(session_id, client_key, params,
                [handle = std::move(handle), store = std::move(store), operation,
                 identity = std::move(identity), known_secrets = std::move(known_secrets)] {
                    const auto summaries = QueryTake(handle->ListToolResults(operation));
                    auto list = Json::array();
                    bool found = false;
                    for (const auto& summary : summaries) {
                        if (!support::Formal(summary)) continue;
                        ResultIdsSafe(summary.identity, known_secrets);
                        if (identity && summary.identity == *identity) found = true;
                        list.push_back({{"identity", support::Identity(summary.identity)}, {"attempt", summary.attempt}});
                    }
                    if (!identity) {
                        Json result{{"results", std::move(list)}, {"formal_selected_only", true}};
                        if (result.dump().size() + 1024 > support::kFrameBytes) throw support::Failure("worker.result_frame_too_large");
                        return result;
                    }
                    if (!found) throw support::Failure("worker.result_not_exposed");
                    auto snapshot = QueryTake(handle->ReadToolResult(*identity, {support::kSnapshotTextBytes}));
                    return store->Read(snapshot);
                });
        }
        if (method == "result.query.get") {
            Fields(params, {"session_id", "query_id"});
            const auto& session = Find(params);
            return queries->Get(session.handle->id(), Id(params, "query_id"));
        }
        if (method == "approval.list" || method == "approval.get") {
            if (method == "approval.list") Fields(params, {"session_id"});
            else Fields(params, {"session_id", "request_id", "text_offset", "text_limit"});
            auto approvals = Find(params).handle->PendingApprovals();
            auto entries = Json::array();
            const auto selected = method == "approval.get" ? Id(params, "request_id") : std::string{};
            for (const auto& approval : approvals) {
                if (!selected.empty() && approval.request_id != selected) continue;
                Json entry{{"request_id", approval.request_id}, {"operation_id", approval.operation_id},
                           {"tool_name", PublicText(approval.tool_name)}};
                if (!selected.empty()) entry["input_json"] = Page(PublicText(approval.input_json), params);
                entries.push_back(std::move(entry));
            }
            if (!selected.empty() && entries.empty()) Reject("worker.approval_not_found");
            return {{"approvals", std::move(entries)}};
        }
        if (method == "approval.resolve") {
            Fields(params, {"session_id", "request_id", "decision", "reason"});
            const auto decision = Text(params, "decision", 32);
            sdk::ApprovalDecision value;
            if (decision == "accept") value = sdk::ApprovalDecision::Accept;
            else if (decision == "accept_for_session") value = sdk::ApprovalDecision::AcceptForSession;
            else if (decision == "decline") value = sdk::ApprovalDecision::Decline;
            else if (decision == "cancel") value = sdk::ApprovalDecision::Cancel;
            else Reject("worker.unsupported_decision");
            Check(Find(params).handle->ResolveApproval(Id(params, "request_id"), value, Text(params, "reason", 4096, false)));
            return {{"resolved", true}};
        }
        if (method == "events.poll") {
            Fields(params, {"session_id", "max_events", "timeout_ms"});
            auto& session = Find(params);
            if (!session.events) Reject("worker.events_closed");
            auto limit = Number(params, "max_events", 32, 128);
            if (!limit) Reject("worker.invalid_number");
            auto timeout = std::chrono::milliseconds(Number(params, "timeout_ms", 0, 1000));
            auto events = Json::array();
            while (events.size() < limit) {
                auto event = Take(session.events->Next(events.empty() ? timeout : std::chrono::milliseconds(0)));
                if (!event) break;
                Json projected{{"sequence", ++session.sequence}, {"kind", event->kind},
                               {"session_id", event->session_id}, {"operation_id", event->operation_id}, {"turn_id", event->turn_id}};
                if (event->approval) projected["request_id"] = event->approval->request_id;
                // Never serialize Event::text or payload_json. They can contain
                // complete tool results, arguments, attachments or provider errors.
                events.push_back(std::move(projected));
            }
            return {{"events", std::move(events)}, {"ephemeral", true}, {"attachment", attachment}};
        }
        Reject("worker.unsupported_method");
    }
};

Host::Host() : impl_(std::make_unique<Impl>()) {}
Host::~Host() {
    // Signal every session before joining any one. Destroying the map first
    // would cancel/join only its first entry while later workers kept running.
    if (impl_->queries) impl_->queries->Stop();
    if (impl_->runtime) (void)impl_->runtime->Shutdown();
    if (impl_->queries) impl_->queries->Join();
}
bool Host::stopping() const { return impl_->stop; }
Json Host::Handle(const Json& request) {
    Json id = nullptr;
    try {
        Fields(request, {"id", "method", "params", "attachment"});
        id = Id(request, "id");
        const auto method = Text(request, "method", 100);
        const Json params = request.contains("params") ? request.at("params") : Json::object();
        if (!params.is_object()) Reject("worker.invalid_object");
        return {{"id", id}, {"result", impl_->Execute(method, params, request)}};
    } catch (const support::Failure& error) {
        return {{"id", id}, {"error", {{"code", error.what()}}}};
    } catch (const Rejected& error) {
        Json result{{"code", error.code}};
        if (!error.sdk_code.empty()) result["sdk_code"] = impl_->PublicText(error.sdk_code);
        return {{"id", id}, {"error", std::move(result)}};
    } catch (const std::exception&) {
        // Exceptions may contain URLs, model credentials or raw provider bodies.
        return {{"id", id}, {"error", {{"code", "worker.internal_error"}}}};
    }
}
} // namespace lubancode::worker_host
