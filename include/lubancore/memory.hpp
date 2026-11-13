#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <lubancore/usage.hpp>

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

using SaveStageReceipt = SaveStage;
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

// Automatic memory learning owner (Off, Review, or Auto durable writeback).
enum class LearningMode { Off, Review, Auto };
struct LearningOptions {
    LearningMode mode = LearningMode::Off;
    std::string model;
    std::string reasoning_effort;
    int timeout_secs = 45;
    std::size_t max_candidates = 3;
    std::size_t max_transcript_bytes = 8192;
    bool auto_write_enabled = true;
    bool operator==(const LearningOptions&) const = default;
};
struct LearningSnapshot {
    LearningMode mode = LearningMode::Off;
    std::string session_id;
    std::string workspace_key;
    std::string memory_directory;
    std::string model;
    std::string reasoning_effort;
    int timeout_secs = 45;
    std::size_t max_candidates = 3;
    std::size_t max_transcript_bytes = 8192;
    bool auto_write_enabled = true;
    std::string plan_sha256;
};
enum class LearningStage {
    NotAttempted,
    Skipped,
    BusyDropped,
    StartFailed,
    Running,
    Failed,
    Cancelled,
    Completed,
    Indeterminate,
};
struct LearningWriteAttempt {
    std::string commit_key;
    std::string source_event_ref;
    std::string save_request_sha256;
    std::string request_sha256;
    std::string queue_stage = "not_queued";      // not_queued | queued | completed | cancelled | failed | indeterminate
    std::string worker_stage = "not_started";    // not_started | running | finished | cancelled | failed | indeterminate
    std::string admission_stage = "not_checked"; // not_checked | admitted | denied | revoked | indeterminate
    std::string commit_state = "not_started";    // not_started | committed | indeterminate
    std::string fallback_candidate_id;
    std::string memory_id;
    std::string memory_path;
    std::string content_sha256;
    std::string committed_at;
    std::vector<SaveStageReceipt> stages;
    bool duplicate = false;
    std::string error_code;
    std::string error;
    bool operator==(const LearningWriteAttempt&) const = default;
};

// Owned per-operation automatic learning receipt. Retained as pure values after Close.
// turn_id names the triggering main turn; learning_turn_id names the bypass turn when called.
struct LearningReport {
    LearningMode mode = LearningMode::Off;
    std::string session_id;
    std::string operation_id;
    std::string turn_id;
    std::string learning_turn_id;
    std::string workspace_key;
    std::string plan_sha256;
    LearningStage stage = LearningStage::NotAttempted;
    std::string skip_reason;
    std::string task_type;
    std::string model;
    std::vector<std::string> candidate_ids;
    std::vector<std::string> retrieval_hints;
    std::vector<LearningWriteAttempt> auto_writes;
    std::string error_code;
    std::string error;
    OperationUsage usage;
    bool model_called = false;
};

struct LearningCandidate {
    std::string id;
    std::string kind; // fact | preference | feedback
    std::string title;
    std::string summary;
    std::string content;
    std::vector<std::string> keywords;
    std::vector<std::string> paths;
    std::string confidence;
    std::string task_type;
    std::string created_at;
    std::string occurred_at;
    bool operator==(const LearningCandidate&) const = default;
};

struct CandidateAcceptReport {
    std::string session_id;
    std::string candidate_id;
    std::string workspace_key;
    std::string learning_plan_sha256;
    std::string write_plan_sha256;
    std::string commit_key;
    std::string source_event_ref;
    std::string save_request_sha256;
    std::string request_sha256;
    std::string state = "not_started"; // not_started | committed | indeterminate
    std::string memory_id;
    std::string memory_path;
    std::string content_sha256;
    std::string committed_at;
    std::vector<SaveStageReceipt> stages;
    bool duplicate = false;
    std::string error_code;
    std::string error;
    bool operator==(const CandidateAcceptReport&) const = default;
};

} // namespace lubancore::memory::v1
