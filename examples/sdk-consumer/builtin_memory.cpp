#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Only installed public SDK headers and std. The host supplies a real project
// theme and a scripted provider; recall, history and persistence live in Core.
namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace memory = sdk::memory::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr auto kSeed = "MEMORY_SEED_BODY_6d49";
constexpr auto kLive = "MEMORY_LIVE_BODY_3c87";
constexpr auto kQuery = "SDKMEMNEEDLE project preference";
constexpr auto kEntry = "preference.sdk-fixture";
void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("memory: " + message);
}
template<class T> T Take(sdk::Result<T> value, const std::string& action) {
    if (!value) throw std::runtime_error("memory: " + action + ": " + value.error().code + " " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value, const std::string& action) {
    if (!value) throw std::runtime_error("memory: " + action + ": " + value.error().code + " " + value.error().message);
}
std::string Utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
fs::path Path(const std::string& value) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
void Write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Check(out.is_open(), "cannot write fixture");
    out << value;
    out.close();
    Check(!out.fail(), "cannot close fixture");
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    Check(in.is_open(), "missing restart fixture");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) { ++count; at += needle.size(); }
    return count;
}
std::string Theme(const std::string& marker) {
    return "---\nname: sdk-memory-fixture\ndescription: SDKMEMNEEDLE project preference\nmetadata:\n"
        "  schema: 3\n  node_type: memory\n  type: preference\n  id: preference.sdk-fixture\n"
        "  confidence: user-stated\n  status: active\n  scope: {level: project, kind: project, value: ''}\n"
        "  keywords: [SDKMEMNEEDLE]\n  evidence: []\n  fingerprints: {}\n---\n\n"
        "# SDKMEMNEEDLE project preference\n\nSDKMEMNEEDLE project preference: " + marker + "\n";
}
struct State {
    std::mutex mutex;
    std::vector<sdk::ModelRequest> requests;
    std::atomic<unsigned> calls{0};
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        if (cancellation.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        ++state_->calls;
        std::lock_guard lock(state_->mutex);
        state_->requests.push_back(request);
        for (const auto& tool : request.tools) if (tool.name == "memory_save")
            return std::unexpected(sdk::Error{"fixture.memory_write_exposed", "read-only recall admitted memory_save"});
        return sdk::ModelReply{"installed memory complete", {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
sdk::SessionOptions Options(const fs::path& base, const std::shared_ptr<State>& state) {
    sdk::SessionOptions options;
    options.cwd = Utf8(base / "project");
    options.model = "installed-memory-fixture";
    options.system_prompt = "Use explicitly recalled project preferences as context.";
    options.backend = std::make_unique<Backend>(state);
    return options;
}
std::unique_ptr<sdk::Runtime> Runtime(const fs::path& base) {
    for (const auto* folder : {"data", "resources", "project"}) fs::create_directories(base / folder);
    return Take(sdk::Runtime::Create({Utf8(base / "data"), Utf8(base / "resources")}), "create runtime");
}
std::pair<sdk::Receipt, sdk::Operation> Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    auto receipt = Take(session->Submit(key, kQuery), "submit memory turn");
    auto operation = Take(session->WaitResult(receipt.operation_id, 30s), "wait memory turn");
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted,
        "memory turn failed: " + operation.error);
    Check(operation.final_text == "installed memory complete", "wrong provider reply");
    return {std::move(receipt), std::move(operation)};
}
memory::RecallReport Report(const std::shared_ptr<sdk::Session>& session,
    const sdk::Operation& operation, const memory::Snapshot& plan) {
    auto report = Take(session->GetMemoryRecall(operation.operation_id), "read owned recall report");
    Check(report.enabled && report.session_id == session->id() && report.operation_id == operation.operation_id &&
        report.turn_id == operation.turn_id && report.workspace_key == plan.workspace_key &&
        report.plan_sha256 == plan.plan_sha256, "report belongs to another session/operation/turn/plan");
    Check(report.state == "admitted" && report.error.empty() && report.context_message_id.size() > 0 &&
        report.context_sha256.size() == 64 && report.bytes > 0, "actual context was not admitted");
    Check(std::count_if(report.entries.begin(), report.entries.end(), [](const auto& entry) {
        return entry.id == kEntry && entry.selected && !entry.stale && !entry.expired && !entry.scope_blocked && entry.bytes > 0;
    }) == 1, "real fixture topic not selected exactly once");
    return report;
}
std::string RequestText(const std::shared_ptr<State>& state, std::size_t index) {
    std::lock_guard lock(state->mutex);
    Check(index < state->requests.size(), "provider request missing");
    std::string result;
    for (const auto& message : state->requests[index].messages) result += message.text + "\n";
    return result;
}
std::string SavedIdentity(const memory::RecallReport& report) {
    return report.session_id + "\n" + report.operation_id + "\n" + report.turn_id + "\n" +
        report.workspace_key + "\n" + report.plan_sha256 + "\n" + report.state + "\n" +
        report.context_message_id + "\n" + report.context_sha256 + "\n" + std::to_string(report.bytes) + "\n";
}
} // namespace

void MemorySeed(const fs::path& base) {
    auto runtime = Runtime(base);
    auto state = std::make_shared<State>();
    auto options = Options(base, state);
    options.memory = memory::RecallOptions{};
    auto session = Take(runtime->OpenSession(std::move(options)), "open explicit recall");
    auto plan = Take(session->DescribeMemory(), "describe project recall");
    Check(plan.enabled && plan.session_id == session->id() && !plan.workspace_key.empty() &&
        !plan.memory_directory.empty() && plan.plan_sha256.size() == 64, "incomplete public Memory plan");
    auto [empty_receipt, empty] = Turn(session, "memory-empty");
    auto no_match = Take(session->GetMemoryRecall(empty.operation_id), "read empty-library report");
    Check(no_match.state == "no_match" && no_match.context_message_id.empty() && no_match.entries.empty(),
        "absent library was not an explicit no_match");
    Check(RequestText(state, 0).find(kSeed) == std::string::npos, "absent library injected fixture");
    const auto topic = Path(plan.memory_directory) / "preferences" / "preference.sdk-fixture.md";
    Write(topic, Theme(kSeed));
    auto [receipt, operation] = Turn(session, "memory-seed");
    const auto report = Report(session, operation, plan);
    const auto text = RequestText(state, 1);
    Check(Count(text, kSeed) == 1 && Count(text, kLive) == 0 && text.find("preference.sdk-fixture.md") != std::string::npos,
        "provider did not receive exact topic payload/source once");
    Check(!fs::exists(Path(plan.memory_directory) / ".state" / "catalog.json"), "read-only SDK repaired project catalog");
    Check(!fs::exists(Path(plan.memory_directory) / ".state" / "recall-traces" / "trace-last.json"),
        "SDK wrote a project-shared last report");
    Write(base / "memory-id.txt", session->id());
    Write(base / "memory-operation.txt", receipt.operation_id);
    Write(base / "memory-plan.txt", plan.plan_sha256);
    Write(base / "memory-report-identity.txt", SavedIdentity(report));
    Take(session->Close(), "close memory seed");
    Check(SavedIdentity(Take(session->GetMemoryRecall(receipt.operation_id), "query closed report")) == SavedIdentity(report),
        "Close changed recall metadata");
    Check(Take(session->DescribeMemory(), "query closed plan").plan_sha256 == plan.plan_sha256, "Close lost plan");
    Take(runtime->Shutdown(), "shutdown memory seed");
    Check(state->calls.load() == 2, "seed queried model beyond its two real operations");
}

void MemoryResume(const fs::path& base) {
    auto runtime = Runtime(base);
    auto state = std::make_shared<State>();
    auto options = Options(base, state);
    options.system_prompt.clear();
    options.resume_session_id = Read(base / "memory-id.txt");
    auto session = Take(runtime->OpenSession(std::move(options)), "resume with omitted Memory selection");
    const auto plan = Take(session->DescribeMemory(), "describe restored recall");
    Check(plan.enabled && plan.plan_sha256 == Read(base / "memory-plan.txt"), "resume lost frozen Memory plan");
    const auto old_operation = Read(base / "memory-operation.txt");
    const auto old_result = Take(session->ReadOperation(old_operation), "read old operation");
    Check(old_result.state == sdk::OperationState::Succeeded && old_result.result_persisted,
        "restart lost the successfully persisted old result");
    const auto old = Report(session, old_result, plan);
    Check(SavedIdentity(old) == Read(base / "memory-report-identity.txt") && state->calls.load() == 0,
        "restart changed old report or reran model");
    Check(!session->GetMemoryRecall("op-foreign-fixture"), "foreign operation borrowed shared last report");
    Write(Path(plan.memory_directory) / "preferences" / "preference.sdk-fixture.md", Theme(kLive));
    auto duplicate = Take(session->Submit("memory-seed", kQuery), "repeat old input");
    Check(duplicate.duplicate && duplicate.operation_id == old_operation && state->calls.load() == 0,
        "duplicate changed operation or repeated recall/model");
    Check(SavedIdentity(Take(session->GetMemoryRecall(old_operation), "query old report after topic change")) == SavedIdentity(old),
        "live library rewrote old report");
    auto [receipt, operation] = Turn(session, "memory-live");
    const auto live = Report(session, operation, plan);
    Check(live.context_message_id != old.context_message_id && live.context_sha256 != old.context_sha256,
        "future recall reused old context identity/hash");
    const auto text = RequestText(state, 0);
    Check(Count(text, kSeed) == 1 && Count(text, kLive) == 1,
        "resume lost old adopted context or duplicated live injection");
    Take(session->Close(), "close resumed recall");
    Check(SavedIdentity(Take(session->GetMemoryRecall(receipt.operation_id), "query closed live report")) == SavedIdentity(live),
        "closed query lost live report");
    options = Options(base, state);
    options.resume_session_id = Read(base / "memory-id.txt");
    options.memory = plan.options;
    ++options.memory->max_bytes;
    Check(!runtime->OpenSession(std::move(options)), "resume changed stored recall limits");
    Check(state->calls.load() == 1, "failed resume or closed query executed model");
    Take(runtime->Shutdown(), "shutdown memory resume");
}
} // namespace lubancore_consumer
