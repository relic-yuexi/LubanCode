#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace lubancode::platform {

// Internal, opt-in observations. No callback or log output runs in the process
// layer, and no observation may run in the post-fork child branch.
enum class ProcessDiagnosticStage : std::uint32_t {
    ToolCallEntered,
    ToolCallReturned,
    ToolCallThrew,
    TestCancelPublishBefore,
    TestCancelPublishAfter,
    ProcessEntered,
    ExecPipeCreated,
    ExecWriteFdFlags,
    ForkBefore,
    ForkParentReturned,
    SetProcessGroupBefore,
    SetProcessGroupAfter,
    ProcessGroupSnapshot,
    ExecHandshakeBefore,
    ExecHandshakeAfter,
    SpawnReady,
    WaitFirst,
    WaitTerminal,
    WaitDeadline,
    OutputLimitObserved,
    CancelObserved,
    TimeoutObserved,
    KillTermBefore,
    KillTermAfter,
    KillForceBefore,
    KillForceAfter,
    ReapBefore,
    ReapAfter,
    ReaderEntered,
    ReaderFirstPoll,
    ReaderFirstReadBefore,
    ReaderFirstReadAfter,
    ReaderReadTerminal,
    ReaderExited,
    ReaderStopPublished,
    ReaderJoinBefore,
    ReaderJoinAfter,
    ProcessReturned,
    CreateProcessBefore,
    CreateProcessAfter,
    CreateJobBefore,
    CreateJobAfter,
    ConfigureJobAfter,
    AssignJobBefore,
    AssignJobAfter,
    ResumeBefore,
    ResumeAfter,
    WaitBeforeFirst,
    WaitAfterFirst,
    WaitTerminationBefore,
    WaitTerminationAfter,
    CloseJobBefore,
    CloseJobAfter,
    TerminateProcessBefore,
    TerminateProcessAfter,
    CancelReaderIoBefore,
    CancelReaderIoAfter,
    ExitCodeRead,
    JobAccountingQueryBefore,
    JobAccountingQueryAfter,
};

// Internal values from one successful JobObjectBasicAccountingInformation
// query. They describe that Job's accounting, never probe or task identity.
struct ProcessDiagnosticJobAccounting {
    std::uint32_t total_processes;
    std::uint32_t active_processes;
    std::uint32_t terminated_processes;
    std::int64_t total_user_time_100ns;
    std::int64_t total_kernel_time_100ns;
};
static_assert(std::is_trivial_v<ProcessDiagnosticJobAccounting>);
static_assert(std::is_standard_layout_v<ProcessDiagnosticJobAccounting>);

struct ProcessDiagnosticRecord {
    ProcessDiagnosticStage stage;
    std::int64_t steady_ns;
    std::int64_t pid;
    std::int64_t pgid;
    std::int64_t rc;
    std::int64_t detail;
    std::uint32_t system_error;
    // Valid only for JobAccountingQueryAfter with a successful native rc.
    ProcessDiagnosticJobAccounting job_accounting;
};
static_assert(std::is_trivial_v<ProcessDiagnosticRecord>);
static_assert(std::is_standard_layout_v<ProcessDiagnosticRecord>);

// Trusted, private same-call observation. No command JSON/public SDK option.
// Initialized before recording starts, then owned by the borrowed buffer.
struct ProcessCommandStartObservation {
    std::string tag;
    // shell-entry, wrapper-ready, user-block-entry, in that order.
    std::array<std::string, 3> paths_utf8;
};

class ProcessDiagnosticBuffer {
public:
    static constexpr std::uint32_t kCapacity = 128;

    bool ConfigureCommandStartObservation(ProcessCommandStartObservation value) {
        if (Reserved() != 0 || command_start_observation_) return false;
        command_start_observation_.emplace(std::move(value));
        return true;
    }
    const ProcessCommandStartObservation* CommandStartObservation() const noexcept {
        return command_start_observation_ ? &*command_start_observation_ : nullptr;
    }

    // The borrowed buffer must outlive every caller/reader that records into it.
    // Each slot has one writer; release publishes its complete POD record.
    void Record(ProcessDiagnosticStage stage, std::int64_t pid = -1,
                std::int64_t pgid = -1, std::int64_t rc = 0,
                std::uint32_t system_error = 0, std::int64_t detail = 0,
                ProcessDiagnosticJobAccounting job_accounting = {}) noexcept {
        const int saved_errno = errno;
        const auto index = reserved_.fetch_add(1, std::memory_order_relaxed);
        if (index >= kCapacity) {
            overflow_.store(true, std::memory_order_release);
            errno = saved_errno;
            return;
        }
        auto& slot = slots_[index];
        slot.record = {stage,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count(),
            pid, pgid, rc, detail, system_error, job_accounting};
        slot.published.store(true, std::memory_order_release);
        errno = saved_errno;
    }

    std::uint32_t Reserved() const noexcept {
        return reserved_.load(std::memory_order_acquire);
    }
    bool Overflowed() const noexcept {
        return overflow_.load(std::memory_order_acquire);
    }
    // Snapshot users only copy published slots. A reservation alone is not a
    // completed observation; never infer a missing stage from it.
    bool ReadPublished(std::uint32_t index, ProcessDiagnosticRecord& record) const noexcept {
        if (index >= kCapacity || !slots_[index].published.load(std::memory_order_acquire)) {
            return false;
        }
        record = slots_[index].record;
        return true;
    }

private:
    struct Slot {
        ProcessDiagnosticRecord record{};
        std::atomic<bool> published{false};
    };
    std::array<Slot, kCapacity> slots_{};
    std::atomic<std::uint32_t> reserved_{0};
    std::atomic<bool> overflow_{false};
    // Never mutated once a caller/reader can observe the buffer.
    std::optional<ProcessCommandStartObservation> command_start_observation_;
};

inline thread_local ProcessDiagnosticBuffer* process_diagnostics = nullptr;

class ScopedProcessDiagnostics {
public:
    explicit ScopedProcessDiagnostics(ProcessDiagnosticBuffer* buffer) noexcept
        : previous_(process_diagnostics) { process_diagnostics = buffer; }
    ~ScopedProcessDiagnostics() { process_diagnostics = previous_; }
    ScopedProcessDiagnostics(const ScopedProcessDiagnostics&) = delete;
    ScopedProcessDiagnostics& operator=(const ScopedProcessDiagnostics&) = delete;
private:
    ProcessDiagnosticBuffer* previous_;
};

inline void RecordProcessDiagnostic(ProcessDiagnosticBuffer* buffer,
                                    ProcessDiagnosticStage stage,
                                    std::int64_t pid = -1, std::int64_t pgid = -1,
                                    std::int64_t rc = 0, std::uint32_t system_error = 0,
                                    std::int64_t detail = 0) noexcept {
    // The ordinary path does not read a clock or touch diagnostic atomics.
    if (buffer) buffer->Record(stage, pid, pgid, rc, system_error, detail);
}

constexpr const char* ProcessDiagnosticStageName(ProcessDiagnosticStage stage) noexcept {
    switch (stage) {
#define LUBAN_PROCESS_DIAGNOSTIC_NAME(name) case ProcessDiagnosticStage::name: return #name;
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ToolCallEntered)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ToolCallReturned)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ToolCallThrew)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(TestCancelPublishBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(TestCancelPublishAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ProcessEntered)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ExecPipeCreated)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ExecWriteFdFlags)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ForkBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ForkParentReturned)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(SetProcessGroupBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(SetProcessGroupAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ProcessGroupSnapshot)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ExecHandshakeBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ExecHandshakeAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(SpawnReady)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitFirst)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitTerminal)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitDeadline)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(OutputLimitObserved)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CancelObserved)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(TimeoutObserved)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(KillTermBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(KillTermAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(KillForceBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(KillForceAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReapBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReapAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderEntered)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderFirstPoll)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderFirstReadBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderFirstReadAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderReadTerminal)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderExited)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderStopPublished)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderJoinBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ReaderJoinAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ProcessReturned)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CreateProcessBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CreateProcessAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CreateJobBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CreateJobAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ConfigureJobAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(AssignJobBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(AssignJobAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ResumeBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ResumeAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitBeforeFirst)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitAfterFirst)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitTerminationBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(WaitTerminationAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CloseJobBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CloseJobAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(TerminateProcessBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(TerminateProcessAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CancelReaderIoBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(CancelReaderIoAfter)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(ExitCodeRead)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(JobAccountingQueryBefore)
        LUBAN_PROCESS_DIAGNOSTIC_NAME(JobAccountingQueryAfter)
#undef LUBAN_PROCESS_DIAGNOSTIC_NAME
    }
    return "UnknownStage";
}

}  // namespace lubancode::platform
