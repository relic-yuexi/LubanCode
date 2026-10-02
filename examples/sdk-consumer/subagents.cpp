#include <lubancore/core.hpp>
#include <lubancore/subagents.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// This exact source also supplies the focused fixture. It uses only the public
// installed SDK and std: no private factory, reader, writer or patched tool.
namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace child = sdk::subagents::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Utf8(const fs::path& p) { const auto s = p.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
fs::path Path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string Read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary); Check(f.is_open(), "fixture cannot read bytes");
    std::string s{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    Check(!f.bad(), "fixture read failed"); return s;
}
void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary | std::ios::trunc);
    Check(f.is_open(), "fixture cannot create file"); f.write(s.data(), static_cast<std::streamsize>(s.size()));
    f.close(); Check(!f.fail(), "fixture file write failed");
}
bool Has(const sdk::ModelRequest& r, const std::string& name) {
    return std::any_of(r.tools.begin(), r.tools.end(), [&](const auto& t) { return t.name == name; });
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; unsigned entered = 0; bool released = false;
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    bool Enter(sdk::Cancellation c) {
        std::unique_lock lock(mutex); ++entered; cv.notify_all();
        while (!released && !c.requested()) cv.wait_for(lock, 5ms);
        return !c.requested();
    }
    void Wait(unsigned count) {
        std::unique_lock lock(mutex);
        Check(cv.wait_for(lock, 20s, [&] { return entered >= count; }), "four children did not overlap");
    }
};
struct ReleaseGate { std::shared_ptr<Gate> gate; ~ReleaseGate() { if (gate) gate->Release(); } };
struct State {
    std::mutex mutex;
    std::vector<sdk::ModelRequest> requests;
    std::atomic<int> calls{0}, tools{0}, active{0}, max_active{0};
    std::string marker = "SDKCHILD_OWN";
    std::string argument = R"({"title":"explicit child","prompt":"SDKCHILD_OWN task","agent_type":"general-purpose"})";
    bool parent_grant_first = false, read_file = false, stop_at_first = false, wall_wait = false, child_final_first = false;
    std::shared_ptr<Gate> gate;
    std::function<void()> before_child_final;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) {
        const auto current = ++active;
        auto largest = max_active.load();
        while (largest < current && !max_active.compare_exchange_weak(largest, current)) {}
        struct Exit { std::atomic<int>& count; ~Exit() { --count; } } exit{active};
        {
            std::lock_guard lock(mutex); requests.push_back(request);
        }
        const auto n = calls.fetch_add(1);
        if (Has(request, "agent")) {
            Check(request.model == "parent-model", "child changed the shared parent model binding");
            if (parent_grant_first && n == 0)
                return sdk::ModelReply{"", {{"same-call", "guarded", "{}"}}, sdk::Usage{1, 1}};
            if (n == (parent_grant_first ? 1 : 0))
                return sdk::ModelReply{"", {{"same-call", "agent", argument}}, sdk::Usage{1, 1}};
            for (const auto& m : request.messages) for (const auto& reply : m.tool_replies)
                if (reply.call_id == "same-call") {
                    Check(reply.text.find("sdk.subagent.dispatch_rejected") != std::string::npos ||
                        reply.text.find(marker) != std::string::npos || stop_at_first || wall_wait,
                        "parent did not receive its child result or explicit refusal");
                }
            return sdk::ModelReply{marker + " parent final", {}, sdk::Usage{1, 1}};
        }
        Check(request.model == "child-model", "child request ignored its frozen model binding");
        Check(!Has(request, "agent") && !Has(request, "memory_save"), "child inherited recursive dispatch or parent Memory write");
        const auto offset = parent_grant_first ? 2 : 1;
        if (n == offset && gate && !gate->Enter(cancel))
            return std::unexpected(sdk::Error{"fixture.cancelled", "own invocation cancelled"});
        if (child_final_first) return sdk::ModelReply{marker + " child final", {}, sdk::Usage{1, 1}};
        if (wall_wait) {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
            Check(cancel.requested(), "child wall budget did not raise its actual cancel flag");
            return std::unexpected(sdk::Error{"fixture.cancelled", "wall budget cancellation observed"});
        }
        if (read_file) {
            Check(request.tools.size() == 1 && Has(request, "read_file"), "Explore tool intersection differs from frozen facts");
            if (n == offset) return sdk::ModelReply{"", {{"same-call", "read_file", R"({"path":"owned.txt"})"}}, sdk::Usage{1, 1}};
        } else {
            Check(request.tools.size() == 1 && Has(request, "guarded"), "child tools widened beyond the exact selected overlay");
            if (n == offset || (!stop_at_first && n == offset + 1))
                return sdk::ModelReply{"", {{"same-call", "guarded", "{}"}}, sdk::Usage{1, 1}};
        }
        if (before_child_final) before_child_final();
        return sdk::ModelReply{marker + " child final", {}, sdk::Usage{1, 1}};
    }
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        return state_->Generate(request, cancel);
    }
private: std::shared_ptr<State> state_;
};
struct Rig {
    fs::path base, cwd;
    std::unique_ptr<sdk::Runtime> runtime;
    explicit Rig(fs::path directory) : base(std::move(directory)) {
        fs::create_directories(base / "project"); fs::create_directories(base / "resources"); base = fs::canonical(base); cwd = base / "project";
        Write(cwd / "owned.txt", "SDKCHILD_OWN file\n");
        auto opened = sdk::Runtime::Create({Utf8(base / "data"), Utf8(base / "resources")});
        Check(opened.has_value(), "Runtime creation failed"); runtime = std::move(*opened);
    }
    sdk::SessionOptions Options(std::shared_ptr<State> state, bool enabled = true, fs::path project = {}) const {
        sdk::SessionOptions o; o.cwd = Utf8(project.empty() ? cwd : project); o.model = "parent-model";
        o.system_prompt = "SDK_PARENT_STATIC_SYSTEM"; o.backend = std::make_unique<Backend>(state);
        o.max_steps_per_turn = 20; o.approval_timeout = 10s;
        sdk::Tool t; t.name = "guarded"; t.description = "owned child fixture effect"; t.requires_approval = true;
        const auto expected_cwd = o.cwd;
        t.execute = [state, expected_cwd](const std::string&, const sdk::ToolContext& context) -> sdk::Result<sdk::ToolResult> {
            Check(context.cwd == expected_cwd, "child effect crossed the actual project cwd");
            Check(!context.cancellation.requested(), "cancelled invocation reached its effect");
            ++state->tools; return sdk::ToolResult{state->marker + " effect", false};
        };
        o.custom_tools.push_back(std::move(t));
        if (enabled) o.subagents = child::Options{true, {{"general-purpose", {"guarded"}, "child-model", 8, 15}}};
        return o;
    }
    std::shared_ptr<sdk::Session> Open(sdk::SessionOptions o) const {
        auto result = runtime->OpenSession(std::move(o));
        const auto issue = result.has_value() ? std::string() : result.error().code + ": " + result.error().message;
        Check(result.has_value(), "Session open failed: " + issue); return *result;
    }
};
struct Completed { sdk::Operation operation; std::vector<sdk::Approval> tickets; };
Completed Execute(const std::shared_ptr<sdk::Session>& session, bool parent_first = false) {
    auto events = session->Subscribe(); Check(events.has_value(), "Subscribe failed");
    auto queued = session->Submit("actual-child-key", "run explicit child"); Check(queued.has_value(), "Submit failed");
    Completed result;
    const auto deadline = std::chrono::steady_clock::now() + 40s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto next = (*events)->Next(100ms); Check(next.has_value(), "event stream closed before operation completed");
        if (!*next) continue;
        auto event = std::move(**next);
        if (event.approval) {
            Check(event.operation_id == queued->operation_id && event.approval->operation_id == queued->operation_id,
                "ticket belongs to another accepted operation");
            if (parent_first && result.tickets.empty()) Check(!event.approval->child, "parent grant fixture started with a child ticket");
            else {
                Check(event.approval->child.has_value(), "real child ticket has no owned scope");
                const auto& s = *event.approval->child;
                Check(s.host_session_id == session->id() && s.parent_session_id == session->id() &&
                    s.host_operation_id == queued->operation_id && !s.child_session_id.empty() && !s.child_run_id.empty() &&
                    !s.child_turn_id.empty() && !s.child_declared_action_id.empty() && !s.child_declared_message_id.empty(),
                    "child ticket lost real host/declaration ownership");
            }
            auto resolved = session->ResolveApproval(event.approval->request_id, sdk::ApprovalDecision::AcceptForSession);
            Check(resolved.has_value(), "current child AcceptForSession did not resolve");
            Check(!session->ResolveApproval(event.approval->request_id, sdk::ApprovalDecision::Accept), "late reply revived a retired ticket");
            result.tickets.push_back(*event.approval);
        }
        if (event.kind == "operation_completed") {
            auto completed = session->WaitResult(queued->operation_id, 5s); Check(completed.has_value(), "Wait failed");
            result.operation = std::move(*completed); return result;
        }
    }
    (void)session->Cancel(queued->operation_id);
    throw std::runtime_error("public child operation did not finish before fixture deadline");
}
std::vector<child::Report> Reports(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation, bool live = true) {
    auto report = session->GetSubagentReports(operation.operation_id);
    const auto issue = report.has_value() ? std::string() : report.error().code + ": " + report.error().message;
    Check(report.has_value() && report->size() == 1, "strict child report missing: " + issue);
    const auto& r = report->front();
    Check(r.operation_id == operation.operation_id && r.parent_turn_id == operation.turn_id && r.parent_attempt > 0 &&
        !r.parent_action_id.empty() && r.adoption_state == child::AdoptionState::Validated && r.adoption.has_value(), "child report has no verified parent adoption");
    const auto& a = *r.adoption;
    Check(a.parent_session_id == session->id() && a.action_id == r.parent_action_id && a.attempt == r.parent_attempt &&
        !a.spawn_event_id.empty() && !a.observation_event_id.empty() && !a.raw_persisted_event_id.empty() &&
        !a.child_terminal_event_id.empty() && !a.child_terminal_hash.empty() &&
        !a.selected_event_id.empty() && !a.original_tool_message_id.empty() && !a.admission_event_id.empty() &&
        !a.consumed_tool_message_id.empty() && !a.prepared_event_id.empty() && a.prepared_context_revision > 0,
        "owned report collapsed spawn/terminal/selection/adoption facts");
    Check(live == r.live_receipt.has_value(), "resume manufactured live Finish/Close evidence");
    if (live) Check(r.live_receipt->child_session_id == a.child_session_id && r.live_receipt->child_run_id == a.child_run_id &&
        r.live_receipt->terminal_event_id == a.child_terminal_event_id && r.live_receipt->terminal_hash == a.child_terminal_hash &&
        r.live_receipt->append_confirmation == "committed" && r.live_receipt->seal_state == "closed",
        "actual checked child Finish/Close differs from historical terminal");
    return *report;
}
fs::path OnePlan(const Rig& rig, const std::string& id) {
    std::vector<fs::path> found;
    for (const auto& p : fs::recursive_directory_iterator(rig.base / "data"))
        if (p.path().filename() == "sdk-subagent-plan.json" && p.path().parent_path().filename() == Path(id)) found.push_back(p.path());
    Check(found.size() == 1, "fixture could not locate the uniquely owned plan"); return found.front();
}
fs::path ChildStream(const fs::path& parent, const std::string& child_id) {
    std::vector<fs::path> found;
    for (const auto& file : fs::recursive_directory_iterator(parent / "subagents"))
        if (file.path().filename() == Path(child_id + ".jsonl")) found.push_back(file.path());
    Check(found.size() == 1, "actual child has no uniquely owned stream"); return found.front();
}
void DiagnoseChildFailure(const char* point, const std::shared_ptr<sdk::Session>& session,
                          const sdk::Operation& operation, State* state = nullptr) {
    std::cerr << "[sdk-child-diagnostic] point=" << point
        << " session=" << std::quoted(session->id())
        << " operation=" << std::quoted(operation.operation_id)
        << " turn=" << std::quoted(operation.turn_id)
        << " state=" << static_cast<int>(operation.state)
        << " persisted=" << operation.result_persisted
        << " error=" << std::quoted(operation.error) << '\n';
    if (state) {
        unsigned parent_requests = 0, child_requests = 0;
        {
            std::lock_guard lock(state->mutex);
            for (const auto& request : state->requests)
                Has(request, "agent") ? ++parent_requests : ++child_requests;
        }
        std::cerr << "[sdk-child-diagnostic] calls=" << state->calls.load()
            << " tools=" << state->tools.load() << " parent_requests=" << parent_requests
            << " child_requests=" << child_requests << " max_active=" << state->max_active.load() << '\n';
    }
    const auto reports = session->GetSubagentReports(operation.operation_id);
    if (!reports) {
        std::cerr << "[sdk-child-diagnostic] report=error code=" << std::quoted(reports.error().code)
            << " error=" << std::quoted(reports.error().message) << '\n';
        return;
    }
    std::cerr << "[sdk-child-diagnostic] reports=" << reports->size() << '\n';
    for (const auto& report : *reports) {
        std::cerr << "[sdk-child-diagnostic] report_operation=" << std::quoted(report.operation_id)
            << " parent_turn=" << std::quoted(report.parent_turn_id)
            << " action=" << std::quoted(report.parent_action_id) << " attempt=" << report.parent_attempt
            << " adoption_state=" << static_cast<int>(report.adoption_state)
            << " issue=" << std::quoted(report.issue) << " adopted=" << report.adoption.has_value()
            << " live=" << report.live_receipt.has_value() << '\n';
        if (!report.live_receipt) continue;
        const auto& live = *report.live_receipt;
        std::cerr << "[sdk-child-diagnostic] child_session=" << std::quoted(live.child_session_id)
            << " child_run=" << std::quoted(live.child_run_id)
            << " execution=" << std::quoted(live.execution_state)
            << " append=" << std::quoted(live.append_confirmation) << " seal=" << std::quoted(live.seal_state)
            << " terminal_kind=" << std::quoted(live.terminal_kind)
            << " terminal=" << std::quoted(live.terminal_event_id) << " seq=" << live.terminal_seq
            << " hash=" << std::quoted(live.terminal_hash)
            << " broken_after_append=" << live.broken_after_append
            << " broken_after_close=" << live.broken_after_close
            << " append_error=" << std::quoted(live.append_error_code)
            << " close_error=" << std::quoted(live.close_error_code) << '\n';
    }
}
void CheckStoppedReport(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation,
                        const std::string& expected_cwd, bool live = true, State* state = nullptr) {
    auto plan = session->DescribeSubagents();
    Check(plan.has_value() && plan->session_id == session->id() && plan->cwd == expected_cwd,
        "cancel/Close lost its own frozen Session/cwd");
    auto reports = session->GetSubagentReports(operation.operation_id);
    Check(reports.has_value() && reports->size() == 1, "known cancelled child lost its owned report");
    const auto& report = reports->front();
    Check(report.operation_id == operation.operation_id && report.parent_turn_id == operation.turn_id &&
        !report.parent_action_id.empty() && report.parent_attempt > 0 && live == report.live_receipt.has_value() &&
        report.adoption_state == child::AdoptionState::Incomplete && !report.adoption &&
        report.issue == "subagent.adoption.prepared_consumption_pending",
        "stopped child report belongs to another operation or lost its actual receipt");
    if (live && !(report.live_receipt->execution_state == "cancelled" &&
        !report.live_receipt->child_session_id.empty() && !report.live_receipt->child_run_id.empty() &&
        report.live_receipt->append_confirmation == "committed" && report.live_receipt->seal_state == "closed"))
        DiagnoseChildFailure("cancelled-child-finish-close", session, operation, state);
    if (live) Check(report.live_receipt->execution_state == "cancelled" &&
        !report.live_receipt->child_session_id.empty() && !report.live_receipt->child_run_id.empty() &&
        report.live_receipt->append_confirmation == "committed" && report.live_receipt->seal_state == "closed",
        "known cancelled child lost its checked Finish/Close");
}
struct ArtifactFault {
    fs::path path, backup; bool active = false, obstacle_written = false;
    std::string setup_error;
    void Block() {
        Check(!active && !fs::exists(backup), "artifact fault was not uniquely armed");
        try {
            fs::rename(path, backup); active = true; Write(path, "not an artifact directory");
            obstacle_written = true;
        } catch (const std::exception& error) {
            setup_error = error.what();
            throw;
        }
    }
    void Restore() {
        if (!active) return;
        Check(fs::remove(path), "fixture artifact obstruction was not removed");
        fs::rename(backup, path); active = false;
    }
    ~ArtifactFault() {
        if (!active) return;
        std::error_code ignored; fs::remove(path, ignored); fs::rename(backup, path, ignored);
    }
};
struct RestoreArtifacts { std::shared_ptr<ArtifactFault> fault; ~RestoreArtifacts() { if (fault && fault->active) {
    std::error_code ignored; fs::remove(fault->path, ignored); fs::rename(fault->backup, fault->path, ignored); fault->active = false;
} } };
void CaptureFault(const fs::path& base) {
    Rig rig(base); auto state = std::make_shared<State>(); auto session = rig.Open(rig.Options(state));
    const auto parent = OnePlan(rig, session->id()).parent_path();
    auto fault = std::make_shared<ArtifactFault>(); fault->path = parent / "artifacts"; fault->backup = parent / "fixture-saved-artifacts";
    RestoreArtifacts restore{fault}; // Restore before Session destructor/Close on failure.
    state->before_child_final = [fault] { fault->Block(); };
    const auto result = Execute(session);
    if (!(result.operation.state == sdk::OperationState::Indeterminate && !result.operation.result_persisted)) {
        DiagnoseChildFailure("parent-capture-fault", session, result.operation, state.get());
        std::error_code path_error, backup_error, regular_error;
        const bool path_exists = fs::exists(fault->path, path_error);
        const bool backup_exists = fs::exists(fault->backup, backup_error);
        const bool path_regular = fs::is_regular_file(fault->path, regular_error);
        std::cerr << "[sdk-child-diagnostic] fault_active=" << fault->active
            << " obstacle_written=" << fault->obstacle_written
            << " logical_path=artifacts native_length=" << fault->path.native().size()
            << " exists=" << path_exists << " exists_error=" << path_error.value()
            << " exists_message=" << std::quoted(path_error.message())
            << " regular=" << path_regular << " regular_error=" << regular_error.value()
            << " logical_backup=fixture-saved-artifacts native_length=" << fault->backup.native().size()
            << " backup_exists=" << backup_exists << " backup_error=" << backup_error.value()
            << " backup_message=" << std::quoted(backup_error.message())
            << " setup_error=" << std::quoted(fault->setup_error) << '\n';
    }
    Check(result.operation.state == sdk::OperationState::Indeterminate && !result.operation.result_persisted,
        "real parent result capture failure claimed completion");
    Check(result.operation.error.find("sdk.side_effect.indeterminate") != std::string::npos &&
        result.operation.error.find("sdk.memory_write.indeterminate") == std::string::npos &&
        result.operation.error.find("tool.capture.store_unavailable") != std::string::npos &&
        result.operation.error.find("Tool capture persistence failed:") != std::string::npos,
        "generic capture failure lost its original diagnostic or borrowed a Memory error code");
    Check(state->calls == 4 && state->tools == 2, "capture failure continued the parent model or reran effects");
    const auto reports = session->GetSubagentReports(result.operation.operation_id);
    Check(reports.has_value() && reports->size() == 1, "capture gap lost its owned child report");
    const auto& report = reports->front();
    Check(report.operation_id == result.operation.operation_id && report.parent_turn_id == result.operation.turn_id &&
        !report.parent_action_id.empty() && report.parent_attempt > 0 && report.adoption_state == child::AdoptionState::Incomplete &&
        report.issue == "subagent.adoption.raw_capture_pending" && !report.adoption && report.live_receipt.has_value() &&
        report.live_receipt->execution_state == "succeeded" && report.live_receipt->append_confirmation == "committed" &&
        report.live_receipt->seal_state == "closed", "parent capture gap fabricated adoption or erased actual child Finish/Close");
    const auto sid = session->id(); const auto original_error = result.operation.error;
    fault->Restore(); (void)session->Close(); session.reset(); // Close may report the real prior failure.
    Check(Read(parent / "operations.jsonl").find("\"executionStatus\":\"interrupted\"") != std::string::npos,
        "unknown effect did not retain its actual interrupted final fact");
    auto recovered_state = std::make_shared<State>(); auto recovered_options = rig.Options(recovered_state, false);
    recovered_options.system_prompt.clear(); recovered_options.resume_session_id = sid;
    auto recovered = rig.Open(std::move(recovered_options));
    auto unknown = recovered->WaitResult(result.operation.operation_id, 1s);
    Check(unknown.has_value() && unknown->state == sdk::OperationState::Indeterminate && !unknown->result_persisted &&
        !unknown->error.empty() && original_error.starts_with(unknown->error) &&
        unknown->error.find("sdk.side_effect.indeterminate") != std::string::npos &&
        unknown->error.find("tool.capture.store_unavailable") != std::string::npos &&
        recovered_state->calls == 0 && recovered_state->tools == 0,
        "interrupted capture resumed as success, lost the real error or reran execution");
    auto gap = recovered->GetSubagentReports(result.operation.operation_id);
    Check(gap.has_value() && gap->size() == 1 && gap->front().adoption_state == child::AdoptionState::Incomplete &&
        gap->front().issue == "subagent.adoption.raw_capture_pending" && !gap->front().adoption && !gap->front().live_receipt &&
        gap->front().operation_id == result.operation.operation_id && gap->front().parent_turn_id == result.operation.turn_id &&
        gap->front().parent_action_id == report.parent_action_id && gap->front().parent_attempt == report.parent_attempt,
        "historical capture gap manufactured live receipt or changed its actual owner");
    Check(recovered->Close().has_value(), "historical interrupted capture Close failed");
}
void ParentStepBudget(const fs::path& base) {
    Rig rig(base); auto state = std::make_shared<State>(); state->child_final_first = true;
    auto options = rig.Options(state); options.max_steps_per_turn = 1; options.subagents->profiles.front().max_steps_per_turn = 1;
    auto session = rig.Open(std::move(options)); const auto result = Execute(session);
    Check(result.operation.state == sdk::OperationState::Failed && result.operation.result_persisted &&
        result.operation.error == "sdk.turn.limit_reached" && state->calls == 2 && state->tools == 0 && result.tickets.empty(),
        "known parent step cap was rewritten unknown or admitted another model turn");
    auto reports = session->GetSubagentReports(result.operation.operation_id);
    Check(reports.has_value() && reports->size() == 1 && reports->front().operation_id == result.operation.operation_id &&
        reports->front().parent_turn_id == result.operation.turn_id && !reports->front().adoption &&
        reports->front().adoption_state == child::AdoptionState::Incomplete &&
        reports->front().issue == "subagent.adoption.prepared_consumption_pending" && reports->front().live_receipt.has_value() &&
        reports->front().live_receipt->execution_state == "succeeded" &&
        reports->front().live_receipt->append_confirmation == "committed" && reports->front().live_receipt->seal_state == "closed",
        "parent limit confused successful child Finish with a consumed parent request");
    const auto sid = session->id(); Check(session->Close().has_value(), "known parent limit Close failed"); session.reset();
    auto resumed_state = std::make_shared<State>(); auto resumed_options = rig.Options(resumed_state, false);
    resumed_options.max_steps_per_turn = 1; resumed_options.system_prompt.clear(); resumed_options.resume_session_id = sid;
    auto resumed = rig.Open(std::move(resumed_options)); auto failed = resumed->WaitResult(result.operation.operation_id, 1s);
    Check(failed.has_value() && failed->state == sdk::OperationState::Failed && failed->result_persisted &&
        failed->error == "sdk.turn.limit_reached" && resumed_state->calls == 0 && resumed_state->tools == 0,
        "known parent limit failed recovery or reran its child");
    auto pending = resumed->GetSubagentReports(result.operation.operation_id);
    Check(pending.has_value() && pending->size() == 1 && !pending->front().live_receipt && !pending->front().adoption &&
        pending->front().adoption_state == child::AdoptionState::Incomplete &&
        pending->front().issue == "subagent.adoption.prepared_consumption_pending" &&
        pending->front().parent_action_id == reports->front().parent_action_id &&
        pending->front().parent_attempt == reports->front().parent_attempt,
        "known parent limit manufactured a consumed or live historical report");
    Check(resumed->Close().has_value(), "known limit resume Close failed");
}
void Successful(const fs::path& base, bool parent_first = false) {
    Rig rig(base); auto state = std::make_shared<State>(); state->parent_grant_first = parent_first;
    auto session = rig.Open(rig.Options(state)); auto result = Execute(session, parent_first);
    Check(result.operation.state == sdk::OperationState::Succeeded && result.operation.result_persisted, "child operation failed");
    Check(state->tools == (parent_first ? 3 : 2) && result.tickets.size() == (parent_first ? 2 : 1), "child grant did not stay within this child");
    Check(state->max_active == 1, "parent and child concurrently used the shared backend");
    const auto report = Reports(session, result.operation);
    auto before = session->DescribeSubagents(); Check(before.has_value() && before->enabled && before->profiles.size() == 1 &&
        before->profiles.front().effective_tools == std::vector<std::string>{"guarded"} &&
        before->profiles.front().effective_model == "child-model" && !before->plan_sha256.empty(), "frozen host plan missing");
    Check(session->Close().has_value(), "checked SDK Close failed");
    auto after = session->DescribeSubagents(); Check(after.has_value() && after->plan_sha256 == before->plan_sha256, "Close lost frozen values");
    Check(Reports(session, result.operation).front().adoption->selected_event_id == report.front().adoption->selected_event_id,
        "Close changed the frozen report");
}
} // namespace

void SubagentCase(const std::string& name, const fs::path& base) {
    if (name == "success") { Successful(base); return; }
    if (name == "parent-grant") { Successful(base, true); return; }
    if (name == "default-off") {
        Rig rig(base); auto state = std::make_shared<State>(); auto o = rig.Options(state, false);
        // Only the main request is expected; no child tool can be admitted.
        class Off final : public sdk::Backend {
        public: sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& r, sdk::Cancellation) override {
            Check(!Has(r, "agent"), "default-off SDK exposed agent"); return sdk::ModelReply{"off", {}, sdk::Usage{1, 1}};
        }};
        o.backend = std::make_unique<Off>(); auto s = rig.Open(std::move(o)); auto result = Execute(s);
        auto plan = s->DescribeSubagents(); auto reports = s->GetSubagentReports(result.operation.operation_id);
        Check(plan.has_value() && reports.has_value() && !plan->enabled && reports->empty(), "default-off report grew a child");
        Check(s->Close().has_value(), "off Close failed"); return;
    }
    if (name == "budgets") {
        Rig rig(base); auto state = std::make_shared<State>();
        for (const auto value : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::max(), std::int64_t{21}}) {
            auto o = rig.Options(state); o.subagents->profiles.front().max_steps_per_turn = value;
            auto result = rig.runtime->OpenSession(std::move(o));
            Check(!result && result.error().code == "sdk.subagent.invalid_plan", "invalid/widening step budget opened");
        }
        for (const auto value : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::max()}) {
            auto o = rig.Options(state); o.subagents->profiles.front().max_wall_seconds = value;
            Check(!rig.runtime->OpenSession(std::move(o)), "missing/overflow duration budget opened");
        }
        auto absent = rig.Options(state); absent.subagents->profiles.clear(); Check(!rig.runtime->OpenSession(std::move(absent)), "enabled empty plan opened");
        Check(state->calls == 0 && state->tools == 0, "invalid plan reached model or effect"); return;
    }
    if (name == "reserved-tools") {
        Rig rig(base); auto state = std::make_shared<State>();
        for (const auto& tool : {"agent", "memory_save", "todo_write"}) {
            auto o = rig.Options(state); o.subagents->profiles.front().tools = {tool};
            auto result = rig.runtime->OpenSession(std::move(o));
            Check(!result && result.error().code == "sdk.subagent.invalid_plan", "reserved child tool opened");
        }
        auto missing = rig.Options(state); missing.subagents->profiles.front().tools = {"missing"};
        auto result = rig.runtime->OpenSession(std::move(missing)); Check(!result && result.error().code == "sdk.subagent.tool_missing", "missing tool opened");
        auto collision = rig.Options(state); sdk::Tool duplicate; duplicate.name = "agent";
        duplicate.execute = [](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> { return sdk::ToolResult{"wrong", false}; };
        collision.custom_tools.push_back(std::move(duplicate)); result = rig.runtime->OpenSession(std::move(collision));
        Check(!result && result.error().code == "sdk.tool.duplicate", "host agent collision opened");
        Check(state->calls == 0 && state->tools == 0, "refused tool plan executed"); return;
    }
    if (name == "modes") {
        const std::array<std::string, 6> rejected{
            R"({"title":"t","prompt":"p","agent_type":"general-purpose","execution_mode":"background"})",
            R"({"title":"t","prompt":"p","agent_type":"general-purpose","isolation":"worktree"})",
            R"({"title":"t","prompt":"p","agent_type":"unselected"})",
            R"({"title":"t","prompt":"p","agent_type":"general-purpose","max_steps_per_turn":9})",
            R"({"title":"t","prompt":"p","agent_type":"general-purpose","max_time_secs":0})",
            R"({"title":"t","prompt":"p","agent_type":"general-purpose","model":"unselected-model"})"};
        for (std::size_t n = 0; n < rejected.size(); ++n) {
            Rig rig(base / std::to_string(n)); auto state = std::make_shared<State>(); state->argument = rejected[n];
            auto s = rig.Open(rig.Options(state)); auto result = Execute(s);
            Check(state->calls == 2 && state->tools == 0 && result.tickets.empty(), "unsupported dispatch touched child model/effect/ticket");
            const auto reports = s->GetSubagentReports(result.operation.operation_id);
            Check(reports.has_value() && reports->empty(), "unsupported dispatch invented a child report");
            const auto subdir = OnePlan(rig, s->id()).parent_path() / "subagents";
            if (fs::exists(subdir)) for (const auto& file : fs::recursive_directory_iterator(subdir))
                Check(file.path().extension() != ".jsonl", "rejected dispatch opened child ledger");
            Check(s->Close().has_value(), "refused dispatch Close failed");
        }
        return;
    }
    if (name == "explore") {
        Rig rig(base); auto state = std::make_shared<State>(); state->read_file = true;
        state->argument = R"({"title":"explore","prompt":"SDKCHILD_OWN task","agent_type":"Explore","execution_mode":"auto"})";
        auto o = rig.Options(state); o.builtin_tools = {"read_file"};
        o.custom_tools.front().name = "search"; // Same name is not read-only metadata.
        o.subagents->profiles = {{"Explore", {"read_file", "search"}, "child-model", 8, 15}};
        auto s = rig.Open(std::move(o)); auto snapshot = s->DescribeSubagents();
        Check(snapshot.has_value() && snapshot->profiles.front().requested.tools == std::vector<std::string>{"read_file", "search"} &&
            snapshot->profiles.front().effective_tools == std::vector<std::string>{"read_file"}, "Unknown same-name tool joined Explore");
        auto result = Execute(s); Check(result.operation.result_persisted && result.tickets.empty(), "real Explore read failed");
        Reports(s, result.operation); Check(state->tools == 0, "excluded custom tool executed"); Check(s->Close().has_value(), "Explore Close failed");
        auto empty = rig.Options(std::make_shared<State>()); empty.custom_tools.front().name = "search";
        empty.subagents->profiles = {{"Explore", {"search"}, "child-model", 8, 15}};
        auto refusal = rig.runtime->OpenSession(std::move(empty)); Check(!refusal && refusal.error().code == "sdk.subagent.empty_tool_surface", "empty readonly intersection opened");
        return;
    }
    if (name == "resume" || name == "plan-drift") {
        Rig rig(base); auto state = std::make_shared<State>(); auto s = rig.Open(rig.Options(state)); auto result = Execute(s);
        const auto plan = s->DescribeSubagents(); Check(plan.has_value(), "seed plan missing");
        const auto saved_report = Reports(s, result.operation);
        auto selected = s->ListToolResults(result.operation.operation_id); Check(selected.has_value(), "seed result list missing");
        std::vector<std::string> formal;
        for (const auto& row : *selected) if (row.tool_name == "agent" && row.selected &&
            row.identity.tool_call_id == saved_report.front().parent_action_id && row.identity.result_id.starts_with("res-"))
            formal.push_back(row.identity.result_id);
        Check(formal.size() == 1, "actual selected agent material is not unique");
        const auto id = s->id(); Check(s->Close().has_value(), "seed Close failed"); s.reset();
        auto resumed_state = std::make_shared<State>();
        auto o = rig.Options(resumed_state, false); o.system_prompt.clear(); o.resume_session_id = id;
        if (name == "plan-drift") {
            const auto path = OnePlan(rig, id), journal = path.parent_path() / Path(id + ".jsonl");
            const auto child_stream = ChildStream(path.parent_path(), saved_report.front().adoption->child_session_id);
            const auto material = path.parent_path() / "artifacts" / Path(formal.front() + ".json");
            const auto operations = path.parent_path() / "operations.jsonl";
            const auto old_operations = Read(operations);
            const auto old = Read(path), old_journal = Read(journal), child_bytes = Read(child_stream), material_bytes = Read(material);
            // Each failure reaches the actual public resume boundary. Restore
            // the owned leaf after checking that preflight wrote no parent row.
            for (int fault = 0; fault != 10; ++fault) {
                if (fault == 0) Write(path, old + "\n");
                if (fault == 1) Check(fs::remove(path), "plan removal was not real");
                if (fault == 2) Write(path, old.substr(0, old.size() / 2));
                if (fault == 3) Check(fs::remove(child_stream), "child removal was not real");
                if (fault == 4) {
                    const auto end = child_bytes.find('\n'); Check(end != std::string::npos, "actual child has no first row");
                    Write(child_stream, child_bytes.substr(0, end + 1));
                }
                if (fault == 5) Check(fs::remove(material), "actual selected material removal was not real");
                if (fault == 6) Write(operations, old_operations + "{\"kind\":");
                if (fault == 7) {
                    const auto begin = old_operations.rfind('\n', old_operations.size() - 2);
                    Check(begin != std::string::npos, "operation final has no preceding fact");
                    const auto final = old_operations.substr(begin + 1);
                    Check(final.find("\"kind\":\"operation.final\"") != std::string::npos, "last actual fact is not final");
                    Write(operations, old_operations + final);
                }
                if (fault == 8) {
                    auto bad = old_operations; const std::string status = "\"executionStatus\":\"success\"";
                    const auto at = bad.find(status);
                    Check(at != std::string::npos && bad.find(status, at + status.size()) == std::string::npos,
                        "actual successful final was not uniquely found");
                    bad.replace(at, status.size(), "\"executionStatus\":\"unknown_status\""); Write(operations, bad);
                }
                if (fault == 9) {
                    auto bad = old_operations;
                    const auto turn = "\"turnId\":\"" + result.operation.turn_id + "\"";
                    const auto at = bad.find(turn);
                    Check(at != std::string::npos && bad.find(turn, at + turn.size()) == std::string::npos,
                        "actual final turn was not uniquely found");
                    bad.replace(at, turn.size(), "\"turnId\":\"foreign-turn\""); Write(operations, bad);
                }
                auto broken = rig.Options(resumed_state, false); broken.system_prompt.clear(); broken.resume_session_id = id;
                auto refusal = rig.runtime->OpenSession(std::move(broken));
                Check(!refusal && (refusal.error().code.starts_with("sdk.subagent.") ||
                    refusal.error().code == "sdk.resume.operation_ledger_invalid"), "broken plan/child/adoption facts resumed");
                Check(Read(journal) == old_journal && resumed_state->calls == 0 && resumed_state->tools == 0,
                    "rejected historical adoption changed parent journal or reran model/effect");
                Write(path, old); Write(child_stream, child_bytes); Write(material, material_bytes); Write(operations, old_operations);
            }
            // A genuine crash prefix has acceptance + dispatch, but no durable
            // final. It remains Indeterminate with an explicit report gap.
            std::string prefix; std::size_t begin = 0; unsigned removed = 0;
            while (begin < old_operations.size()) {
                const auto end = old_operations.find('\n', begin); Check(end != std::string::npos, "operation fact was incomplete");
                const auto row = old_operations.substr(begin, end - begin + 1);
                if (row.find("\"kind\":\"operation.final\"") != std::string::npos) ++removed;
                else prefix += row;
                begin = end + 1;
            }
            Check(removed == 1, "fixture did not remove the actual final fact"); Write(operations, prefix);
            std::string open_journal; begin = 0; bool found_close = false;
            while (begin < old_journal.size()) {
                const auto end = old_journal.find('\n', begin); Check(end != std::string::npos, "parent journal row was incomplete");
                const auto row = old_journal.substr(begin, end - begin + 1);
                if (row.find("\"kind\":\"session.ended\"") != std::string::npos) { found_close = true; break; }
                open_journal += row; begin = end + 1;
            }
            Check(found_close, "fixture did not find actual checked parent Close"); Write(journal, open_journal);
            auto interrupted_options = rig.Options(resumed_state, false); interrupted_options.system_prompt.clear(); interrupted_options.resume_session_id = id;
            auto interrupted = rig.Open(std::move(interrupted_options));
            auto unknown = interrupted->WaitResult(result.operation.operation_id, 1s);
            Check(unknown.has_value() && unknown->state == sdk::OperationState::Indeterminate && !unknown->result_persisted,
                "no-final crash prefix was rejected or rewritten as completed");
            auto gap = interrupted->GetSubagentReports(result.operation.operation_id);
            Check(!gap && gap.error().code == "sdk.subagent.report_unavailable" && !gap.error().message.empty(),
                "interrupted owner manufactured an adopted child report");
            Check(resumed_state->calls == 0 && resumed_state->tools == 0, "interrupted dispatch reran");
            Check(interrupted->Close().has_value(), "interrupted session Close failed"); interrupted.reset();
            Write(operations, old_operations); Write(journal, old_journal);
            o = rig.Options(resumed_state, false); o.system_prompt.clear(); o.resume_session_id = id;
        }
        auto resumed = rig.Open(std::move(o)); Check(resumed->id() == id && resumed_state->calls == 0, "resume replaced Session or reran model");
        auto resumed_plan = resumed->DescribeSubagents();
        Check(resumed_plan.has_value() && resumed_plan->plan_sha256 == plan->plan_sha256, "omitted resume changed frozen plan");
        Reports(resumed, result.operation, false); Check(resumed->Close().has_value(), "resume Close failed"); resumed.reset();
        auto mismatch = rig.Options(resumed_state); mismatch.system_prompt.clear(); mismatch.resume_session_id = id;
        mismatch.subagents->profiles.front().max_steps_per_turn = 7;
        auto refused = rig.runtime->OpenSession(std::move(mismatch)); Check(!refused && refused.error().code == "sdk.subagent.resume_mismatch", "resume budget silently changed");
        Check(resumed_state->calls == 0, "resume gate reran accepted operation"); return;
    }
    if (name == "step-budget" || name == "wall-budget") {
        Rig rig(base); auto state = std::make_shared<State>(); state->stop_at_first = true; state->wall_wait = name == "wall-budget";
        auto o = rig.Options(state); o.approval_mode = sdk::ApprovalMode::Yolo;
        if (state->wall_wait) o.subagents->profiles.front().max_wall_seconds = 1;
        else o.subagents->profiles.front().max_steps_per_turn = 1;
        auto s = rig.Open(std::move(o)); const auto began = std::chrono::steady_clock::now(); auto result = Execute(s);
        auto report = Reports(s, result.operation);
        Check(report.front().live_receipt->execution_state != "succeeded", "child budget claimed successful execution");
        if (state->wall_wait) Check(std::chrono::steady_clock::now() - began >= 700ms && state->tools == 0, "wall limit did not guard the real child");
        else Check(state->tools == 1 && state->calls == 3, "step budget admitted another child model turn");
        Check(s->Close().has_value(), "budget Close failed");
        if (name == "step-budget") {
            CaptureFault(base / "capture-fault"); ParentStepBudget(base / "parent-step-budget");
        }
        return;
    }
    if (name == "four-sessions") {
        Rig rig(base); fs::create_directories(rig.base / "project-b"); fs::create_directories(rig.base / "project-c");
        auto gate = std::make_shared<Gate>();
        std::array<std::shared_ptr<State>, 4> states;
        std::array<std::shared_ptr<sdk::Session>, 4> sessions;
        std::array<std::shared_ptr<sdk::EventStream>, 4> streams;
        std::array<sdk::Receipt, 4> receipts;
        // Declared last: failure unwind opens the gate before SDK Close/join.
        ReleaseGate release{gate};
        for (std::size_t n = 0; n < 4; ++n) {
            states[n] = std::make_shared<State>(); states[n]->marker = "SDKCHILD_OWN_" + std::to_string(n); states[n]->gate = gate;
            states[n]->argument = "{\"title\":\"t\",\"prompt\":\"" + states[n]->marker + " task\",\"agent_type\":\"general-purpose\"}";
            const auto cwd = n < 2 ? rig.cwd : rig.base / (n == 2 ? "project-b" : "project-c");
            sessions[n] = rig.Open(rig.Options(states[n], true, cwd)); auto event = sessions[n]->Subscribe(); Check(event.has_value(), "isolation subscribe failed"); streams[n] = *event;
            auto receipt = sessions[n]->Submit("same-key", states[n]->marker + " request"); Check(receipt.has_value(), "isolation submit failed"); receipts[n] = *receipt;
        }
        gate->Wait(4); Check(sessions[0]->Cancel(receipts[0].operation_id).has_value(), "local cancel failed");
        Check(sessions[1]->Close().has_value(), "closing one held child did not join"); gate->Release();
        for (std::size_t n : {std::size_t{2}, std::size_t{3}}) {
            unsigned approvals = 0; bool completed = false;
            std::vector<std::string> resolved;
            const auto deadline = std::chrono::steady_clock::now() + 40s;
            while (!completed && std::chrono::steady_clock::now() < deadline) {
                auto next = streams[n]->Next(100ms); Check(next.has_value(), "another Session closed this stream"); if (!*next) continue;
                if ((**next).approval) {
                    const auto& a = *(**next).approval;
                    const auto plan = sessions[n]->DescribeSubagents();
                    Check(plan.has_value() && a.child.has_value() && a.operation_id == receipts[n].operation_id &&
                        a.cwd == plan->cwd && a.child->cwd == plan->cwd && a.child->host_session_id == sessions[n]->id() &&
                        a.child->host_operation_id == receipts[n].operation_id, "ticket crossed project/operation owner");
                    Check(sessions[n]->ResolveApproval(a.request_id, sdk::ApprovalDecision::AcceptForSession).has_value(), "owned isolation ticket failed");
                    resolved.push_back(a.request_id); ++approvals;
                }
                completed = (**next).kind == "operation_completed";
            }
            Check(completed && approvals == 1, "child-only grant crossed its Session or did not persist");
            auto result = sessions[n]->WaitResult(receipts[n].operation_id, 5s); Check(result.has_value() && result->state == sdk::OperationState::Succeeded &&
                result->final_text == states[n]->marker + " parent final" && states[n]->tools == 2, "cancel/close crossed to another Session");
            Reports(sessions[n], *result);
            {
                std::lock_guard lock(states[n]->mutex);
                for (const auto& request : states[n]->requests) for (const auto& message : request.messages)
                    for (std::size_t other = 0; other < 4; ++other) if (other != n)
                        Check(message.text.find(states[other]->marker) == std::string::npos, "foreign session text entered request");
            }
            Check(sessions[n]->Close().has_value(), "other Session Close failed");
            for (const auto& id : resolved)
                Check(!sessions[n]->ResolveApproval(id, sdk::ApprovalDecision::Accept), "late child reply revived a closed Session ticket");
        }
        for (std::size_t n : {std::size_t{0}, std::size_t{1}}) {
            auto stopped = sessions[n]->WaitResult(receipts[n].operation_id, 10s);
            Check(stopped.has_value() && stopped->state == sdk::OperationState::Cancelled && stopped->result_persisted && states[n]->tools == 0,
                "cancelled/closed child executed its effect");
            CheckStoppedReport(sessions[n], *stopped, Utf8(rig.cwd), true, states[n].get());
            for (unsigned count = 0; count != 128; ++count) {
                auto event = streams[n]->Next(0ms); if (!event || !*event) break;
                Check(!(**event).approval, "cancelled/closed child issued a fresh ticket after the held model");
            }
            Check(sessions[n]->Close().has_value(), "stopped Session Close failed");
            CheckStoppedReport(sessions[n], *stopped, Utf8(rig.cwd), true, states[n].get());
            const auto sid = sessions[n]->id();
            const auto old_reports = sessions[n]->GetSubagentReports(receipts[n].operation_id);
            Check(old_reports.has_value() && old_reports->size() == 1, "cancel seed owned report missing");
            auto resumed_state = std::make_shared<State>(); auto options = rig.Options(resumed_state, false);
            options.system_prompt.clear(); options.resume_session_id = sid;
            auto resumed = rig.Open(std::move(options)); auto known = resumed->WaitResult(receipts[n].operation_id, 1s);
            Check(known.has_value() && known->state == sdk::OperationState::Cancelled && known->result_persisted &&
                known->turn_id == stopped->turn_id && resumed_state->calls == 0 && resumed_state->tools == 0,
                "healthy cancellation did not resume its known terminal or reran a child");
            CheckStoppedReport(resumed, *known, Utf8(rig.cwd), false, resumed_state.get());
            const auto fresh_reports = resumed->GetSubagentReports(receipts[n].operation_id);
            Check(fresh_reports.has_value() && fresh_reports->front().parent_action_id == old_reports->front().parent_action_id &&
                fresh_reports->front().parent_attempt == old_reports->front().parent_attempt, "cancel resume changed owned child action");
            Check(resumed->Close().has_value(), "known cancel resume Close failed");
        }
        return;
    }
    throw std::runtime_error("unknown child fixture case: " + name);
}

void Subagents(const fs::path& base) {
    for (const auto& name : {"default-off", "success", "parent-grant", "budgets", "reserved-tools", "modes", "explore", "resume", "plan-drift", "step-budget", "wall-budget", "four-sessions"})
        SubagentCase(name, base / name);
}
void SubagentSeed(const fs::path& base) {
    Rig rig(base); auto state = std::make_shared<State>(); auto session = rig.Open(rig.Options(state));
    const auto result = Execute(session); Check(result.operation.result_persisted, "restart seed incomplete");
    Reports(session, result.operation); auto plan = session->DescribeSubagents(); Check(plan.has_value(), "restart plan missing");
    Write(rig.base / "owned-child.state", session->id() + "\n" + result.operation.operation_id + "\n" +
        result.operation.turn_id + "\n" + plan->plan_sha256 + "\n");
    Check(session->Close().has_value(), "restart seed checked Close failed");
}
void SubagentResume(const fs::path& base) {
    Rig rig(base); std::ifstream saved(rig.base / "owned-child.state");
    std::string id, op, turn, sha; std::getline(saved, id); std::getline(saved, op); std::getline(saved, turn); std::getline(saved, sha);
    Check(saved.good() && !id.empty() && !op.empty() && !turn.empty() && !sha.empty(), "restart seed facts unavailable");
    auto state = std::make_shared<State>(); auto options = rig.Options(state, false);
    options.system_prompt.clear(); options.resume_session_id = id;
    auto resumed = rig.Open(std::move(options)); auto completed = resumed->WaitResult(op, 1s);
    Check(completed.has_value() && completed->result_persisted && completed->turn_id == turn && state->calls == 0 && state->tools == 0,
        "fresh process reran operation or lost accepted identity");
    auto plan = resumed->DescribeSubagents(); Check(plan.has_value() && plan->plan_sha256 == sha, "fresh process changed frozen plan");
    Reports(resumed, *completed, false); Check(resumed->Close().has_value(), "fresh process Close failed");
}
} // namespace lubancore_consumer
