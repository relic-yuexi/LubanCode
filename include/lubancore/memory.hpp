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

} // namespace lubancore::memory::v1
