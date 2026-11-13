#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/managed_session_ownership.hpp"
#include "trajectory/session_lock.hpp"

namespace {
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
namespace traj = lubancode::trajectory;
using Json = nlohmann::json;
using Ownership = traj::ManagedSessionOwnership;
using Publication = traj::ManagedSessionOwnershipPublication;
using Knowledge = Publication::Knowledge;

struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("managed-ownership-" +
            std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        REQUIRE(fs::create_directory(root));
        root = fs::canonical(root);
    }
    ~Directory() {
        std::error_code ignored;
        fs::remove_all(platform::FileIoPath(root), ignored);
    }
    fs::path Session(const std::string& relative = "session-1") const {
        const auto result = root / relative;
        REQUIRE(fs::create_directories(platform::FileIoPath(result)));
        return result;
    }
};

traj::SessionLock Acquire(const fs::path& directory) {
    traj::SessionLockOwner owner;
    owner.pid = platform::CurrentProcessId();
    owner.process_start_token = traj::CurrentProcessStartToken();
    owner.acquired_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto result = traj::SessionLock::Acquire(directory, owner);
    const std::string error = result ? std::string() : result.error();
    REQUIRE_MESSAGE(result.has_value(), error);
    REQUIRE(result->holds());
    return std::move(*result);
}

Ownership Expected(const std::string& tenant = "tenant-a") {
    return {tenant, "project-1", "workspace-1", "session-1", 1};
}

void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    REQUIRE_FALSE(file.fail());
}

std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary);
    REQUIRE(file.is_open());
    std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(file.bad());
    return bytes;
}

fs::path MarkerFile(const fs::path& directory) {
    return directory / traj::kManagedSessionOwnershipFile;
}

Publication PublishNew(const fs::path& directory, const traj::SessionLock& lock,
                       const Ownership& expected = Expected()) {
    auto attempt = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, expected);
    const std::string error = attempt ? std::string() : attempt.error();
    REQUIRE_MESSAGE(attempt.has_value(), error);
    REQUIRE_FALSE((*attempt)->FirstPublication().has_value());
    auto receipt = (*attempt)->Publish(lock);
    REQUIRE(receipt.knowledge == Knowledge::Committed);
    REQUIRE(receipt.requested == platform::WriteDurability::ProcessCrashDurability);
    REQUIRE(receipt.native.has_value());
    REQUIRE(receipt.native->has_value());
    REQUIRE(receipt.native->value().outcome == platform::WriteOutcome::CommittedDurable);
    REQUIRE(receipt.error_code.empty());
    REQUIRE(Read(MarkerFile(directory)) == receipt.publication_bytes);
    REQUIRE(receipt.captured_bytes.empty());
    REQUIRE(receipt.expected == expected);
    return receipt;
}

void SamePublication(const Publication& actual, const Publication& expected) {
    REQUIRE(actual.knowledge == expected.knowledge);
    REQUIRE(actual.expected == expected.expected);
    REQUIRE(actual.captured_bytes == expected.captured_bytes);
    REQUIRE(actual.publication_bytes == expected.publication_bytes);
    REQUIRE(actual.requested == expected.requested);
    REQUIRE(actual.error_code == expected.error_code);
    REQUIRE(actual.message == expected.message);
    REQUIRE(actual.native.has_value() == expected.native.has_value());
    if (!actual.native) return;
    REQUIRE(actual.native->has_value() == expected.native->has_value());
    if (*actual.native) {
        REQUIRE(actual.native->value().outcome == expected.native->value().outcome);
    } else {
        REQUIRE(actual.native->error().code == expected.native->error().code);
        REQUIRE(actual.native->error().message == expected.native->error().message);
        REQUIRE(actual.native->error().outcome == expected.native->error().outcome);
        REQUIRE(actual.native->error().failure_kind == expected.native->error().failure_kind);
    }
}

// Windows junctions do not require the symlink privilege. This creates a real
// reparse object in-process; no shell, network, production seam or skipped branch.
void DirectoryLink(const fs::path& target, const fs::path& link) {
#ifdef _WIN32
    struct JunctionData {
        DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
        WORD length = 0, reserved = 0;
        WORD substitute_offset = 0, substitute_length = 0;
        WORD print_offset = 0, print_length = 0;
        std::array<wchar_t, 4096> names{};
    } data;
    const auto print = target.native();
    const std::wstring substitute = L"\\??\\" + print;
    const auto characters = substitute.size() + 1 + print.size() + 1;
    REQUIRE(characters <= data.names.size());
    const auto total = offsetof(JunctionData, names) + characters * sizeof(wchar_t);
    REQUIRE(total - 8 <= (std::numeric_limits<WORD>::max)());
    data.length = static_cast<WORD>(total - 8);
    data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::wmemcpy(data.names.data(), substitute.data(), substitute.size());
    std::wmemcpy(data.names.data() + substitute.size() + 1, print.data(), print.size());
    REQUIRE(fs::create_directory(platform::FileIoPath(link)));
    const HANDLE handle = CreateFileW(platform::FileIoPath(link).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    DWORD returned = 0;
    const BOOL set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(total), nullptr, 0, &returned, nullptr);
    const DWORD error = set ? ERROR_SUCCESS : GetLastError();
    REQUIRE_MESSAGE(set != FALSE, error);
    const auto attributes = GetFileAttributesW(platform::FileIoPath(link).c_str());
    REQUIRE(attributes != INVALID_FILE_ATTRIBUTES);
    REQUIRE((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
    fs::create_directory_symlink(target, link);
    REQUIRE(fs::is_symlink(fs::symlink_status(link)));
#endif
}

struct FlushFaultReset {
    ~FlushFaultReset() {
        platform::SetFileFlushFailureForTest(false);
        platform::SetDirectoryFlushFailureForTest(false);
    }
};

void Marker(const char* name) {
    std::cout << "[managed-session-ownership-path] " << name << '\n';
}
} // namespace

TEST_CASE("managed ownership: actual locked publication owns the exact native receipt") {
    Directory fixture;
    const auto directory = fixture.Session();
    const auto expected = Expected();
    auto before = traj::CaptureManagedSessionOwnership(directory);
    REQUIRE(before.has_value());
    REQUIRE(before->state == traj::ManagedSessionOwnershipCapture::State::Absent);
    REQUIRE(traj::CheckLocalTrustedSessionOwnership(*before).has_value());
    auto lock = Acquire(directory);
    auto locked = traj::CaptureManagedSessionOwnershipLocked(directory, lock);
    REQUIRE(locked.has_value());
    REQUIRE(traj::CheckManagedSessionOwnershipCaptureUnchanged(*before, *locked).has_value());
    const auto published = PublishNew(directory, lock, expected);
    const auto json = Json::parse(published.publication_bytes);
    REQUIRE(json.size() == 7);
    REQUIRE(json.at("schemaVersion") == 1);
    REQUIRE(json.at("mode") == "Managed");
    REQUIRE(json.at("tenantId") == expected.tenant_id);
    REQUIRE(json.at("projectId") == expected.project_id);
    REQUIRE(json.at("workspaceKey") == expected.workspace_key);
    REQUIRE(json.at("sessionId") == expected.session_id);
    REQUIRE(json.at("bindingVersion") == expected.binding_version);
    auto captured = traj::CaptureManagedSessionOwnershipLocked(directory / "", lock);
    REQUIRE(captured.has_value());
    REQUIRE(captured->bytes == published.publication_bytes);
    REQUIRE(traj::CheckManagedSessionOwnership(*captured, expected).has_value());
    auto existing = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, expected);
    REQUIRE(existing.has_value());
    const auto matching = (*existing)->Publish(lock);
    REQUIRE(matching.knowledge == Knowledge::ExistingMatching);
    REQUIRE_FALSE(matching.requested.has_value());
    REQUIRE_FALSE(matching.native.has_value());
    REQUIRE(matching.captured_bytes == published.publication_bytes);
    REQUIRE(Read(MarkerFile(directory)) == published.publication_bytes);
    REQUIRE_FALSE(fs::exists(directory / "session-1.jsonl"));
    lock.Release();
    SamePublication((*existing)->Publish(lock), matching);
    const auto boundary_directory = fixture.Session("boundary/session-1");
    auto boundary_lock = Acquire(boundary_directory);
    auto boundary = Expected(std::string(512, 't'));
    boundary.project_id = "project-\xe9\xa1\xb9\xe7\x9b\xae";
    boundary.workspace_key = std::string(512, 'w');
    boundary.binding_version = (std::numeric_limits<std::uint64_t>::max)();
    const auto boundary_receipt = PublishNew(boundary_directory, boundary_lock, boundary);
    auto boundary_capture = traj::CaptureManagedSessionOwnership(boundary_directory);
    REQUIRE(boundary_capture.has_value());
    REQUIRE(traj::CheckManagedSessionOwnership(*boundary_capture, boundary).has_value());
    REQUIRE(boundary_capture->bytes == boundary_receipt.publication_bytes);
    Marker("actual");
}

TEST_CASE("managed ownership: local trusted eligibility rejects marked malformed and linked objects") {
    Directory fixture;
    const auto directory = fixture.Session();
    auto absent = traj::CaptureManagedSessionOwnership(directory);
    REQUIRE(absent.has_value());
    REQUIRE(traj::CheckLocalTrustedSessionOwnership(*absent).has_value());
    auto missing = traj::CheckManagedSessionOwnership(*absent, Expected());
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error() == "managed.ownership.required");
    auto lock = Acquire(directory);
    const auto receipt = PublishNew(directory, lock);
    auto marked = traj::CaptureManagedSessionOwnership(directory);
    REQUIRE(marked.has_value());
    REQUIRE_FALSE(traj::CheckLocalTrustedSessionOwnership(*marked).has_value());
    auto base = Json::parse(receipt.publication_bytes);
    std::vector<std::string> invalid{"", "{}", "null", "[]", "not-json", receipt.publication_bytes + "\n"};
    auto bad = base; bad["extra"] = true; invalid.push_back(bad.dump());
    bad = base; bad.erase("projectId"); invalid.push_back(bad.dump());
    bad = base; bad["schemaVersion"] = 2; invalid.push_back(bad.dump());
    bad = base; bad["schemaVersion"] = 1.0; invalid.push_back(bad.dump());
    bad = base; bad["mode"] = "LocalTrusted"; invalid.push_back(bad.dump());
    for (const auto* key : {"tenantId", "projectId", "workspaceKey", "sessionId"}) {
        for (const auto& value : {std::string(), std::string(513, 'x'), std::string("bad\nvalue"),
                                  std::string("bad\x7f"), std::string("bad\0", 4)}) {
            bad = base; bad[key] = value; invalid.push_back(bad.dump());
        }
        bad = base; bad[key] = 1; invalid.push_back(bad.dump());
    }
    for (const auto& version : {Json(0), Json(-1), Json(1.5), Json("1")}) {
        bad = base; bad["bindingVersion"] = version; invalid.push_back(bad.dump());
    }
    bad = base; bad["sessionId"] = "another-session"; invalid.push_back(bad.dump());
    invalid.push_back(std::string(traj::kManagedSessionOwnershipMaxBytes + 1, 'x'));
    invalid.push_back(std::string(1, static_cast<char>(0xff)));
    invalid.push_back("{\"schemaVersion\":1," + receipt.publication_bytes.substr(1));
    for (const auto& bytes : invalid) {
        Write(MarkerFile(directory), bytes);
        REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory).has_value());
        REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, Expected()).has_value());
        REQUIRE(Read(MarkerFile(directory)) == bytes);
    }
    REQUIRE(fs::remove(MarkerFile(directory)));
    REQUIRE(fs::create_directory(MarkerFile(directory)));
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory).has_value());
    REQUIRE(fs::remove(MarkerFile(directory)));
    const auto target = fixture.Session("external");
    DirectoryLink(target, MarkerFile(directory));
    const auto linked = traj::CaptureManagedSessionOwnership(directory);
    REQUIRE_FALSE(linked.has_value());
    REQUIRE(linked.error() == "managed.ownership.metadata_is_link");
    REQUIRE(fs::remove(MarkerFile(directory)));
    const auto directory_link = fixture.root / "linked-session";
    DirectoryLink(directory, directory_link);
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory_link).has_value());
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory_link / "").has_value());
    REQUIRE(fs::remove(directory_link));
#ifndef _WIN32
    fs::create_symlink(fixture.root / "absent-target", MarkerFile(directory));
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory).has_value());
    REQUIRE(fs::remove(MarkerFile(directory)));
    REQUIRE(::mkfifo(MarkerFile(directory).c_str(), 0600) == 0);
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory).has_value());
    REQUIRE(fs::remove(MarkerFile(directory)));
#endif
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(fs::path("session-1")).has_value());
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory / "..").has_value());
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnership(directory.root_path()).has_value());
    Marker("local");
}

TEST_CASE("managed ownership: recovery checks complete owner values and never adopts legacy content") {
    Directory fixture;
    const auto first = fixture.Session("tenant-a/session-1");
    const auto second = fixture.Session("tenant-b/session-1");
    auto first_lock = Acquire(first);
    auto second_lock = Acquire(second);
    const auto first_receipt = PublishNew(first, first_lock, Expected("tenant-a"));
    const auto second_receipt = PublishNew(second, second_lock, Expected("tenant-b"));
    REQUIRE(first_receipt.publication_bytes != second_receipt.publication_bytes);
    Write(first / "session-1.jsonl", "EXISTING_BODY_DO_NOT_READ_OR_REWRITE");
    auto first_capture = traj::CaptureManagedSessionOwnership(first);
    auto second_capture = traj::CaptureManagedSessionOwnership(second);
    REQUIRE(first_capture.has_value());
    REQUIRE(second_capture.has_value());
    REQUIRE(traj::CheckManagedSessionOwnership(*first_capture, Expected("tenant-a")).has_value());
    REQUIRE(traj::CheckManagedSessionOwnership(*second_capture, Expected("tenant-b")).has_value());
    REQUIRE_FALSE(traj::CheckManagedSessionOwnership(*first_capture, Expected("tenant-b")).has_value());
    auto recovery = traj::ManagedSessionOwnershipAttempt::Prepare(first, first_lock, Expected());
    REQUIRE(recovery.has_value());
    const auto same = (*recovery)->Publish(first_lock);
    REQUIRE(same.knowledge == Knowledge::ExistingMatching);
    REQUIRE_FALSE(same.native.has_value());
    REQUIRE_FALSE(same.requested.has_value());
    REQUIRE(Read(first / "session-1.jsonl") == "EXISTING_BODY_DO_NOT_READ_OR_REWRITE");
    for (unsigned field = 0; field != 5; ++field) {
        auto wrong = Expected();
        if (field == 0) wrong.tenant_id = "other-tenant";
        if (field == 1) wrong.project_id = "other-project";
        if (field == 2) wrong.workspace_key = "other-workspace";
        if (field == 3) wrong.session_id = "other-session";
        if (field == 4) wrong.binding_version = 2;
        REQUIRE_FALSE(traj::CheckManagedSessionOwnership(*first_capture, wrong).has_value());
        REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(first, first_lock, wrong).has_value());
    }
    for (const auto* entry : {"session-1.jsonl", "old-v2.json", "sdk-plan.json", "unknown-entry"}) {
        const auto legacy = fixture.Session(std::string(entry) + "/session-1");
        auto legacy_lock = Acquire(legacy);
        Write(legacy / entry, "LEGACY_BYTES");
        auto eligible = traj::CaptureManagedSessionOwnership(legacy);
        REQUIRE(eligible.has_value());
        REQUIRE(traj::CheckLocalTrustedSessionOwnership(*eligible).has_value());
        const auto attempt = traj::ManagedSessionOwnershipAttempt::Prepare(legacy, legacy_lock, Expected());
        REQUIRE_FALSE(attempt.has_value());
        REQUIRE(attempt.error() == "managed.ownership.legacy_directory");
        REQUIRE_FALSE(fs::exists(MarkerFile(legacy)));
        REQUIRE(Read(legacy / entry) == "LEGACY_BYTES");
    }
    REQUIRE(Read(MarkerFile(first)) == first_receipt.publication_bytes);
    REQUIRE(Read(MarkerFile(second)) == second_receipt.publication_bytes);
    Marker("recovery");
}

TEST_CASE("managed ownership: actual lock and directory pairing precede any publication") {
    Directory fixture;
    const auto first = fixture.Session();
    const auto second = fixture.Session("other/session-1");
    traj::SessionLock missing;
    REQUIRE_FALSE(traj::CaptureManagedSessionOwnershipLocked(first, missing).has_value());
    REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(first, missing, Expected()).has_value());
    auto first_lock = Acquire(first);
    auto second_lock = Acquire(second);
    const auto wrong = traj::CaptureManagedSessionOwnershipLocked(first, second_lock);
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error() == "managed.ownership.lock_mismatch");
    REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(first, second_lock, Expected()).has_value());
    auto attempt = traj::ManagedSessionOwnershipAttempt::Prepare(first, first_lock, Expected());
    REQUIRE(attempt.has_value());
    first_lock.Release();
    const auto rejected = (*attempt)->Publish(first_lock);
    REQUIRE(rejected.knowledge == Knowledge::RejectedBeforeIO);
    REQUIRE(rejected.error_code == "managed.ownership.lock_required");
    REQUIRE_FALSE(rejected.requested.has_value());
    REQUIRE_FALSE(rejected.native.has_value());
    REQUIRE_FALSE(fs::exists(MarkerFile(first)));
    auto replacement = Acquire(first);
    SamePublication((*attempt)->Publish(replacement), rejected);
    REQUIRE_FALSE(fs::exists(MarkerFile(first)));
    REQUIRE((*attempt)->FirstPublication().has_value());
    SamePublication(*(*attempt)->FirstPublication(), rejected);
    auto invalid = Expected(); invalid.binding_version = 0;
    REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(first, replacement, invalid).has_value());
    invalid = Expected(); invalid.tenant_id = std::string(1, static_cast<char>(0xff));
    REQUIRE_FALSE(traj::ManagedSessionOwnershipAttempt::Prepare(first, replacement, invalid).has_value());
    REQUIRE_FALSE(fs::exists(MarkerFile(first)));
    Marker("lock");
}

TEST_CASE("managed ownership: captured bytes and empty-directory admission are rechecked under lock") {
    Directory fixture;
    const auto directory = fixture.Session();
    const auto before = traj::CaptureManagedSessionOwnership(directory);
    REQUIRE(before.has_value());
    auto lock = Acquire(directory);
    auto pending = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, Expected());
    REQUIRE(pending.has_value());
    const auto receipt = PublishNew(directory, lock);
    const auto captured = traj::CaptureManagedSessionOwnershipLocked(directory, lock);
    REQUIRE(captured.has_value());
    REQUIRE_FALSE(traj::CheckManagedSessionOwnershipCaptureUnchanged(*before, *captured).has_value());
    auto stale = (*pending)->Publish(lock);
    REQUIRE(stale.knowledge == Knowledge::RejectedBeforeIO);
    REQUIRE(stale.error_code == "managed.ownership.source_changed");
    REQUIRE_FALSE(stale.requested.has_value());
    REQUIRE_FALSE(stale.native.has_value());
    auto existing = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, Expected());
    REQUIRE(existing.has_value());
    auto changed_json = Json::parse(receipt.publication_bytes);
    changed_json["bindingVersion"] = 2;
    Write(MarkerFile(directory), changed_json.dump());
    auto changed = traj::CaptureManagedSessionOwnershipLocked(directory, lock);
    REQUIRE(changed.has_value());
    REQUIRE_FALSE(traj::CheckManagedSessionOwnershipCaptureUnchanged(*captured, *changed).has_value());
    const auto rejected = (*existing)->Publish(lock);
    REQUIRE(rejected.knowledge == Knowledge::RejectedBeforeIO);
    REQUIRE(rejected.error_code == "managed.ownership.source_changed");
    REQUIRE_FALSE(rejected.requested.has_value());
    REQUIRE_FALSE(rejected.native.has_value());
    Write(MarkerFile(directory), receipt.publication_bytes);
    SamePublication((*existing)->Publish(lock), rejected);
    REQUIRE(Read(MarkerFile(directory)) == receipt.publication_bytes);
    const auto late_body = fixture.Session("late/session-1");
    auto late_lock = Acquire(late_body);
    auto new_attempt = traj::ManagedSessionOwnershipAttempt::Prepare(late_body, late_lock, Expected());
    REQUIRE(new_attempt.has_value());
    Write(late_body / "session-1.jsonl", "ARRIVED_AFTER_PREPARE");
    const auto denied = (*new_attempt)->Publish(late_lock);
    REQUIRE(denied.knowledge == Knowledge::RejectedBeforeIO);
    REQUIRE(denied.error_code == "managed.ownership.legacy_directory");
    REQUIRE_FALSE(denied.requested.has_value());
    REQUIRE_FALSE(denied.native.has_value());
    REQUIRE_FALSE(fs::exists(MarkerFile(late_body)));
    REQUIRE(Read(late_body / "session-1.jsonl") == "ARRIVED_AFTER_PREPARE");
    Marker("drift");
}

TEST_CASE("managed ownership: first native failure stays owned and never upgrades from visible bytes") {
    Directory fixture;
    FlushFaultReset restore_faults;
    const auto directory = fixture.Session();
    auto lock = Acquire(directory);
    auto attempt = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, Expected());
    REQUIRE(attempt.has_value());
    // Same linked platform instance: actual temporary write and rename succeed;
    // only the existing directory-flush seam substitutes the post-rename flush.
    platform::SetDirectoryFlushFailureForTest(true);
    const auto first = (*attempt)->Publish(lock);
    platform::SetDirectoryFlushFailureForTest(false);
    REQUIRE(first.knowledge == Knowledge::Unconfirmed);
    REQUIRE(first.requested == platform::WriteDurability::ProcessCrashDurability);
    REQUIRE(first.native.has_value());
    REQUIRE_FALSE(first.native->has_value());
    REQUIRE(first.native->error().outcome == platform::WriteOutcome::CommittedDurabilityUnconfirmed);
    REQUIRE(first.native->error().code == "atomic.durability_flush_failed");
    REQUIRE(first.native->error().failure_kind == platform::WriteFailureKind::Permanent);
    REQUIRE(first.error_code == first.native->error().code);
    REQUIRE(first.message == first.native->error().message);
    REQUIRE(Read(MarkerFile(directory)) == first.publication_bytes);
    SamePublication((*attempt)->Publish(lock), first);
    auto fresh = traj::ManagedSessionOwnershipAttempt::Prepare(directory, lock, Expected());
    REQUIRE(fresh.has_value());
    const auto matching = (*fresh)->Publish(lock);
    REQUIRE(matching.knowledge == Knowledge::ExistingMatching);
    REQUIRE_FALSE(matching.native.has_value());
    REQUIRE_FALSE(matching.requested.has_value());
    REQUIRE_FALSE(fs::exists(directory / "session-1.jsonl"));
    REQUIRE(Read(MarkerFile(directory)) == first.publication_bytes);
    // Invalid subsequent bytes and a released lock cannot turn a historical
    // result into fresh IO, rewrite the marker, or read back an upgrade.
    Write(MarkerFile(directory), "VISIBLE_SENTINEL_AFTER_FIRST_RESULT");
    lock.Release();
    SamePublication((*attempt)->Publish(lock), first);
    REQUIRE((*attempt)->FirstPublication().has_value());
    SamePublication(*(*attempt)->FirstPublication(), first);
    SamePublication((*fresh)->Publish(lock), matching);
    REQUIRE(Read(MarkerFile(directory)) == "VISIBLE_SENTINEL_AFTER_FIRST_RESULT");
    attempt->reset();
    fresh->reset();
    REQUIRE(Read(MarkerFile(directory)) == "VISIBLE_SENTINEL_AFTER_FIRST_RESULT");

    const auto not_committed = fixture.Session("not-committed/session-1");
    auto other_lock = Acquire(not_committed);
    auto before_rename = traj::ManagedSessionOwnershipAttempt::Prepare(not_committed, other_lock, Expected());
    REQUIRE(before_rename.has_value());
    platform::SetFileFlushFailureForTest(true);
    const auto failed = (*before_rename)->Publish(other_lock);
    platform::SetFileFlushFailureForTest(false);
    REQUIRE(failed.knowledge == Knowledge::NotCommitted);
    REQUIRE(failed.requested == platform::WriteDurability::ProcessCrashDurability);
    REQUIRE(failed.native.has_value());
    REQUIRE_FALSE(failed.native->has_value());
    REQUIRE(failed.native->error().outcome == platform::WriteOutcome::NotCommitted);
    REQUIRE(failed.native->error().code == "atomic.tmp_write_failed");
    REQUIRE(failed.error_code == failed.native->error().code);
    REQUIRE(failed.message == failed.native->error().message);
    REQUIRE_FALSE(fs::exists(MarkerFile(not_committed)));
    SamePublication((*before_rename)->Publish(other_lock), failed);
    REQUIRE_FALSE(fs::exists(MarkerFile(not_committed)));
    for (const auto& entry : fs::directory_iterator(not_committed)) {
        REQUIRE(entry.path().filename() == "session.lock");
    }
    const auto independent = PublishNew(not_committed, other_lock);
    REQUIRE(independent.knowledge == Knowledge::Committed);
    SamePublication((*before_rename)->Publish(other_lock), failed);
    REQUIRE(Read(MarkerFile(not_committed)) == independent.publication_bytes);
    Marker("unknown");
}
