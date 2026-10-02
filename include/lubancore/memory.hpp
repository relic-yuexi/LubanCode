#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lubancore::memory::v1 {

// Trusted local project recall only. No learning, writes or background workers.
struct RecallOptions {
    // Budget for selected sections and payloads. Provenance/guards are additional;
    // the complete context has a separate fixed 131072-byte bound.
    std::size_t max_bytes = 8192;
    std::size_t max_results = 3;
    bool operator==(const RecallOptions&) const = default;
};
struct Snapshot {
    bool enabled = false;
    std::string session_id;
    std::string workspace_key;
    std::string memory_directory;
    RecallOptions options;
    std::string plan_sha256;
};
struct Entry {
    std::string id;
    int score = 0;
    bool selected = false; // selected by recall; admission is described by report.state
    bool stale = false;
    bool expired = false;
    bool scope_blocked = false;
    bool budget_dropped = false;
    bool below_threshold = false;
    bool weak = false;
    std::string reason;
    std::size_t bytes = 0;
};
// Owned per-operation metadata. operation_id is scoped to session_id; wire
// callers must address the (session_id, operation_id) pair. It contains no query
// or recalled source text.
// context_message_id identifies the complete, once-admitted V3 input snapshot.
struct RecallReport {
    bool enabled = false;
    std::string session_id;
    std::string operation_id;
    std::string turn_id;
    std::string workspace_key;
    std::string plan_sha256;
    std::string state; // disabled | not_attempted | no_match | admitted | failed
    std::string error;
    std::string context_message_id;
    std::string context_sha256;
    std::size_t bytes = 0;
    std::vector<Entry> entries;
};

// Explicit trusted local project writes. Enabling this does not enable recall,
// user memory, automatic extraction or a remote Worker permission.
struct WriteOptions {
    bool enabled = true;
    bool operator==(const WriteOptions&) const = default;
};
struct WriteSnapshot {
    bool enabled = false;
    std::string session_id;
    std::string workspace_key;
    std::string memory_directory;
    std::string plan_sha256;
};
struct SaveStage {
    std::string stage;   // intent | snapshot | topic | cleanup | catalog | index | result
    std::string outcome; // not_committed | visible | unconfirmed | durable
    bool operator==(const SaveStage&) const = default;
};
// Owned metadata for one actual tool action. operation_id is Session scoped.
// request_sha256 includes the gate target/source; save_request_sha256 identifies
// the normalized SaveRequest before its requested-event reference exists.
// It contains no submitted body. selected/tool-result success is not a receipt.
struct SaveReport {
    std::string session_id;
    std::string operation_id;
    std::string turn_id;
    std::string action_id;
    std::size_t attempt = 0;
    std::string workspace_key;
    std::string plan_sha256;
    std::string commit_key;
    std::string requested_event_id;
    std::string receipted_event_id;
    std::string source_event_ref;
    std::string save_request_sha256;
    std::string request_sha256;
    std::string state; // not_started | committed | indeterminate
    std::string memory_id;
    std::string memory_path;
    std::string content_sha256;
    std::string committed_at;
    std::vector<SaveStage> stages;
    bool duplicate = false;
    std::string error_code;
    std::string error;
};

} // namespace lubancore::memory::v1
