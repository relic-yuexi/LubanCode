#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  if defined(LUBANCORE_BUILDING)
#    define LUBANCORE_API __declspec(dllexport)
#  else
#    define LUBANCORE_API __declspec(dllimport)
#  endif
#else
#  define LUBANCORE_API __attribute__((visibility("default")))
#endif

// Experimental C++23 API. Consumer and library must use a compatible compiler,
// standard library and (on Windows) CRT. No stable cross-toolchain ABI is promised.
namespace lubancore {

struct Error { std::string code; std::string message; };
template<class T> using Result = std::expected<T, Error>;

struct Cancellation {
    // Borrowed only for the duration of Backend::Generate / Tool::execute.
    const std::atomic<bool>* flag = nullptr;
    bool requested() const noexcept { return flag && flag->load(); }
};

struct ToolCall { std::string id; std::string name; std::string input_json; };
struct ToolReply { std::string call_id; std::string text; bool is_error = false; };
struct Message {
    std::string role;
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::vector<ToolReply> tool_replies;
};
struct ToolDefinition { std::string name; std::string description; std::string input_schema_json; };
struct ModelRequest {
    std::string model;
    std::string system;
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::optional<int> max_output_tokens;
};
struct Usage { std::int64_t input_tokens = 0; std::int64_t output_tokens = 0; };
struct ModelReply {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::optional<Usage> usage;
};

// Text/tool-call injection surface, useful for an embedded provider or fixture.
// Unsupported rich history is rejected explicitly, never silently flattened.
class Backend {
public:
    virtual ~Backend() = default;
    virtual Result<ModelReply> Generate(const ModelRequest&, Cancellation) = 0;
};

enum class Wire { Anthropic, ChatCompletions, Responses, Gemini };
struct Connection {
    Wire wire = Wire::ChatCompletions;
    std::string base_url;
    std::string api_key;
    int connect_timeout_ms = 10000;
    int idle_timeout_seconds = 60;
    int request_timeout_seconds = 300;
};
struct ToolResult { std::string text; bool is_error = false; };
struct ToolContext { std::string cwd; Cancellation cancellation; };
struct Tool {
    std::string name;
    std::string description;
    std::string input_schema_json = R"({"type":"object","properties":{}})";
    // Custom tools conservatively require external-effect approval by default.
    bool requires_approval = true;
    std::function<Result<ToolResult>(const std::string&, const ToolContext&)> execute;
};
enum class ApprovalMode { Confirm, AcceptEdits, DontAsk, Yolo };
enum class ApprovalDecision { Accept, AcceptForSession, Decline, Cancel };
struct Approval {
    std::string request_id; // opaque; scoped to this session and durable turn
    std::string operation_id;
    std::string tool_call_id;
    std::string tool_name;
    std::string input_json;
    std::string cwd;
    std::string reason;
};
struct McpServer {
    std::string name;
    std::string command;
    std::vector<std::string> arguments;
    // An explicit complete child environment. No ambient parent env is inherited.
    std::vector<std::pair<std::string, std::string>> environment;
    // Exact discovered tool names, without the mcp__<server>__ prefix.
    std::vector<std::string> tools;
    int startup_timeout_ms = 30000;
    int call_timeout_ms = 120000;
};
struct RuntimeOptions {
    // Required absolute UTF-8 paths. data_root is the owned persistence root;
    // resource_root identifies installed resources (no ambient home lookup).
    std::string data_root;
    std::string resource_root;
};
struct SessionOptions {
    std::string cwd; // required absolute existing directory; never process chdir
    std::string model;
    // On resume, empty preserves the saved effective system prompt. Nonempty is
    // an explicit replacement recorded through the existing V3 system transition.
    std::string system_prompt;
    // Exactly one of backend and connection must be set. Ownership is per session.
    std::unique_ptr<Backend> backend;
    std::optional<Connection> connection;
    // Empty creates a new V3 session. Nonempty strictly resumes that same V3 ID.
    std::string resume_session_id;
    // Explicit admission. Currently read_file/write_file/edit_file/run_command.
    // run_command is foreground-only; detached jobs and CLI parity are not claimed.
    std::vector<std::string> builtin_tools;
    std::vector<Tool> custom_tools;
    std::vector<McpServer> mcp_servers;
    ApprovalMode approval_mode = ApprovalMode::Confirm;
    std::chrono::milliseconds approval_timeout{300000};
    int max_steps_per_turn = 0;
    std::size_t context_window_tokens = 128000;
};
struct Receipt { std::string operation_id; std::string input_id; bool duplicate = false; };
enum class OperationState { Accepted, Running, Succeeded, Failed, Cancelled, Indeterminate };
struct Operation {
    std::string operation_id;
    std::string turn_id;
    OperationState state = OperationState::Accepted;
    std::string final_text;
    std::string error;
    bool result_persisted = false;
};
struct Event {
    // Runtime event names plus approval_requested and operation_completed.
    std::string kind;
    std::string session_id;
    std::string operation_id;
    std::string turn_id;
    std::string text;
    std::string payload_json;
    std::optional<Approval> approval;
};

class LUBANCORE_API EventStream {
public:
    ~EventStream();
    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;
    // nullopt = timeout. Closed/overflow streams return an explicit error.
    // One or more callers may wait; each event is consumed once per subscription.
    Result<std::optional<Event>> Next(std::chrono::milliseconds timeout);
    // Wakes and waits for in-flight Next calls; no callbacks or detached threads.
    void Close();
private:
    struct Impl;
    explicit EventStream(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class Session;
};

class LUBANCORE_API Session {
public:
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    std::string id() const;
    // Durable acceptance only. Execution and completion happen on the owned worker.
    // Required nonempty key: same payload repeats return the original operation.
    Result<Receipt> Submit(std::string client_operation_id, std::string text);
    Result<std::shared_ptr<EventStream>> Subscribe(std::size_t capacity = 4096);
    std::vector<Approval> PendingApprovals() const;
    Result<void> ResolveApproval(std::string request_id, ApprovalDecision, std::string reason = {});
    Result<void> Cancel(std::string operation_id);
    Result<Operation> ReadOperation(std::string operation_id) const;
    Result<Operation> WaitResult(std::string operation_id, std::chrono::milliseconds timeout) const;
    // Rejects new work, cancels/wakes pending work, joins worker, then closes files.
    // Cooperative custom tools/backends MUST return after cancellation; Close waits
    // for them and never destroys live borrowed state or pretends a timeout stopped it.
    // Blocking lifecycle methods are rejected inside any SDK backend/tool callback;
    // do not destroy owning Runtime/Session handles from these callbacks.
    Result<void> Close();
private:
    struct Impl;
    explicit Session(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class Runtime;
};

class LUBANCORE_API Runtime {
public:
    static Result<std::unique_ptr<Runtime>> Create(RuntimeOptions);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Result<std::shared_ptr<Session>> OpenSession(SessionOptions);
    Result<void> Shutdown();
private:
    struct Impl;
    explicit Runtime(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

LUBANCORE_API std::string Version();
} // namespace lubancore
