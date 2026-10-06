#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

#include "lubancore/core.hpp"
#include "platform/owned_file_path.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "runtime/trajectory_session.hpp"
#include "sdk/results.hpp"
#include "trajectory/named_result_opening.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/v3/result_store.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
namespace results = sdk::results::v1;
namespace traj = lubancode::trajectory;
namespace v3 = traj::v3;
namespace platform = lubancode::platform;
using namespace std::chrono_literals;

void Mark(const char* value) { std::cout << "[sdk-owned-file-path] " << value << '\n'; }
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close(); REQUIRE_FALSE(file.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary);
    REQUIRE(file.is_open());
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(file.bad()); return bytes;
}
void DirectoryLink(const fs::path& target, const fs::path& link) {
#ifdef _WIN32
    // A real junction requires no symbolic-link privilege; no shell or skip.
    REQUIRE(fs::create_directory(platform::FileIoPath(link)));
    struct Junction {
        DWORD tag;
        WORD length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        std::array<WCHAR, 4096> names{};
    } data{};
    static_assert(offsetof(Junction, names) == 16);
    const auto print = target.native();
    const auto substitute = std::wstring(L"\\??\\") + print;
    const auto text = substitute + L'\0' + print + L'\0';
    REQUIRE(text.size() < data.names.size());
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.length = static_cast<WORD>(8 + text.size() * sizeof(WCHAR));
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(WCHAR));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(WCHAR));
    data.print_length = static_cast<WORD>(print.size() * sizeof(WCHAR));
    std::copy(text.begin(), text.end(), data.names.begin());
    const HANDLE handle = CreateFileW(platform::FileIoPath(link).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(8 + data.length), nullptr, 0, &returned, nullptr) != FALSE);
    const auto attributes = GetFileAttributesW(platform::FileIoPath(link).c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
    fs::create_directory_symlink(target, link);
    REQUIRE(fs::is_symlink(fs::symlink_status(link)));
#endif
}
void TerminalLink(const fs::path& file_target, const fs::path& directory_target, const fs::path& link) {
#ifdef _WIN32
    (void)file_target;
    DirectoryLink(directory_target, link); // terminal reparse, including a missing target
    std::cout << "[sdk-owned-file-link-kind] terminal-directory-junction\n";
#else
    (void)directory_target;
    fs::create_symlink(file_target, link);
    REQUIRE(fs::is_symlink(fs::symlink_status(link)));
    std::cout << "[sdk-owned-file-link-kind] terminal-file-symlink\n";
#endif
}
struct Fixture {
    fs::path root, real, host;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("owned-file-paths-" + std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        real = root / "actual"; host = root / "host-alias";
        fs::create_directories(real / "project"); fs::create_directories(real / "resources");
        Write(real / "project" / "input.txt", "ACTUAL-OWNED-FILE-BYTES\n");
        DirectoryLink(real, host);
        REQUIRE_FALSE(platform::RejectReparsePoint(host).has_value());
        REQUIRE(fs::equivalent(real, host));
        // Keep this spelling: canonicalizing inputs would hide the production bug.
    }
    ~Fixture() { std::error_code error; fs::remove_all(platform::FileIoPath(root), error); }
    sdk::RuntimeOptions Roots() const {
        return {platform::PathToUtf8(host / "state"), platform::PathToUtf8(host / "resources")};
    }
    fs::path Session(const std::string& id) const {
        for (const auto& item : fs::recursive_directory_iterator(host / "state"))
            if (item.is_regular_file() && item.path().filename() == id + ".jsonl") return item.path().parent_path();
        REQUIRE_MESSAGE(false, "actual Session main journal was not created"); return {};
    }
};
struct RelativeFixture {
    fs::path cwd, relative, root;
    bool created = false;
    RelativeFixture() {
        static std::atomic<unsigned> serial{0};
        cwd = fs::current_path(); REQUIRE(cwd.is_absolute());
        relative = "owned-relative-" + std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial);
        REQUIRE(relative.is_relative()); REQUIRE(relative.parent_path().empty());
        root = cwd / relative;
        REQUIRE(root.is_absolute()); REQUIRE(root.parent_path() == cwd); REQUIRE(root.filename() == relative);
        created = fs::create_directory(platform::FileIoPath(root)); REQUIRE(created);
    }
    ~RelativeFixture() {
        if (!created || relative.empty() || !relative.parent_path().empty() ||
            root.parent_path() != cwd || root != cwd / relative) return;
        std::error_code error; fs::remove_all(platform::FileIoPath(root), error);
    }
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<std::atomic<unsigned>> calls) : calls_(std::move(calls)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++*calls_;
        if (!request.messages.empty() && request.messages.back().role == "user" && request.messages.back().tool_replies.empty())
            return sdk::ModelReply{{}, {{"owned-provider-call", "owned_file", "{}"}}, {}};
        return sdk::ModelReply{"owned complete", {}, {}};
    }
private:
    std::shared_ptr<std::atomic<unsigned>> calls_;
};
sdk::SessionOptions Options(const Fixture& fixture, const std::shared_ptr<std::atomic<unsigned>>& models,
    const std::shared_ptr<std::atomic<unsigned>>& tools, const std::string& resume = {}) {
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.host / "project"); options.model = "owned-path-fixture";
    options.system_prompt = "Read the declared file and report completion.";
    options.backend = std::make_unique<Backend>(models); options.max_steps_per_turn = 4;
    options.resume_session_id = resume; options.result_policy = results::SessionResultOptions{results::Mode::Full, 1};
    sdk::Tool tool; tool.name = "owned_file"; tool.description = "Read actual fixture input.";
    tool.requires_approval = false;
    tool.execute = [tools, path = fixture.host / "project" / "input.txt"](const std::string&, const sdk::ToolContext&)
        -> sdk::Result<sdk::ToolResult> { ++*tools; return sdk::ToolResult{Read(path), false}; };
    options.custom_tools.push_back(std::move(tool)); return options;
}
sdk::Operation Turn(const std::shared_ptr<sdk::Session>& session, const std::string& key) {
    const auto receipt = session->Submit(key, key); REQUIRE(receipt);
    const auto operation = session->WaitResult(receipt->operation_id, 20s); REQUIRE(operation);
    INFO(operation->error); REQUIRE(operation->state == sdk::OperationState::Succeeded);
    REQUIRE(operation->result_persisted); return *operation;
}
results::ToolResultIdentity Selected(const std::shared_ptr<sdk::Session>& session, const sdk::Operation& operation) {
    const auto list = session->ListToolResults(operation.operation_id); REQUIRE(list);
    const auto entry = std::find_if(list->begin(), list->end(), [](const auto& value) {
        return value.selected && value.identity.result_id.starts_with("res-");
    });
    REQUIRE(entry != list->end()); REQUIRE(entry->identity.session_id == session->id());
    REQUIRE(entry->identity.operation_id == operation.operation_id); REQUIRE(entry->identity.turn_id == operation.turn_id);
    return entry->identity;
}
const results::ToolResultChannel& Combined(const results::SavedSnapshot& value) {
    const auto& channels = value.result().channels;
    const auto found = std::find_if(channels.begin(), channels.end(), [](const auto& channel) { return channel.channel == "combined"; });
    REQUIRE(found != channels.end()); return *found;
}
traj::SessionLockOwner LockOwner() {
    return {platform::CurrentProcessId(), traj::CurrentProcessStartToken(), 1};
}
v3::ResultStore::PersistRequest Material() {
    v3::ResultStore::PersistRequest request;
    request.result_kind = "text"; request.tool_call_id = "owned-native-call";
    request.execution_event_ref = "owned-native-execution";
    request.outputs.push_back({"combined", "text/plain", "ACTUAL-NATIVE-OWNED-BYTES\n"}); return request;
}
const nlohmann::json& CombinedRef(const v3::ResultStore::PersistedResult& saved) {
    const auto ref = std::find_if(saved.result_ref.begin(), saved.result_ref.end(), [](const auto& value) {
        return value.value("kind", std::string()) == "combined";
    });
    REQUIRE(ref != saved.result_ref.end()); return *ref;
}
std::expected<std::string, traj::CasError> ReadNative(const std::shared_ptr<traj::NamedResultCapability>& cap,
    const nlohmann::json& ref) {
    return cap->Read(ref.at("path").get<std::string>(), ref.at("sha256").get<std::string>(),
        ref.at("bytes").get<std::uint64_t>(), ref.at("mediaType").get<std::string>(), 4096);
}
} // namespace

TEST_CASE("SDK owned File paths: actual host alias opens persists closes and restores public Session") {
    Fixture fixture; auto models = std::make_shared<std::atomic<unsigned>>(0); auto tools = std::make_shared<std::atomic<unsigned>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools)); REQUIRE(opened); auto session = *opened;
    const auto operation = Turn(session, "before-close"); const auto identity = Selected(session, operation);
    auto saved = session->ReadToolResult(identity); REQUIRE(saved);
    REQUIRE(saved->result().metadata_state == results::ArtifactState::Verified);
    REQUIRE(Combined(*saved).artifact_verified); REQUIRE(Combined(*saved).text);
    REQUIRE(*Combined(*saved).text == Read(fixture.host / "project" / "input.txt"));
    REQUIRE(saved->policy().mode == results::Mode::Full); REQUIRE(saved->policy().version == 1);
    const auto directory = fixture.Session(session->id());
    const auto policy = Read(directory / "sdk-result-policy.json");
    REQUIRE(nlohmann::json::parse(policy).at("sessionId") == session->id());
    REQUIRE(session->Close());
    auto closed = session->ReadToolResult(identity); REQUIRE(closed); REQUIRE(Combined(*closed).text == Combined(*saved).text);
    {
        // Runtime roots use their original canonicalization. Exercise the real
        // SDK reader/policy through the preserved alias of this same saved scene,
        // using only its verified main index and an independent closed File cap.
        auto lock = traj::SessionLock::Acquire(directory, LockOwner()); REQUIRE(lock); REQUIRE(lock->holds());
        const auto ledger = v3::ReadV3Ledger(directory / (session->id() + ".jsonl")); REQUIRE(ledger);
        const auto index = sdk::detail::IndexToolResults(*ledger, session->id(), operation.operation_id, operation.turn_id); REQUIRE(index);
        const auto entry = std::find_if(index->entries.begin(), index->entries.end(), [&](const auto& value) {
            return value.summary.identity == identity;
        });
        REQUIRE(entry != index->entries.end());
        const auto workspace = lubancode::workspace::ResolveWorkspaceIdentity(fixture.host / "project", fixture.host / "state"); REQUIRE(workspace);
        auto lease = traj::OpenNamedResultCapability({workspace->workspace_key, session->id()}, directory); REQUIRE(lease);
        lease->CloseWrites();
        const auto frozen = sdk::detail::FreezeResultPolicy(directory, session->id(), results::SessionResultOptions{results::Mode::Full, 1}, true);
        REQUIRE(frozen); REQUIRE(*frozen == saved->policy());
        const auto dotted = sdk::detail::FreezeResultPolicy(directory / ".", session->id(), results::SessionResultOptions{results::Mode::Full, 1}, true);
        REQUIRE(dotted); REQUIRE(*dotted == *frozen);
        const auto trailing = sdk::detail::FreezeResultPolicy(directory / "", session->id(), results::SessionResultOptions{results::Mode::Full, 1}, true);
        REQUIRE(trailing); REQUIRE(*trailing == *frozen);
        const auto aliased = sdk::detail::ReadIndexedToolResult(directory, *entry, *frozen, {}, lease->share()); REQUIRE(aliased);
        REQUIRE(aliased->result().metadata_state == results::ArtifactState::Verified);
        REQUIRE(aliased->result().metadata_sha256 == saved->result().metadata_sha256);
        REQUIRE(Combined(*aliased).artifact_verified); REQUIRE(Combined(*aliased).text == Combined(*saved).text);
    }
    REQUIRE((*runtime)->Shutdown()); runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto restored = (*runtime)->OpenSession(Options(fixture, models, tools, session->id())); REQUIRE(restored);
    REQUIRE((*restored)->id() == session->id()); REQUIRE(Read(directory / "sdk-result-policy.json") == policy);
    auto restored_material = (*restored)->ReadToolResult(identity); REQUIRE(restored_material);
    REQUIRE(restored_material->result().metadata_sha256 == saved->result().metadata_sha256);
    REQUIRE(Combined(*restored_material).artifact_verified); REQUIRE(Combined(*restored_material).text == Combined(*saved).text);
    const auto next = Selected(*restored, Turn(*restored, "after-resume")); REQUIRE(next.result_id != identity.result_id);
    REQUIRE(models->load() == 4); REQUIRE(tools->load() == 2);
    REQUIRE((*restored)->Close()); REQUIRE((*runtime)->Shutdown()); Mark("public-roundtrip");
}

TEST_CASE("SDK owned File paths: real File capability reads after Close and relative standalone still persists") {
    Fixture fixture;
    {
        lubancode::runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root = fixture.host / "ledger-state" / "workspaces";
        options.workspace_root = fixture.host / "project";
        options.lubancode_version = "owned-path-fixture";
        auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(std::move(options));
        REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
        REQUIRE(ledger->v3_main_writer() != nullptr);
        REQUIRE(fs::is_regular_file(ledger->session_dir() / (ledger->session_id() + ".jsonl")));
        const auto relative = ledger->session_dir().lexically_relative(fixture.host / "ledger-state" / "workspaces");
        REQUIRE_FALSE(relative.empty()); REQUIRE(*relative.begin() != "..");
    }
    const auto directory = fixture.host / "standalone"; fs::create_directory(directory);
    auto lock = traj::SessionLock::Acquire(directory, LockOwner()); REQUIRE(lock); REQUIRE(lock->holds());
    auto opening = traj::OpenLockedNamedResults({"owned-workspace", "standalone"}, directory, {}); REQUIRE(opening);
    auto cap = opening->lease.share(); REQUIRE(cap->FileSessionDirectory() == directory);
    auto store = v3::ResultStore::Open(cap); REQUIRE(store); const auto saved = store->Persist(Material()); REQUIRE(saved.ok);
    REQUIRE(saved.publication); REQUIRE(saved.publication->knowledge == v3::ResultStore::PersistedResult::Knowledge::Committed);
    REQUIRE(saved.publication->files.front().native); REQUIRE(saved.publication->files.front().native->ok());
    auto bytes = ReadNative(cap, CombinedRef(saved)); REQUIRE(bytes); REQUIRE(*bytes == Material().outputs.front().data);
    REQUIRE(CombinedRef(saved).at("sha256") == platform::Sha256Hex(*bytes));
    REQUIRE(CombinedRef(saved).at("bytes") == bytes->size());
    opening->lease.CloseWrites(); auto closed = ReadNative(cap, CombinedRef(saved)); REQUIRE(closed); REQUIRE(*closed == *bytes);
    RelativeFixture working;
    auto standalone = v3::ResultStore::Open(working.relative, "relative-"); REQUIRE(standalone);
    const auto other = standalone->Persist(Material()); REQUIRE(other.ok); REQUIRE(other.result_id.starts_with("relative-"));
    const auto relative_file = working.relative / CombinedRef(other).at("path").get<std::string>();
    REQUIRE(relative_file.is_relative()); REQUIRE(Read(working.root / CombinedRef(other).at("path").get<std::string>()) == *bytes);
    REQUIRE(platform::IsUnlinkedOwnedPath(working.relative, relative_file));
    REQUIRE(platform::IsUnlinkedOwnedPath(working.relative / ".", relative_file));
    REQUIRE(platform::IsUnlinkedOwnedPath(working.relative / "", relative_file));
    REQUIRE(platform::IsUnlinkedOwnedPath(".", relative_file));
    REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(working.relative, working.relative / "artifacts" / ".." / "foreign"));
    REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(working.relative, working.relative / "artifacts" / ".." / "artifacts" / "foreign"));
#ifdef _WIN32
    // Exercise a drive-relative spelling only for an actual drive cwd. An UNC
    // cwd is not a drive name, and must not be replaced with an invented C: base.
    const auto drive_name = working.cwd.root_name().native();
    if (drive_name.size() == 2 && drive_name[1] == L':') {
        const auto drive_root = working.cwd.root_name() / working.relative;
        const auto drive_file = working.cwd.root_name() / relative_file;
        REQUIRE(drive_root.is_relative()); REQUIRE(drive_file.is_relative());
        REQUIRE(platform::IsUnlinkedOwnedPath(drive_root, drive_file));
        REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(drive_root, drive_root / "artifacts" / ".." / "foreign"));
        std::cout << "[sdk-owned-file-raw-base] actual-current-drive-relative\n";
    }
    const auto rooted = fs::path(L"\\") / working.root.relative_path();
    const auto rooted_file = fs::path(L"\\") / (working.root / CombinedRef(other).at("path").get<std::string>()).relative_path();
    REQUIRE(rooted.has_root_directory()); REQUIRE_FALSE(rooted.has_root_name()); REQUIRE(rooted.is_relative());
    REQUIRE(platform::IsUnlinkedOwnedPath(rooted, rooted_file));
    REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(rooted, rooted / "artifacts" / ".." / "foreign"));
    std::cout << "[sdk-owned-file-raw-base] actual-current-root-relative\n";
#endif
    REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(directory, directory / "artifacts" / ".." / "foreign"));
    REQUIRE_FALSE(platform::IsUnlinkedOwnedPath(directory, fixture.host / "foreign")); Mark("file-relative");
}

TEST_CASE("SDK owned File paths: real locked Named opening refuses terminal and dangling plan links") {
    Fixture fixture;
    for (const bool dangling : {false, true}) {
        const auto directory = fixture.host / (dangling ? "dangling-plan-session" : "linked-plan-session"); fs::create_directory(directory);
        auto lock = traj::SessionLock::Acquire(directory, LockOwner()); REQUIRE(lock); REQUIRE(lock->holds());
        const auto target = fixture.real / (dangling ? "absent-plan" : "actual-plan.json");
        const auto directory_target = fixture.real / (dangling ? "absent-plan-directory" : "plan-directory");
        if (!dangling) { Write(target, "{}"); fs::create_directory(directory_target); }
        TerminalLink(target, directory_target, directory / "sdk-named-results-plan.json");
        const auto opened = traj::OpenLockedNamedResults({"owned-workspace", dangling ? "dangling-plan-session" : "linked-plan-session"}, directory, {});
        REQUIRE_FALSE(opened); REQUIRE(opened.error() == "named_result.plan_path_rejected");
        REQUIRE_FALSE(fs::exists(directory / "artifacts"));
    }
    Mark("plan-terminal");
}

TEST_CASE("SDK owned File paths: Session boundary link is refused by locked opening and closed File reader") {
    Fixture fixture; const auto actual = fixture.host / "actual-session"; fs::create_directory(actual);
    auto lock = traj::SessionLock::Acquire(actual, LockOwner()); REQUIRE(lock); REQUIRE(lock->holds());
    const auto linked = fixture.host / "linked-session"; DirectoryLink(actual, linked);
    const auto opened = traj::OpenLockedNamedResults({"owned-workspace", "actual-session"}, linked, {});
    REQUIRE_FALSE(opened); REQUIRE(opened.error() == "named_result.plan_path_rejected");
    auto lease = traj::OpenNamedResultCapability({"owned-workspace", "actual-session"}, linked); REQUIRE(lease);
    auto store = v3::ResultStore::Open(actual); REQUIRE(store); const auto saved = store->Persist(Material()); REQUIRE(saved.ok);
    lease->CloseWrites(); const auto refused = ReadNative(lease->share(), CombinedRef(saved));
    REQUIRE_FALSE(refused); REQUIRE(refused.error().code == "named_result.path_rejected");
    REQUIRE(Read(actual / CombinedRef(saved).at("path").get<std::string>()) == Material().outputs.front().data);
    Mark("root-link");
}

TEST_CASE("SDK owned File paths: actual public resume refuses saved policy terminal and dangling links") {
    Fixture fixture; auto models = std::make_shared<std::atomic<unsigned>>(0); auto tools = std::make_shared<std::atomic<unsigned>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools)); REQUIRE(opened);
    (void)Turn(*opened, "policy-source"); REQUIRE((*opened)->Close());
    const auto directory = fixture.Session((*opened)->id()); const auto path = directory / "sdk-result-policy.json";
    const auto saved = fixture.real / "actual-policy.json"; fs::rename(path, saved);
    const auto directory_target = fixture.real / "policy-directory"; fs::create_directory(directory_target);
    for (const bool dangling : {false, true}) {
        TerminalLink(dangling ? fixture.real / "absent-policy" : saved,
            dangling ? fixture.real / "absent-policy-directory" : directory_target, path);
        const auto denied = (*runtime)->OpenSession(Options(fixture, models, tools, (*opened)->id()));
        REQUIRE_FALSE(denied); REQUIRE(denied.error().code == "sdk.result.policy_invalid"); REQUIRE(fs::remove(path));
    }
    fs::rename(saved, path); auto restored = (*runtime)->OpenSession(Options(fixture, models, tools, (*opened)->id())); REQUIRE(restored);
    REQUIRE(models->load() == 2); REQUIRE(tools->load() == 1); REQUIRE((*restored)->Close()); REQUIRE((*runtime)->Shutdown());
    Mark("policy-link");
}

TEST_CASE("SDK owned File paths: actual SDK closed material refuses terminal and internal artifact links") {
    Fixture fixture; auto models = std::make_shared<std::atomic<unsigned>>(0); auto tools = std::make_shared<std::atomic<unsigned>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto opened = (*runtime)->OpenSession(Options(fixture, models, tools)); REQUIRE(opened);
    const auto identity = Selected(*opened, Turn(*opened, "artifact-source")); REQUIRE((*opened)->Close());
    const auto directory = fixture.Session((*opened)->id()); const auto artifacts = directory / "artifacts";
    const auto path = artifacts / (identity.result_id + ".combined.txt");
    const auto saved = fixture.real / "actual-channel.txt"; fs::rename(path, saved);
    const auto directory_target = fixture.real / "channel-directory"; fs::create_directory(directory_target);
    for (const bool dangling : {false, true}) {
        TerminalLink(dangling ? fixture.real / "absent-channel" : saved,
            dangling ? fixture.real / "absent-channel-directory" : directory_target, path);
        const auto refused = (*opened)->ReadToolResult(identity); REQUIRE(refused);
        REQUIRE(refused->result().metadata_state == results::ArtifactState::Verified);
        REQUIRE(Combined(*refused).state == results::ArtifactState::Corrupt);
        REQUIRE(Combined(*refused).issue_code == "sdk.result.path_rejected"); REQUIRE_FALSE(Combined(*refused).artifact_verified);
        REQUIRE(fs::remove(path));
    }
    fs::rename(saved, path); const auto backup = fixture.real / "actual-artifacts"; fs::rename(artifacts, backup);
    DirectoryLink(backup, artifacts); const auto indirect = (*opened)->ReadToolResult(identity); REQUIRE(indirect);
    REQUIRE(indirect->result().metadata_state == results::ArtifactState::Corrupt); REQUIRE(indirect->result().channels.empty());
    REQUIRE(fs::remove(artifacts)); fs::rename(backup, artifacts);
    const auto restored = (*opened)->ReadToolResult(identity); REQUIRE(restored);
    REQUIRE(restored->result().metadata_state == results::ArtifactState::Verified); REQUIRE(Combined(*restored).artifact_verified);
    REQUIRE(Combined(*restored).text); REQUIRE(*Combined(*restored).text == Read(fixture.host / "project" / "input.txt"));
    REQUIRE(models->load() == 2); REQUIRE(tools->load() == 1); REQUIRE((*runtime)->Shutdown()); Mark("artifact-links");
}
