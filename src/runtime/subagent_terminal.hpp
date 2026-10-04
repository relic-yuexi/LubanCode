#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "trajectory/recorder.hpp"
#include "trajectory/v3/writer.hpp"
#include "trajectory/v3/subagent.hpp"

namespace lubancode::runtime {

enum class SubagentExecutionOutcome { Succeeded, Failed, Cancelled, StartupRejected, Indeterminate };
enum class SubagentJournalFormat { V2, V3 };
enum class SubagentAppendConfirmation { Committed, RejectedBeforeCommit, DurabilityUnconfirmed };
enum class SubagentSealState { NotAttempted, Closed, CloseFailed };
// This value comes from the dispatch decision, including auto/profile/defaults.
enum class SubagentDispatchMode { Foreground, Background };

struct SubagentTerminalRef {
    std::string session_id;
    std::string run_id;
    std::string event_id;
    std::uint64_t seq = 0;
    std::string hash;
};

// Immutable source values only: no writer, task, bridge or parent book borrow.
struct SubagentSpawnProvenance {
    trajectory::v3::ParentActionRef parent_action;
    trajectory::v3::ChildSessionRef child;
    std::string task_id;
    SubagentTerminalRef spawn;
    trajectory::v3::WriteReceipt native_spawn;
    std::uint64_t attempt = 1;
    std::filesystem::path parent_journal;
};

// Owned evidence only. A child receipt is not a persisted parent observation
// or proof that the parent model adopted the final text.
struct SubagentTerminalReceipt {
    SubagentJournalFormat format = SubagentJournalFormat::V3;
    std::string session_id;
    std::string run_id;
    std::string terminal_kind;
    SubagentExecutionOutcome execution = SubagentExecutionOutcome::Failed;
    std::string reason;
    std::variant<std::monostate, trajectory::RecordReceipt, trajectory::v3::WriteReceipt> append;
    SubagentAppendConfirmation confirmation = SubagentAppendConfirmation::DurabilityUnconfirmed;
    std::optional<SubagentTerminalRef> terminal;
    bool broken_after_append = false;
    bool broken_after_close = false;
    std::string append_error_code;
    std::string append_error_message;
    SubagentSealState seal = SubagentSealState::NotAttempted;
    std::string close_error_code;
    std::string close_error_message;
    std::string journal_sha256;  // V2 checked Close digest only.

    bool durable() const {
        return confirmation == SubagentAppendConfirmation::Committed && terminal.has_value() &&
               seal == SubagentSealState::Closed;
    }
};

// One session's shared owner; replacement on a session switch keeps late child
// publications out of the new session. It protects receipts, not parent writers.
class SubagentTerminalRegistry {
public:
    void Store(SubagentTerminalReceipt receipt) {
        std::lock_guard lock(mutex_);
        const auto run_id = receipt.run_id;
        receipts_.try_emplace(run_id, std::move(receipt));
    }
    std::optional<SubagentTerminalReceipt> Find(const std::string& run_id) const {
        std::lock_guard lock(mutex_);
        const auto found = receipts_.find(run_id);
        if (found == receipts_.end()) return std::nullopt;
        return found->second;
    }
private:
    mutable std::mutex mutex_;
    std::map<std::string, SubagentTerminalReceipt> receipts_;
};

}  // namespace lubancode::runtime
