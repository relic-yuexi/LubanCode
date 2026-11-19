#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "trajectory/journal.hpp"

namespace lubancode::trajectory {

// File-owned immutable bytes and the actual opened native object. This handle
// never retains a V3Writer, SessionManager, SessionService or public Session.
// Close only releases the anchor; already captured bytes remain readable.
class JournalReadHandle {
public:
    JournalReadHandle() = default;
    const std::filesystem::path& path() const;
    const std::string& bytes() const;
    std::expected<void, std::string> Close() const;
    explicit operator bool() const noexcept { return static_cast<bool>(state_); }
    // Internal compatibility witness for existing File callers, not a token
    // that a storage provider or public host can construct.
    std::shared_ptr<JournalFileAnchor> native_anchor() const;
private:
    struct State;
    explicit JournalReadHandle(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::shared_ptr<State> state_;
    friend class FileJournalAdapter;
};

// The first adapter owns the original native writer; it does not reimplement
// fwrite, newline, fflush, fsync/FlushFileBuffers, errno or native Close.
// V2 JournalWriter users and legacy append entry points remain File.
class FileJournalAdapter {
public:
    FileJournalAdapter() = default;
    FileJournalAdapter(FileJournalAdapter&&) noexcept = default;
    FileJournalAdapter& operator=(FileJournalAdapter&&) noexcept = default;
    FileJournalAdapter(const FileJournalAdapter&) = delete;
    FileJournalAdapter& operator=(const FileJournalAdapter&) = delete;
    static std::expected<FileJournalAdapter, std::string> CreateNew(
        const std::filesystem::path&, std::shared_ptr<JournalNativeIoProbe> = {});
    static std::expected<FileJournalAdapter, std::string> ContinueFile(
        const std::filesystem::path&); // Legacy File caller supplies validation.
    static std::expected<FileJournalAdapter, std::string> ContinueOwnedPrefix(
        const std::filesystem::path&, std::string_view, const JournalFileAnchor&);
    static std::expected<JournalReadHandle, std::string> CaptureExisting(
        const std::filesystem::path&, std::optional<std::size_t>, bool follow_path = true);
    JournalAppendReceipt AppendLine(std::string_view line, Durability durability) noexcept;
    JournalCloseReceipt Close() noexcept;
    const std::filesystem::path& path() const { return writer_.path(); }
    std::optional<JournalAppendReceipt> first_native_unconfirmed() const noexcept {
        return writer_.first_unconfirmed_append();
    }
private:
    explicit FileJournalAdapter(JournalWriter writer) : writer_(std::move(writer)) {}
    JournalWriter writer_;
};

struct JournalAppendIdentity {
    std::string id;
    std::uint64_t seq = 0;
    std::string line_hash;
    std::size_t canonical_bytes = 0;
};
struct JournalOwnerUnconfirmed {
    enum class Phase { NativeAppend, SemanticCompletion } phase = Phase::NativeAppend;
    JournalAppendIdentity identity;
    // Actual native facts: SemanticCompletion may contain Committed here.
    // That is never inserted into first_native_unconfirmed().
    JournalAppendReceipt native;
};

// One write lease and one first uncertainty for the owned File stream. V3 still
// owns canonical bytes, sequence/hash chains and context state. The containing
// SessionManager acquires its real SessionLock before constructing this owner.
// Standalone V3/child/legacy File callers retain their original entry points;
// this internal owner is not an external JournalStore or an ACL authority.
class JournalOwner {
public:
    struct State;
    class AppendLease {
    public:
        AppendLease(AppendLease&&) noexcept;
        AppendLease& operator=(AppendLease&&) noexcept;
        AppendLease(const AppendLease&) = delete;
        AppendLease& operator=(const AppendLease&) = delete;
        ~AppendLease();
        const JournalAppendReceipt& native() const noexcept { return receipt_.native; }
        // Called only after V3 has completed its in-memory state update.
        void Complete() noexcept;
        // No allocations/native calls. Freeze before releasing the same gate.
        void UnconfirmSemantic() noexcept;
    private:
        AppendLease(std::shared_ptr<State>, std::unique_lock<std::mutex>, JournalOwnerUnconfirmed);
        void Retire() noexcept;
        // The write lease independently keeps its actual State/mutex/native
        // handle alive when the owner facade is moved, replaced or destroyed.
        std::shared_ptr<State> state_;
        std::unique_lock<std::mutex> lock_;
        JournalOwnerUnconfirmed receipt_;
        bool complete_ = false;
        friend class JournalOwner;
    };
    JournalOwner();
    JournalOwner(JournalOwner&&) noexcept;
    JournalOwner& operator=(JournalOwner&&) noexcept;
    JournalOwner(const JournalOwner&) = delete;
    JournalOwner& operator=(const JournalOwner&) = delete;
    ~JournalOwner();
    static std::expected<JournalOwner, std::string> CreateNew(
        const std::filesystem::path&, std::shared_ptr<JournalNativeIoProbe> = {});
    static std::expected<JournalOwner, std::string> ContinueFile(const std::filesystem::path&);
    static std::expected<JournalOwner, std::string> ContinueOwnedPrefix(
        const std::filesystem::path&, std::string_view, const JournalFileAnchor&);
    static std::expected<JournalOwner, std::string> ContinueCaptured(const JournalReadHandle&);
    static std::expected<JournalReadHandle, std::string> CaptureExisting(
        const std::filesystem::path&, std::optional<std::size_t>, bool follow_path = true);
    // Identity ownership/allocation finishes before invoking actual native I/O.
    // The returned lease holds the same gate through the semantic completion.
    AppendLease AppendLine(std::string_view, Durability, JournalAppendIdentity);
    JournalCloseReceipt CloseDetailed() noexcept;
    bool Close() noexcept { return CloseDetailed().ok(); }
    const std::filesystem::path& path() const;
    std::optional<JournalOwnerUnconfirmed> first_unconfirmed() const;
    bool semantic_unconfirmed() const noexcept;
    std::optional<JournalAppendReceipt> first_unconfirmed_append() const noexcept;
    std::expected<JournalReadHandle, std::string> Capture(std::optional<std::size_t> = {}) const;
private:
    explicit JournalOwner(FileJournalAdapter);
    std::shared_ptr<State> state_;
};

} // namespace lubancode::trajectory
