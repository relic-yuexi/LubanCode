#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Only the relocated public SDK and std. This fixture does not include a
// producer header, call a helper, or substitute a host tool for memory_save.
namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace mem = sdk::memory::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr auto kTopic = "preference.installed-write";
constexpr auto kFirst = "INSTALLED_SAVE_FIRST_7149";
constexpr auto kSecond = "INSTALLED_SAVE_SECOND_1538";
constexpr auto kThird = "INSTALLED_SAVE_THIRD_0842";
constexpr auto kPrompt = "Save the explicitly stated project preference.";

void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("memory-save: " + message);
}
template<class T> T Take(sdk::Result<T> result, const std::string& action) {
    if (!result) throw std::runtime_error("memory-save: " + action + ": " + result.error().code + " " + result.error().message);
    return std::move(*result);
}
void Take(sdk::Result<void> result, const std::string& action) {
    if (!result) throw std::runtime_error("memory-save: " + action + ": " + result.error().code + " " + result.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
fs::path Path(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Check(file.is_open(), "cannot write fixture " + Utf8(path));
    file << value;
    file.close();
    Check(!file.fail(), "cannot close fixture " + Utf8(path));
}
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    Check(file.is_open(), "missing fixture " + Utf8(path));
    std::string value{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(!file.bad(), "cannot read fixture " + Utf8(path));
    return value;
}
std::string Identity(const mem::SaveReport& report) {
    std::ostringstream out;
    for (const auto* value : {&report.session_id, &report.operation_id, &report.turn_id,
         &report.action_id, &report.workspace_key, &report.plan_sha256, &report.commit_key,
         &report.requested_event_id, &report.receipted_event_id, &report.source_event_ref, &report.save_request_sha256,
         &report.request_sha256, &report.state, &report.memory_id, &report.memory_path,
         &report.content_sha256, &report.committed_at, &report.error_code, &report.error})
        out << std::quoted(*value) << '\n';
    out << report.attempt << '\n' << report.duplicate << '\n' << report.stages.size() << '\n';
    for (const auto& stage : report.stages) out << std::quoted(stage.stage) << ' ' << std::quoted(stage.outcome) << '\n';
    return out.str();
}
struct State {
    std::atomic<unsigned> calls{0};
    bool writing = false;
    std::vector<std::string> bodies;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        if (cancellation.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        const auto call = state_->calls.fetch_add(1);
        const auto exposed = std::count_if(request.tools.begin(), request.tools.end(),
            [](const auto& tool) { return tool.name == "memory_save"; });
        if (exposed != (state_->writing ? 1 : 0))
            return std::unexpected(sdk::Error{"fixture.memory_save.surface", "write permission crossed the Session boundary"});
        if (!state_->writing) return sdk::ModelReply{"write disabled", {}, std::nullopt};
        if (call % 2 == 0) {
            const auto index = call / 2;
            if (index >= state_->bodies.size())
                return std::unexpected(sdk::Error{"fixture.memory_save.extra_model", "unexpected later model request"});
            // Reuse the provider call ID across real turns. The gate must use
            // the actual action identity, not this opaque provider string.
            const auto input = std::string(R"({"kind":"preference","id":"preference.installed-write","title":"Installed SDK project preference","summary":"Explicitly stated fixture preference","confidence":"user-stated","content":")") +
                state_->bodies[index] + R"("})";
            return sdk::ModelReply{"", {{"provider-reused-call", "memory_save", input}}, std::nullopt};
        }
        if (request.messages.empty() || std::none_of(request.messages.back().tool_replies.begin(),
            request.messages.back().tool_replies.end(), [](const auto& reply) {
                return reply.call_id == "provider-reused-call" && !reply.is_error && reply.text.find("committed") != std::string::npos;
            })) return std::unexpected(sdk::Error{"fixture.memory_save.receipt", "no real committed tool reply"});
        return sdk::ModelReply{"installed save complete", {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
std::shared_ptr<State> Writer(std::vector<std::string> bodies) {
    auto state = std::make_shared<State>();
    state->writing = true;
    state->bodies = std::move(bodies);
    return state;
}
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& base) {
    for (const auto* folder : {"data", "resources", "project"}) fs::create_directories(base / folder);
    return Take(sdk::Runtime::Create({Utf8(base / "data"), Utf8(base / "resources")}), "create runtime");
}
sdk::SessionOptions Options(const fs::path& base, const std::shared_ptr<State>& state) {
    sdk::SessionOptions options;
    options.cwd = Utf8(base / "project");
    options.model = "installed-memory-save";
    options.system_prompt = "Save only explicitly stated project preferences.";
    options.backend = std::make_unique<Backend>(state);
    options.approval_mode = sdk::ApprovalMode::Yolo;
    return options;
}
std::pair<sdk::Receipt, sdk::Operation> Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    auto receipt = Take(session->Submit(key, kPrompt), "submit");
    auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "wait");
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted,
        "save operation did not persist: " + operation.error);
    return {std::move(receipt), std::move(operation)};
}
mem::SaveReport Report(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation,
                      const mem::WriteSnapshot& plan) {
    auto reports = Take(session->GetMemorySaves(operation.operation_id), "query owned save reports");
    Check(reports.size() == 1, "one actual action did not produce exactly one report");
    auto report = std::move(reports.front());
    Check(report.session_id == session->id() && report.operation_id == operation.operation_id &&
        report.turn_id == operation.turn_id && report.workspace_key == plan.workspace_key &&
        report.plan_sha256 == plan.plan_sha256, "save report borrowed another Session/operation/turn/plan");
    Check(report.state == "committed" && report.memory_id == kTopic && !report.duplicate &&
        report.error.empty() && report.error_code.empty(), "save report did not confirm this new action");
    Check(report.action_id.starts_with("action-") && report.action_id != "provider-reused-call" &&
        !report.commit_key.empty() && !report.requested_event_id.empty() && !report.receipted_event_id.empty() &&
        !report.source_event_ref.empty(),
        "save used a provider ID or lacked its real requested fact");
    Check(report.save_request_sha256.size() == 64 && report.request_sha256.size() == 64 &&
        report.content_sha256.size() == 64 && !report.committed_at.empty() && !report.memory_path.empty(),
        "save report lacks request/body identity");
    for (const auto* name : {"intent", "snapshot", "topic", "catalog", "index", "result"})
        Check(std::count_if(report.stages.begin(), report.stages.end(), [&](const auto& stage) {
            return stage.stage == name && stage.outcome == "durable";
        }) == 1, "save stage not durably confirmed: " + std::string(name));
    auto results = Take(session->ListToolResults(operation.operation_id), "query actual tool result");
    Check(results.size() == 1 && results[0].identity.session_id == report.session_id &&
        results[0].identity.operation_id == report.operation_id && results[0].identity.turn_id == report.turn_id &&
        results[0].identity.tool_call_id == report.action_id && results[0].attempt == report.attempt &&
        results[0].tool_name == "memory_save" && results[0].selected,
        "tool result and write report belong to different actions");
    return report;
}
} // namespace

void MemorySaveSeed(const fs::path& base) {
    auto runtime = Runtime(base);
    auto off_state = std::make_shared<State>();
    auto off = Take(runtime->OpenSession(Options(base, off_state)), "open default disabled");
    Check(!Take(off->DescribeMemoryWrite(), "describe default off").enabled, "default enabled writing");
    auto [off_receipt, off_operation] = Turn(off, "save-off");
    Check(Take(off->GetMemorySaves(off_receipt.operation_id), "query disabled report").empty() &&
        off_state->calls.load() == 1, "disabled write manufactured a receipt or called another model");
    Check(!off->GetMemorySaves("op-foreign-fixture"), "disabled query accepted a foreign operation");
    Take(off->Close(), "close disabled");

    auto first_state = Writer({kFirst});
    auto options = Options(base, first_state);
    options.memory_write = mem::WriteOptions{};
    auto first = Take(runtime->OpenSession(std::move(options)), "open first writer");
    const auto plan = Take(first->DescribeMemoryWrite(), "describe write plan");
    Check(plan.enabled && plan.session_id == first->id() && !plan.workspace_key.empty() &&
        !plan.memory_directory.empty() && plan.plan_sha256.size() == 64, "write plan missing owned identity");
    Check(!Take(first->DescribeMemory(), "describe independent recall").enabled, "write also enabled recall");
    auto [receipt, operation] = Turn(first, "save-seed");
    Check(operation.final_text == "installed save complete", "wrong first provider reply");
    const auto report = Report(first, operation, plan);
    const auto frozen = Identity(report);
    Check(Read(Path(plan.memory_directory) / Path(report.memory_path)).find(kFirst) != std::string::npos,
        "committed receipt has no actual project body");
    Write(base / "save-id.txt", first->id());
    Write(base / "save-operation.txt", receipt.operation_id);
    Write(base / "save-plan.txt", plan.plan_sha256);
    Write(base / "save-report.txt", frozen);

    auto second_state = Writer({kSecond, kThird});
    options = Options(base, second_state);
    options.memory_write = mem::WriteOptions{};
    auto second = Take(runtime->OpenSession(std::move(options)), "open second writer in same project");
    const auto second_plan = Take(second->DescribeMemoryWrite(), "describe second plan");
    Check(second_plan.workspace_key == plan.workspace_key && second_plan.memory_directory == plan.memory_directory &&
        second_plan.session_id != plan.session_id, "shared project was split into forced per-session directories");
    auto [second_receipt, second_operation] = Turn(second, "save-second");
    const auto second_report = Report(second, second_operation, second_plan);
    Check(second_report.commit_key != report.commit_key && second_report.content_sha256 != report.content_sha256,
        "two sessions reused commit/body identity");
    Check(Identity(Take(first->GetMemorySaves(receipt.operation_id), "query first after shared update").at(0)) == frozen,
        "shared project update rewrote another session's old report");
    Take(first->Close(), "close first writer while second remains open");
    Check(Identity(Take(first->GetMemorySaves(receipt.operation_id), "query first after Close").at(0)) == frozen &&
        Take(first->DescribeMemoryWrite(), "query closed plan").plan_sha256 == plan.plan_sha256,
        "Close lost owned report or plan");
    auto [third_receipt, third_operation] = Turn(second, "save-third");
    const auto third_report = Report(second, third_operation, second_plan);
    Check(third_report.action_id != second_report.action_id && third_report.commit_key != second_report.commit_key,
        "reused provider ID reused an actual action/commit");
    Check(!first->GetMemorySaves(third_receipt.operation_id), "closed first Session borrowed the second Session's later operation");
    Check(Read(Path(plan.memory_directory) / Path(third_report.memory_path)).find(kThird) != std::string::npos,
        "second Session stopped writing when first closed");
    Take(second->Close(), "close second writer");
    auto mismatch_state = std::make_shared<State>();
    options = Options(base, mismatch_state);
    options.resume_session_id = first->id();
    options.memory_write = mem::WriteOptions{false};
    Check(!runtime->OpenSession(std::move(options)), "resume changed frozen write permission");
    Check(first_state->calls.load() == 2 && second_state->calls.load() == 4 && mismatch_state->calls.load() == 0,
        "Close/query/mismatch reran a model");
    Take(runtime->Shutdown(), "shutdown write seed");
}

void MemorySaveResume(const fs::path& base) {
    auto runtime = Runtime(base);
    auto state = Writer({"INSTALLED_SAVE_AFTER_RESTART_6581"});
    auto options = Options(base, state);
    options.system_prompt.clear();
    options.resume_session_id = Read(base / "save-id.txt");
    auto session = Take(runtime->OpenSession(std::move(options)), "resume with omitted write option");
    const auto plan = Take(session->DescribeMemoryWrite(), "describe restored plan");
    Check(plan.enabled && plan.plan_sha256 == Read(base / "save-plan.txt"), "resume lost frozen write permission");
    const auto old_id = Read(base / "save-operation.txt");
    const auto old_operation = Take(session->ReadOperation(old_id), "query old result");
    Check(old_operation.state == sdk::OperationState::Succeeded && old_operation.result_persisted,
        "process restart lost complete old operation");
    const auto old = Report(session, old_operation, plan);
    Check(Identity(old) == Read(base / "save-report.txt") && state->calls.load() == 0,
        "restart changed immutable report or reran save/model");
    Check(Read(Path(plan.memory_directory) / Path(old.memory_path)).find(kThird) != std::string::npos,
        "resume restored an old body over a later shared-project update");
    const auto duplicate = Take(session->Submit("save-seed", kPrompt), "repeat original request");
    Check(duplicate.duplicate && duplicate.operation_id == old_id && state->calls.load() == 0,
        "duplicate repeated model/save or changed operation identity");
    auto [receipt, operation] = Turn(session, "save-after-restart");
    const auto fresh = Report(session, operation, plan);
    Check(fresh.turn_id != old.turn_id && fresh.action_id != old.action_id && fresh.commit_key != old.commit_key,
        "restart reused actual turn/action/commit identity");
    Check(Identity(Take(session->GetMemorySaves(old_id), "query old after new save").at(0)) == Identity(old),
        "later write changed old report");
    Take(session->Close(), "close resumed writer");
    Check(Identity(Take(session->GetMemorySaves(receipt.operation_id), "query closed new save").at(0)) == Identity(fresh),
        "closed resumed writer lost new metadata");
    Check(state->calls.load() == 2, "restart query duplicated a model invocation");
    Take(runtime->Shutdown(), "shutdown write resume");
}
} // namespace lubancore_consumer
