#include <lubancore/core.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

// Deliberately only the installed public header and the C++ standard library.
// The fixture supplies model replies; Agent, permissions, tools and persistence
// all run inside the actual SDK library.
namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void Progress(std::string_view stage) {
    std::cerr << "[sdk-consumer] " << stage << std::endl;
}

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<class T>
T Take(sdk::Result<T> result, const std::string& action) {
    if (!result) throw std::runtime_error(action + ": " + result.error().code + " " + result.error().message);
    Progress("completed: " + action);
    return std::move(*result);
}
void Take(sdk::Result<void> result, const std::string& action) {
    if (!result) throw std::runtime_error(action + ": " + result.error().code + " " + result.error().message);
    Progress("completed: " + action);
}

std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
fs::path Path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(in.is_open(), "cannot read " + Utf8(path));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Write(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << value;
    out.close();
    Check(!out.fail(), "cannot write " + Utf8(path));
}

struct Paths {
    fs::path root;
    fs::path data;
    fs::path cwd;
    fs::path resources;
};
Paths PathsAt(const fs::path& root) {
    Paths paths{root, root / "data", root / "project", root / "resources"};
    fs::create_directories(paths.data);
    fs::create_directories(paths.cwd);
    fs::create_directories(paths.resources);
    return paths;
}
Paths Fresh(const fs::path& base, const std::string& name) {
    static std::atomic<unsigned> serial{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return PathsAt(base / (name + "-" + std::to_string(stamp) + "-" + std::to_string(++serial)));
}
std::unique_ptr<sdk::Runtime> Runtime(const Paths& paths) {
    Progress("creating runtime");
    return Take(sdk::Runtime::Create({Utf8(paths.data), Utf8(paths.resources)}), "create runtime");
}

using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class FixtureBackend final : public sdk::Backend {
public:
    explicit FixtureBackend(GenerateFunction generate) : generate_(std::move(generate)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        return generate_(request, cancel);
    }
private:
    GenerateFunction generate_;
};

sdk::SessionOptions Options(const Paths& paths, GenerateFunction generate) {
    sdk::SessionOptions options;
    options.cwd = Utf8(paths.cwd);
    options.model = "sdk-fixture";
    options.system_prompt = "Follow the explicit fixture input.";
    options.backend = std::make_unique<FixtureBackend>(std::move(generate));
    options.max_steps_per_turn = 8;
    options.approval_timeout = 10s;
    return options;
}
sdk::ModelReply Text(std::string text) {
    sdk::ModelReply reply;
    reply.text = std::move(text);
    reply.usage = sdk::Usage{11, 7};
    return reply;
}
sdk::ModelReply Call(std::string id, std::string name, std::string input) {
    sdk::ModelReply reply;
    reply.tool_calls.push_back({std::move(id), std::move(name), std::move(input)});
    reply.usage = sdk::Usage{11, 7};
    return reply;
}
bool HasText(const sdk::ModelRequest& request, const std::string& needle) {
    for (const auto& message : request.messages) {
        if (message.text.find(needle) != std::string::npos) return true;
    }
    return false;
}
bool HasReply(const sdk::ModelRequest& request, const std::string& needle) {
    for (const auto& message : request.messages) {
        for (const auto& result : message.tool_replies) {
            if (!result.is_error && result.text.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}
bool Terminal(sdk::OperationState state) {
    return state != sdk::OperationState::Accepted && state != sdk::OperationState::Running;
}
sdk::Operation Finished(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    auto operation = Take(session->WaitResult(receipt.operation_id, 15s), "wait result");
    Check(Terminal(operation.state), "wait returned a nonterminal operation");
    Check(operation.result_persisted, "terminal result was not persisted: " + operation.error);
    return operation;
}
void Succeeded(const sdk::Operation& operation) {
    Check(operation.state == sdk::OperationState::Succeeded, "operation failed: " + operation.error);
}
sdk::Approval ApprovalFor(const std::shared_ptr<sdk::Session>& session,
                         const std::shared_ptr<sdk::EventStream>& events, const std::string& operation) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = Take(events->Next(100ms), "read approval stream");
        if (event && event->approval && event->approval->operation_id == operation) {
            const auto pending = session->PendingApprovals();
            Check(std::any_of(pending.begin(), pending.end(), [&](const auto& item) {
                return item.request_id == event->approval->request_id;
            }), "approval event is absent from the pending query");
            return *event->approval;
        }
        const auto state = Take(session->ReadOperation(operation), "read pending operation");
        Check(!Terminal(state.state), "operation ended before expected approval: " + state.error);
    }
    throw std::runtime_error("approval did not arrive");
}

void FileAndCommand(const fs::path& base) {
    Progress("begin: FileAndCommand");
    const auto paths = Fresh(base, "files");
    const auto process_cwd = fs::current_path();
    auto runtime = Runtime(paths);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(paths, [calls](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        switch (calls->fetch_add(1)) {
            case 0:
                return Call("write-once", "write_file", R"({"path":"message.txt","content":"SDK file\n"})");
            case 1:
                return Call("read-back", "read_file", R"({"path":"message.txt"})");
            case 2:
                Check(HasReply(request, "SDK file"), "real read_file result did not reach model history");
                return Call("run-once", "run_command", R"({"command":"echo sdk-command","timeout_ms":5000})");
            case 3:
                Check(HasReply(request, "sdk-command"), "real command output did not reach model history");
                return Text("file command complete");
            case 4:
                Check(HasText(request, "file command complete"), "second turn lost prior assistant history");
                Check(HasText(request, "first file request"), "second turn lost prior user history");
                return Text("follow-up complete");
            default: throw std::runtime_error("duplicate submission called backend again");
        }
    });
    options.builtin_tools = {"write_file", "read_file", "run_command"};
    auto session = Take(runtime->OpenSession(std::move(options)), "open file session");
    auto events = Take(session->Subscribe(), "subscribe file events");
    const auto first = Take(session->Submit("file-key", "first file request"), "submit file request");
    const auto duplicate = Take(session->Submit("file-key", "first file request"), "duplicate while running");
    Check(duplicate.duplicate && duplicate.operation_id == first.operation_id, "running duplicate changed operation");
    auto conflict = session->Submit("file-key", "changed input");
    Check(!conflict && conflict.error().code == "operation_conflict", "same key with different input was accepted");
    std::set<std::string> approved;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!Terminal(Take(session->ReadOperation(first.operation_id), "file operation state").state)) {
        Check(std::chrono::steady_clock::now() < deadline, "file/command turn stalled");
        auto event = Take(events->Next(100ms), "file event");
        if (event && event->approval) {
            Check(event->approval->operation_id == first.operation_id, "approval belongs to another operation");
            Check(event->approval->tool_name == "write_file" || event->approval->tool_name == "run_command",
                  "unexpected capability requested approval");
            approved.insert(event->approval->tool_name);
            Take(session->ResolveApproval(event->approval->request_id, sdk::ApprovalDecision::Accept), "approve real tool");
        }
    }
    const auto result = Finished(session, first);
    Succeeded(result);
    Check(result.final_text == "file command complete", "wrong final result");
    Check(approved == std::set<std::string>{"write_file", "run_command"}, "effectful tools skipped approval");
    Check(Read(paths.cwd / "message.txt") == "SDK file\n", "write_file used the wrong cwd/content");
    Check(fs::current_path() == process_cwd, "SDK changed process cwd");
    const auto repeated = Take(session->Submit("file-key", "first file request"), "duplicate after completion");
    Check(repeated.duplicate && repeated.operation_id == first.operation_id && calls->load() == 4,
          "completed duplicate reran model/tool work");
    const auto next = Take(session->Submit("follow-up-key", "continue this conversation"), "second turn");
    Succeeded(Finished(session, next));
    Take(session->Close(), "close file session");
    Check(!session->Submit("after-close", "must reject"), "closed session accepted new input");
    Check(Take(session->ReadOperation(first.operation_id), "read after close").final_text == result.final_text,
          "close erased the query result");
    // Keep Runtime and the public Session alive: Close must release every file,
    // including operations.jsonl. Windows rename proves more than a destructor.
    fs::rename(paths.data, paths.root / "closed-data");
    fs::rename(paths.root / "closed-data", paths.data);
    Take(runtime->Shutdown(), "shutdown file runtime");
}

struct Rendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    void Meet() {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        Check(cv.wait_for(lock, 5s, [&] { return arrived == 2; }), "two same-cwd sessions did not execute concurrently");
    }
};

void SharedDirectoryIsolation(const fs::path& base) {
    Progress("begin: SharedDirectoryIsolation");
    const auto paths = Fresh(base, "shared-cwd");
    auto runtime = Runtime(paths);
    auto rendezvous = std::make_shared<Rendezvous>();
    auto alpha_count = std::make_shared<std::atomic<int>>(0);
    auto beta_count = std::make_shared<std::atomic<int>>(0);
    const auto make = [&](const std::string& label, std::shared_ptr<std::atomic<int>> executed) {
        auto steps = std::make_shared<std::atomic<int>>(0);
        auto options = Options(paths, [label, steps, rendezvous](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            const int step = steps->fetch_add(1);
            if (step == 0) rendezvous->Meet();
            if (step % 2 == 0) return Call(label + "-call-" + std::to_string(step), "probe", R"({})");
            return Text(label + " done");
        });
        sdk::Tool tool;
        tool.name = "probe";
        tool.description = "Count one explicitly approved invocation.";
        tool.execute = [executed, expected = Utf8(fs::weakly_canonical(paths.cwd))](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(context.cwd == expected, "custom tool received another session cwd");
            ++*executed;
            return sdk::ToolResult{"counted", false};
        };
        options.custom_tools.push_back(std::move(tool));
        return Take(runtime->OpenSession(std::move(options)), "open " + label);
    };
    auto alpha = make("alpha", alpha_count);
    auto beta = make("beta", beta_count);
    Check(alpha->id() != beta->id(), "same cwd collapsed two session identities");
    auto alpha_events = Take(alpha->Subscribe(), "subscribe alpha");
    auto beta_events = Take(beta->Subscribe(), "subscribe beta");
    const auto a1 = Take(alpha->Submit("shared-key", "alpha input"), "alpha first");
    const auto b1 = Take(beta->Submit("shared-key", "beta input"), "beta first");
    const auto a_approval = ApprovalFor(alpha, alpha_events, a1.operation_id);
    const auto b_approval = ApprovalFor(beta, beta_events, b1.operation_id);
    Check(!alpha->ResolveApproval(b_approval.request_id, sdk::ApprovalDecision::Accept), "another session resolved beta approval");
    Take(alpha->ResolveApproval(a_approval.request_id, sdk::ApprovalDecision::AcceptForSession), "allow alpha for session");
    Take(beta->ResolveApproval(b_approval.request_id, sdk::ApprovalDecision::Decline), "decline beta");
    Succeeded(Finished(alpha, a1));
    Succeeded(Finished(beta, b1));
    Check(alpha_count->load() == 1 && beta_count->load() == 0, "approval/decline crossed sessions");
    const auto a2 = Take(alpha->Submit("second-key", "alpha again"), "alpha second");
    const auto b2 = Take(beta->Submit("second-key", "beta again"), "beta second");
    const auto b_pending = ApprovalFor(beta, beta_events, b2.operation_id);
    Succeeded(Finished(alpha, a2));
    Check(alpha_count->load() == 2 && alpha->PendingApprovals().empty(), "alpha session allowance did not persist");
    Check(beta_count->load() == 0, "alpha allowance leaked into beta");
    Take(beta->Cancel(b2.operation_id), "cancel pending beta approval");
    Check(Finished(beta, b2).state == sdk::OperationState::Cancelled, "approval cancellation did not cancel the turn");
    Check(!beta->ResolveApproval(b_pending.request_id, sdk::ApprovalDecision::Accept), "late approval resurrected cancelled work");
    Check(!alpha->Cancel(a1.operation_id), "late cancellation accepted an already terminal operation");
    Check(Take(alpha->ReadOperation(a2.operation_id), "alpha remains complete").state == sdk::OperationState::Succeeded,
          "late cancellation damaged the next operation");
    Take(runtime->Shutdown(), "shutdown shared-cwd runtime");
}

void CloseAndStreams(const fs::path& base) {
    Progress("begin: CloseAndStreams");
    const auto paths = Fresh(base, "close");
    auto runtime = Runtime(paths);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto observed_cancel = std::make_shared<std::atomic<bool>>(false);
    auto session = Take(runtime->OpenSession(Options(paths,
        [entered, observed_cancel](const sdk::ModelRequest&, sdk::Cancellation cancel) -> sdk::Result<sdk::ModelReply> {
            entered->store(true);
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
            observed_cancel->store(cancel.requested());
            return std::unexpected(sdk::Error{"fixture.cancelled", "cooperative backend returned"});
        })), "open cancellable session");
    auto events = Take(session->Subscribe(), "subscribe before close");
    const auto receipt = Take(session->Submit("close-key", "wait for cancellation"), "submit cancellation fixture");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    Check(entered->load(), "backend did not start");
    // A fresh independent stream has no earlier queued events.
    auto waiting_stream = Take(session->Subscribe(), "subscribe waiting reader");
    auto waiting = std::async(std::launch::async, [waiting_stream]() -> sdk::Result<std::optional<sdk::Event>> {
        for (;;) {
            auto event = waiting_stream->Next(30s);
            if (!event) return event;
            Check(event->has_value(), "reader timed out before stream closure");
            // Completion events may legitimately arrive before Close closes the
            // stream. Drain them without relying on internal event names.
        }
    });
    Take(session->Close(), "close running session");
    Check(observed_cancel->load(), "Close did not signal backend cancellation");
    Check(waiting.wait_for(1s) == std::future_status::ready, "Close left Next blocked");
    Check(!waiting.get(), "closed event stream returned a successful read");
    Check(Finished(session, receipt).state == sdk::OperationState::Cancelled, "Close did not preserve cancelled terminal state");
    Check(!events->Next(0ms), "old stream remained live after session close");
    Take(session->Close(), "idempotent close");
    Take(runtime->Shutdown(), "shutdown cancellation runtime");
}

void ReentryAndOverflow(const fs::path& base) {
    Progress("begin: ReentryAndOverflow");
    const auto paths = Fresh(base, "reentry");
    auto runtime = Runtime(paths);
    auto holder = std::make_shared<std::weak_ptr<sdk::Session>>();
    auto checked = std::make_shared<std::atomic<bool>>(false);
    auto session = Take(runtime->OpenSession(Options(paths,
        [holder, checked](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            auto self = holder->lock();
            Check(static_cast<bool>(self), "fixture session disappeared");
            auto close = self->Close();
            Check(!close && close.error().code == "sdk.lifecycle.reentrant", "worker Close was not explicitly rejected");
            auto wait = self->WaitResult("unused", 1ms);
            Check(!wait && wait.error().code == "sdk.lifecycle.reentrant", "worker WaitResult could self-block");
            checked->store(true);
            return Text("reentry remained safe");
        })), "open reentry fixture");
    *holder = session;
    auto tiny = Take(session->Subscribe(1), "subscribe deliberately slow consumer");
    const auto receipt = Take(session->Submit("reentry-key", "exercise callback boundaries"), "submit reentry fixture");
    Succeeded(Finished(session, receipt));
    Check(checked->load(), "fixture did not exercise worker reentry");
    const auto overflow = tiny->Next(0ms);
    Check(!overflow && overflow.error().code == "sdk.events.overflow", "event loss was not reported explicitly");
    Succeeded(Take(session->ReadOperation(receipt.operation_id), "query despite stream overflow"));
    Take(runtime->Shutdown(), "shutdown reentry runtime");
    Check(!session->Submit("shutdown-key", "must reject"), "shutdown runtime left session writable");
}

std::size_t SessionDirectoryCount(const fs::path& data) {
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(data)) {
        if (entry.is_directory() && entry.path().filename() == "sessions") {
            for (const auto& child : fs::directory_iterator(entry.path())) if (child.is_directory()) ++count;
        }
    }
    return count;
}
void InvalidOptions(const fs::path& base) {
    Progress("begin: InvalidOptions");
    const auto paths = Fresh(base, "invalid");
    auto runtime = Runtime(paths);
    const auto plain = [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Text("unused"); };
    auto options = Options(paths, plain);
    sdk::Tool bad;
    bad.name = "malformed";
    bad.input_schema_json = R"({"type":3})";
    bad.execute = [](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> { return sdk::ToolResult{}; };
    options.custom_tools.push_back(std::move(bad));
    auto malformed = runtime->OpenSession(std::move(options));
    Check(!malformed && malformed.error().code == "sdk.tool.invalid_definition", "malformed schema did not return a stable validation error");
    auto missing = Options(paths, plain);
    missing.resume_session_id = "missing-session";
    const auto before = SessionDirectoryCount(paths.data);
    Check(!runtime->OpenSession(std::move(missing)), "missing resume silently created a new session");
    Check(SessionDirectoryCount(paths.data) == before, "failed explicit resume left a replacement session");
    auto invalid = Options(paths, plain);
    invalid.resume_session_id = "../other";
    Check(!runtime->OpenSession(std::move(invalid)), "path-shaped resume ID was accepted");
    auto relative = Options(paths, plain);
    relative.cwd = ".";
    Check(!runtime->OpenSession(std::move(relative)), "relative cwd consumed ambient process state");
    auto valid = Take(runtime->OpenSession(Options(paths, plain)), "open input validation session");
    const std::string invalid_utf8(1, static_cast<char>(0xff));
    auto invalid_text = valid->Submit("utf8-key", invalid_utf8);
    Check(!invalid_text && invalid_text.error().code == "sdk.input.invalid_utf8", "invalid text escaped the Result error boundary");
    auto invalid_key = valid->Submit(invalid_utf8, "valid text");
    Check(!invalid_key && invalid_key.error().code == "sdk.input.invalid_utf8", "invalid operation key was accepted");
    const auto corrected = Take(valid->Submit("utf8-key", "valid text"), "submit corrected input under the same key");
    Check(!corrected.duplicate, "rejected input polluted durable idempotency");
    Succeeded(Finished(valid, corrected));
    Take(runtime->Shutdown(), "shutdown invalid-options runtime");
}

void Seed(const fs::path& base) {
    Progress("begin: Seed");
    const auto paths = Fresh(base, "restart");
    auto runtime = Runtime(paths);
    auto step = std::make_shared<int>(0);
    auto options = Options(paths, [step](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return (*step)++ == 0 ? Call("seed-stamp", "stamp", R"({})") : Text("seed final marker");
    });
    sdk::Tool stamp;
    stamp.name = "stamp";
    stamp.description = "Append one byte to the external side-effect witness.";
    stamp.requires_approval = false;
    stamp.execute = [](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
        std::ofstream out(Path(context.cwd) / "effect-count.txt", std::ios::binary | std::ios::app);
        out << 'x';
        out.close();
        Check(!out.fail(), "side-effect witness was not written");
        return sdk::ToolResult{"stamped", false};
    };
    options.custom_tools.push_back(std::move(stamp));
    auto session = Take(runtime->OpenSession(std::move(options)), "open seed session");
    const auto receipt = Take(session->Submit("restart-key", "remember sdk restart token"), "submit seed");
    const auto result = Finished(session, receipt);
    Succeeded(result);
    Check(Read(paths.cwd / "effect-count.txt") == "x", "seed tool was not invoked exactly once");
    Write(paths.root / "identity.txt", session->id() + "\n" + receipt.operation_id + "\n" + result.turn_id + "\n");
    Take(session->Close(), "close seed session");
    Take(runtime->Shutdown(), "shutdown seed runtime");
    Write(base / "restart-active.txt", Utf8(paths.root.filename()));
}
void Resume(const fs::path& base) {
    Progress("begin: Resume");
    const auto leaf = Read(base / "restart-active.txt");
    Check(!leaf.empty() && leaf.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos,
          "invalid restart fixture directory");
    const auto paths = PathsAt(base / Path(leaf));
    std::ifstream identity(paths.root / "identity.txt");
    std::string session_id, operation_id, turn_id;
    std::getline(identity, session_id);
    std::getline(identity, operation_id);
    std::getline(identity, turn_id);
    Check(!session_id.empty() && !operation_id.empty() && !turn_id.empty(), "missing seed identity");
    auto runtime = Runtime(paths);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(paths, [calls](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        Check(HasText(request, "remember sdk restart token"), "V3 resume lost prior input");
        Check(HasText(request, "seed final marker"), "V3 resume lost prior assistant output");
        return Text("resumed final marker");
    });
    options.resume_session_id = session_id;
    auto session = Take(runtime->OpenSession(std::move(options)), "strict V3 resume in a new process");
    Check(session->id() == session_id, "V3 resume changed session ID");
    const auto old = Take(session->ReadOperation(operation_id), "read persisted old result");
    Succeeded(old);
    Check(old.result_persisted && old.final_text == "seed final marker", "old final result did not survive process restart");
    const auto duplicate = Take(session->Submit("restart-key", "remember sdk restart token"), "retry original key after restart");
    Check(duplicate.duplicate && duplicate.operation_id == operation_id, "restart washed away idempotency");
    Check(calls->load() == 0, "duplicate restarted execution");
    Check(!session->Submit("restart-key", "different intent"), "restart lost same-key conflict detection");
    const auto receipt = Take(session->Submit("after-restart-key", "continue after restart"), "new turn after restart");
    const auto result = Finished(session, receipt);
    Succeeded(result);
    Check(result.turn_id != turn_id, "same-ID resume reused a turn ID from the previous process");
    Check(result.final_text == "resumed final marker" && calls->load() == 1, "resume did not execute exactly the new turn");
    Check(Read(paths.cwd / "effect-count.txt") == "x", "resume replayed an old tool side effect");
    Take(session->Close(), "close resumed session");
    fs::rename(paths.data, paths.root / "closed-data");
    fs::rename(paths.root / "closed-data", paths.data);
    Take(runtime->Shutdown(), "shutdown resumed runtime");
}

void RecoverySeed(const fs::path& base) {
    Progress("begin: RecoverySeed");
    const auto paths = Fresh(base, "recovery");
    auto runtime = Runtime(paths);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto session = Take(runtime->OpenSession(Options(paths,
        [entered](const sdk::ModelRequest&, sdk::Cancellation cancel) -> sdk::Result<sdk::ModelReply> {
            entered->store(true);
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
            return std::unexpected(sdk::Error{"fixture.interrupted", "first request is deliberately unfinished"});
        })), "open interrupted fixture");
    const auto running = Take(session->Submit("running-key", "first request was dispatched"), "submit dispatched input");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!entered->load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    Check(entered->load(), "first input never dispatched");
    // One worker is blocked in Generate. The second durable receipt therefore
    // cannot have been dispatched before this process exits.
    const auto queued = Take(session->Submit("queued-key", "second request was only accepted"), "submit queued input");
    Check(Take(session->ReadOperation(queued.operation_id), "read queued input").state == sdk::OperationState::Accepted,
          "queued fixture unexpectedly started");
    Write(paths.root / "identity.txt", session->id() + "\n" + running.operation_id + "\n" + queued.operation_id + "\n");
    Write(base / "recovery-active.txt", Utf8(paths.root.filename()));
    // Intentional process-loss fixture. Do not close Session/Runtime: that would
    // turn the test into clean shutdown and erase the recovery window.
    Progress("simulating process loss via _Exit");
    std::_Exit(0);
}

void RecoveryResume(const fs::path& base) {
    Progress("begin: RecoveryResume");
    const auto leaf = Read(base / "recovery-active.txt");
    Check(!leaf.empty() && leaf.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos,
          "invalid recovery fixture directory");
    const auto paths = PathsAt(base / Path(leaf));
    std::ifstream identity(paths.root / "identity.txt");
    std::string session_id, running_id, queued_id;
    std::getline(identity, session_id);
    std::getline(identity, running_id);
    std::getline(identity, queued_id);
    Check(!session_id.empty() && !running_id.empty() && !queued_id.empty(), "missing interrupted identities");
    auto runtime = Runtime(paths);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(paths, [calls](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        Check(HasText(request, "second request was only accepted"), "recovery lost the durable queued input");
        return Text("queued request recovered once");
    });
    options.resume_session_id = session_id;
    auto session = Take(runtime->OpenSession(std::move(options)), "resume interrupted session");
    Check(session->id() == session_id, "interrupted resume replaced session identity");
    const auto unknown = Take(session->ReadOperation(running_id), "read dispatched-without-final input");
    Check(unknown.state == sdk::OperationState::Indeterminate && !unknown.result_persisted,
          "unfinished dispatched operation was treated as completed or safely repeatable");
    const auto queued = Take(session->WaitResult(queued_id, 15s), "wait automatically recovered input");
    Succeeded(queued);
    Check(queued.result_persisted && queued.final_text == "queued request recovered once", "accepted input did not finish durably");
    const auto same_running = Take(session->Submit("running-key", "first request was dispatched"), "retry uncertain input");
    const auto same_queued = Take(session->Submit("queued-key", "second request was only accepted"), "retry recovered input");
    Check(same_running.duplicate && same_running.operation_id == running_id, "uncertain operation lost its original key");
    Check(same_queued.duplicate && same_queued.operation_id == queued_id, "queued operation got a new identity");
    Check(Take(session->ReadOperation(running_id), "uncertain state remains").state == sdk::OperationState::Indeterminate,
          "retry replayed the uncertain operation");
    Take(session->Close(), "close recovered session");
    Check(calls->load() == 1, "recovery executed more than the single accepted input");
    Take(runtime->Shutdown(), "shutdown interrupted recovery runtime");
}
} // namespace

int main(int argc, char** argv) {
    Progress("entered main");
    try {
        Check(argc == 3, "usage: lubancore_consumer smoke|seed|resume|recovery-seed|recovery-resume ABSOLUTE_STATE_DIRECTORY");
        const fs::path base = Path(argv[2]);
        Check(base.is_absolute(), "state directory must be absolute");
        fs::create_directories(base);
        const std::string mode = argv[1];
        if (mode == "smoke") {
            Check(!sdk::Version().empty(), "installed library has no version");
            FileAndCommand(base);
            SharedDirectoryIsolation(base);
            CloseAndStreams(base);
            ReentryAndOverflow(base);
            InvalidOptions(base);
        } else if (mode == "seed") Seed(base);
        else if (mode == "resume") Resume(base);
        else if (mode == "recovery-seed") RecoverySeed(base);
        else if (mode == "recovery-resume") RecoveryResume(base);
        else throw std::runtime_error("unknown mode: " + mode);
        std::cout << "installed SDK consumer " << mode << " passed" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "installed SDK consumer failed: " << error.what() << '\n';
        return 1;
    }
}
