#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>
#include "memory/frontmatter.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "runtime/session_service.hpp"
#include "sdk/memory.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace traj = lubancode::trajectory;
namespace rt = lubancode::runtime;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr auto kNeedle = "RECOVERYMEMNEEDLE";
std::string Utf8(const fs::path& value) { return lubancode::platform::PathToUtf8(value); }
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-recovery-view-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd"); fs::create_directories(root / "resources"); root = fs::canonical(root);
    }
    ~Directory() { std::error_code ignored; fs::remove_all(lubancode::platform::FileIoPath(root), ignored); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
void Write(const fs::path& file, const std::string& bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream out(lubancode::platform::FileIoPath(file), std::ios::binary | std::ios::trunc); REQUIRE(out.is_open());
    out << bytes; out.close(); REQUIRE_FALSE(out.fail());
}
std::string Read(const fs::path& file) {
    std::ifstream input(lubancode::platform::FileIoPath(file), std::ios::binary); REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct RestoreFile {
    fs::path path; std::string bytes;
    ~RestoreFile() { std::ofstream output(lubancode::platform::FileIoPath(path), std::ios::binary | std::ios::trunc); output << bytes; }
};
struct Capture {
    std::mutex mutex;
    std::vector<sdk::ModelRequest> requests;
    std::size_t Count() { std::lock_guard lock(mutex); return requests.size(); }
    sdk::ModelRequest At(std::size_t index) { std::lock_guard lock(mutex); REQUIRE(index < requests.size()); return requests.at(index); }
};
struct Backend final : sdk::Backend {
    Capture& capture;
    explicit Backend(Capture& value) : capture(value) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        std::lock_guard lock(capture.mutex); capture.requests.push_back(request);
        return sdk::ModelReply{"RECOVERY_SDK_ANSWER", {}, sdk::Usage{3, 2}};
    }
};
sdk::SessionOptions Options(const Directory& directory, Capture& capture, const fs::path& cwd = {}) {
    sdk::SessionOptions options; options.cwd = Utf8(cwd.empty() ? directory.root / "cwd" : cwd);
    options.model = "recovery-model"; options.system_prompt = "RECOVERY_SDK_SYSTEM";
    options.backend = std::make_unique<Backend>(capture); options.memory = sdk::memory::v1::RecallOptions{};
    return options;
}
fs::path Memory(const std::shared_ptr<sdk::Session>& session) {
    const auto snapshot = session->DescribeMemory(); REQUIRE(snapshot.has_value()); return lubancode::platform::Utf8ToPath(snapshot->memory_directory);
}
fs::path Folder(const std::shared_ptr<sdk::Session>& session) { return Memory(session).parent_path() / "sessions" / session->id(); }
void Seed(const fs::path& root, const std::string& marker) {
    lubancode::memory::MemoryEntry entry; entry.schema = 3; entry.id = "preference.recovery"; entry.name = entry.id;
    entry.title = kNeedle; entry.summary = kNeedle; entry.kind = lubancode::memory::MemoryKind::Preference;
    entry.status = "active"; entry.confidence = "user-stated"; entry.keywords = {kNeedle};
    Write(root / "preferences" / (entry.id + ".md"), lubancode::memory::frontmatter::BuildTopicText(entry, Json::object(), std::string(kNeedle) + " " + marker));
}
sdk::Receipt Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key, const std::string& text = kNeedle) {
    const auto receipt = session->Submit(key, text); REQUIRE(receipt.has_value());
    const auto result = session->WaitResult(receipt->operation_id, 30s); REQUIRE(result.has_value());
    INFO(result->error); REQUIRE(result->state == sdk::OperationState::Succeeded); REQUIRE(result->result_persisted); return *receipt;
}
std::string Body(const sdk::ModelRequest& request) { std::string text; for (const auto& message : request.messages) text += message.text + "\n"; return text; }
std::size_t Count(const std::string& text, const std::string& needle) {
    std::size_t count = 0, start = 0; while ((start = text.find(needle, start)) != std::string::npos) { ++count; start += needle.size(); } return count;
}
sdk::SessionOptions Resume(const Directory& directory, Capture& capture, const std::string& sid, const fs::path& cwd = {}) {
    auto options = Options(directory, capture, cwd); options.resume_session_id = sid; options.memory.reset(); options.system_prompt.clear(); return options;
}
void Marker(const char* path) { std::cout << "[sdk-recovery-path] " << path << '\n'; }

// Internal read adapter participates in the true locked SDK Memory opening.
// It is not a public provider, and does not replace SessionService/live readers.
struct ComponentHost {
    std::shared_ptr<sdk::detail::SessionMemory> memory;
    std::unique_ptr<rt::SessionService> service;
    unsigned gates = 0;
    ComponentHost(const Directory& directory, const std::string& sid, traj::SessionRecoveryFactory factory) {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(directory.root / "cwd", directory.root / "data"); REQUIRE(identity.has_value());
        auto prepared = sdk::detail::SessionMemory::Prepare(std::nullopt, directory.root / "data", *identity, sid, directory.root / "cwd");
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().code)); memory = *prepared;
        rt::SessionLaunchRequest launch; launch.cwd_utf8 = Utf8(directory.root / "cwd"); launch.workspace_identity = *identity;
        launch.workspaces_root = directory.root / "data" / "workspaces"; launch.resume_at_launch = true; launch.require_v3_resume = true; launch.resume_source_session_id = sid;
        launch.recovery_capture.limits = traj::RecoveryReadLimits{}; launch.recovery_capture.memory_metadata = memory->RequiresRecoveryMetadata(); launch.recovery_factory = std::move(factory);
        auto opening = memory->OpeningParticipant();
        launch.v3_opening_participant = [this, opening = std::move(opening)](const traj::V3OpeningContext& context) -> std::expected<Json, std::string> {
            ++gates; CHECK(traj::SessionLock::Inspect(context.session_dir).has_value()); REQUIRE(context.recovery_view != nullptr); REQUIRE(context.source != nullptr);
            auto result = opening(context); if (!result) return result;
            const auto* system = context.source->FindMessage(context.source->context.system_message_ref); REQUIRE(system != nullptr); REQUIRE(system->system_meta.has_value());
            // Preserve the other already-frozen public host bindings. Memory has
            // just validated its actual binding and report against this view.
            return Json{{"hostBindings", system->system_meta->at("hostBindings")}};
        };
        service = std::make_unique<rt::SessionService>(std::move(launch));
    }
};
} // namespace

TEST_CASE("SDK recovery view: finite preflight fails without effects then raised budget resumes actual history") {
    Directory directory; Capture capture;
    auto runtime = sdk::Runtime::Create(directory.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(directory, capture)); REQUIRE(session.has_value()); Seed(Memory(*session), "SDK_BUDGET_HISTORY");
    const auto receipt = Turn(*session, "budget-input"); const auto folder = Folder(*session); const auto sid = (*session)->id();
    const auto report = (*session)->GetMemoryRecall(receipt.operation_id); REQUIRE(report.has_value()); REQUIRE(report->state == "admitted");
    REQUIRE((*session)->Close().has_value()); const auto file = folder / (sid + ".jsonl"); const auto old_main = Read(file);
    const auto old_plan = Read(folder / "sdk-memory-plan.json"); const auto old_ops = Read(folder / "operations.jsonl");
    const auto old_report = Read(folder / "sdk-memory-recalls" / (receipt.operation_id + ".json"));
    auto invalid = Resume(directory, capture, sid); invalid.recovery_read_limits.view_total_bytes = 0;
    auto invalid_open = (*runtime)->OpenSession(std::move(invalid)); REQUIRE_FALSE(invalid_open.has_value()); CHECK(invalid_open.error().code == "sdk.recovery.invalid_limits");
    auto small = Resume(directory, capture, sid); small.recovery_read_limits.journal.max_bytes = old_main.size() - 1;
    auto rejected = (*runtime)->OpenSession(std::move(small)); REQUIRE_FALSE(rejected.has_value());
    CHECK(capture.Count() == 1); CHECK(Read(file) == old_main); CHECK(Read(folder / "sdk-memory-plan.json") == old_plan);
    CHECK(Read(folder / "operations.jsonl") == old_ops); CHECK(Read(folder / "sdk-memory-recalls" / (receipt.operation_id + ".json")) == old_report);
    CHECK_FALSE(traj::SessionLock::Inspect(folder).has_value());
    auto larger = Resume(directory, capture, sid); larger.recovery_read_limits.journal.max_bytes = old_main.size();
    auto restored = (*runtime)->OpenSession(std::move(larger)); REQUIRE_MESSAGE(restored.has_value(), (restored ? std::string() : restored.error().code + ":" + restored.error().message));
    CHECK((*restored)->id() == sid); const auto saved = (*restored)->GetMemoryRecall(receipt.operation_id); REQUIRE(saved.has_value());
    CHECK(saved->context_message_id == report->context_message_id); CHECK(saved->context_sha256 == report->context_sha256); CHECK(saved->turn_id == report->turn_id);
    Turn(*restored, "after-budget", "NEW_SDK_INPUT"); REQUIRE(capture.Count() == 2); CHECK(Count(Body(capture.At(1)), "SDK_BUDGET_HISTORY") == 1);
    REQUIRE((*runtime)->Shutdown().has_value()); Marker("public-budget-raise");
}

TEST_CASE("SDK recovery view: locked fake reports are consumed once and cannot forge adopted ownership") {
    Directory directory; Capture capture;
    auto runtime = sdk::Runtime::Create(directory.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(directory, capture)); REQUIRE(session.has_value()); Seed(Memory(*session), "SDK_VIEW_REPORT");
    const auto receipt = Turn(*session, "view-report"); const auto folder = Folder(*session); const auto sid = (*session)->id();
    REQUIRE((*session)->Close().has_value());
    const auto report_path = folder / "sdk-memory-recalls" / (receipt.operation_id + ".json"); const auto original_report = Read(report_path);
    unsigned factory_calls = 0;
    { RestoreFile restore{report_path, original_report};
      ComponentHost host(directory, sid, [&](const traj::SessionRecoveryView& reference) -> std::expected<traj::SessionRecoveryView, std::string> {
          ++factory_calls; CHECK(traj::SessionLock::Inspect(folder).has_value());
          REQUIRE(reference.Find(traj::RecoveryKeyKind::MemoryRecall, receipt.operation_id) != nullptr);
          CHECK(reference.Find(traj::RecoveryKeyKind::MemoryRecall, receipt.operation_id)->bytes == original_report);
          Write(report_path, "{actual disk changed after capture}"); return reference;
      });
      REQUIRE_MESSAGE(host.service->runtime() != nullptr, host.service->launch_error()); CHECK(host.gates == 1); CHECK(factory_calls == 1);
      CHECK(Read(report_path) != original_report); REQUIRE(host.service->Close("exit").error_code.empty()); }
    REQUIRE(Read(report_path) == original_report);
    for (unsigned mode = 0; mode < 5; ++mode) {
        CAPTURE(mode); const auto before = Read(folder / (sid + ".jsonl"));
        ComponentHost rejected(directory, sid, [&](const traj::SessionRecoveryView& reference) -> std::expected<traj::SessionRecoveryView, std::string> {
            ++factory_calls; CHECK(traj::SessionLock::Inspect(folder).has_value()); auto copy = reference;
            auto& bytes = copy.values.at({traj::RecoveryKeyKind::MemoryRecall, receipt.operation_id}).bytes;
            if (mode == 0) bytes = "{";
            else if (mode < 4) {
                auto envelope = Json::parse(bytes); auto& payload = envelope["payload"];
                if (mode == 1) payload["sessionId"] = "foreign-session";
                if (mode == 2) payload["turnId"] = "turn-999999";
                if (mode == 3) payload["contextMessageId"] = "msg-999999";
                envelope["sha256"] = lubancode::platform::Sha256Hex(payload.dump()); bytes = envelope.dump();
            } else { auto& operations = copy.values.at({traj::RecoveryKeyKind::Operations, {}}).bytes;
                const auto lines = traj::RecoveryStreamLines(operations, std::nullopt); REQUIRE(lines.has_value()); operations += lines->back() + "\n"; }
            return copy;
        });
        CHECK(rejected.gates == 1); REQUIRE(rejected.service->runtime() == nullptr); CHECK_FALSE(rejected.service->launch_error().empty());
        CHECK(Read(folder / (sid + ".jsonl")) == before); CHECK(Read(report_path) == original_report); CHECK_FALSE(traj::SessionLock::Inspect(folder).has_value());
    }
    CHECK(factory_calls == 6); CHECK(capture.Count() == 1); REQUIRE((*runtime)->Shutdown().has_value()); Marker("locked-memory-owned-view");
}

TEST_CASE("SDK recovery view: corrupt completed report rejects while unfinished dispatch remains indeterminate") {
    Directory directory; Capture capture;
    auto runtime = sdk::Runtime::Create(directory.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(directory, capture)); REQUIRE(session.has_value()); Seed(Memory(*session), "SDK_PARTIAL_REPORT");
    const auto receipt = Turn(*session, "partial-input"); const auto folder = Folder(*session); const auto sid = (*session)->id(); REQUIRE((*session)->Close().has_value());
    const auto report_path = folder / "sdk-memory-recalls" / (receipt.operation_id + ".json");
    const auto original_report = Read(report_path); const auto original_main = Read(folder / (sid + ".jsonl")); const auto original_ops = Read(folder / "operations.jsonl");
    Write(report_path, "{"); auto completed = (*runtime)->OpenSession(Resume(directory, capture, sid)); REQUIRE_FALSE(completed.has_value());
    CHECK(capture.Count() == 1); CHECK(Read(folder / (sid + ".jsonl")) == original_main); CHECK(Read(folder / "operations.jsonl") == original_ops);
    const auto lines = traj::RecoveryStreamLines(original_ops, std::nullopt); REQUIRE(lines.has_value()); std::string unfinished;
    for (const auto& line : *lines) if (Json::parse(line).at("kind") != "operation.final") unfinished += line + "\n";
    Write(folder / "operations.jsonl", unfinished);
    auto resumed = (*runtime)->OpenSession(Resume(directory, capture, sid)); REQUIRE_MESSAGE(resumed.has_value(), (resumed ? std::string() : resumed.error().code));
    const auto operation = (*resumed)->ReadOperation(receipt.operation_id); REQUIRE(operation.has_value());
    CHECK(operation->state == sdk::OperationState::Indeterminate); CHECK_FALSE(operation->result_persisted);
    const auto duplicate = (*resumed)->Submit("partial-input", kNeedle); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate); CHECK(duplicate->operation_id == receipt.operation_id);
    CHECK(capture.Count() == 1); CHECK_FALSE((*resumed)->GetMemoryRecall(receipt.operation_id).has_value()); REQUIRE((*resumed)->Close().has_value());
    Write(report_path, original_report); Write(folder / "operations.jsonl", original_ops);
    auto repaired = (*runtime)->OpenSession(Resume(directory, capture, sid)); REQUIRE_MESSAGE(repaired.has_value(), (repaired ? std::string() : repaired.error().code));
    const auto restored_report = (*repaired)->GetMemoryRecall(receipt.operation_id); REQUIRE(restored_report.has_value()); CHECK(restored_report->state == "admitted");
    CHECK(capture.Count() == 1); REQUIRE((*runtime)->Shutdown().has_value()); Marker("completed-versus-partial");
}

TEST_CASE("SDK recovery view: two shared-project and two separate-project resumes retain only their own histories") {
    Directory directory; std::array<Capture, 4> captures; const auto process_cwd = fs::current_path();
    auto runtime = sdk::Runtime::Create(directory.Roots()); REQUIRE(runtime.has_value());
    std::array<std::shared_ptr<sdk::Session>, 4> sessions; std::array<std::string, 4> ids;
    std::array<fs::path, 4> folders, projects; std::array<sdk::Receipt, 4> receipts;
    const std::array<std::string, 4> markers{"SHARED_RECOVERY_MEMORY", "SHARED_RECOVERY_MEMORY", "PROJECT_TWO_RECOVERY_MEMORY", "PROJECT_THREE_RECOVERY_MEMORY"};
    for (std::size_t i = 0; i < 4; ++i) {
        projects[i] = i < 2 ? directory.root / "cwd" : directory.root / ("project-" + std::to_string(i)); fs::create_directories(projects[i]);
        auto options = Options(directory, captures[i], projects[i]); options.model += "-" + std::to_string(i); options.system_prompt += "-" + std::to_string(i);
        auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value()); sessions[i] = *session; ids[i] = sessions[i]->id(); folders[i] = Folder(sessions[i]);
        if (i != 1) Seed(Memory(sessions[i]), markers[i]);
        receipts[i] = Turn(sessions[i], "own-input", std::string(kNeedle) + " INPUT_" + std::to_string(i));
        if (i == 1) Turn(sessions[i], "only-second-session", "EXTRA_SECOND_SESSION");
    }
    CHECK(folders[0].parent_path() == folders[1].parent_path()); CHECK(folders[2].parent_path() != folders[3].parent_path());
    for (auto& session : sessions) REQUIRE(session->Close().has_value());
    for (std::size_t i = 0; i < 4; ++i) {
        auto options = Resume(directory, captures[i], ids[i], projects[i]); options.model += "-" + std::to_string(i);
        auto restored = (*runtime)->OpenSession(std::move(options)); REQUIRE_MESSAGE(restored.has_value(), (restored ? std::string() : restored.error().code)); sessions[i] = *restored;
        const auto own_report = sessions[i]->GetMemoryRecall(receipts[i].operation_id); REQUIRE(own_report.has_value()); CHECK(own_report->session_id == ids[i]); CHECK(own_report->state == "admitted");
        const auto duplicate = sessions[i]->Submit("own-input", std::string(kNeedle) + " INPUT_" + std::to_string(i)); REQUIRE(duplicate.has_value()); CHECK(duplicate->duplicate);
        CHECK(captures[i].Count() == (i == 1 ? 2 : 1));
        const auto foreign_op = sessions[i]->ReadOperation("op-2"); CHECK(foreign_op.has_value() == (i == 1));
        const auto report2 = sessions[i]->GetMemoryRecall("op-2"); CHECK(report2.has_value() == (i == 1));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        Turn(sessions[i], "after-resume", "AFTER_OWN_" + std::to_string(i)); const auto body = Body(captures[i].At(i == 1 ? 2 : 1));
        const auto request = captures[i].At(i == 1 ? 2 : 1);
        CHECK(request.model == "recovery-model-" + std::to_string(i));
        CHECK(Count(request.system, "RECOVERY_SDK_SYSTEM-" + std::to_string(i)) == 1); CHECK(Count(body, markers[i]) == 1);
        CHECK(Count(body, "INPUT_" + std::to_string(i)) == 1);
        for (std::size_t j = 0; j < 4; ++j) if (i != j) { CHECK(Count(request.system, "RECOVERY_SDK_SYSTEM-" + std::to_string(j)) == 0); CHECK(Count(body, "INPUT_" + std::to_string(j)) == 0); if (markers[i] != markers[j]) CHECK(Count(body, markers[j]) == 0); }
        REQUIRE(sessions[i]->Close().has_value());
    }
    CHECK(fs::current_path() == process_cwd); REQUIRE((*runtime)->Shutdown().has_value()); Marker("four-session-isolation");
}
