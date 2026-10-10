#pragma once

#include <cstddef>
#include <compare>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trajectory/journal.hpp"
#include "trajectory/journal_owner.hpp"

namespace lubancode::trajectory {

// Neutral owned values; the SDK converts its public budget at the boundary.
struct RecoveryStreamReadLimits {
    std::size_t max_bytes = 0, max_lines = 0, max_line_bytes = 0;
};
struct RecoveryReadLimits {
    RecoveryStreamReadLimits journal{128u * 1024u * 1024u, 262144u, 8u * 1024u * 1024u};
    RecoveryStreamReadLimits operations{16u * 1024u * 1024u, 65536u, 256u * 1024u};
    std::size_t result_total_bytes = 128u * 1024u * 1024u;
    std::size_t view_total_bytes = 384u * 1024u * 1024u;
    std::size_t result_directory_entries = 4096u, view_directory_entries = 8192u;
    std::size_t directory_name_bytes = 1024u, directory_name_total_bytes = 8u * 1024u * 1024u;
};
bool ValidRecoveryReadLimits(const RecoveryReadLimits& limits);

enum class RecoveryKeyKind { MainV3, MemoryPlan, Operations, SdkResult, MemoryRecall };
struct RecoveryKey {
    RecoveryKeyKind kind = RecoveryKeyKind::MainV3;
    std::string operation_id;
    auto operator<=>(const RecoveryKey&) const = default;
};
enum class RecoveryReadState { Absent, Value, Corrupt, IOError };
struct RecoveryValue {
    RecoveryReadState state = RecoveryReadState::Absent;
    std::string bytes;
    std::string error;
};
struct RecoveryDirectoryEntry {
    std::string name;
    bool regular = false;
    bool temporary = false;
    bool operator==(const RecoveryDirectoryEntry&) const = default;
};
struct RecoveryDirectory {
    bool present = false;
    std::vector<RecoveryDirectoryEntry> entries;
    bool operator==(const RecoveryDirectory&) const = default;
};
struct SessionRecoveryView {
    std::string workspace_key, session_id;
    std::string stream = "main";
    std::string snapshot_token; // Consistency identity, never authentication.
    std::map<RecoveryKey, RecoveryValue> values;
    RecoveryDirectory results, reports;
    const RecoveryValue* Find(RecoveryKeyKind kind, const std::string& operation_id = {}) const;
};
// Internal successful SDK preflight evidence. It only restricts a later actual
// locked capture; it supplies no bytes, native anchor, provider or permission.
struct RecoveryMainExpectation {
    std::string workspace_key, session_id, stream;
    std::size_t bytes = 0;
    std::string sha256;
};
struct RecoveryCaptureRequest {
    std::optional<RecoveryReadLimits> limits; // CLI unset preserves its total-size policy.
    bool memory_metadata = false;
    std::optional<RecoveryMainExpectation> expected_main;
};

// Internal read adapter. It receives owned File reference values inside the real
// locked opening; native anchor and physical path are deliberately excluded.
using SessionRecoveryFactory = std::function<std::expected<SessionRecoveryView, std::string>(
    const SessionRecoveryView& reference)>;
struct SessionRecoveryCapture {
    SessionRecoveryView view;
    std::shared_ptr<JournalFileAnchor> anchor; // Existing File-only compatibility.
    JournalReadHandle main_journal; // Actual immutable main capture, no Writer owner.
};
std::expected<SessionRecoveryCapture, std::string> CaptureSessionRecovery(
    const std::filesystem::path& session_dir, std::string workspace_key, std::string session_id,
    const RecoveryCaptureRequest& request, const SessionRecoveryFactory& factory = {});

// Reject malformed bytes and incomplete logical rosters before domain adoption.
std::expected<void, std::string> CheckRecoveryView(
    const SessionRecoveryView& view, const RecoveryCaptureRequest& request);
std::expected<std::vector<std::string>, std::string> RecoveryStreamLines(
    std::string_view bytes, const std::optional<RecoveryStreamReadLimits>& limits,
    bool skip_empty_lines = false);

} // namespace lubancode::trajectory
