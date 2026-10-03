#include <lubancore/core.hpp>
#include <lubancore/api.hpp>
#include <lubancore/extensions.hpp>
#include <lubancore/results.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace lubancore_consumer {
void Packages();
void Actions(const std::filesystem::path& base);
void BuiltinSearch(const std::filesystem::path& base, const std::filesystem::path& resource_root);
void MemorySeed(const std::filesystem::path& base);
void MemoryResume(const std::filesystem::path& base);
void MemorySaveSeed(const std::filesystem::path& base);
void MemorySaveResume(const std::filesystem::path& base);
void Subagents(const std::filesystem::path& base);
void SubagentSeed(const std::filesystem::path& base);
void SubagentResume(const std::filesystem::path& base);
void Lua(const std::filesystem::path& base);
void LuaSeed(const std::filesystem::path& base);
void LuaResume(const std::filesystem::path& base);
}

// Deliberately only installed public headers and the C++ standard library.
// The fixture supplies model replies; Agent, permissions, tools and persistence
// all run inside the actual SDK library.
namespace {
namespace sdk = lubancore;
namespace ext = lubancore::extensions::v1;
namespace result = lubancore::results::v1;
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
                // Exercise real process output, approval and history delivery.
                // The bounded turn gate also covers write/read and approvals;
                // the consumer CTest timeout remains 120s.
#ifdef _WIN32
                // cmd is sufficient for this echo check. The tool regressions
                // also cover Windows' default PowerShell execution path.
                return Call("run-once", "run_command", R"({"command":"echo sdk-command","shell":"cmd","timeout_ms":20000})");
#else
                return Call("run-once", "run_command", R"({"command":"echo sdk-command","timeout_ms":20000})");
#endif
            case 3:
                if (!HasReply(request, "sdk-command")) {
                    std::string details;
                    for (const auto& message : request.messages) {
                        for (const auto& reply : message.tool_replies) {
                            if (reply.call_id == "run-once") {
                                details += " is_error=" + std::to_string(reply.is_error) + " text=" + reply.text;
                            }
                        }
                    }
                    throw std::runtime_error("real command output did not reach model history:" +
                                             (details.empty() ? " missing run-once reply" : details));
                }
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
    const auto deadline = std::chrono::steady_clock::now() + 45s;
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
        tool.execute = [executed, expected = fs::weakly_canonical(paths.cwd)](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(fs::equivalent(Path(context.cwd), expected), "custom tool received another session cwd");
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
    Check(alpha_count->load() == 1 && beta_count->load() == 0,
          "approval/decline crossed sessions: alpha=" + std::to_string(alpha_count->load()) +
              " beta=" + std::to_string(beta_count->load()));
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

struct IsolationRendezvous {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned arrived = 0;
    void Meet() {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        Check(cv.wait_for(lock, 10s, [&] { return arrived == 4; }),
              "four sessions did not execute independently");
    }
};
struct IsolationProbe {
    std::atomic<int> backend_calls{0};
    std::atomic<int> tool_calls{0};
    std::atomic<int> hold_calls{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    std::atomic<bool> observed_cancel{false};
    std::atomic<int> live_backends{0};
    std::atomic<bool> backend_reentry_rejected{false};
};
class IsolationBackend final : public sdk::Backend {
public:
    IsolationBackend(std::shared_ptr<IsolationProbe> state, GenerateFunction generate)
        : state_(std::move(state)), generate_(std::move(generate)) { ++state_->live_backends; }
    ~IsolationBackend() override { --state_->live_backends; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        ++state_->backend_calls;
        return generate_(request, cancel);
    }
private:
    std::shared_ptr<IsolationProbe> state_;
    GenerateFunction generate_;
};

void FourSessionIsolation(const fs::path& base) {
    Progress("begin: FourSessionIsolation");
    const auto paths = Fresh(base, "isolation");
    const auto host_cwd = fs::current_path();
    auto runtime = Runtime(paths);
    const std::array<std::string, 4> labels{
        "ISOLATION_ALPHA", "ISOLATION_BETA", "ISOLATION_GAMMA", "ISOLATION_DELTA"};
    const std::array<fs::path, 4> directories{
        paths.cwd, paths.cwd, paths.root / "project-c", paths.root / "project-d"};
    for (const auto& directory : directories) fs::create_directories(directory);
    auto rendezvous = std::make_shared<IsolationRendezvous>();
    std::array<std::shared_ptr<IsolationProbe>, 4> probes;
    std::array<std::shared_ptr<sdk::Session>, 4> sessions;
    std::array<std::shared_ptr<sdk::EventStream>, 4> streams;
    std::array<sdk::Receipt, 4> first;
    std::array<sdk::Operation, 4> first_results;
    std::array<std::string, 4> ids;
    std::array<std::vector<sdk::Event>, 4> observed;
    const auto options_for = [&](unsigned index, bool resume) {
        const auto label = labels[index];
        const auto state = probes[index];
        auto options = Options(paths, {});
        options.cwd = Utf8(directories[index]);
        options.model = label + "_MODEL";
        options.system_prompt = label + "_SYSTEM";
        options.backend = std::make_unique<IsolationBackend>(state,
            [=](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
                Check(request.model == label + "_MODEL", "model crossed session: " + label);
                Check(request.system.find(label + "_SYSTEM") != std::string::npos,
                      "effective system prompt crossed session: " + label);
                for (const auto& foreign : labels) {
                    if (foreign == label) continue;
                    Check(request.system.find(foreign) == std::string::npos, "foreign system prompt leaked");
                    for (const auto& message : request.messages) {
                        Check(message.text.find(foreign) == std::string::npos, "foreign conversation leaked");
                        for (const auto& reply : message.tool_replies)
                            Check(reply.text.find(foreign) == std::string::npos, "foreign tool result leaked");
                    }
                }
                if (!resume && state->backend_calls.load() == 1) rendezvous->Meet();
                std::string input;
                for (const auto& message : request.messages)
                    if (message.role == "user" && !message.text.empty()) input = message.text;
                if (input == label + "/resumed") {
                    Check(HasText(request, label + "/round1"), "resume lost the original user input");
                    Check(HasText(request, label + "/done1"), "resume lost the original assistant answer");
                    Check(HasReply(request, label + "/tool1"), "resume lost the original tool result");
                    return Text(label + "/resume-done");
                }
                const bool second = input == label + "/round2";
                Check(second || input == label + "/round1", "wrong user input reached backend");
                const std::string call_id = label + (second ? "-call2" : "-call1");
                bool replied = false;
                for (const auto& message : request.messages)
                    for (const auto& reply : message.tool_replies)
                        replied = replied || reply.call_id == call_id;
                if (!replied) return Call(call_id, second && index >= 2 ? "hold" : "probe",
                                         second ? R"({"phase":"round2"})" : R"({"phase":"round1"})");
                if (index != 1 || second)
                    Check(HasReply(request, label + (second ? "/tool2" : "/tool1")), "own tool result was lost");
                return Text(label + (second ? "/done2" : "/done1"));
            });
        sdk::Tool probe;
        probe.name = "probe";
        probe.description = "Record one effect after this session's own approval.";
        probe.execute = [=](const std::string& input, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(fs::equivalent(Path(context.cwd), directories[index]), "approved tool used another project");
            Check(!context.cancellation.requested(), "another session cancelled this approved tool");
            ++state->tool_calls;
            return sdk::ToolResult{label + (input.find("round2") == std::string::npos ? "/tool1" : "/tool2"), false};
        };
        options.custom_tools.push_back(std::move(probe));
        sdk::Tool hold;
        hold.name = "hold";
        hold.description = "Keep a real SDK tool callback active until release or cancellation.";
        hold.requires_approval = false;
        hold.execute = [=](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(fs::equivalent(Path(context.cwd), directories[index]), "held callback used another project");
            ++state->hold_calls;
            state->entered.store(true);
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (!state->released.load() && !context.cancellation.requested() &&
                   std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
            state->observed_cancel.store(context.cancellation.requested());
            Check(state->released.load() || context.cancellation.requested(), "held callback timed out");
            // These borrowed fields must remain alive all the way to callback exit.
            Check(fs::equivalent(Path(context.cwd), directories[index]), "Close destroyed the borrowed tool context");
            return sdk::ToolResult{label + "/tool2", context.cancellation.requested()};
        };
        options.custom_tools.push_back(std::move(hold));
        return options;
    };
    const auto approval = [&](unsigned index, const sdk::Receipt& receipt) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto event = Take(streams[index]->Next(100ms), "isolation approval event");
            if (!event) continue;
            Check(event->session_id == ids[index], "event reached another session subscription");
            observed[index].push_back(*event);
            if (!event->approval) continue;
            Check(event->operation_id == receipt.operation_id && event->approval->operation_id == receipt.operation_id,
                  "approval reached another operation");
            Check(fs::equivalent(Path(event->approval->cwd), directories[index]), "approval described another project");
            Check(event->approval->tool_name == "probe", "unexpected isolation approval");
            return *event->approval;
        }
        throw std::runtime_error("isolation approval did not arrive");
    };
    const auto check_events = [&](unsigned index, const sdk::Receipt& receipt, const sdk::Operation& result) {
        bool complete = false;
        const auto validate = [&](const sdk::Event& event) {
            Check(event.session_id == ids[index], "event session identity crossed sessions");
            if (event.operation_id != receipt.operation_id) return;
            if (event.kind != "approval_requested")
                Check(event.turn_id == result.turn_id && !event.turn_id.empty(), "event lost the canonical turn identity");
            if (event.kind == "operation_completed") complete = true;
        };
        for (const auto& event : observed[index]) validate(event);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!complete && std::chrono::steady_clock::now() < deadline) {
            auto event = Take(streams[index]->Next(100ms), "isolation completion event");
            if (event) { observed[index].push_back(*event); validate(*event); }
        }
        Check(complete, "isolation operation did not publish its completion");
    };
    std::set<std::string> unique_ids;
    for (unsigned index = 0; index != 4; ++index) {
        probes[index] = std::make_shared<IsolationProbe>();
        sessions[index] = Take(runtime->OpenSession(options_for(index, false)), "open isolation session");
        ids[index] = sessions[index]->id();
        unique_ids.insert(ids[index]);
        streams[index] = Take(sessions[index]->Subscribe(), "subscribe isolated events");
        first[index] = Take(sessions[index]->Submit("same-client-key", labels[index] + "/round1"), "submit isolated round1");
    }
    Check(unique_ids.size() == 4, "session identities collapsed across projects");
    std::array<sdk::Approval, 4> approvals;
    for (unsigned index = 0; index != 4; ++index) approvals[index] = approval(index, first[index]);
    for (unsigned index = 0; index != 4; ++index)
        Check(!sessions[index]->ResolveApproval(approvals[(index + 1) % 4].request_id, sdk::ApprovalDecision::Accept),
              "another session resolved an approval");
    Take(sessions[0]->ResolveApproval(approvals[0].request_id, sdk::ApprovalDecision::AcceptForSession), "allow alpha for session");
    Take(sessions[1]->ResolveApproval(approvals[1].request_id, sdk::ApprovalDecision::Decline), "decline beta only");
    Take(sessions[2]->ResolveApproval(approvals[2].request_id, sdk::ApprovalDecision::Accept), "allow gamma once");
    Take(sessions[3]->Cancel(first[3].operation_id), "cancel delta only");
    for (unsigned index = 0; index != 4; ++index) {
        first_results[index] = Finished(sessions[index], first[index]);
        if (index == 3) Check(first_results[index].state == sdk::OperationState::Cancelled, "delta cancellation was lost");
        else Succeeded(first_results[index]);
        check_events(index, first[index], first_results[index]);
        Check(probes[index]->tool_calls.load() == (index == 0 || index == 2 ? 1 : 0), "approval or cancellation crossed sessions");
    }
    std::array<sdk::Receipt, 4> second;
    for (unsigned index = 0; index != 4; ++index)
        second[index] = Take(sessions[index]->Submit("same-second-key", labels[index] + "/round2"), "submit isolated round2");
    const auto beta_pending = approval(1, second[1]);
    const auto alpha_result = Finished(sessions[0], second[0]);
    Succeeded(alpha_result);
    check_events(0, second[0], alpha_result);
    Check(sessions[0]->PendingApprovals().empty() && probes[0]->tool_calls.load() == 2, "session allowance was lost");
    Take(sessions[0]->Close(), "close alpha while peers remain active");
    Check(probes[0]->live_backends.load() == 0, "closed alpha retained its execution backend");
    Check(probes[1]->live_backends.load() == 1 && sessions[1]->PendingApprovals().size() == 1,
          "closing alpha destroyed beta or resolved its pending approval");
    Take(sessions[1]->ResolveApproval(beta_pending.request_id, sdk::ApprovalDecision::Accept), "beta still owns its approval");
    const auto beta_result = Finished(sessions[1], second[1]);
    Succeeded(beta_result);
    check_events(1, second[1], beta_result);
    Check(probes[1]->tool_calls.load() == 1, "alpha session allowance leaked to beta");
    const auto entered_deadline = std::chrono::steady_clock::now() + 10s;
    while ((!probes[2]->entered.load() || !probes[3]->entered.load()) &&
           std::chrono::steady_clock::now() < entered_deadline) std::this_thread::sleep_for(2ms);
    Check(probes[2]->entered.load() && probes[3]->entered.load(), "independent project callbacks did not both start");
    Take(sessions[2]->Close(), "close gamma during its own callback");
    Check(probes[2]->observed_cancel.load() && probes[2]->live_backends.load() == 0, "gamma close did not join its callback");
    const auto gamma_result = Finished(sessions[2], second[2]);
    Check(gamma_result.state == sdk::OperationState::Cancelled, "gamma close left a noncancelled operation");
    const auto gamma_persisted = Take(sessions[2]->ReadOperation(second[2].operation_id), "read closed gamma result");
    Check(gamma_persisted.state == sdk::OperationState::Cancelled && gamma_persisted.result_persisted,
          "gamma close did not preserve its terminal result");
    Check(!sessions[2]->Submit("after-gamma-close", labels[2] + "/round3"), "closed gamma accepted new input");
    Check(!probes[3]->observed_cancel.load() && probes[3]->live_backends.load() == 1,
          "closing gamma cancelled or destroyed delta");
    Check(Take(sessions[3]->ReadOperation(second[3].operation_id), "delta remains running").state == sdk::OperationState::Running,
          "gamma close completed delta's operation");
    probes[3]->released.store(true);
    const auto delta_result = Finished(sessions[3], second[3]);
    Succeeded(delta_result);
    check_events(3, second[3], delta_result);
    Check(!probes[3]->observed_cancel.load(), "delta saw a foreign cancellation");
    // Reopen a closed same-project session while its peer still owns live state.
    auto resumed_options = options_for(0, true);
    resumed_options.system_prompt.clear(); // preserve the persisted effective prompt
    resumed_options.resume_session_id = ids[0];
    auto resumed = Take(runtime->OpenSession(std::move(resumed_options)), "resume alpha alongside live peers");
    Check(resumed->id() == ids[0], "resume replaced alpha's identity");
    const auto repeated = Take(resumed->Submit("same-client-key", labels[0] + "/round1"), "retry alpha after resume");
    Check(repeated.duplicate && repeated.operation_id == first[0].operation_id, "resume lost alpha's durable operation key");
    const auto after_resume = Take(resumed->Submit("resumed-key", labels[0] + "/resumed"), "new alpha turn after resume");
    const auto resumed_result = Finished(resumed, after_resume);
    Succeeded(resumed_result);
    Check(resumed_result.final_text == labels[0] + "/resume-done" && probes[0]->tool_calls.load() == 2,
          "resume replayed an old side effect or lost history");
    Check(fs::current_path() == host_cwd, "four sessions changed the host cwd");
    Take(runtime->Shutdown(), "shutdown isolated runtime");
    for (unsigned index = 0; index != 4; ++index)
        Check(probes[index]->live_backends.load() == 0, "Shutdown retained an isolated backend");
}

struct PublicCaptureQueries {
    struct Request {
        std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
        bool result = false;
    };
    std::atomic<bool> armed{false};
    std::atomic<unsigned> callable_queries{0};
    std::atomic<unsigned> final_queries{0};
    std::atomic<unsigned> failed_queries{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Request>> pending;
    bool stopped = false;
    std::function<bool()> query;
    std::jthread observer;
    void Start(std::function<bool()> callback) {
        query = std::move(callback);
        observer = std::jthread([this] {
            for (;;) {
                std::shared_ptr<Request> request;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return stopped || !pending.empty(); });
                    if (pending.empty()) return;
                    request = std::move(pending.front());
                    pending.pop_front();
                }
                bool result = false;
                try { result = query(); } catch (...) {}
                std::lock_guard lock(request->mutex);
                request->result = result;
                request->completed = true;
                request->cv.notify_all();
            }
        });
    }
    void Observe(bool final) noexcept {
        if (!armed.load()) return;
        if (final) ++final_queries;
        else ++callable_queries;
        try {
            auto request = std::make_shared<Request>();
            {
                std::lock_guard lock(mutex);
                if (stopped) { ++failed_queries; return; }
                pending.push_back(request);
                cv.notify_all();
            }
            std::unique_lock lock(request->mutex);
            if (!request->cv.wait_for(lock, 3s, [&] { return request->completed; }) || !request->result) ++failed_queries;
        } catch (...) { ++failed_queries; }
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            stopped = true;
            cv.notify_all();
        }
        if (observer.joinable()) observer.join();
    }
    ~PublicCaptureQueries() { Stop(); }
};
struct PublicSmallCapture {
    std::shared_ptr<PublicCaptureQueries> queries;
    std::shared_ptr<IsolationProbe> state;
    fs::path cwd;
    PublicSmallCapture(std::shared_ptr<PublicCaptureQueries> reader,
                       std::shared_ptr<IsolationProbe> owner, fs::path directory)
        : queries(std::move(reader)), state(std::move(owner)), cwd(std::move(directory)) {}
    ~PublicSmallCapture() { queries->Observe(true); }
};
struct PublicSmallTool {
    std::shared_ptr<PublicSmallCapture> capture;
    explicit PublicSmallTool(std::shared_ptr<PublicSmallCapture> probe) noexcept : capture(std::move(probe)) {}
    PublicSmallTool(const PublicSmallTool&) noexcept = default;
    PublicSmallTool(PublicSmallTool&&) noexcept = default;
    ~PublicSmallTool() { if (capture) capture->queries->Observe(false); }
    sdk::Result<sdk::ToolResult> operator()(const std::string&, const sdk::ToolContext& context) const {
        Check(fs::equivalent(Path(context.cwd), capture->cwd), "small tool capture used another cwd");
        ++capture->state->tool_calls;
        return sdk::ToolResult{"small-owned-tool-result", false};
    }
};
static_assert(sizeof(PublicSmallTool) == sizeof(std::shared_ptr<PublicSmallCapture>));
static_assert(std::is_nothrow_copy_constructible_v<PublicSmallTool>);

class FailedOpenBackend final : public sdk::Backend {
public:
    FailedOpenBackend(std::shared_ptr<IsolationProbe> state, sdk::Runtime& runtime)
        : state_(std::move(state)), runtime_(runtime) { ++state_->live_backends; }
    ~FailedOpenBackend() override {
        struct Watchdog {
            std::mutex mutex;
            std::condition_variable cv;
            bool finished = false;
        } watchdog;
        std::jthread watcher([&] {
            std::unique_lock lock(watchdog.mutex);
            if (watchdog.cv.wait_for(lock, 3s, [&] { return watchdog.finished; })) return;
            Progress("failed initialization backend destructor blocked in Runtime::Shutdown");
            // A broken cleanup guard could wait on this OpenSession's own
            // mutex. Fail this consumer process rather than leave CI hanging.
            std::_Exit(1);
        });
        const auto rejected = runtime_.Shutdown();
        state_->backend_reentry_rejected.store(!rejected && rejected.error().code == "sdk.lifecycle.reentrant");
        --state_->live_backends;
        {
            std::lock_guard lock(watchdog.mutex);
            watchdog.finished = true;
            watchdog.cv.notify_all();
        }
    }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++state_->backend_calls;
        Check(request.model == "failed-open-borrow", "unexpected model request during failed initialization");
        return Text("failed-open-backend-alive");
    }
private:
    std::shared_ptr<IsolationProbe> state_;
    sdk::Runtime& runtime_;
};

enum class SmallOpenFailure { MissingResume, RequiredMcp, InvalidOptions };
void FailedOpenSmallCapture(sdk::Runtime& runtime, const Paths& paths, SmallOpenFailure failure) {
    const std::string phase = failure == SmallOpenFailure::RequiredMcp ? "required MCP startup failure" :
        failure == SmallOpenFailure::MissingResume ? "missing resume failure" : "invalid options failure";
    Progress("begin: small callback " + phase);
    auto state = std::make_shared<IsolationProbe>();
    auto queries = std::make_shared<PublicCaptureQueries>();
    auto capture = std::make_shared<PublicSmallCapture>(queries, state, paths.cwd);
    const std::weak_ptr<PublicSmallCapture> captured = capture;
    auto backend = std::make_unique<FailedOpenBackend>(state, runtime);
    // The SDK accepts unique ownership, so retain only a plain public pointer.
    // Each destructor first checks the independent alive flag before using it.
    auto* borrowed_backend = backend.get();
    auto options = Options(paths, {});
    options.backend = std::move(backend);
    sdk::Tool tool;
    tool.name = "failed_open_probe";
    tool.description = "Keep a small callback alive through failed SDK initialization.";
    tool.requires_approval = false;
    tool.execute = PublicSmallTool(capture);
    options.custom_tools.push_back(std::move(tool));
    if (failure == SmallOpenFailure::RequiredMcp) {
        sdk::McpServer server;
        server.name = "required_missing_fixture";
        server.command = Utf8(paths.root / "absent-required-mcp.exe");
        server.tools = {"echo"};
        server.startup_timeout_ms = 1000;
        server.call_timeout_ms = 1000;
        options.mcp_servers.push_back(std::move(server));
    } else if (failure == SmallOpenFailure::MissingResume) {
        options.resume_session_id = "missing-small-callback-session";
    } else {
        options.context_window_tokens = 0;
    }
    tool.execute = {};
    capture.reset();
    queries->Start([borrowed_backend, state] {
        if (state->live_backends.load() != 1) return false;
        sdk::ModelRequest request;
        request.model = "failed-open-borrow";
        const auto result = borrowed_backend->Generate(request, {});
        return result && result->text == "failed-open-backend-alive" && state->live_backends.load() == 1;
    });
    queries->armed.store(true);
    const auto opened = runtime.OpenSession(std::move(options));
    const bool released = captured.expired();
    const auto callable_queries = queries->callable_queries.load();
    const auto final_queries = queries->final_queries.load();
    const auto failed_queries = queries->failed_queries.load();
    queries->armed.store(false);
    queries->Stop();
    Check(!opened, phase + " unexpectedly opened a session");
    const std::string expected_error = failure == SmallOpenFailure::RequiredMcp ? "sdk.mcp.start_failed" :
        failure == SmallOpenFailure::MissingResume ? "sdk.session.open_failed" : "sdk.session.invalid_options";
    Check(opened.error().code == expected_error,
          phase + " failed at another initialization boundary: " + opened.error().code);
    Check(released, phase + " retained an SDK-owned small callback");
    Check(callable_queries > 0 && final_queries == 1, phase + " did not release every callback copy");
    Check(failed_queries == 0, phase + " released its backend before a borrowing callback");
    Check(state->backend_calls.load() == static_cast<int>(callable_queries + final_queries),
          phase + " skipped an actual public backend query");
    Check(state->tool_calls.load() == 0, phase + " executed a tool without a session");
    Check(state->live_backends.load() == 0, phase + " retained its backend after returning");
    Check(state->backend_reentry_rejected.load(), phase + " restored the lifecycle guard before destroying its backend");
    Progress("completed: small callback " + phase);
}

void SmallToolCaptureLifetime(const fs::path& base) {
    Progress("begin: SmallToolCaptureLifetime");
    const auto paths = Fresh(base, "small-tool-capture");
    auto runtime = Runtime(paths);
    FailedOpenSmallCapture(*runtime, paths, SmallOpenFailure::MissingResume);
    FailedOpenSmallCapture(*runtime, paths, SmallOpenFailure::RequiredMcp);
    FailedOpenSmallCapture(*runtime, paths, SmallOpenFailure::InvalidOptions);
    auto state = std::make_shared<IsolationProbe>();
    auto queries = std::make_shared<PublicCaptureQueries>();
    auto capture = std::make_shared<PublicSmallCapture>(queries, state, paths.cwd);
    const std::weak_ptr<PublicSmallCapture> captured = capture;
    auto options = Options(paths, {});
    options.backend = std::make_unique<IsolationBackend>(state,
        [](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            if (!HasReply(request, "small-owned-tool-result")) return Call("small-call", "small_probe", "{}");
            return Text("small-capture-complete");
        });
    sdk::Tool tool;
    tool.name = "small_probe";
    tool.description = "Exercise an owned callback that fits inside std::function's inline buffer.";
    tool.requires_approval = false;
    tool.execute = PublicSmallTool(capture);
    options.custom_tools.push_back(std::move(tool));
    auto session = Take(runtime->OpenSession(std::move(options)), "open small callback session");
    // A standard library may retain inline targets in moved-from callables.
    // Clear caller copies before checking the SDK's ownership promise.
    options.custom_tools.clear();
    tool.execute = {};
    capture.reset();
    const auto receipt = Take(session->Submit("small-capture-key", "run the small callback"), "submit small callback");
    Succeeded(Finished(session, receipt));
    Check(state->tool_calls.load() == 1, "small callback did not run exactly once");
    const std::weak_ptr<sdk::Session> weak_session = session;
    queries->Start([weak_session, state, operation_id = receipt.operation_id] {
        auto owner = weak_session.lock();
        if (!owner || state->live_backends.load() != 1) return false;
        // ReadOperation is a nonblocking public query. A bounded observer also
        // detects accidentally clearing captures under Session's mutex, then
        // drains its query after that destructor returns and releases the lock.
        const auto operation = owner->ReadOperation(operation_id);
        return operation && operation->state == sdk::OperationState::Succeeded && operation->result_persisted &&
            state->live_backends.load() == 1;
    });
    queries->armed.store(true);
    const auto closed = session->Close();
    const bool released = captured.expired();
    const auto callable_queries = queries->callable_queries.load();
    const auto final_queries = queries->final_queries.load();
    const auto failed_queries = queries->failed_queries.load();
    // Disarm before reporting any failure, so retained captures still unwind
    // safely without querying a session whose backend has already been freed.
    queries->armed.store(false);
    queries->Stop();
    Take(closed, "close small callback session");
    Check(released, "Close retained the small custom tool callback");
    Check(callable_queries > 0 && final_queries == 1, "Close did not destroy every owned small callback copy");
    Check(failed_queries == 0, "small callback destructor could not query its live session before backend release");
    Check(state->live_backends.load() == 0, "Close retained the small callback backend");
    Take(runtime->Shutdown(), "shutdown small callback runtime");
    Progress("completed: SmallToolCaptureLifetime");
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
using ExtensionInvoke = std::function<sdk::Result<ext::HandlerReturn>(const ext::Context&, const ext::Input&, ext::Next)>;
class ConsumerExtension final : public ext::Instance {
public:
    explicit ConsumerExtension(ExtensionInvoke invoke, std::function<void()> destroy = {})
        : invoke_(std::move(invoke)), destroy_(std::move(destroy)) {}
    ~ConsumerExtension() override { if (destroy_) destroy_(); }
    sdk::Result<ext::HandlerReturn> Invoke(const ext::Context& context, const ext::Input& input, ext::Next next) override {
        return invoke_(context, input, std::move(next));
    }
private:
    ExtensionInvoke invoke_;
    std::function<void()> destroy_;
};
ext::HandlerDefinition ExtensionDefinition(std::string name, ext::Point point = ext::Point::PreUser,
                                          ext::Stage stage = ext::Stage::Default) {
    ext::HandlerDefinition definition;
    definition.name = std::move(name);
    definition.point = point;
    definition.stage = stage;
    definition.definition_hash = "installed-consumer-v1";
    return definition;
}
ext::Registration ExtensionRegistration(std::string id, std::vector<ext::HandlerDefinition> definitions,
                                        ExtensionInvoke invoke, std::function<void()> destroy = {}) {
    ext::Registration registration;
    registration.manifest.id = std::move(id);
    registration.manifest.version = "1.0.0";
    registration.manifest.handlers = std::move(definitions);
    registration.source_label = "relocated installed host";
    registration.factory = [invoke = std::move(invoke), destroy = std::move(destroy)](const ext::SessionContext&)
        -> sdk::Result<std::unique_ptr<ext::Instance>> {
        return std::make_unique<ConsumerExtension>(invoke, destroy);
    };
    return registration;
}
sdk::Result<ext::HandlerReturn> ExtensionContinue(ext::Next next, std::optional<std::string> candidate = std::nullopt) {
    auto result = next.Call(std::move(candidate));
    if (!result) return std::unexpected(result.error());
    if (result->kind == ext::DownstreamOutcome::Kind::Denied)
        return ext::HandlerReturn::Denied(result->code, result->message);
    if (result->kind == ext::DownstreamOutcome::Kind::Failed)
        return std::unexpected(sdk::Error{result->code, result->message});
    return ext::HandlerReturn{};
}
sdk::Operation ExtensionRun(const std::shared_ptr<sdk::Session>& session, std::string key, std::string input) {
    return Finished(session, Take(session->Submit(std::move(key), std::move(input)), "submit extension operation"));
}

void ExtensionChain(const fs::path& base) {
    Progress("begin: ExtensionChain");
    const auto paths = Fresh(base, "extension-chain");
    auto runtime = Runtime(paths);
    auto saved = std::make_shared<ext::Next>();
    auto names = std::make_shared<std::vector<std::string>>();
    auto issued = std::make_shared<std::vector<ext::Context>>();
    auto options = Options(paths, [](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        Check(HasText(request, "rewritten-public-input"), "public extension rewrite did not reach the real model request");
        Check(!HasText(request, "original-public-input"), "the original input also entered the model request");
        Check(HasText(request, "pre-public-context") && HasText(request, "post-public-context"),
              "extension material was omitted from model history");
        return Text("public-extension-answer");
    });
    auto rewrite = ExtensionDefinition("prompt.rewrite");
    rewrite.priority = 900;
    rewrite.before = {"prompt.wrap"};
    auto wrap = ExtensionDefinition("prompt.wrap");
    wrap.priority = 1;
    auto post = ExtensionDefinition("memory.post", ext::Point::PostUser);
    auto read = ExtensionDefinition("request.read", ext::Point::PreRequest, ext::Stage::Estimate);
    read.after = {"context.token_estimate"};
    options.extensions.push_back(ExtensionRegistration("public-chain", {wrap, read, rewrite, post},
        [=](const ext::Context& context, const ext::Input& input, ext::Next next) -> sdk::Result<ext::HandlerReturn> {
            Check(input.schema_version == 1, "extension JSON schema version changed");
            Check(!context.session_id.empty() && !context.operation_id.empty() && context.turn_id &&
                  !context.dispatch_id.empty() && !context.invocation_id.empty(), "extension lacks issued identities");
            names->push_back(context.hook_name);
            issued->push_back(context);
            if (context.hook_name == "prompt.rewrite") {
                *saved = next;
                auto foreign = std::async(std::launch::async, [next] { return next.Call(); }).get();
                Check(!foreign && foreign.error().code == "hook.next.wrong_thread", "cross-thread Next reached native code");
                auto result = ExtensionContinue(next, R"({"prompt":"rewritten-public-input"})");
                auto twice = saved->Call();
                Check(!twice && twice.error().code == "hook.next.already_consumed", "copied Next invoked downstream twice");
                return result;
            }
            if (context.point == ext::Point::PreRequest) {
                Check(context.stage == ext::Stage::Estimate && context.step_id && !context.request_id,
                      "frozen-request hook omitted step identity or invented an unissued request identity");
                Check(input.json.find("rewritten-public-input") != std::string::npos &&
                      input.json.find("post-public-context") != std::string::npos, "request hook saw stale body");
                return ExtensionContinue(next);
            }
            Check(input.json == R"({"prompt":"rewritten-public-input"})", "post/rewrite chain saw the wrong input");
            auto result = ExtensionContinue(next);
            if (result) result->effects.push_back({ext::EffectType::ContextAppend, context.point == ext::Point::PreUser ?
                R"({"text":"pre-public-context"})" : R"({"text":"post-public-context"})"});
            return result;
        }));
    auto session = Take(runtime->OpenSession(std::move(options)), "open extension chain session");
    const auto result = ExtensionRun(session, "public-chain-key", "original-public-input");
    Succeeded(result);
    Check(result.final_text == "public-extension-answer", "extension chain lost final response");
    Check(*names == std::vector<std::string>{"prompt.rewrite", "prompt.wrap", "memory.post", "request.read"},
          "dependency order or hook stages did not match the frozen plan");
    for (const auto& context : *issued)
        Check(context.session_id == session->id() && context.operation_id == result.operation_id &&
              context.turn_id == result.turn_id, "extension identities crossed operation/session boundaries");
    auto expired = saved->Call();
    Check(!expired && expired.error().code == "hook.next.expired", "saved continuation retained its invocation grant");
    const auto description = Take(session->DescribeExtensions(), "describe extension plan");
    Check(description.find("public-chain") != std::string::npos, "public plan omitted registered manifest");
    Take(session->Close(), "close extension chain");
    Check(Take(session->DescribeExtensions(), "describe closed extension plan") == description,
          "closed plan differs from frozen running plan");
    expired = saved->Call();
    Check(!expired && expired.error().code == "hook.next.expired", "closed session resurrected saved continuation");
    Take(runtime->Shutdown(), "shutdown extension chain runtime");
    Progress("completed: ExtensionChain");
}

void ExtensionIsolationAndResume(const fs::path& base) {
    Progress("begin: ExtensionIsolationAndResume");
    const auto paths = Fresh(base, "extension-isolation");
    auto runtime = Runtime(paths);
    auto factories = std::make_shared<std::atomic<unsigned>>(0);
    auto destroyed = std::make_shared<std::atomic<unsigned>>(0);
    const auto options_for = [&](std::string resume = {}, bool changed = false, bool absent = false) {
        auto options = Options(paths, [](const sdk::ModelRequest& request, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it)
                if (it->role == "user") return Text(it->text);
            return std::unexpected(sdk::Error{"fixture.history.empty", "no actual user message"});
        });
        options.resume_session_id = std::move(resume);
        if (absent) return options;
        auto definition = ExtensionDefinition("session.counter");
        if (changed) definition.definition_hash = "installed-consumer-v2";
        auto registration = ExtensionRegistration("public-counter", {definition}, {});
        registration.factory = [=](const ext::SessionContext& context) -> sdk::Result<std::unique_ptr<ext::Instance>> {
            Check(Path(context.cwd) == fs::weakly_canonical(paths.cwd), "factory received another project's cwd");
            ++*factories;
            auto count = std::make_shared<unsigned>(0);
            return std::make_unique<ConsumerExtension>(
                [count, id = context.session_id](const ext::Context& invocation, const ext::Input&, ext::Next next) {
                    Check(invocation.session_id == id, "per-session instance crossed session identity");
                    return ExtensionContinue(next, "{\"prompt\":\"" + id + "/" + std::to_string(++*count) + "\"}");
                }, [destroyed] { ++*destroyed; });
        };
        options.extensions.push_back(std::move(registration));
        return options;
    };
    auto alpha = Take(runtime->OpenSession(options_for()), "open extension alpha");
    auto beta = Take(runtime->OpenSession(options_for()), "open extension beta in same cwd");
    Check(alpha->id() != beta->id(), "same cwd reused an extension session");
    auto a = std::async(std::launch::async, [alpha] { return ExtensionRun(alpha, "a1", "alpha input"); });
    auto b = std::async(std::launch::async, [beta] { return ExtensionRun(beta, "b1", "beta input"); });
    Check(a.get().final_text == alpha->id() + "/1", "alpha instance state mixed with beta");
    Check(b.get().final_text == beta->id() + "/1", "beta instance state mixed with alpha");
    Check(ExtensionRun(alpha, "a2", "alpha second").final_text == alpha->id() + "/2", "instance counter reset within session");
    const auto id = alpha->id();
    Take(alpha->Close(), "close extension alpha only");
    Check(destroyed->load() == 1, "closing alpha retained its instance or released beta");
    Check(ExtensionRun(beta, "b2", "beta second").final_text == beta->id() + "/2", "alpha close stopped beta");
    auto changed = runtime->OpenSession(options_for(id, true));
    Check(!changed && changed.error().code == "sdk.extension.resume_mismatch", "changed resume plan was accepted");
    auto missing = runtime->OpenSession(options_for(id, false, true));
    Check(!missing && missing.error().code == "sdk.extension.resume_mismatch", "resume silently omitted an extension");
    auto resumed = Take(runtime->OpenSession(options_for(id)), "resume matching extension plan");
    Check(resumed->id() == id, "resume changed session identity");
    Check(ExtensionRun(resumed, "a3", "alpha resume").final_text == id + "/1",
          "resume reused old instance state rather than calling the factory");
    Check(factories->load() == 3, "failed resume instantiated an incompatible plan");
    Take(resumed->Close(), "close resumed extension");
    Take(beta->Close(), "close extension beta");
    Check(destroyed->load() == 3, "Close retained extension instances");
    Take(runtime->Shutdown(), "shutdown extension isolation runtime");
    Progress("completed: ExtensionIsolationAndResume");
}

struct ExtensionCaptureState {
    sdk::Backend* backend = nullptr;
    sdk::Runtime* runtime = nullptr;
    std::atomic<bool> alive{false}, armed{false};
    std::atomic<unsigned> final_count{0}, probes{0}, guarded_shutdowns{0}, lifetime_errors{0};
    ext::Next saved;
};
struct ExtensionCapture {
    std::shared_ptr<ExtensionCaptureState> state;
    ~ExtensionCapture() {
        if (!state->armed.load()) return;
        ++state->final_count;
        if (!state->alive.load()) { ++state->lifetime_errors; return; }
        sdk::ModelRequest request;
        request.model = "extension-lifetime-probe";
        auto result = state->backend->Generate(request, {});
        if (!result || result->text != "extension-backend-alive") ++state->lifetime_errors;
        const auto shutdown = state->runtime->Shutdown();
        if (!shutdown && shutdown.error().code == "sdk.lifecycle.reentrant") ++state->guarded_shutdowns;
        else ++state->lifetime_errors;
    }
};
class ExtensionCaptureBackend final : public sdk::Backend {
public:
    explicit ExtensionCaptureBackend(std::shared_ptr<ExtensionCaptureState> state) : state_(std::move(state)) {
        state_->alive.store(true);
    }
    ~ExtensionCaptureBackend() override { state_->alive.store(false); }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        if (request.model == "extension-lifetime-probe") { ++state_->probes; return Text("extension-backend-alive"); }
        return Text("extension-capture-answer");
    }
private:
    std::shared_ptr<ExtensionCaptureState> state_;
};
// Shared_ptr-only functors exercise std::function's inline-copy path on libc++.
struct SmallExtensionFactory {
    std::shared_ptr<ExtensionCapture> capture;
    SmallExtensionFactory(const SmallExtensionFactory&) noexcept = default;
    explicit SmallExtensionFactory(std::shared_ptr<ExtensionCapture> value) : capture(std::move(value)) {}
    sdk::Result<std::unique_ptr<ext::Instance>> operator()(const ext::SessionContext&) const {
        return std::make_unique<ConsumerExtension>(
            [owned = capture](const ext::Context&, const ext::Input&, ext::Next next) {
                owned->state->saved = next;
                return ExtensionContinue(next);
            });
    }
};
static_assert(sizeof(SmallExtensionFactory) == sizeof(std::shared_ptr<ExtensionCapture>));
static_assert(std::is_nothrow_copy_constructible_v<SmallExtensionFactory>);

void ExtensionCaptureRollback(const fs::path& base) {
    Progress("begin: ExtensionCaptureRollback");
    const auto paths = Fresh(base, "extension-captures");
    auto runtime = Runtime(paths);
    for (const bool fail : {false, true}) {
        auto state = std::make_shared<ExtensionCaptureState>();
        state->runtime = runtime.get();
        auto options = Options(paths, [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Text("unused"); });
        auto backend = std::make_unique<ExtensionCaptureBackend>(state);
        state->backend = backend.get();
        options.backend = std::move(backend);
        auto capture = std::make_shared<ExtensionCapture>();
        capture->state = state;
        std::weak_ptr<ExtensionCapture> weak = capture;
        auto registration = ExtensionRegistration("small-factory", {ExtensionDefinition("small.pass")}, {});
        registration.factory = SmallExtensionFactory(capture);
        options.extensions.push_back(std::move(registration));
        // Clear caller-side inline sources before testing the SDK's ownership.
        registration.factory = nullptr;
        capture.reset();
        if (fail) {
            auto broken = ExtensionRegistration("broken-factory", {ExtensionDefinition("broken.pass")}, {});
            broken.factory = [](const ext::SessionContext&) -> sdk::Result<std::unique_ptr<ext::Instance>> {
                return std::unexpected(sdk::Error{"fixture.factory.failed", "deliberate second-factory failure"});
            };
            options.extensions.push_back(std::move(broken));
            state->armed.store(true);
        }
        auto opened = runtime->OpenSession(std::move(options));
        if (fail) Check(!opened && opened.error().code == "sdk.extension.factory_failed", "factory failure did not fail OpenSession");
        else {
            auto session = Take(std::move(opened), "open small factory session");
            Succeeded(ExtensionRun(session, "small-factory-key", "small factory input"));
            state->armed.store(true);
            Take(session->Close(), "close small factory session");
        }
        Check(weak.expired(), "SDK retained a source factory capture after Close/rollback");
        Check(state->final_count.load() == 1 && state->probes.load() == 1 && state->guarded_shutdowns.load() == 1 &&
              state->lifetime_errors.load() == 0, "capture retired after backend or outside lifecycle guard");
        Check(!state->alive.load(), "extension cleanup retained its session backend");
        const auto expired = state->saved.Call();
        Check(!expired && expired.error().code == "hook.next.expired", "saved Next kept an extension/backend alive");
    }
    auto healthy = Take(runtime->OpenSession(Options(paths,
        [](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> { return Text("after rollback"); })),
        "open healthy session after failed extension factory");
    Succeeded(ExtensionRun(healthy, "healthy-after-rollback", "healthy input"));
    Take(healthy->Close(), "close healthy rollback session");
    Take(runtime->Shutdown(), "shutdown extension capture runtime");
    Progress("completed: ExtensionCaptureRollback");
}

void PublicExtensions(const fs::path& base) {
    ExtensionChain(base);
    ExtensionIsolationAndResume(base);
    ExtensionCaptureRollback(base);
}

const std::string kResultSecret = "fixture-known-result-key-73ad9";
std::string ResultBody() {
    std::string body = "RESULT_HEAD\n" + kResultSecret + "\n";
    for (int i = 0; i < 4000; ++i) body += "结果片段🙂\n";
    return body + "RESULT_TAIL";
}
sdk::SessionOptions ResultOptions(const Paths& paths, result::Mode mode,
                                 std::shared_ptr<std::atomic<int>> model_calls,
                                 std::shared_ptr<std::atomic<int>> tool_calls) {
    auto options = Options(paths, [model_calls](const sdk::ModelRequest&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        const int call = model_calls->fetch_add(1);
        if (call == 0) return Call("saved-result-call", "saved_result_fixture", "{}");
        if (call == 1) return Text("saved result complete");
        throw std::runtime_error("durable result query unexpectedly called the model");
    });
    options.result_policy = result::SessionResultOptions{mode, 7};
    sdk::Tool tool;
    tool.name = "saved_result_fixture";
    tool.description = "Supply a durable UTF-8 tool result.";
    tool.requires_approval = false;
    tool.execute = [tool_calls](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        ++*tool_calls;
        return sdk::ToolResult{ResultBody(), false};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}
result::NodeResultPolicy ResultNode(bool allow_full = false) {
    return {allow_full, 128, "consumer-node-v1"};
}
result::SavedSnapshot ReadSingleResult(const std::shared_ptr<sdk::Session>& session,
                                      const sdk::Receipt& receipt) {
    Succeeded(Finished(session, receipt));
    const auto summaries = Take(session->ListToolResults(receipt.operation_id), "list saved tool results");
    const auto selected = std::find_if(summaries.begin(), summaries.end(), [](const auto& summary) {
        return summary.selected && summary.identity.result_id.starts_with("res-");
    });
    Check(selected != summaries.end(), "completed operation did not expose its formally selected result");
    Check(selected->identity.session_id == session->id() &&
          selected->identity.operation_id == receipt.operation_id && !selected->identity.turn_id.empty() &&
          !selected->identity.tool_call_id.empty() && !selected->identity.persisted_event_id.empty(),
          "saved identity escaped its operation or has an incomplete durable binding");
    auto snapshot = Take(session->ReadToolResult(selected->identity), "read saved tool result");
    Check(snapshot.result().metadata_state == result::ArtifactState::Verified, "result metadata was not verified");
    Check(std::any_of(snapshot.result().channels.begin(), snapshot.result().channels.end(), [](const auto& channel) {
        return channel.artifact_verified && channel.capture_complete && channel.text && *channel.text == ResultBody();
    }), "original result did not survive persistence independently of the model preview");
    return snapshot;
}
void PublicResults(const fs::path& base) {
    Progress("begin: PublicResults");
    const auto paths = Fresh(base, "public-results");
    auto runtime = Runtime(paths);
    auto model_calls = std::make_shared<std::atomic<int>>(0);
    auto tool_calls = std::make_shared<std::atomic<int>>(0);
    auto preview = Take(runtime->OpenSession(ResultOptions(paths, result::Mode::Preview, model_calls, tool_calls)),
                        "open preview result session");
    const auto receipt = Take(preview->Submit("saved-result-key", "make durable result"), "submit durable result");
    const auto snapshot = ReadSingleResult(preview, receipt);
    auto node = ResultNode();
    auto projector = Take(result::ResultProjector::Create(node, snapshot.policy(), {kResultSecret}), "create preview projector");
    auto frozen = Take(projector->Project(snapshot), "project verified result");
    const auto wire = Take(frozen.ForTransmission(*projector), "transmit preview result");
    Check(wire.find(kResultSecret) == std::string::npos && wire.find("[REDACTED]") != std::string::npos,
          "known secret reached the preview wire");
    Check(wire.find("RESULT_HEAD") != std::string::npos && wire.find("RESULT_TAIL") == std::string::npos,
          "preview was not a single bounded prefix");
    Check(wire.find(Utf8(paths.data)) == std::string::npos, "projection exposed its local result-store path");
    const auto storage = Take(frozen.SerializeForStorage(), "serialize frozen preview");
    auto restored = Take(projector->RestoreSavedProjection(storage, snapshot), "restore frozen preview");
    Check(Take(restored.ForTransmission(*projector), "retransmit frozen preview") == wire,
          "restored record selected another preview window");
    auto changed_storage = storage;
    const auto head = changed_storage.find("RESULT_HEAD");
    Check(head != std::string::npos, "storage is missing its frozen prefix");
    changed_storage[head] = 'X';
    Check(!projector->RestoreSavedProjection(changed_storage, snapshot), "modified saved projection passed its digest");
    auto other_identity = snapshot.result().summary.identity;
    other_identity.operation_id = "another-operation";
    Check(!preview->ReadToolResult(other_identity), "arbitrary operation read another result");
    auto elevated = snapshot.policy();
    elevated.mode = result::Mode::Full;
    Check(!result::ResultProjector::Create(node, elevated, {kResultSecret}), "session full exceeded the Node permission");
    node.allow_full_tool_results = true;
    auto elevation = Take(result::ResultProjector::Create(node, elevated, {kResultSecret}), "create alternate full projector");
    Check(!elevation->Project(snapshot), "a new projector upgraded an existing preview session");
    auto changed_node = ResultNode();
    changed_node.version = "consumer-node-v2";
    auto changed = Take(result::ResultProjector::Create(changed_node, snapshot.policy(), {kResultSecret}), "create changed Node policy");
    Check(!frozen.ForTransmission(*changed), "old frozen result ignored a changed Node version");
    Take(preview->Close(), "close preview result session");
    const auto closed = Take(preview->ReadToolResult(snapshot.result().summary.identity), "read closed session result");
    Check(Take(Take(projector->RestoreSavedProjection(storage, closed), "restore after Close").ForTransmission(*projector),
               "transmit after Close") == wire, "Close invalidated frozen result queries");
    auto full_model_calls = std::make_shared<std::atomic<int>>(0);
    auto full_tool_calls = std::make_shared<std::atomic<int>>(0);
    auto full = Take(runtime->OpenSession(ResultOptions(paths, result::Mode::Full, full_model_calls, full_tool_calls)),
                     "open full result session in same cwd");
    const auto full_receipt = Take(full->Submit("saved-result-key", "make durable result"), "submit full result");
    const auto full_snapshot = ReadSingleResult(full, full_receipt);
    Check(!full->ReadToolResult(snapshot.result().summary.identity), "same cwd session read its peer result");
    auto full_projector = Take(result::ResultProjector::Create(node, full_snapshot.policy(), {kResultSecret}), "create permitted full projector");
    auto full_record = Take(full_projector->Project(full_snapshot), "project full result");
    const auto full_wire = Take(full_record.ForTransmission(*full_projector), "transmit full result");
    Check(full_wire.find("RESULT_TAIL") != std::string::npos && full_wire.find(kResultSecret) == std::string::npos,
          "full mode truncated the result or leaked its known secret");
    Check(!frozen.ForTransmission(*full_projector), "frozen record crossed into another Session");
    Take(full->Close(), "close full result session");
    Take(runtime->Shutdown(), "shutdown public result runtime");
    Check(model_calls->load() == 2 && tool_calls->load() == 1 &&
          full_model_calls->load() == 2 && full_tool_calls->load() == 1, "result queries repeated model or tool work");
    Progress("completed: PublicResults");
}

std::size_t CountText(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size()) ++count;
    return count;
}
sdk::SessionOptions SkillOptions(const Paths& paths, bool attachment, std::string system,
                                const std::shared_ptr<std::atomic<int>>& calls) {
    auto options = Options(paths, [attachment, system, calls](const sdk::ModelRequest& request, sdk::Cancellation)
        -> sdk::Result<sdk::ModelReply> {
        Check(request.system.starts_with(system), "Skills lost the effective user prompt");
        Check(CountText(request.system, "paint: fixture paint") == 1, "Skills prompt missing or appended twice");
        Check(std::count_if(request.tools.begin(), request.tools.end(), [](const auto& t) { return t.name == "skill"; }) == 1,
              "Skills tool missing or registered twice");
        if ((*calls)++ == 0) return Call("skill-call", "skill", attachment ?
            R"({"name":"paint","path":"references/live.txt"})" : R"({"name":"paint"})");
        Check(HasReply(request, attachment ? "LIVE_AFTER_RESTART" : "FROZEN_BODY_MARKER"), "real SkillTool returned wrong content");
        return Text("skill-answer");
    });
    options.system_prompt = std::move(system);
    options.skills = sdk::skills::v1::Selection{Utf8(paths.root / "skills"), {"paint"}};
    return options;
}
result::ToolResultIdentity CheckSkillResult(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt,
                                          const std::string& marker) {
    const auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "wait Skills turn");
    Check(operation.state == sdk::OperationState::Succeeded && operation.final_text == "skill-answer", operation.error);
    const auto all = Take(session->ListToolResults(receipt.operation_id), "list Skills results");
    const auto plan = Take(session->DescribeSkills(), "describe Skills result owner");
    Check(plan.entries.size() == 1 && plan.entries[0].name == "paint", "Skills result owner changed");
    const auto expected_body = marker == "FROZEN_BODY_MARKER" ?
        "技能目录: " + plan.entries[0].directory + "(技能内相对路径以此为基准)\nFROZEN_BODY_MARKER\n" :
        "技能材料 paint/references/live.txt:\nLIVE_AFTER_RESTART";
    int raw = 0, formal = 0;
    result::ToolResultIdentity identity;
    std::optional<result::ToolResultSummary> raw_row, formal_row;
    std::string raw_body, formal_body;
    for (const auto& row : all) {
        if (!row.selected || row.tool_name != "skill") continue;
        const auto snapshot = Take(session->ReadToolResult(row.identity), "read complete Skills result");
        Check(snapshot.result().summary.identity == row.identity && snapshot.result().metadata_state == result::ArtifactState::Verified,
              "Skills durable metadata is not verified");
        Check(row.identity.session_id == session->id() && row.identity.operation_id == receipt.operation_id &&
              row.identity.turn_id == operation.turn_id && !row.identity.tool_call_id.empty() &&
              !row.identity.persisted_event_id.empty() && row.attempt > 0, "Skills result identity escaped its turn");
        std::string body;
        for (const auto& channel : snapshot.result().channels) {
            if (!channel.text) continue;
            Check(channel.capture_complete && channel.artifact_verified, "Skills result is incomplete");
            Check(channel.state == result::ArtifactState::Verified, "Skills channel is not verified");
            body += *channel.text;
        }
        Check(body == expected_body, "persisted Skills result changed exact content or source directory");
        if (row.identity.result_id.starts_with("capture-")) { ++raw; raw_row = row; raw_body = body; }
        else if (row.identity.result_id.starts_with("res-")) { ++formal; identity = row.identity; formal_row = row; formal_body = body; }
        else Check(false, "unexpected selected Skills identity");
    }
    Check(raw == 1 && formal == 1, "Skills raw/formal result identities differ from the public contract");
    Check(raw_row->identity.tool_call_id == formal_row->identity.tool_call_id && raw_row->attempt == formal_row->attempt &&
          raw_row->identity.persisted_event_id != formal_row->identity.persisted_event_id &&
          raw_row->identity.result_id != formal_row->identity.result_id && raw_body == formal_body,
          "Skills raw/formal sources do not share one action or preserve equal bytes");
    return identity;
}
void SkillsSeed(const fs::path& base) {
    auto paths = PathsAt(base);
    fs::create_directories(base / "skills" / "paint" / "references");
    Write(base / "skills" / "paint" / "SKILL.md", "---\nname: paint\ndescription: fixture paint\n---\nFROZEN_BODY_MARKER\n");
    Write(base / "skills" / "paint" / "references" / "live.txt", "LIVE_BEFORE_RESTART");
    auto runtime = Runtime(paths);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto session = Take(runtime->OpenSession(SkillOptions(paths, false, "SKILLS_USER_A", calls)), "open Skills seed");
    auto plan = Take(session->DescribeSkills(), "describe Skills seed");
    Check(plan.enabled && plan.entries.size() == 1 && plan.entries[0].name == "paint" && plan.plan_sha256.size() == 64,
          "public Skills snapshot is not frozen");
    const auto receipt = Take(session->Submit("skills-seed", "load body"), "submit Skills seed");
    const auto identity = CheckSkillResult(session, receipt, "FROZEN_BODY_MARKER");
    Write(base / "skills-id.txt", session->id());
    Write(base / "skills-plan-hash.txt", plan.plan_sha256);
    Write(base / "skills-identity.txt", identity.session_id + "\n" + identity.operation_id + "\n" + identity.turn_id + "\n" +
          identity.tool_call_id + "\n" + identity.persisted_event_id + "\n" + identity.result_id + "\n");
    Take(session->Close(), "close Skills seed");
    Check(Take(session->DescribeSkills(), "describe closed Skills").plan_sha256 == plan.plan_sha256, "Close lost Skills value snapshot");
    Take(session->ReadToolResult(identity), "read closed Skills body");
    Take(runtime->Shutdown(), "shutdown Skills seed");
    Check(calls->load() == 2, "Skills seed repeated model execution");
}
void SkillsResume(const fs::path& base) {
    auto paths = PathsAt(base);
    Write(base / "skills" / "paint" / "references" / "live.txt", "LIVE_AFTER_RESTART");
    auto runtime = Runtime(paths);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = SkillOptions(paths, true, "SKILLS_USER_A", calls);
    options.system_prompt.clear();
    options.resume_session_id = Read(base / "skills-id.txt");
    auto session = Take(runtime->OpenSession(std::move(options)), "resume Skills in another process");
    Check(Take(session->DescribeSkills(), "describe resumed Skills").plan_sha256 == Read(base / "skills-plan-hash.txt"), "resume changed frozen plan");
    result::ToolResultIdentity identity;
    std::ifstream in(base / "skills-identity.txt", std::ios::binary);
    for (auto* value : {&identity.session_id, &identity.operation_id, &identity.turn_id, &identity.tool_call_id,
                        &identity.persisted_event_id, &identity.result_id}) Check(static_cast<bool>(std::getline(in, *value)), "missing Skills identity");
    Take(session->ReadToolResult(identity), "read body after process restart");
    Check(calls->load() == 0, "restart query repeated model execution");
    CheckSkillResult(session, Take(session->Submit("skills-live", "read live attachment"), "submit resumed Skills"), "LIVE_AFTER_RESTART");
    Take(session->Close(), "close resumed Skills");
    calls = std::make_shared<std::atomic<int>>(0);
    options = SkillOptions(paths, true, "SKILLS_USER_B", calls);
    options.resume_session_id = Read(base / "skills-id.txt");
    session = Take(runtime->OpenSession(std::move(options)), "replace Skills user prompt");
    CheckSkillResult(session, Take(session->Submit("skills-replaced", "read attachment again"), "submit replaced Skills"), "LIVE_AFTER_RESTART");
    Take(session->Close(), "close replaced Skills");
    calls = std::make_shared<std::atomic<int>>(0);
    options = SkillOptions(paths, true, "SKILLS_USER_B", calls);
    options.system_prompt.clear();
    options.resume_session_id = Read(base / "skills-id.txt");
    session = Take(runtime->OpenSession(std::move(options)), "preserve adopted Skills prompt");
    CheckSkillResult(session, Take(session->Submit("skills-preserved", "read attachment once more"), "submit preserved Skills"), "LIVE_AFTER_RESTART");
    Take(session->Close(), "close preserved Skills");
    Take(runtime->Shutdown(), "shutdown Skills resume");
}
void ResultSeed(const fs::path& base) {
    const auto paths = PathsAt(base);
    auto runtime = Runtime(paths);
    auto models = std::make_shared<std::atomic<int>>(0);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto session = Take(runtime->OpenSession(ResultOptions(paths, result::Mode::Preview, models, tools)), "open result seed session");
    const auto receipt = Take(session->Submit("saved-result-key", "make durable result"), "submit result seed");
    const auto snapshot = ReadSingleResult(session, receipt);
    auto projector = Take(result::ResultProjector::Create(ResultNode(), snapshot.policy(), {kResultSecret}), "create result seed projector");
    const auto frozen = Take(projector->Project(snapshot), "freeze result seed");
    const auto& id = snapshot.result().summary.identity;
    Write(base / "result-identity.txt", id.session_id + "\n" + id.operation_id + "\n" + id.turn_id + "\n" +
          id.tool_call_id + "\n" + id.persisted_event_id + "\n" + id.result_id + "\n");
    Write(base / "result-frozen.json", Take(frozen.SerializeForStorage(), "store result seed projection"));
    Write(base / "result-wire.json", Take(frozen.ForTransmission(*projector), "store result seed wire"));
    Take(session->Close(), "close result seed");
    Take(runtime->Shutdown(), "shutdown result seed");
    Check(models->load() == 2 && tools->load() == 1, "seed did not run exactly one real tool");
}
void ResultResume(const fs::path& base) {
    const auto paths = PathsAt(base);
    auto runtime = Runtime(paths);
    result::ToolResultIdentity identity;
    std::ifstream saved(base / "result-identity.txt", std::ios::binary);
    Check(saved.is_open(), "result seed identity is absent");
    for (auto* field : {&identity.session_id, &identity.operation_id, &identity.turn_id,
                       &identity.tool_call_id, &identity.persisted_event_id, &identity.result_id}) {
        Check(static_cast<bool>(std::getline(saved, *field)) && !field->empty(), "result seed identity is incomplete");
    }
    auto models = std::make_shared<std::atomic<int>>(2);
    auto tools = std::make_shared<std::atomic<int>>(0);
    auto mismatched = ResultOptions(paths, result::Mode::Full, models, tools);
    mismatched.resume_session_id = identity.session_id;
    Check(!runtime->OpenSession(std::move(mismatched)), "resume upgraded the saved preview policy");
    auto options = ResultOptions(paths, result::Mode::Preview, models, tools);
    options.result_policy.reset();
    options.resume_session_id = identity.session_id;
    auto session = Take(runtime->OpenSession(std::move(options)), "resume result session in another process");
    auto snapshot = Take(session->ReadToolResult(identity), "read result after process restart");
    Check(snapshot.policy().mode == result::Mode::Preview && snapshot.policy().version == 7,
          "omitted resume option did not preserve the frozen policy");
    auto projector = Take(result::ResultProjector::Create(ResultNode(), snapshot.policy(), {kResultSecret}), "create restart projector");
    auto frozen = Take(projector->RestoreSavedProjection(Read(base / "result-frozen.json"), snapshot), "restore result after process restart");
    Check(Take(frozen.ForTransmission(*projector), "transmit restart result") == Read(base / "result-wire.json"),
          "process restart changed the frozen preview");
    const auto duplicate = Take(session->Submit("saved-result-key", "make durable result"), "repeat result seed key");
    Check(duplicate.duplicate && duplicate.operation_id == identity.operation_id, "restart lost the source operation idempotency key");
    Take(session->Close(), "close restart result session");
    Take(runtime->Shutdown(), "shutdown restart result runtime");
    Check(models->load() == 2 && tools->load() == 0, "restart query reran the model or source tool");
}
} // namespace

int main(int argc, char** argv) {
    Progress("entered main");
    try {
        Check(argc >= 2, "usage: lubancore_consumer MODE ABSOLUTE_STATE_DIRECTORY [ABSOLUTE_INSTALLED_RESOURCE_ROOT]");
        const std::string mode = argv[1];
        Check(argc == (mode == "builtin-search" ? 4 : 3), "builtin-search requires STATE and installed ROOT; other modes require STATE only");
        const fs::path base = Path(argv[2]);
        Check(base.is_absolute(), "state directory must be absolute");
        fs::create_directories(base);
        if (mode == "smoke") {
            Check(!sdk::Version().empty(), "installed library has no version");
            FileAndCommand(base);
            SharedDirectoryIsolation(base);
            CloseAndStreams(base);
            ReentryAndOverflow(base);
            InvalidOptions(base);
        } else if (mode == "packages") { lubancore_consumer::Packages();
        } else if (mode == "isolation") {
            FourSessionIsolation(base);
            SmallToolCaptureLifetime(base);
        }
        else if (mode == "extensions") PublicExtensions(base);
        else if (mode == "actions") lubancore_consumer::Actions(base);
        else if (mode == "results") PublicResults(base);
        else if (mode == "skills-seed") SkillsSeed(base);
        else if (mode == "skills-resume") SkillsResume(base);
        else if (mode == "memory-seed") lubancore_consumer::MemorySeed(base);
        else if (mode == "memory-resume") lubancore_consumer::MemoryResume(base);
        else if (mode == "memory-save-seed") lubancore_consumer::MemorySaveSeed(base);
        else if (mode == "memory-save-resume") lubancore_consumer::MemorySaveResume(base);
        else if (mode == "subagents") lubancore_consumer::Subagents(base);
        else if (mode == "subagent-seed") lubancore_consumer::SubagentSeed(base);
        else if (mode == "subagent-resume") lubancore_consumer::SubagentResume(base);
        else if (mode == "lua") lubancore_consumer::Lua(base);
        else if (mode == "lua-seed") lubancore_consumer::LuaSeed(base);
        else if (mode == "lua-resume") lubancore_consumer::LuaResume(base);
        else if (mode == "builtin-search") lubancore_consumer::BuiltinSearch(base, Path(argv[3]));
        else if (mode == "result-seed") ResultSeed(base);
        else if (mode == "result-resume") ResultResume(base);
        else if (mode == "seed") Seed(base);
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
