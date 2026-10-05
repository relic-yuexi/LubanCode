// Installed public SDK + STL only. Commands invoke the explicit CI-owned probe.
#include <lubancore/core.hpp>
#include <lubancore/jobs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error("command-jobs: " + reason); }
template<class T> T Take(sdk::Result<T> value, const char* name) {
    if (!value) throw std::runtime_error(std::string("command-jobs: ") + name + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const char* name) {
    if (!value) throw std::runtime_error(std::string("command-jobs: ") + name + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string JsonString(const std::string& input) {
    std::string result = "\"";
    for (const unsigned char c : input) {
        if (c == '\\' || c == '"') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else { Check(c >= 32, "control byte in fixture argument"); result.push_back(static_cast<char>(c)); }
    }
    return result + '"';
}
std::string Quote(const std::string& input) {
#ifdef _WIN32
    Check(input.find_first_of("\"%\r\n") == std::string::npos, "unsafe fixture cmd argument");
    return '"' + input + '"';
#else
    std::string quoted = "'";
    for (const char ch : input) quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    return quoted + "'";
#endif
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary); Check(in.is_open(), "missing " + Utf8(path));
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Check(!in.bad(), "read failed"); return value;
}
void Write(const fs::path& path, const std::string& value = "release") {
    std::ofstream out(path, std::ios::binary | std::ios::trunc); Check(out.is_open(), "write open failed");
    out << value; out.close(); Check(!out.fail(), "write failed");
}
struct Fixture {
    fs::path root, project, resources, probe;
    Fixture(const fs::path& base, const fs::path& original) {
        static std::atomic<unsigned> serial{0};
        Check(base.is_absolute() && original.is_absolute() && fs::is_regular_file(original), "explicit state/probe required");
        root = base / ("command-jobs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        project = root / "project"; resources = root / "resources";
        fs::create_directories(project); fs::create_directories(resources);
        probe = root / original.filename(); Check(fs::copy_file(original, probe), "probe copy failed");
        fs::permissions(probe, fs::status(original).permissions());
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    std::unique_ptr<sdk::Runtime> Runtime() const {
        return Take(sdk::Runtime::Create({Utf8(root / "state"), Utf8(resources)}), "runtime");
    }
    std::string Input(const std::string& tag, bool job = true, bool wait = false, std::uint64_t bytes = 64,
        std::uint64_t budget = 0) const {
        std::string command = Quote(Utf8(probe));
        for (const auto& arg : std::vector<std::string>{tag + ".started", tag + ".done", std::to_string(bytes), "0", tag, "J", wait ? tag + ".release" : "-"})
            command += " " + Quote(arg);
        std::string json = "{\"command\":" + JsonString(command);
#ifdef _WIN32
        json += ",\"shell\":\"cmd\"";
#else
        json += ",\"shell\":\"sh\"";
#endif
        if (job) json += ",\"execution_mode\":\"session_job\"";
        if (budget) json += ",\"job_budget_ms\":" + std::to_string(budget);
        return json + '}';
    }
};
struct Script {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> calls;
    std::string call_prefix = "job-call-";
    bool block_after_admission = false, release = false, entered = false, fail_parent = false;
    std::size_t generations = 0, admission_replies = 0;
    std::vector<std::string> replies;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<Script> script) : script_(std::move(script)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        std::unique_lock lock(script_->mutex);
        const auto index = script_->generations++;
        if (index == 0) {
            sdk::ModelReply reply;
            for (std::size_t i = 0; i != script_->calls.size(); ++i)
                reply.tool_calls.push_back({script_->call_prefix + std::to_string(i), "run_command", script_->calls[i]});
            if (reply.tool_calls.empty()) reply.text = "no command";
            return reply;
        }
        Check(index == 1, "parent unexpectedly called backend again");
        for (const auto& message : request.messages) for (const auto& reply : message.tool_replies) {
            if (reply.call_id.starts_with(script_->call_prefix)) {
                script_->replies.push_back(reply.text);
                if (!reply.is_error && reply.text.find("\"jobId\"") != std::string::npos) ++script_->admission_replies;
            }
        }
        Check(script_->replies.size() == script_->calls.size(), "parent did not receive exactly one reply per original declaration");
        script_->entered = true;
        script_->cv.notify_all();
        while (script_->block_after_admission && !script_->release && !cancel.requested()) {
            lock.unlock(); std::this_thread::sleep_for(10ms); lock.lock();
        }
        if (script_->fail_parent) return std::unexpected(sdk::Error{"fixture.parent_failed", "controlled parent failure"});
        if (cancel.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "parent cancellation"});
        return sdk::ModelReply{"parent accepted the handles", {}, {}};
    }
private:
    std::shared_ptr<Script> script_;
};
sdk::SessionOptions Options(const fs::path& cwd, std::shared_ptr<Script> script, bool jobs = true) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd); options.model = "controlled-command-jobs";
    options.backend = std::make_unique<Backend>(std::move(script));
    options.builtin_tools = {"run_command"}; options.approval_mode = sdk::ApprovalMode::Yolo;
    options.max_steps_per_turn = 3;
    if (jobs) options.command_jobs = sdk::jobs::v1::CommandOptions{30000, 20000, 65536, 1, 8};
    return options;
}
void Started(const fs::path& cwd, const std::string& tag) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        std::ifstream in(cwd / (tag + ".started"), std::ios::binary);
        if (in.is_open()) {
            std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            const auto end = bytes.find('\n');
            if (!in.bad() && end != std::string::npos && bytes.substr(0, end) == tag && end + 1 < bytes.size()) {
                std::error_code error;
                if (fs::equivalent(fs::u8path(bytes.substr(end + 1)), cwd, error) && !error) return;
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("command-jobs: actual probe did not enter its requested cwd");
}
sdk::Operation Parent(const std::shared_ptr<sdk::Session>& session, const char* id) {
    auto receipt = Take(session->Submit(id, "run the explicit command"), "submit");
    return Take(session->WaitResult(receipt.operation_id, 20s), "wait parent");
}
std::vector<sdk::jobs::v1::JobView> Jobs(const std::shared_ptr<sdk::Session>& session, std::size_t count) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        auto jobs = Take(session->ListJobs(), "list Jobs");
        if (jobs.size() == count && std::all_of(jobs.begin(), jobs.end(), [](const auto& job) { return !job.identity.operation_id.empty(); })) return jobs;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("command-jobs: real Job operation binding not published");
}
void Complete(const fs::path& base, const fs::path& probe,
    const std::function<void(const fs::path&, const sdk::jobs::v1::Identity&)>& inspect = {}) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("complete", true, true, 8194)};
    auto session = Take(runtime->OpenSession(Options(fixture.project, script)), "session");
    const auto parent = Parent(session, "parent-complete");
    Check(parent.state == sdk::OperationState::Succeeded, "parent failed: " + parent.error);
    Started(fixture.project, "complete");
    const auto jobs = Jobs(session, 1); const auto id = jobs.front().identity;
    Check(id.session_id == session->id() && id.parent_operation_id == parent.operation_id &&
        id.operation_id != parent.operation_id && id.turn_id == parent.turn_id && id.attempt == 1, "Job identity was fabricated or aliased to parent");
    Check(!session->WaitJob(id, 10ms), "a gated command was reported complete");
    Check(!session->ReadJobPreview(id), "an unfinished Job fabricated a preview");
    Write(fixture.project / "complete.release");
    const auto result = Take(session->WaitJob(id, 20s), "wait Job");
    Check(result.state == "succeeded" && result.execution_state == "succeeded" && result.gap.empty(), "Job did not complete independently after its parent");
    Check(Read(fixture.project / "complete.done") == "complete", "real probe did not complete");
    const auto preview = Take(session->ReadJobPreview(id), "preview");
    Check(preview.capture_complete && preview.preview_truncated && preview.text.size() <= 4096 &&
        preview.text.find(std::string(32, 'J')) != std::string::npos && !preview.result_id.empty() && !preview.persisted_event_id.empty(), "preview lacks actual bounded captured material");
    Check(!session->ReadJobPreview(id, 4097), "Full result escaped preview cap");
    const auto small = Take(session->ReadJobPreview(id, 32), "small preview");
    Check(small.text.size() <= 32 && small.preview_truncated, "caller preview bound ignored");
    if (inspect) inspect(fixture.root / "state", id);
    { std::lock_guard lock(script->mutex); Check(script->generations == 2 && script->admission_replies == 1, "duplicate/missing admission reply"); }
    Take(session->Close(), "close");
    Check(Take(session->ReadJobPreview(id), "closed preview").text == preview.text, "close lost the owned preview");
    auto resume_options = Options(fixture.project, std::make_shared<Script>());
    resume_options.command_jobs.reset(); resume_options.resume_session_id = session->id();
    auto resumed = Take(runtime->OpenSession(std::move(resume_options)), "resume");
    const auto held = Take(resumed->ReadJob(id), "held Job");
    Check(!held.owner_available && held.state == "succeeded", "recovery revived an old owner");
    Check(Take(resumed->ReadJobPreview(id), "held preview").text == preview.text, "recovery lost actual captured preview");
    Check(!resumed->CancelJob(id), "historical cancellation claimed a live process");
    Take(resumed->Close(), "close resume"); Take(runtime->Shutdown(), "shutdown");
}
void Admission(const fs::path& base, const fs::path& probe) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    for (const std::string mode : {"off", "unknown", "detached", "budget"}) {
        auto script = std::make_shared<Script>();
        auto input = fixture.Input(mode);
        if (mode == "unknown") { auto at = input.find("session_job"); input.replace(at, 11, "unknown_job"); }
        if (mode == "detached") input.insert(input.size() - 1, ",\"run_in_background\":true");
        if (mode == "budget") input.insert(input.size() - 1, ",\"job_budget_ms\":0");
        script->calls = {input};
        auto session = Take(runtime->OpenSession(Options(fixture.project, script, mode != "off")), "reject session");
        const auto parent = Parent(session, ("reject-" + mode).c_str());
        Check(parent.state == sdk::OperationState::Succeeded, "controlled refusal prevented the next model turn");
        Check(Take(session->ListJobs(), "empty list").empty(), "invalid request registered a Job");
        Check(!fs::exists(fixture.project / (mode + ".started")), "invalid request launched a command");
        Take(session->Close(), "close refusal");
    }
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("foreground", false)};
    auto session = Take(runtime->OpenSession(Options(fixture.project, script)), "foreground");
    Check(Parent(session, "foreground").state == sdk::OperationState::Succeeded, "foreground regressed");
    Check(Read(fixture.project / "foreground.done") == "foreground" && Take(session->ListJobs(), "foreground jobs").empty(), "foreground became a Job");
    for (const bool enabled : {false, true}) {
        const auto tag = enabled ? "foreground-false-on" : "foreground-false-off";
        auto legacy = std::make_shared<Script>();
        auto input = fixture.Input(tag, false);
        input.insert(input.size() - 1, ",\"run_in_background\":false,\"max_runtime_ms\":0");
        legacy->calls = {input};
        auto foreground = Take(runtime->OpenSession(Options(fixture.project, legacy, enabled)), "legacy foreground");
        Check(Parent(foreground, tag).state == sdk::OperationState::Succeeded, "explicit false foreground was rejected");
        Check(Read(fixture.project / (std::string(tag) + ".done")) == tag, "legacy foreground command did not execute");
        Check(Take(foreground->ListJobs(), "legacy foreground list").empty(), "legacy false request acquired Job ownership");
        Take(foreground->Close(), "close legacy foreground");
    }
    auto invalid = Options(fixture.project, std::make_shared<Script>()); invalid.command_jobs->max_running = 0;
    Check(!runtime->OpenSession(std::move(invalid)), "zero host budget accepted");
    unsigned factories = 0;
    auto incompatible = Options(fixture.project, std::make_shared<Script>());
    sdk::extensions::v1::Registration registration;
    registration.manifest.id = "job-action-incompatible"; registration.manifest.version = "1";
    sdk::extensions::v1::HandlerDefinition handler;
    handler.point = sdk::extensions::v1::Point::PreAction; handler.name = "pre-action";
    handler.definition_hash = std::string(64, 'a');
    registration.manifest.handlers.push_back(handler);
    registration.factory = [&](const auto&) -> sdk::Result<std::unique_ptr<sdk::extensions::v1::Instance>> {
        ++factories; return std::unexpected(sdk::Error{"fixture.factory_called", "must reject before factory"});
    };
    incompatible.extensions.push_back(std::move(registration));
    const auto denied = runtime->OpenSession(std::move(incompatible));
    Check(!denied && denied.error().code == "sdk.job.action_combination_unsupported" && factories == 0,
        "Action combination reached an extension factory");
    Take(session->Close(), "close foreground"); Take(runtime->Shutdown(), "shutdown");
}
void Queue(const fs::path& base, const fs::path& probe) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("blocker", true, true), fixture.Input("expired", true, false, 64, 1200)};
    auto session = Take(runtime->OpenSession(Options(fixture.project, script)), "queue session");
    Check(Parent(session, "parent-queue").state == sdk::OperationState::Succeeded, "queue parent failed");
    Started(fixture.project, "blocker"); auto jobs = Jobs(session, 2);
    std::sort(jobs.begin(), jobs.end(), [](const auto& a, const auto& b) { return a.identity.action_id < b.identity.action_id; });
    const auto expired = Take(session->WaitJob(jobs.back().identity, 10s), "queued deadline");
    Check(expired.state == "cancelled" && expired.command_not_invoked, "queued deadline did not prevent invocation");
    Check(!fs::exists(fixture.project / "expired.started"), "expired Job ran its command");
    Write(fixture.project / "blocker.release");
    Check(Take(session->WaitJob(jobs.front().identity, 20s), "blocker completion").state == "succeeded", "unrelated blocker was cancelled");
    Take(session->Close(), "close queue"); Take(runtime->Shutdown(), "shutdown");
}
void BlockedParent(const fs::path& base, const fs::path& probe, bool fail) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("blocked", true, true)};
    script->block_after_admission = true; script->fail_parent = fail;
    auto session = Take(runtime->OpenSession(Options(fixture.project, script)), "blocked session");
    const auto receipt = Take(session->Submit("parent-blocked", "launch"), "blocked submit");
    Started(fixture.project, "blocked"); const auto id = Jobs(session, 1).front().identity;
    {
        std::unique_lock lock(script->mutex);
        Check(script->cv.wait_for(lock, 10s, [&] { return script->entered; }), "parent backend never entered its actual blocking call");
        Check(script->block_after_admission && !script->release, "parent backend did not retain its blocking gate");
    }
    if (fail) {
        { std::lock_guard lock(script->mutex); script->release = true; }
        const auto parent = Take(session->WaitResult(receipt.operation_id, 20s), "failed parent");
        Check(parent.state == sdk::OperationState::Failed, "controlled parent failure disappeared");
        Check(Take(session->WaitJob(id, 20s), "failed-parent cancellation").state == "cancelled", "failed parent left its process running");
        Check(!fs::exists(fixture.project / "blocked.done"), "cancelled probe reached done");
    } else {
        Write(fixture.project / "blocked.release");
        const auto until = std::chrono::steady_clock::now() + 10s;
        while (!fs::is_regular_file(fixture.project / "blocked.done") && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
        Check(Read(fixture.project / "blocked.done") == "blocked", "actual command did not finish");
        const auto waiting = session->WaitJob(id, 20ms);
        Check(!waiting && waiting.error().code == "sdk.wait.timeout", "query pumped writer while backend was blocked");
        { std::lock_guard lock(script->mutex); script->release = true; }
        Check(Take(session->WaitResult(receipt.operation_id, 20s), "unblocked parent").state == sdk::OperationState::Succeeded, "unblocked parent failed");
        Check(Take(session->WaitJob(id, 20s), "unblocked Job").state == "succeeded", "host did not settle the completed envelope");
    }
    Take(session->Close(), "close blocked"); Take(runtime->Shutdown(), "shutdown");
}
void Isolation(const fs::path& base, const fs::path& probe) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    std::vector<std::shared_ptr<sdk::Session>> sessions;
    std::vector<sdk::jobs::v1::Identity> ids;
    for (unsigned i = 0; i < 4; ++i) {
        const auto cwd = i < 2 ? fixture.project : fixture.root / ("project-" + std::to_string(i)); fs::create_directories(cwd);
        auto script = std::make_shared<Script>(); script->calls = {fixture.Input("iso-" + std::to_string(i), true, true)};
        sessions.push_back(Take(runtime->OpenSession(Options(cwd, script)), "isolation session"));
        Check(Parent(sessions.back(), ("parent-" + std::to_string(i)).c_str()).state == sdk::OperationState::Succeeded, "isolation parent failed");
        Started(cwd, "iso-" + std::to_string(i)); ids.push_back(Jobs(sessions.back(), 1).front().identity);
    }
    Check(!sessions[1]->ReadJob(ids[0]) && !sessions[1]->CancelJob(ids[0]) && !sessions[1]->ReadJobPreview(ids[0]), "Job escaped its Session scope");
    Take(sessions[0]->CancelJob(ids[0]), "cancel one Job");
    Check(Take(sessions[0]->WaitJob(ids[0], 20s), "cancelled Job").state == "cancelled", "target process did not stop");
    for (unsigned i = 1; i < 4; ++i) {
        const auto cwd = i < 2 ? fixture.project : fixture.root / ("project-" + std::to_string(i));
        Write(cwd / ("iso-" + std::to_string(i) + ".release"));
        Check(Take(sessions[i]->WaitJob(ids[i], 20s), "independent Job").state == "succeeded", "cancellation crossed sessions");
    }
    for (auto& session : sessions) Take(session->Close(), "close independent session");
    Take(runtime->Shutdown(), "shutdown");
}
void Close(const fs::path& base, const fs::path& probe) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("closing", true, true)};
    auto session = Take(runtime->OpenSession(Options(fixture.project, script)), "closing session");
    Check(Parent(session, "parent-close").state == sdk::OperationState::Succeeded, "closing parent failed");
    Started(fixture.project, "closing"); const auto id = Jobs(session, 1).front().identity;
    Take(runtime->Shutdown(), "shutdown with live Job");
    const auto ended = Take(session->ReadJob(id), "closed status");
    Check(ended.terminal && ended.state == "cancelled" && !ended.owner_available, "shutdown did not join the actual Job");
    Check(!fs::exists(fixture.project / "closing.done"), "cancelled process completed");
    auto another_runtime = fixture.Runtime();
    auto another_script = std::make_shared<Script>(); another_script->calls = {fixture.Input("dropping", true, true)};
    auto last_handle = Take(another_runtime->OpenSession(Options(fixture.project, another_script)), "last handle session");
    Check(Parent(last_handle, "parent-drop").state == sdk::OperationState::Succeeded, "last handle parent failed");
    Started(fixture.project, "dropping");
    last_handle.reset(); // Runtime's weak registry must not keep this owner executing.
    Check(!fs::exists(fixture.project / "dropping.done"), "dropped public handle left the process executing");
    Take(another_runtime->Shutdown(), "shutdown after last handle");
    Check(fs::remove(fixture.probe), "joined process still retains the relocated probe executable");
}
sdk::Approval Approval(const std::shared_ptr<sdk::Session>& session) {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
        const auto pending = session->PendingApprovals();
        if (!pending.empty()) { Check(pending.size() == 1, "unexpected concurrent approval"); return pending.front(); }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("command-jobs: approval never arrived");
}
void Approvals(const fs::path& base, const fs::path& probe) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->call_prefix = "ordinary-";
    script->calls = {fixture.Input("ordinary", false)};
    auto options = Options(fixture.project, script); options.approval_mode = sdk::ApprovalMode::Confirm;
    auto session = Take(runtime->OpenSession(std::move(options)), "approval session");
    auto ordinary = Take(session->Submit("ordinary-parent", "foreground first"), "ordinary submit");
    const auto ticket = Approval(session);
    Check(!ticket.job && !ticket.child, "ordinary approval acquired a Job scope");
    Take(session->ResolveApproval(ticket.request_id, sdk::ApprovalDecision::AcceptForSession), "parent grant");
    Check(Take(session->WaitResult(ordinary.operation_id, 20s), "ordinary wait").state == sdk::OperationState::Succeeded,
        "ordinary grant failed");
    Check(Read(fixture.project / "ordinary.done") == "ordinary", "ordinary command did not run");
    {
        std::lock_guard lock(script->mutex);
        script->generations = 0; script->admission_replies = 0; script->replies.clear(); script->call_prefix = "jobs-";
        script->calls = {fixture.Input("approved", true, true), fixture.Input("declined", true)};
    }
    const auto receipt = Take(session->Submit("job-approvals", "two Jobs"), "Job approval submit");
    const auto first = Approval(session);
    Check(first.job.has_value() && first.job->parent_operation_id == receipt.operation_id &&
        first.job->provider_call_id == "jobs-0" && !first.job->effective_input_sha256.empty(), "Job borrowed the parent grant");
    Take(session->ResolveApproval(first.request_id, sdk::ApprovalDecision::AcceptForSession), "Job-only grant");
    Started(fixture.project, "approved");
    const auto second = Approval(session);
    Check(second.job.has_value() && second.job->provider_call_id == "jobs-1" && *second.job != *first.job,
        "sibling Job borrowed another Job grant");
    Check(!session->ResolveApproval(first.request_id, sdk::ApprovalDecision::Accept), "old approval ticket revived");
    Take(session->ResolveApproval(second.request_id, sdk::ApprovalDecision::Decline), "decline sibling");
    Check(Take(session->WaitResult(receipt.operation_id, 20s), "approval parent").state == sdk::OperationState::Succeeded,
        "ordinary rejected-tool reply was lost");
    const auto jobs = Jobs(session, 1);
    Check(!fs::exists(fixture.project / "declined.started"), "declined sibling executed");
    Write(fixture.project / "approved.release");
    Check(Take(session->WaitJob(jobs.front().identity, 20s), "approved Job").state == "succeeded", "approved Job lost its permission");
    Take(session->Close(), "close approvals"); Take(runtime->Shutdown(), "shutdown");
}
void ParentCancellation(const fs::path& base, const fs::path& probe, bool step_limit) {
    Fixture fixture(base, probe); auto runtime = fixture.Runtime();
    auto script = std::make_shared<Script>(); script->calls = {fixture.Input("parent-stop", true, true)};
    script->block_after_admission = !step_limit;
    auto options = Options(fixture.project, script);
    if (step_limit) options.max_steps_per_turn = 1;
    auto session = Take(runtime->OpenSession(std::move(options)), "parent stop session");
    const auto receipt = Take(session->Submit("parent-stop", "launch"), "parent stop submit");
    if (!step_limit) { Started(fixture.project, "parent-stop"); Take(session->Cancel(receipt.operation_id), "cancel live parent"); }
    const auto parent = Take(session->WaitResult(receipt.operation_id, 20s), "parent stop wait");
    Check(parent.state == (step_limit ? sdk::OperationState::Failed : sdk::OperationState::Cancelled),
        "parent stop outcome changed");
    const auto id = Jobs(session, 1).front().identity;
    Check(Take(session->WaitJob(id, 20s), "parent child stop").state == "cancelled", "stopped parent left a Job alive");
    Check(!fs::exists(fixture.project / "parent-stop.done"), "stopped Job reached done");
    Take(session->Close(), "close parent stop"); Take(runtime->Shutdown(), "shutdown");
}
} // namespace

void CommandJobsCase(const std::string& path, const fs::path& base, const fs::path& probe) {
    if (path == "admission") Admission(base, probe);
    else if (path == "completion") Complete(base, probe);
    else if (path == "queue") Queue(base, probe);
    else if (path == "blocked-parent") BlockedParent(base, probe, false);
    else if (path == "failed-parent") BlockedParent(base, probe, true);
    else if (path == "isolation") Isolation(base, probe);
    else if (path == "close") Close(base, probe);
    else if (path == "approvals") Approvals(base, probe);
    else if (path == "parent-cancel") ParentCancellation(base, probe, false);
    else if (path == "step-limit") ParentCancellation(base, probe, true);
    else throw std::runtime_error("unknown command Job consumer path");
    std::cout << "[sdk-command-jobs-path] " << path << '\n';
}
void CommandJobs(const fs::path& base, const fs::path& probe) {
    for (const auto* path : {"admission", "completion", "queue", "blocked-parent", "failed-parent", "isolation", "close",
                            "approvals", "parent-cancel", "step-limit"})
        CommandJobsCase(path, base, probe);
    std::cout << "[sdk-command-jobs-consumer] complete\n";
}
void InspectCommandJobs(const fs::path& base, const fs::path& probe,
    const std::function<void(const fs::path&, const sdk::jobs::v1::Identity&)>& inspect) {
    Complete(base, probe, inspect);
}
} // namespace lubancore_consumer
