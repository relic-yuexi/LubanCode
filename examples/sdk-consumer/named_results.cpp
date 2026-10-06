// Installed SDK headers and STL only. Every read below uses actual second-root
// bytes; a provider map never substitutes a prebuilt tool reply for disk data.
#include <lubancore/core.hpp>
#include <lubancore/named_results.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace blob = sdk::named_results::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error("named-results: " + message); }
template<class T> T Take(sdk::Result<T> value, const char* where) {
    if (!value) throw std::runtime_error(std::string("named-results: ") + where + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* where) {
    if (!value) throw std::runtime_error(std::string("named-results: ") + where + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string Quote(const std::string& value) {
#ifdef _WIN32
    Check(value.find_first_of("\"%\r\n") == std::string::npos, "unsafe probe argument");
    return '"' + value + '"';
#else
    std::string result = "'";
    for (const char c : value) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
#endif
}
std::string JsonString(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        Check(c >= 32, "unexpected control byte in argument"); out.push_back(static_cast<char>(c));
    }
    return out + '"';
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); Check(input.is_open(), "missing file " + Utf8(path));
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    Check(!input.bad(), "file read failed"); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); Check(output.is_open(), "file open failed");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush(); Check(output.good(), "file flush failed"); output.close(); Check(!output.fail(), "file close failed");
}
std::string Key(const blob::Scope& scope) { return scope.workspace_key + "\n" + scope.session_id; }
enum class Fault { None, AfterPublish, SecondReject, WrongScope, OversizeReceipt, OpenThrow, FirstReject };
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    fs::path root;
    std::map<std::string, fs::path> directories;
    std::vector<blob::Reference> publications, reads;
    std::size_t opens = 0, calls = 0, stores_destroyed = 0, providers_destroyed = 0;
    bool hold_open = false, hold_read = false, hold_write = false;
    bool open_entered = false, read_entered = false, write_entered = false;
    bool release_open = false, release_read = false, release_write = false;
    Fault fault = Fault::None;
    std::function<void()> on_open, on_read, on_write;
    void Release() {
        std::lock_guard lock(mutex); release_open = release_read = release_write = true; cv.notify_all();
    }
    bool Await(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex); return cv.wait_for(lock, timeout, predicate);
    }
    fs::path Directory(const blob::Scope& scope) {
        std::lock_guard lock(mutex);
        const auto key = Key(scope);
        auto found = directories.find(key);
        if (found != directories.end()) return found->second;
        const auto directory = root / ("scope-" + std::to_string(directories.size()));
        fs::create_directories(directory); directories.emplace(key, directory); return directory;
    }
};
class Store final : public blob::Store {
public:
    Store(std::shared_ptr<State> state, blob::OpenRequest request)
        : state_(std::move(state)), request_(std::move(request)), directory_(state_->Directory(request_.scope)) {}
    ~Store() override { std::lock_guard lock(state_->mutex); ++state_->stores_destroyed; state_->cv.notify_all(); }
    blob::WriteReceipt PublishNew(blob::WriteRequest request) override {
        Check(request.reference.scope == request_.scope && request.reference.binding_id == request_.binding_id,
              "publish crossed actual locked scope");
        Check(request.bytes.size() == request.reference.bytes && !request.request_key.empty(), "publish bytes/key changed");
        Check(request.reference.logical_name.find_first_of("/\\:") == std::string::npos, "nonlogical publish name");
        blob::WriteReceipt result; result.reference = request.reference; result.request_key = request.request_key;
        Fault fault; std::size_t call; std::function<void()> callback;
        {
            std::unique_lock lock(state_->mutex); call = ++state_->calls; fault = state_->fault; callback = state_->on_write;
            if (state_->hold_write && call == 1) {
                state_->write_entered = true; state_->cv.notify_all();
                state_->cv.wait(lock, [&] { return state_->release_write; });
            }
        }
        if (callback) callback();
        if (fault == Fault::FirstReject && call == 1) { result.error = {"fixture.known_rejection", {}}; return result; }
        if (fault == Fault::SecondReject && call == 2) { result.error = {"fixture.known_rejection", {}}; return result; }
        // A newly created private directory exclusively reserves this temporary
        // name. Hard-link publication is create-new on both native platforms.
        fs::path temporary;
        for (unsigned n = 0; n < 100; ++n) {
            const auto candidate = directory_ / (".publish-" + std::to_string(call) + "-" + std::to_string(n));
            if (fs::create_directory(candidate)) { temporary = candidate; break; }
        }
        Check(!temporary.empty(), "exclusive temporary reservation failed");
        const auto body = temporary / "body"; Write(body, request.bytes);
        std::error_code error;
        fs::create_hard_link(body, directory_ / request.reference.logical_name, error);
        fs::remove(body); fs::remove(temporary);
        if (error) { result.error = {"fixture.name_conflict", error.message()}; return result; }
        {
            std::lock_guard lock(state_->mutex); state_->publications.push_back(request.reference);
        }
        Check(Bytes(directory_ / request.reference.logical_name) == request.bytes, "published bytes differ");
        if (fault == Fault::AfterPublish && call == 1) throw std::runtime_error("actual host publication completed before exception");
        result.state = blob::CommitState::Committed;
        // Closed streams and an acknowledged namespace link survive this process
        // exiting; this fixture does not claim machine/power-loss confirmation.
        result.confirmed_durability = blob::Durability::ProcessCrash;
        if (fault == Fault::WrongScope && call == 1) result.reference.scope.session_id += "-foreign";
        if (fault == Fault::OversizeReceipt && call == 1) result.error.message.assign(8193, 'E');
        return result;
    }
    sdk::Result<std::string> Read(blob::Reference reference, std::size_t cap) override {
        Check(reference.scope == request_.scope && reference.binding_id == request_.binding_id, "read crossed actual scope");
        Check(reference.bytes <= cap, "read exceeded supplied cap");
        std::function<void()> callback;
        {
            std::unique_lock lock(state_->mutex); state_->reads.push_back(reference); callback = state_->on_read;
            if (state_->hold_read) {
                state_->read_entered = true; state_->cv.notify_all();
                state_->cv.wait(lock, [&] { return state_->release_read; });
            }
        }
        if (callback) callback();
        const auto path = directory_ / reference.logical_name;
        if (!fs::is_regular_file(path)) return std::unexpected(sdk::Error{"named_result.missing", {}});
        if (fs::file_size(path) > cap) return std::unexpected(sdk::Error{"fixture.read_limit", {}});
        return Bytes(path);
    }
    sdk::Result<blob::NameSnapshot> SnapshotNames(blob::ListLimits limits) override {
        blob::NameSnapshot result{request_.scope, request_.binding_id, "actual-file-inventory-v1", true, {}};
        std::size_t bytes = 0;
        for (const auto& entry : fs::directory_iterator(directory_)) {
            const auto name = Utf8(entry.path().filename()); bytes += name.size();
            if (name.size() > limits.name_bytes || bytes > limits.total_name_bytes || result.names.size() >= limits.entries)
                return std::unexpected(sdk::Error{"fixture.inventory_limit", {}});
            result.names.push_back(name);
        }
        std::sort(result.names.begin(), result.names.end());
        std::uint64_t version = 1469598103934665603ull;
        for (const auto& name : result.names) {
            for (const unsigned char byte : name) { version ^= byte; version *= 1099511628211ull; }
            version ^= 0; version *= 1099511628211ull;
        }
        result.token = "inventory-" + std::to_string(version); // view token, never an authentication hash
        return result;
    }
private:
    std::shared_ptr<State> state_;
    blob::OpenRequest request_;
    fs::path directory_;
};
class Provider final : public blob::Provider {
public:
    explicit Provider(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~Provider() override { std::lock_guard lock(state_->mutex); ++state_->providers_destroyed; state_->cv.notify_all(); }
    sdk::Result<std::unique_ptr<blob::Store>> Open(blob::OpenRequest request) override {
        std::function<void()> callback; Fault fault;
        {
            std::unique_lock lock(state_->mutex); const auto ordinal = ++state_->opens; callback = state_->on_open; fault = state_->fault;
            if (state_->hold_open && ordinal == 1) {
                state_->open_entered = true; state_->cv.notify_all();
                state_->cv.wait(lock, [&] { return state_->release_open; });
            }
        }
        if (callback) callback();
        if (fault == Fault::OpenThrow) throw std::runtime_error("controlled host factory exception");
        return std::unique_ptr<blob::Store>(std::make_unique<Store>(state_, std::move(request)));
    }
private:
    std::shared_ptr<State> state_;
};

struct Script {
    std::mutex mutex;
    unsigned turns = 0, summary_calls = 0;
    bool summary_material_seen = false, summary_adopted = false, historical_reply_seen = false;
    std::string command, expected_history, prefix;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<Script> script) : script_(std::move(script)) {
        static std::atomic<unsigned> serial{0}; script_->prefix = "named-call-" + std::to_string(++serial) + "-";
    }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        std::lock_guard lock(script_->mutex);
        if (request.system.find("Summarize already-executed tool evidence") != std::string::npos) {
            ++script_->summary_calls; Check(request.tools.empty(), "summary was allowed to execute tools");
            Check(request.max_output_tokens == 1024, "summary lost its actual requested output budget");
            Check(request.messages.size() == 1 && request.messages.front().role == "user" &&
                  request.messages.front().tool_calls.empty() && request.messages.front().tool_replies.empty(),
                  "summary Generate arguments changed the exact material-only request");
            for (const auto& message : request.messages)
                script_->summary_material_seen |= message.text.find("SUMMARY-EVIDENCE") != std::string::npos;
            return sdk::ModelReply{R"({"summary":"named summary accepted","side_effects":[],"open_items":[],"evidence":["SUMMARY-EVIDENCE"]})", {}, {}};
        }
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies) {
            script_->historical_reply_seen |= !script_->expected_history.empty() && reply.call_id == script_->expected_history;
            script_->summary_adopted |= reply.text.find("named summary accepted") != std::string::npos &&
                reply.text.find("source_result_event_ref") != std::string::npos;
        }
        const auto step = script_->turns++;
        if (step % 2 == 1) return sdk::ModelReply{"done", {}, {}};
        std::string text;
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it)
            if (it->role == "user" && !it->text.empty()) { text = it->text; break; }
        const auto id = script_->prefix + std::to_string(step);
        if (text == "job") return sdk::ModelReply{{}, {{id, "run_command", script_->command}}, {}};
        if (text == "summary") return sdk::ModelReply{{}, {{id, "large_evidence", "{}"}}, {}};
        return sdk::ModelReply{{}, {{id, "read_file", R"({"path":"input.txt"})"}}, {}};
    }
private:
    std::shared_ptr<Script> script_;
};
struct Task {
    std::thread thread;
    std::atomic<bool> done{false};
    std::exception_ptr error;
};
struct World {
    fs::path root, project, resources, probe;
    std::shared_ptr<State> state = std::make_shared<State>();
    std::unique_ptr<sdk::Runtime> runtime;
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<std::shared_ptr<Task>> tasks;
    World(const fs::path& base, const fs::path& source_probe) {
        static std::atomic<unsigned> serial{0};
        Check(base.is_absolute(), "absolute state required");
        root = base / ("named-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        project = root / "project"; resources = root / "resources"; state->root = root / "external";
        fs::create_directories(project); fs::create_directories(resources); fs::create_directories(state->root);
        Write(project / "input.txt", "ACTUAL-NAMED-TOOL-EVIDENCE\n");
        if (!source_probe.empty()) {
            Check(source_probe.is_absolute() && fs::is_regular_file(source_probe), "actual probe required");
            probe = root / source_probe.filename(); Check(fs::copy_file(source_probe, probe), "probe copy failed");
            fs::permissions(probe, fs::status(source_probe).permissions());
        }
        runtime = Take(sdk::Runtime::Create({Utf8(root / "state"), Utf8(resources)}), "create runtime");
    }
    ~World() {
        state->Release();
        for (auto& task : tasks) if (task->thread.joinable()) task->thread.join();
        { std::lock_guard lock(state->mutex); state->on_open = {}; state->on_read = {}; state->on_write = {}; }
        if (runtime) (void)runtime->Shutdown();
        sessions.clear(); runtime.reset();
        std::error_code error; fs::remove_all(root, error);
    }
    std::shared_ptr<Task> Run(std::function<void()> action) {
        auto task = std::make_shared<Task>(); tasks.push_back(task);
        task->thread = std::thread([task, state = state, action = std::move(action)] {
            try { action(); } catch (...) { task->error = std::current_exception(); }
            { std::lock_guard lock(state->mutex); task->done.store(true, std::memory_order_release); state->cv.notify_all(); }
        });
        return task;
    }
    void Join(const std::shared_ptr<Task>& task) {
        Check(state->Await([&] { return task->done.load(std::memory_order_acquire); }), "actual task did not retire");
        task->thread.join(); if (task->error) std::rethrow_exception(task->error);
    }
    sdk::SessionOptions Options(const std::shared_ptr<Script>& script, bool jobs = false) {
        sdk::SessionOptions options; options.cwd = Utf8(project); options.model = "named-fixture";
        options.system_prompt = "Use the explicitly declared tools, then report completion.";
        options.backend = std::make_unique<Backend>(script); options.approval_mode = sdk::ApprovalMode::Yolo;
        options.builtin_tools = {"read_file"}; options.max_steps_per_turn = 4;
        options.named_results.emplace(); options.named_results->binding_id = "actual-second-root-v1";
        options.named_results->provider = std::make_unique<Provider>(state);
        if (jobs) { options.builtin_tools.push_back("run_command"); options.command_jobs = sdk::jobs::v1::CommandOptions{30000, 20000, 65536, 1, 8}; }
        return options;
    }
    std::shared_ptr<sdk::Session> Open(sdk::SessionOptions options) {
        auto session = Take(runtime->OpenSession(std::move(options)), "open session"); sessions.push_back(session); return session;
    }
    fs::path SessionDirectory(const std::string& id) const {
        for (const auto& entry : fs::recursive_directory_iterator(root / "state"))
            if (entry.is_regular_file() && entry.path().filename() == id + ".jsonl") return entry.path().parent_path();
        throw std::runtime_error("named-results: actual Session journal missing");
    }
    void NoMirrors() const {
        for (const auto& entry : fs::recursive_directory_iterator(root / "state")) {
            const auto name = Utf8(entry.path().filename());
            Check(!name.starts_with("res-") && !name.starts_with("capture-") && !name.starts_with("job-admission-"), "named bytes mirrored into Session tree");
        }
    }
    std::string Command() const {
        Check(!probe.empty(), "Job acceptance needs explicit real probe");
        std::string command = Quote(Utf8(probe));
        for (const auto& arg : std::vector<std::string>{"named.started", "named.done", "8194", "0", "named-job", "J", "-"}) command += " " + Quote(arg);
        return "{\"command\":" + JsonString(command) + ",\"execution_mode\":\"session_job\",\"shell\":" +
#ifdef _WIN32
            "\"cmd\"}";
#else
            "\"sh\"}";
#endif
    }
};

sdk::Operation Run(const std::shared_ptr<sdk::Session>& session, const std::string& id, const std::string& text = "read") {
    auto receipt = Take(session->Submit(id, text), "submit");
    return Take(session->WaitResult(receipt.operation_id, 20s), "wait result");
}
sdk::results::v1::SavedSnapshot Saved(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation) {
    Check(operation.state == sdk::OperationState::Succeeded, "operation failed: " + operation.error);
    auto list = Take(session->ListToolResults(operation.operation_id), "list results");
    Check(!list.empty(), "real tool result index empty");
    auto snapshot = Take(session->ReadToolResult(list.back().identity), "read actual result");
    Check(snapshot.result().metadata_state == sdk::results::v1::ArtifactState::Verified, "metadata not verified");
    Check(!snapshot.result().channels.empty(), "saved channels empty");
    for (const auto& channel : snapshot.result().channels) Check(channel.artifact_verified, "channel not verified");
    return snapshot;
}

void Roundtrip(World& world) {
    auto script = std::make_shared<Script>(); auto session = world.Open(world.Options(script));
    const auto operation = Run(session, "roundtrip"); auto saved = Saved(session, operation);
    const auto identity = saved.result().summary.identity;
    Check(std::any_of(saved.result().channels.begin(), saved.result().channels.end(), [](const auto& channel) {
        return channel.text && channel.text->find("ACTUAL-NAMED-TOOL-EVIDENCE") != std::string::npos;
    }), "saved tool bytes did not come from the actual input file");
    auto projector = Take(sdk::results::v1::ResultProjector::Create({false, 4096, "named-node-v1"}, saved.policy()), "projector");
    auto projection = Take(projector->Project(saved), "project actual result");
    Check(!Take(projection.ForTransmission(*projector), "wire projection").empty(), "empty bounded projection");
    Take(session->Close(), "close first scene");
    auto after = Take(session->ReadToolResult(identity), "read after close");
    Check(after.result().metadata_sha256 == saved.result().metadata_sha256 && after.result().metadata_state == sdk::results::v1::ArtifactState::Verified,
          "closed scene lost immutable read ownership");
    Take(world.runtime->Shutdown(), "retire first Runtime");
    world.runtime = Take(sdk::Runtime::Create({Utf8(world.root / "state"), Utf8(world.resources)}), "fresh resume Runtime");
    std::size_t writes;
    { std::lock_guard lock(world.state->mutex); writes = world.state->calls; }
    auto resumed_script = std::make_shared<Script>(); resumed_script->expected_history = identity.tool_call_id;
    auto options = world.Options(resumed_script); options.resume_session_id = session->id(); options.system_prompt.clear();
    auto resumed = world.Open(std::move(options));
    auto restored = Take(resumed->ReadToolResult(identity), "restore actual result");
    Check(restored.result().metadata_sha256 == saved.result().metadata_sha256, "same-ID metadata identity drifted");
    { std::lock_guard lock(world.state->mutex); Check(world.state->calls == writes, "recovery republished old material"); }
    auto next = Saved(resumed, Run(resumed, "after-resume"));
    Check(next.result().summary.identity.result_id != identity.result_id, "resume reused an occupied name");
    { std::lock_guard lock(resumed_script->mutex); Check(resumed_script->historical_reply_seen, "real restored model request lost historical tool reply"); }
    world.NoMirrors(); Take(resumed->Close(), "close restored scene");
}

void Jobs(World& world) {
    auto script = std::make_shared<Script>(); script->command = world.Command();
    auto session = world.Open(world.Options(script, true));
    const auto first = Saved(session, Run(session, "before-job"));
    const auto parent = Run(session, "job-parent", "job");
    Check(parent.state == sdk::OperationState::Succeeded, "Job parent failed: " + parent.error);
    auto list = Take(session->ListJobs(), "actual Job list"); Check(list.size() == 1, "missing actual registered Job");
    const auto id = list.front().identity;
    const auto completed = Take(session->WaitJob(id, 20s), "actual command completion");
    Check(completed.state == "succeeded", "actual Job failed: " + completed.gap);
    Check(Bytes(world.project / "named.done") == "named-job", "real command did not finish");
    const auto started = Bytes(world.project / "named.started"); const auto newline = started.find('\n');
    Check(newline != std::string::npos && started.substr(0, newline) == "named-job" &&
        fs::equivalent(fs::u8path(started.substr(newline + 1)), world.project), "command ran in another project");
    const auto preview = Take(session->ReadJobPreview(id), "actual Job preview");
    Check(preview.text.find('J') != std::string::npos, "Job preview did not read actual captured bytes");
    Check(preview.text.size() <= 4096 && preview.preview_truncated, "named SPI changed the bounded local Job preview");
    Check(!session->ReadJobPreview(id, 4097), "oversized local Job preview request was accepted");
    const auto last = Saved(session, Run(session, "after-job"));
    Check(first.result().summary.identity.result_id != preview.result_id && preview.result_id != last.result().summary.identity.result_id &&
        first.result().summary.identity.result_id != last.result().summary.identity.result_id, "foreground/Job/foreground number pools collided");
    bool admission = false;
    { std::lock_guard lock(world.state->mutex); for (const auto& ref : world.state->publications) admission |= ref.logical_name.starts_with("job-admission-"); }
    Check(admission, "Job parent handle bypassed the named provider");
    world.NoMirrors(); Take(session->Close(), "close Job scene");
    Check(Take(session->ReadJobPreview(id), "closed Job preview").text == preview.text, "Close lost cached Job preview");
    Take(world.runtime->Shutdown(), "retire Job Runtime");
    world.runtime = Take(sdk::Runtime::Create({Utf8(world.root / "state"), Utf8(world.resources)}), "fresh Job resume Runtime");
    std::size_t writes; { std::lock_guard lock(world.state->mutex); writes = world.state->calls; }
    auto options = world.Options(std::make_shared<Script>(), true); options.command_jobs.reset();
    options.resume_session_id = session->id(); options.system_prompt.clear();
    auto resumed = world.Open(std::move(options)); const auto held = Take(resumed->ReadJob(id), "passive historical Job");
    Check(!held.owner_available && held.state == "succeeded", "same-ID restore revived a Job owner");
    Check(Take(resumed->ReadJobPreview(id), "restored Job preview").text == preview.text, "restored Job bytes changed");
    { std::lock_guard lock(world.state->mutex); Check(world.state->calls == writes, "Hold restoration published new material"); }
    Check(!resumed->CancelJob(id), "historical Job fabricated a live cancellation"); Take(resumed->Close(), "close Job restore");
}

void Summary(World& world, const std::function<void(const fs::path&, const std::string&)>& inspect) {
    auto script = std::make_shared<Script>(); auto options = world.Options(script); options.context_window_tokens = 16384;
    sdk::Tool tool; tool.name = "large_evidence"; tool.description = "Return actual controlled evidence"; tool.requires_approval = false;
    tool.execute = [](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        return sdk::ToolResult{"SUMMARY-EVIDENCE\n" + std::string(60000, 'S'), false};
    };
    options.custom_tools.push_back(std::move(tool)); auto session = world.Open(std::move(options));
    const auto operation = Run(session, "summary-parent", "summary");
    {
        std::lock_guard lock(script->mutex);
        Check(script->summary_calls > 0 && script->summary_material_seen && script->summary_adopted,
              "ordinary batch budget never produced an adopted actual backend summary");
    }
    {
        std::lock_guard lock(world.state->mutex);
        Check(std::any_of(world.state->reads.begin(), world.state->reads.end(), [](const auto& ref) {
            return ref.logical_name.starts_with("res-") && ref.logical_name.ends_with(".combined.txt") && ref.bytes > 60000;
        }), "automatic summary bypassed the same-scope named reader");
    }
    const auto saved = Saved(session, operation); world.NoMirrors(); Take(session->Close(), "close summary scene");
    const auto directory = world.SessionDirectory(session->id()); const auto journal = Bytes(directory / (session->id() + ".jsonl"));
    Check(journal.find("tool.result.summary.finished") != std::string::npos && journal.find("\"state\":\"accepted\"") != std::string::npos &&
        journal.find("sourceContextRevision") != std::string::npos, "accepted summary provenance was not durably recorded");
    if (inspect) inspect(directory, saved.result().summary.identity.tool_call_id);
}

void Publication(const fs::path& base, const fs::path& probe) {
    for (const auto fault : {Fault::AfterPublish, Fault::SecondReject, Fault::WrongScope, Fault::OversizeReceipt}) {
        World world(base, probe); world.state->fault = fault;
        auto script = std::make_shared<Script>(); auto session = world.Open(world.Options(script));
        struct Reentry {
            std::mutex mutex;
            std::atomic<unsigned> callbacks{0};
            std::unique_ptr<sdk::Result<sdk::Receipt>> submitted;
        };
        auto reentry = std::make_shared<Reentry>();
        // This is the actual publishing callback, while the capability owns its
        // material/write gate. API admission must neither borrow that gate nor
        // allow this accepted item to execute once the first unknown is known.
        world.state->on_write = [weak = std::weak_ptr<sdk::Session>(session), reentry] {
            if (reentry->callbacks.fetch_add(1) != 0) return;
            auto current = weak.lock(); Check(current != nullptr, "publishing callback lost its Session");
            auto submitted = current->Submit("queued-before-unknown", "read");
            std::lock_guard lock(reentry->mutex);
            reentry->submitted = std::make_unique<sdk::Result<sdk::Receipt>>(std::move(submitted));
        };
        const auto operation = Run(session, "uncertain-publish");
        Check(operation.state != sdk::OperationState::Succeeded, "unknown publication was reported successful");
        Check(operation.state == sdk::OperationState::Indeterminate && !operation.result_persisted &&
              operation.error.find("sdk.named_results.publication_unconfirmed") != std::string::npos,
              "current operation lost its actual unknown publication state");
        const auto reread = Take(session->ReadOperation(operation.operation_id), "read current unknown");
        Check(reread.state == operation.state && reread.error == operation.error && !reread.result_persisted,
              "ReadOperation disagreed with the actual unknown final");
        const auto queued = [&] {
            std::lock_guard lock(reentry->mutex);
            Check(reentry->submitted != nullptr, "provider callback never returned from public Submit");
            return Take(*reentry->submitted, "accept actual queued callback input");
        }();
        Check(!queued.duplicate, "queued callback did not create its own actual operation");
        const auto stopped = Take(session->WaitResult(queued.operation_id, 20s), "wait queued owner fence");
        Check(stopped.state == sdk::OperationState::Indeterminate && !stopped.result_persisted && stopped.turn_id.empty() &&
              stopped.error == "sdk.named_results.publication_unconfirmed",
              "queued work crossed the unknown owner or claimed a dispatched turn");
        std::size_t calls, writes;
        { std::lock_guard lock(world.state->mutex); calls = world.state->calls; writes = world.state->publications.size(); }
        Check(calls == (fault == Fault::SecondReject ? 2u : 1u) && writes == 1, "fault did not follow a real first publication");
        auto retry = session->Submit("no-retry", "read");
        if (retry) Check(Take(session->WaitResult(retry->operation_id, 20s), "wait refused retry").state != sdk::OperationState::Succeeded,
                         "a fresh operation ignored the unknown storage owner");
        Check(!retry && retry.error().code == "sdk.named_results.publication_unconfirmed",
              "fresh Submit did not refuse the retained unknown owner");
        { std::lock_guard lock(script->mutex); Check(script->turns == 1, "queued or fresh work called Generate after unknown publication"); }
        (void)session->Close();
        { std::lock_guard lock(world.state->mutex); Check(world.state->calls == calls, "first unknown was retried or overwritten"); }
        world.NoMirrors();
    }
    {
        World world(base, probe); world.state->fault = Fault::FirstReject;
        auto session = world.Open(world.Options(std::make_shared<Script>()));
        const auto rejected = Run(session, "known-zero-publication");
        Check(rejected.state != sdk::OperationState::Succeeded && rejected.state != sdk::OperationState::Indeterminate,
              "known zero-publication rejection was confused with unknown");
        { std::lock_guard lock(world.state->mutex); Check(world.state->calls == 1 && world.state->publications.empty(), "known rejection published material"); }
        Check(Run(session, "text-after-known-rejection").state == sdk::OperationState::Succeeded,
              "known rejection poisoned a text-only operation");
        (void)Saved(session, Run(session, "tool-after-known-rejection"));
        { std::lock_guard lock(world.state->mutex); Check(world.state->calls > 1 && !world.state->publications.empty(), "known rejection sealed the live named owner"); }
        Take(session->Close(), "close known rejection"); world.NoMirrors();
    }
    {
        World world(base, probe); auto options = world.Options(std::make_shared<Script>()); options.named_results.reset();
        auto session = world.Open(std::move(options));
        (void)Saved(session, Run(session, "ordinary-file-first"));
        (void)Saved(session, Run(session, "ordinary-file-second"));
        Take(session->Close(), "close ordinary File");
        std::lock_guard lock(world.state->mutex); Check(world.state->opens == 0 && world.state->calls == 0,
              "ordinary File unexpectedly entered the external named provider");
    }
}

void Identity(World& world) {
    auto session = world.Open(world.Options(std::make_shared<Script>())); const auto operation = Run(session, "identity");
    const auto saved = Saved(session, operation); const auto identity = saved.result().summary.identity;
    blob::Reference metadata;
    {
        std::lock_guard lock(world.state->mutex);
        const auto name = identity.result_id + ".json";
        auto found = std::find_if(world.state->publications.begin(), world.state->publications.end(), [&](const auto& ref) {
            return ref.scope.session_id == session->id() && ref.logical_name == name;
        });
        Check(found != world.state->publications.end(), "formal metadata never reached actual provider"); metadata = *found;
    }
    const auto path = world.state->Directory(metadata.scope) / metadata.logical_name; const auto original = Bytes(path);
    auto corrupted = original; Check(!corrupted.empty(), "metadata unexpectedly empty"); corrupted[0] ^= 1; Write(path, corrupted);
    auto bad = Take(session->ReadToolResult(identity), "read corrupt metadata");
    Check(bad.result().metadata_state == sdk::results::v1::ArtifactState::Corrupt, "corrupt provider bytes became verified");
    fs::remove(path); auto missing = Take(session->ReadToolResult(identity), "read missing metadata");
    Check(missing.result().metadata_state == sdk::results::v1::ArtifactState::Missing, "missing provider bytes became verified");
    Write(path, original); auto wrong = identity; wrong.session_id += "-foreign";
    Check(!session->ReadToolResult(wrong), "foreign Session identity borrowed this reader");
    Take(session->Close(), "close identity scene"); world.NoMirrors();
}

void Isolation(World& world) {
    std::vector<std::shared_ptr<sdk::Session>> scenes;
    for (unsigned i = 0; i < 4; ++i) {
        auto options = world.Options(std::make_shared<Script>());
        if (i > 1) {
            const auto project = world.root / ("project-" + std::to_string(i)); fs::create_directories(project);
            Write(project / "input.txt", "SEPARATE-PROJECT-" + std::to_string(i)); options.cwd = Utf8(project);
        }
        scenes.push_back(world.Open(std::move(options)));
    }
    std::vector<std::shared_ptr<Task>> work;
    for (unsigned i = 0; i < scenes.size(); ++i) {
        const auto scene = scenes[i];
        work.push_back(world.Run([scene, i] {
            const auto saved = Saved(scene, Run(scene, "isolated-" + std::to_string(i)));
            Check(saved.result().summary.identity.session_id == scene->id(), "result borrowed sibling Session identity");
            Check(saved.result().summary.identity.result_id == "res-000001", "separate Session shared a number pool");
            const std::string expected = i > 1 ? "SEPARATE-PROJECT-" + std::to_string(i) : "ACTUAL-NAMED-TOOL-EVIDENCE";
            Check(std::any_of(saved.result().channels.begin(), saved.result().channels.end(), [&](const auto& channel) {
                return channel.text && channel.text->find(expected) != std::string::npos;
            }), "read material crossed projects");
        }));
    }
    for (const auto& task : work) world.Join(task);
    { std::lock_guard lock(world.state->mutex); Check(world.state->directories.size() == 4, "per-Session Store scopes were aliased"); }
    for (const auto& scene : scenes) Take(scene->Close(), "close isolated scene"); world.NoMirrors();
}

void CloseRead(World& world) {
    auto session = world.Open(world.Options(std::make_shared<Script>())); const auto operation = Run(session, "close-read");
    const auto saved = Saved(session, operation); const auto identity = saved.result().summary.identity;
    {
        std::lock_guard lock(world.state->mutex); world.state->hold_read = true;
        const auto weak = std::weak_ptr<sdk::Session>(session);
        world.state->on_read = [weak, identity] {
            auto actual = weak.lock(); Check(actual != nullptr, "reader lost actual Session handle");
            auto recursive = actual->ReadToolResult(identity);
            Check(!recursive && recursive.error().code == "sdk.result.reentrant", "recursive provider read did not fail before locks");
            auto close = actual->Close();
            Check(!close && close.error().code == "sdk.lifecycle.reentrant", "provider callback recursively closed its owner");
            auto wait = actual->WaitResult(identity.operation_id, 1ms);
            Check(!wait && wait.error().code == "sdk.lifecycle.reentrant", "provider callback entered blocking Wait");
            Check(!Take(actual->ListToolResults(identity.operation_id), "cached reentrant list").empty(), "cached query unavailable");
        };
    }
    auto reading = world.Run([session, identity] {
        const auto result = Take(session->ReadToolResult(identity), "held external read");
        Check(result.result().metadata_state == sdk::results::v1::ArtifactState::Verified, "held read lost immutable bytes");
    });
    Check(world.state->Await([&] { return world.state->read_entered; }), "actual provider read did not enter");
    auto closing = world.Run([session] { Take(session->Close(), "Close with ongoing read"); });
    world.Join(closing); // must finish before the read callback is released
    { std::lock_guard lock(world.state->mutex); Check(world.state->stores_destroyed == 0, "Close destroyed an active read provider"); }
    const auto directory = world.SessionDirectory(session->id()); const auto moved = directory.parent_path() / (session->id() + "-closed");
    fs::rename(directory, moved); // actual writer/operations handles must have retired on Windows too
    world.state->Release(); world.Join(reading);
    Check(Take(session->ReadToolResult(identity), "read after move").result().metadata_state == sdk::results::v1::ArtifactState::Verified,
          "closed reader secretly reopened Session artifacts");
    world.sessions.clear(); session.reset();
    { std::lock_guard lock(world.state->mutex); world.state->on_read = {}; }
    Take(world.runtime->Shutdown(), "shutdown after read");
    { std::lock_guard lock(world.state->mutex); Check(world.state->stores_destroyed == 1 && world.state->providers_destroyed == 1,
        "last read owner did not retire its Store/Provider exactly once"); }
}

void CloseWrite(World& world) {
    world.state->hold_write = true;
    auto session = world.Open(world.Options(std::make_shared<Script>()));
    const auto receipt = Take(session->Submit("close-write", "read"), "submit held write");
    Check(world.state->Await([&] { return world.state->write_entered; }), "actual publishing callback did not enter");
    auto cancel = world.Run([session, receipt] { Take(session->Cancel(receipt.operation_id), "cancel while provider owns write"); });
    world.Join(cancel); // cancellation must not borrow the provider's serial gate
    const auto close_called = std::make_shared<std::atomic<bool>>(false);
    auto closing = world.Run([session, close_called, state = world.state] {
        { std::lock_guard lock(state->mutex); close_called->store(true, std::memory_order_release); state->cv.notify_all(); }
        (void)session->Close();
    });
    Check(world.state->Await([&] { return close_called->load(std::memory_order_acquire); }), "Close thread never entered");
    Check(!world.state->Await([&] { return closing->done.load(std::memory_order_acquire); }, 100ms), "Close falsely retired a held write");
    world.state->Release(); world.Join(closing);
    const auto result = Take(session->ReadOperation(receipt.operation_id), "closed operation");
    Check(result.state != sdk::OperationState::Running && result.state != sdk::OperationState::Accepted, "Close left an active operation");
    world.NoMirrors();
}

void Opening(const fs::path& base, const fs::path& probe) {
    {
        World world(base, probe); world.state->hold_open = true;
        auto first = world.Run([&] {
            auto result = world.runtime->OpenSession(world.Options(std::make_shared<Script>()));
            Check(!result && result.error().code == "sdk.runtime.closed", "opening that lost Shutdown returned a runnable Session");
        });
        Check(world.state->Await([&] { return world.state->open_entered; }), "actual opening factory did not block");
        auto independent = world.Run([&] {
            auto scene = Take(world.runtime->OpenSession(world.Options(std::make_shared<Script>())), "independent opening");
            Take(scene->Close(), "independent scene close");
        });
        world.Join(independent); // old Runtime-wide factory lock would deadlock here
        auto shutdown = world.Run([&] { Take(world.runtime->Shutdown(), "Shutdown waits for unpublished opening"); });
        bool closed = false;
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < until) {
            auto attempt = world.runtime->OpenSession(world.Options(std::make_shared<Script>()));
            if (!attempt) { Check(attempt.error().code == "sdk.runtime.closed", "unexpected opening rejection"); closed = true; break; }
            Take((*attempt)->Close(), "close legitimate race winner"); std::this_thread::yield();
        }
        Check(closed, "Shutdown did not close admission");
        Check(!shutdown->done.load(std::memory_order_acquire), "Shutdown forgot its admitted unfinished factory");
        world.state->Release(); world.Join(first); world.Join(shutdown);
        { std::lock_guard lock(world.state->mutex); Check(world.state->stores_destroyed == world.state->opens,
            "unpublished opening retained an execution Store"); }
    }
    {
        World world(base, probe); world.state->fault = Fault::OpenThrow;
        auto result = world.runtime->OpenSession(world.Options(std::make_shared<Script>())); Check(!result, "throwing factory opened a Session");
        auto shutdown = world.Run([&] { (void)world.runtime->Shutdown(); }); world.Join(shutdown);
        std::lock_guard lock(world.state->mutex); Check(world.state->opens == 1 && world.state->providers_destroyed == 1,
            "failed opening failed to retire exactly once");
    }
    {
        World world(base, probe); bool reentered = false;
        world.state->on_open = [&] {
            auto recursive = world.runtime->OpenSession(world.Options(std::make_shared<Script>()));
            Check(!recursive && recursive.error().code == "sdk.lifecycle.reentrant", "factory recursively opened same Runtime");
            auto closing = world.runtime->Shutdown();
            Check(!closing && closing.error().code == "sdk.lifecycle.reentrant", "factory recursively waited for itself"); reentered = true;
        };
        auto session = world.Open(world.Options(std::make_shared<Script>())); Check(reentered, "actual factory callback was not tested");
        Take(session->Close(), "close reentry scene");
    }
}

void Bindings(World& world) {
    auto scene = world.Open(world.Options(std::make_shared<Script>())); (void)Saved(scene, Run(scene, "binding-seed"));
    Take(scene->Close(), "close binding seed");
    const auto directory = world.SessionDirectory(scene->id()); const auto plan = directory / "sdk-named-results-plan.json";
    const auto original = Bytes(plan); const auto ledger = Bytes(directory / (scene->id() + ".jsonl"));
    std::size_t opens; { std::lock_guard lock(world.state->mutex); opens = world.state->opens; }
    for (const std::string mode : {"missing-provider", "namespace", "changed-plan", "missing-plan"}) {
        auto options = world.Options(std::make_shared<Script>()); options.resume_session_id = scene->id(); options.system_prompt.clear();
        if (mode == "missing-provider") options.named_results.reset();
        if (mode == "namespace") options.named_results->binding_id = "foreign-namespace";
        if (mode == "changed-plan") Write(plan, original + " ");
        if (mode == "missing-plan") fs::remove(plan);
        auto resumed = world.runtime->OpenSession(std::move(options));
        Check(!resumed, "storage binding drift was silently adopted: " + mode);
        { std::lock_guard lock(world.state->mutex); Check(world.state->opens == opens, "provider factory ran before frozen binding refusal"); }
        Check(Bytes(directory / (scene->id() + ".jsonl")) == ledger, "binding refusal appended to the original source");
        Write(plan, original);
    }
    auto options = world.Options(std::make_shared<Script>()); options.resume_session_id = scene->id(); options.system_prompt.clear();
    auto restored = world.Open(std::move(options)); Take(restored->Close(), "close correctly restored binding");
    world.NoMirrors();
}

void Path(const fs::path& base, const fs::path& probe, const std::string& path,
    const std::function<void(const fs::path&, const std::string&)>& inspect) {
    if (path == "publication") Publication(base, probe);
    else if (path == "opening") Opening(base, probe);
    else {
        World world(base, probe);
        if (path == "roundtrip") Roundtrip(world);
        else if (path == "jobs") Jobs(world);
        else if (path == "summary") Summary(world, inspect);
        else if (path == "identity") Identity(world);
        else if (path == "isolation") Isolation(world);
        else if (path == "close-read") CloseRead(world);
        else if (path == "close-write") CloseWrite(world);
        else if (path == "bindings") Bindings(world);
        else throw std::runtime_error("unknown named result acceptance path");
    }
    std::cout << "[sdk-named-results-path] " << path << '\n';
}

} // namespace

void NamedResultsCase(const fs::path& base, const fs::path& probe, const std::string& path,
    const std::function<void(const fs::path&, const std::string&)>& inspect) { Path(base, probe, path, inspect); }
void NamedResults(const fs::path& base, const fs::path& probe) {
    for (const char* path : {"roundtrip", "jobs", "summary", "publication", "identity", "isolation",
                             "close-read", "close-write", "opening", "bindings"}) Path(base, probe, path, {});
    std::cout << "[sdk-named-results-consumer] complete\n";
}
} // namespace lubancore_consumer
