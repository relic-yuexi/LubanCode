#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "agent/agent.hpp"
#include "hooks/hash.hpp"
#include "platform/paths.hpp"
#include "runtime/assembly/session_resources.hpp"
#include "runtime/session_execution.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_turn_bridge.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace traj = lubancode::trajectory;
namespace rt = lubancode::runtime;
namespace v3 = traj::v3;
using Json = nlohmann::json;
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("recovery-view-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "project"); root = fs::canonical(root);
    }
    ~Directory() { std::error_code ignored; fs::remove_all(lubancode::platform::FileIoPath(root), ignored); }
};
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(lubancode::platform::FileIoPath(path.parent_path()));
    std::ofstream file(lubancode::platform::FileIoPath(path), std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open()); file << bytes; file.close(); REQUIRE_FALSE(file.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream file(lubancode::platform::FileIoPath(path), std::ios::binary); REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
fs::path Plant(Directory& directory, const std::string& sid = "session-1") {
    const auto folder = directory.root / sid; fs::create_directories(folder);
    auto writer = v3::V3Writer::Start(folder / (sid + ".jsonl"), sid, "run-1", "OWNED_SYSTEM");
    REQUIRE_MESSAGE(writer.has_value(), (writer ? std::string() : writer.error()));
    REQUIRE(writer->Close().has_value()); return folder;
}
void Marker(const char* path) { std::cout << "[session-recovery-path] " << path << '\n'; }
traj::SessionManagerOptions ManagerOptions(const Directory& directory) {
    traj::SessionManagerOptions options;
    options.workspaces_root = directory.root / "workspaces";
    options.workspace_root = directory.root / "project";
    options.identity = lubancode::workspace::MakeFallbackIdentity(options.workspace_root);
    options.launch_cwd = lubancode::platform::PathToUtf8(options.workspace_root);
    options.v3_system_content = "RECOVERY_MANAGER_SYSTEM";
    return options;
}
rt::SessionLaunchRequest Launch(const Directory& directory) {
    rt::SessionLaunchRequest request;
    request.cwd_utf8 = lubancode::platform::PathToUtf8(directory.root / "project");
    request.workspace_identity = lubancode::workspace::MakeFallbackIdentity(directory.root / "project");
    request.workspaces_root = directory.root / "workspaces";
    request.v3_system_content = "CLI_RECOVERY_SYSTEM"; request.wire_name = "chat";
    return request;
}
struct Backend final : lubancode::api::Backend {
    std::vector<lubancode::api::Request>& calls;
    explicit Backend(std::vector<lubancode::api::Request>& value) : calls(value) {}
    std::expected<void, lubancode::api::Error> send_stream(const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& emit, const std::atomic<bool>*) override {
        calls.push_back(request);
        emit(lubancode::api::MessageStart{"recovery-message", request.model});
        emit(lubancode::api::TextDelta{"RECOVERY_ANSWER"});
        emit(lubancode::api::ContentBlockDone{0});
        emit(lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{3, 2, 0, 0, 0}});
        return {};
    }
};
} // namespace

TEST_CASE("recovery view: native chunk read handles finite boundary MAX and nonregular objects") {
    Directory directory; const auto file = directory.root / "bytes"; const std::string bytes(8192, 'x'); Write(file, bytes);
    for (const auto cap : {std::optional<std::size_t>(bytes.size()), std::optional<std::size_t>((std::numeric_limits<std::size_t>::max)()), std::optional<std::size_t>()}) {
        auto read = traj::JournalFileAnchor::ReadExisting(file, cap); REQUIRE(read.has_value()); CHECK(read->bytes == bytes);
        REQUIRE(read->anchor->Close().has_value()); REQUIRE(read->anchor->Close().has_value());
    }
    const auto short_read = traj::JournalFileAnchor::ReadExisting(file, bytes.size() - 1);
    REQUIRE_FALSE(short_read.has_value()); CHECK(short_read.error() == "recovery.byte_limit");
    CHECK_FALSE(traj::JournalFileAnchor::ReadExisting(directory.root / "missing", std::nullopt).has_value());
    CHECK_FALSE(fs::exists(directory.root / "missing"));
    CHECK_FALSE(traj::JournalFileAnchor::ReadExisting(directory.root, std::nullopt).has_value());
#ifndef _WIN32
    const auto fifo = directory.root / "fifo"; REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(traj::JournalFileAnchor::ReadExisting(fifo, std::nullopt).has_value());
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
#endif
    CHECK(Read(file) == bytes); Marker("native-byte-bounds");
}

TEST_CASE("recovery view: selected metadata owns absence listing and actual byte budgets") {
    Directory directory; const auto folder = Plant(directory);
    traj::RecoveryCaptureRequest request; request.memory_metadata = true;
    auto absent = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(absent.has_value());
    REQUIRE(absent->view.Find(traj::RecoveryKeyKind::MemoryPlan) != nullptr);
    CHECK(absent->view.Find(traj::RecoveryKeyKind::MemoryPlan)->state == traj::RecoveryReadState::Absent);
    CHECK_FALSE(absent->view.reports.present); REQUIRE(absent->anchor->Close().has_value());
    Write(folder / "sdk-memory-plan.json", "{}"); Write(folder / "sdk-results" / "op-1.json", "{}");
    fs::create_directories(folder / "sdk-memory-recalls" / "pending.tmp");
    request.limits = traj::RecoveryReadLimits{};
    auto captured = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(captured.has_value());
    CHECK((captured->view.results.entries == std::vector<traj::RecoveryDirectoryEntry>{{"op-1.json", true, false}}));
    CHECK((captured->view.reports.entries == std::vector<traj::RecoveryDirectoryEntry>{{"pending.tmp", false, true}}));
    CHECK(captured->view.Find(traj::RecoveryKeyKind::SdkResult, "op-1")->bytes == "{}");
    REQUIRE(captured->anchor->Close().has_value());
    request.limits->view_directory_entries = 1;
    CHECK_FALSE(traj::CaptureSessionRecovery(folder, "project-1", "session-1", request).has_value());
    request.limits->view_directory_entries = 2; request.limits->directory_name_bytes = 10;
    CHECK_FALSE(traj::CaptureSessionRecovery(folder, "project-1", "session-1", request).has_value());
    request.limits->directory_name_bytes = 11;
    auto exact = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(exact.has_value());
    const auto actual = exact->view.Find(traj::RecoveryKeyKind::MainV3)->bytes.size() + 4 + 9 + 11;
    REQUIRE(exact->anchor->Close().has_value()); request.limits->view_total_bytes = actual;
    auto exact_total = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(exact_total.has_value());
    REQUIRE(exact_total->anchor->Close().has_value()); --request.limits->view_total_bytes;
    CHECK_FALSE(traj::CaptureSessionRecovery(folder, "project-1", "session-1", request).has_value());
    request.memory_metadata = false;
    auto plain = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(plain.has_value());
    CHECK(plain->view.values.size() == 1); REQUIRE(plain->anchor->Close().has_value());
    // Exercise canonical and iterator namespace normalization with real files,
    // including extended DOS native I/O on Windows rather than a fake path.
    const auto long_folder = directory.root / std::string(150, 'a') / std::string(150, 'b') / "session-long";
    fs::create_directories(lubancode::platform::FileIoPath(long_folder));
    auto long_writer = v3::V3Writer::Start(lubancode::platform::FileIoPath(long_folder / "session-long.jsonl"), "session-long", "run-long", "LONG_SYSTEM");
    REQUIRE(long_writer.has_value()); REQUIRE(long_writer->Close().has_value());
    Write(long_folder / "sdk-memory-plan.json", "{}"); Write(long_folder / "sdk-results" / "op-1.json", "{}");
    traj::RecoveryCaptureRequest long_request; long_request.memory_metadata = true; long_request.limits = traj::RecoveryReadLimits{};
    auto long_view = traj::CaptureSessionRecovery(long_folder, "project-1", "session-long", long_request); REQUIRE_MESSAGE(long_view.has_value(), (long_view ? std::string() : long_view.error()));
    CHECK(long_view->view.Find(traj::RecoveryKeyKind::SdkResult, "op-1")->bytes == "{}"); REQUIRE(long_view->anchor->Close().has_value());
    Marker("metadata-roster-bounds");
}

TEST_CASE("recovery view: fake reference cannot change main token scope roster or read states") {
    Directory directory; const auto folder = Plant(directory); const auto original = Read(folder / "session-1.jsonl");
    traj::RecoveryCaptureRequest request; request.memory_metadata = true;
    for (unsigned mode = 0; mode < 7; ++mode) {
        CAPTURE(mode);
        auto result = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request,
            [mode](const traj::SessionRecoveryView& reference) -> std::expected<traj::SessionRecoveryView, std::string> {
                auto copy = reference;
                if (mode == 0) { copy.values.at({traj::RecoveryKeyKind::MainV3, {}}).bytes += "\n"; copy.snapshot_token = lubancode::hooks::Sha256Hex(copy.values.at({traj::RecoveryKeyKind::MainV3, {}}).bytes); }
                if (mode == 1) copy.snapshot_token = std::string(64, '0');
                if (mode == 2) copy.session_id = "foreign-session";
                if (mode == 3) copy.results.present = true;
                if (mode == 4) copy.values.at({traj::RecoveryKeyKind::MemoryPlan, {}}).state = traj::RecoveryReadState::Value;
                if (mode == 5) copy.values.emplace(traj::RecoveryKey{traj::RecoveryKeyKind::SdkResult, "op-99"}, traj::RecoveryValue{});
                if (mode == 6) throw std::runtime_error("owned adapter rejected");
                return copy;
            });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().starts_with(mode == 6 ? "recovery.capture_exception:" : "recovery.reference_mismatch"));
        CHECK(Read(folder / "session-1.jsonl") == original);
    }
    auto valid = traj::CaptureSessionRecovery(folder, "project-1", "session-1", request); REQUIRE(valid.has_value());
    valid->view.snapshot_token = std::string(64, '0'); CHECK_FALSE(traj::CheckRecoveryView(valid->view, request).has_value());
    REQUIRE(valid->anchor->Close().has_value()); Marker("immutable-main-reference");
}

TEST_CASE("recovery view: strict owned operations preserve actual acceptance dispatch and final order") {
    Directory directory; rt::SessionService service(Launch(directory)); REQUIRE(service.runtime() != nullptr);
    const auto receipt = service.SubmitInput({"actual-key", "actual input", {}}); REQUIRE(receipt.accepted);
    const auto pending = service.PopPendingInput(); REQUIRE(pending.status == rt::SessionService::PendingPop::Status::Ok);
    REQUIRE(service.RecordTurnFinal({receipt.operation_id, "turn-000001", "success", {}, true}));
    const auto raw = Read(service.trajectory()->session_dir() / "operations.jsonl");
    auto facts = rt::SessionService::ReadOperationFactsOwned(raw); REQUIRE(facts.has_value()); REQUIRE(facts->size() == 3);
    CHECK(facts->at(0).operation_id == receipt.operation_id); CHECK(facts->at(2).turn_id == "turn-000001");
    auto lines = traj::RecoveryStreamLines(raw, std::nullopt); REQUIRE(lines.has_value()); REQUIRE(lines->size() == 3);
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(lines->at(1) + "\n").has_value());
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(lines->at(0) + "\n" + lines->at(2) + "\n").has_value());
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw + lines->at(0) + "\n").has_value());
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw + lines->at(2) + "\n").has_value());
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw.substr(0, raw.size() - 1)).has_value());
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw + "\n").has_value());
    auto second = Json::parse(lines->at(0)); second["operationId"] = "op-2";
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw + second.dump() + "\n").has_value());
    second["clientOperationId"] = "another-key"; second["payloadHash"] = "invalid";
    CHECK_FALSE(rt::SessionService::ReadOperationFactsOwned(raw + second.dump() + "\n").has_value());
    CHECK(rt::SessionService::ReadOperationFactsOwned("")->empty());
    REQUIRE(service.Close("exit").error_code.empty()); Marker("strict-operation-order");
}

TEST_CASE("recovery view: native Continue refuses missing changed prefix and identical replacement") {
    Directory directory;
    for (unsigned mode = 0; mode < 3; ++mode) {
        const auto sid = "session-" + std::to_string(mode + 1); const auto folder = Plant(directory, sid);
        const auto file = folder / (sid + ".jsonl");
        auto captured = traj::CaptureSessionRecovery(folder, "project-1", sid, {}); REQUIRE(captured.has_value());
        const auto original = captured->view.Find(traj::RecoveryKeyKind::MainV3)->bytes;
        if (mode == 0) fs::remove(file);
        if (mode == 1) Write(file, original + "changed\n");
        if (mode == 2) { fs::rename(file, folder / "previous.jsonl"); Write(file, original); }
        auto writer = v3::V3Writer::ContinueOwnedPrefix(file, original, *captured->anchor);
        REQUIRE_FALSE(writer.has_value());
        if (mode == 0) CHECK_FALSE(fs::exists(file));
        if (mode == 1) CHECK(Read(file) == original + "changed\n");
        if (mode == 2) { CHECK(writer.error() == "recovery.object_changed"); CHECK(Read(file) == original); CHECK(Read(folder / "previous.jsonl") == original); }
        REQUIRE(captured->anchor->Close().has_value());
    }
    Marker("native-existing-prefix");
}

TEST_CASE("recovery view: owned projection survives path loss and Continue preserves physical blank lines") {
    Directory directory; const auto folder = Plant(directory); const auto file = folder / "session-1.jsonl";
    const auto original = Read(file); Write(file, original + "\n"); REQUIRE(v3::VerifyV3File(file).ok);
    auto captured = traj::CaptureSessionRecovery(folder, "project-1", "session-1", {}); REQUIRE(captured.has_value());
    const auto bytes = captured->view.Find(traj::RecoveryKeyKind::MainV3)->bytes;
    auto raw = traj::RecoveryStreamLines(bytes, std::nullopt, true); REQUIRE(raw.has_value());
    auto ledger = v3::ReadV3LedgerOwned(file, *raw); REQUIRE(ledger.has_value());
    fs::rename(file, folder / "moved.jsonl");
    auto projection = v3::ProjectResume(*ledger); REQUIRE(projection.has_value());
    CHECK(projection->model_context.system_content == "OWNED_SYSTEM");
    fs::rename(folder / "moved.jsonl", file);
    auto continued = v3::V3Writer::ContinueOwnedPrefix(file, bytes, *captured->anchor); REQUIRE(continued.has_value());
    REQUIRE(captured->anchor->Close().has_value());
    v3::MessageDraft next; next.turn_id = "turn-000001"; next.message = {{"role", "user"}, {"content", "append marker"}};
    const auto written = continued->AppendMessage(std::move(next), traj::Durability::PowerLoss);
    REQUIRE(written.status == v3::WriteReceipt::Status::Committed); CHECK(written.seq == ledger->lines + 1);
    REQUIRE(continued->Close().has_value()); CHECK(Read(file).starts_with(bytes)); REQUIRE(v3::VerifyV3File(file).ok);
    traj::RecoveryStreamReadLimits finite{bytes.size(), raw->size(), bytes.size()};
    CHECK_FALSE(traj::RecoveryStreamLines(bytes, finite, true).has_value()); // blank consumes a physical line slot
    CHECK_FALSE(traj::RecoveryStreamLines(bytes.substr(0, bytes.size() - 2), std::nullopt, true).has_value());
    Marker("owned-projection-append");
}

TEST_CASE("recovery view: finite preflight rejects before host and CLI unset keeps long source") {
    Directory directory; auto options = ManagerOptions(directory); options.v3_system_content = std::string(20000, 'x');
    std::string sid; fs::path file;
    { traj::SessionManager source(options); const auto active = source.LaunchSession(); REQUIRE(active.has_value());
      sid = (*active)->session_id(); file = (*active)->directory.v3_stream_path(); traj::NullClearParticipant participant;
      REQUIRE(source.Close({}, &participant).error_code.empty()); }
    const auto original = Read(file); unsigned gates = 0, factories = 0;
    options.v3_opening_participant = [&](const traj::V3OpeningContext& context) -> std::expected<Json, std::string> {
        ++gates; CHECK(context.recovery_view != nullptr); CHECK(traj::SessionLock::Inspect(context.session_dir).has_value()); return Json::object(); };
    options.recovery_factory = [&](const traj::SessionRecoveryView& value) -> std::expected<traj::SessionRecoveryView, std::string> { ++factories; return value; };
    options.recovery_capture.limits = traj::RecoveryReadLimits{}; options.recovery_capture.limits->journal.max_bytes = original.size() - 1;
    traj::ResumeRequest request; request.source_session_id = sid;
    { traj::SessionManager rejected(options); const auto result = rejected.ResumeAsNew(request);
      CHECK(result.error_code == "resume.source_corrupt"); CHECK(result.message == "recovery.byte_limit"); CHECK(rejected.active() == nullptr); }
    CHECK(gates == 0); CHECK(factories == 0); CHECK(Read(file) == original); CHECK_FALSE(traj::SessionLock::Inspect(file.parent_path()).has_value());
    options.recovery_capture.limits->journal.max_bytes = original.size();
    { traj::SessionManager restored(options); const auto result = restored.ResumeAsNew(request); REQUIRE_MESSAGE(result.error_code.empty(), result.message);
      CHECK(restored.active()->session_id() == sid); CHECK(gates == 1); CHECK(factories == 1); traj::NullClearParticipant participant; REQUIRE(restored.Close({}, &participant).error_code.empty()); }
    options.recovery_capture.limits.reset(); options.v3_opening_participant = {}; options.recovery_factory = {};
    { traj::SessionManager cli(options); const auto result = cli.ResumeAsNew(request); REQUIRE_MESSAGE(result.error_code.empty(), result.message);
      CHECK(cli.active()->recovery_view->Find(traj::RecoveryKeyKind::MainV3)->bytes.size() > 20000); traj::NullClearParticipant participant; REQUIRE(cli.Close({}, &participant).error_code.empty()); }
    Marker("bounded-preflight-cli-unset");
}

TEST_CASE("recovery view: CLI SessionService feeds locked owned history into the actual shared Agent") {
    Directory directory; std::string sid; fs::path file;
    { rt::SessionService source(Launch(directory)); REQUIRE(source.runtime() != nullptr); auto* ledger = source.trajectory(); REQUIRE(ledger != nullptr);
      sid = ledger->session_id(); file = ledger->session_dir() / (sid + ".jsonl");
      auto bridge = ledger->NewTurnBridge({"recovery", "recovery", "terminal"});
      const auto turn = ledger->v3_main_writer()->NewTurnId(); bridge->BeginTurn(turn, "external_user");
      bridge->RecordInput({lubancode::api::Role::User, {lubancode::api::TextBlock{"CLI_OWNED_HISTORY"}}});
      bridge->EndTurn(true, false, "success"); REQUIRE(source.Close("exit").error_code.empty()); }
    const auto original = Read(file); unsigned captures = 0;
    auto launch = Launch(directory); launch.resume_at_launch = true; launch.require_v3_resume = true; launch.resume_source_session_id = sid;
    launch.recovery_factory = [&](const traj::SessionRecoveryView& view) -> std::expected<traj::SessionRecoveryView, std::string> {
        ++captures; CHECK(view.Find(traj::RecoveryKeyKind::MainV3)->bytes == original);
        CHECK(traj::SessionLock::Inspect(file.parent_path()).has_value()); return view; };
    std::vector<lubancode::api::Request> calls; // borrowed backend storage outlives execution
    rt::SessionService restored(std::move(launch)); REQUIRE_MESSAGE(restored.runtime() != nullptr, restored.launch_error());
    CHECK(captures == 1); CHECK(restored.trajectory()->session_id() == sid);
    rt::assembly::SessionResourcesRequest resources;
    resources.backend_factory = [&] { return std::make_unique<Backend>(calls); };
    resources.registry_factory = [](std::span<const rt::assembly::McpServerRuntime>) -> rt::assembly::SessionRegistryResult { return std::make_unique<lubancode::tools::ToolRegistry>(); };
    auto built = rt::assembly::BuildSessionResources(std::move(resources)); REQUIRE(built.has_value());
    lubancode::agent::AgentProfile profile; profile.provider = "recovery"; profile.request.model = "recovery-model"; profile.system_prompt = "CLI_RECOVERY_SYSTEM";
    restored.InitializeExecution(std::move(*built), std::move(profile), restored.trajectory()->LaunchResumeHistory());
    REQUIRE(restored.execution() != nullptr); REQUIRE(restored.execution()->agent().Run("CLI_NEW_INPUT", {}).has_value());
    REQUIRE(calls.size() == 1); std::string body;
    for (const auto& message : calls.front().messages) for (const auto& block : message.content)
        if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) body += text->text;
    CHECK(body.find("CLI_OWNED_HISTORY") != std::string::npos); CHECK(body.find("CLI_NEW_INPUT") != std::string::npos);
    CHECK(Read(file).starts_with(original)); REQUIRE(restored.Close("exit").error_code.empty()); Marker("cli-real-resumed-model");
}
