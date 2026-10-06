#include "trajectory/journal_owner.hpp"

#include <utility>

namespace lubancode::trajectory {

struct JournalReadHandle::State {
    std::filesystem::path path;
    std::string bytes;
    std::shared_ptr<JournalFileAnchor> anchor;
    mutable std::mutex mutex;
    mutable std::optional<std::expected<void, std::string>> first_close;
};
const std::filesystem::path& JournalReadHandle::path() const {
    static const std::filesystem::path empty;
    return state_ ? state_->path : empty;
}
const std::string& JournalReadHandle::bytes() const {
    static const std::string empty;
    return state_ ? state_->bytes : empty;
}
std::shared_ptr<JournalFileAnchor> JournalReadHandle::native_anchor() const {
    return state_ ? state_->anchor : nullptr;
}
std::expected<void, std::string> JournalReadHandle::Close() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    if (!state_->first_close) state_->first_close.emplace(state_->anchor->Close());
    return *state_->first_close;
}

std::expected<FileJournalAdapter, std::string> FileJournalAdapter::CreateNew(
    const std::filesystem::path& path, std::shared_ptr<JournalNativeIoProbe> probe) {
    auto writer = probe ? JournalWriter::OpenWithNativeIoProbe(path, JournalWriter::OpenMode::CreateNew, std::move(probe))
                        : JournalWriter::Open(path, JournalWriter::OpenMode::CreateNew);
    if (!writer) return std::unexpected(writer.error());
    return FileJournalAdapter(std::move(*writer));
}
std::expected<FileJournalAdapter, std::string> FileJournalAdapter::ContinueFile(const std::filesystem::path& path) {
    auto writer = JournalWriter::Open(path, JournalWriter::OpenMode::Append);
    if (!writer) return std::unexpected(writer.error());
    return FileJournalAdapter(std::move(*writer));
}
std::expected<FileJournalAdapter, std::string> FileJournalAdapter::ContinueOwnedPrefix(
    const std::filesystem::path& path, std::string_view prefix, const JournalFileAnchor& anchor) {
    auto writer = JournalWriter::OpenExistingVerified(path, prefix, anchor);
    if (!writer) return std::unexpected(writer.error());
    return FileJournalAdapter(std::move(*writer));
}
std::expected<JournalReadHandle, std::string> FileJournalAdapter::CaptureExisting(
    const std::filesystem::path& path, std::optional<std::size_t> max_bytes, bool follow_path) {
    // Reserve handle ownership/path before opening native resources. Captured
    // bytes and native anchor then move without allocation or a second read.
    auto state = std::make_shared<JournalReadHandle::State>();
    state->path = path;
    auto capture = JournalFileAnchor::ReadExisting(path, max_bytes, follow_path);
    if (!capture) return std::unexpected(capture.error());
    state->bytes = std::move(capture->bytes);
    state->anchor = std::move(capture->anchor);
    return JournalReadHandle(std::move(state));
}
JournalAppendReceipt FileJournalAdapter::AppendLine(std::string_view line, Durability durability) noexcept {
    return writer_.AppendLineDetailed(line, durability);
}
JournalCloseReceipt FileJournalAdapter::Close() noexcept { return writer_.CloseDetailed(); }

struct JournalOwner::State {
    explicit State(FileJournalAdapter value) : file(std::move(value)) {}
    mutable std::mutex mutex;
    FileJournalAdapter file;
    bool closed = false;
    std::optional<JournalOwnerUnconfirmed> first_unconfirmed;
    std::optional<JournalCloseReceipt> first_close;
};
JournalOwner::JournalOwner() = default;
JournalOwner::JournalOwner(FileJournalAdapter file) : state_(std::make_unique<State>(std::move(file))) {}
JournalOwner::JournalOwner(JournalOwner&&) noexcept = default;
JournalOwner& JournalOwner::operator=(JournalOwner&&) noexcept = default;
JournalOwner::~JournalOwner() = default;
std::expected<JournalOwner, std::string> JournalOwner::CreateNew(
    const std::filesystem::path& path, std::shared_ptr<JournalNativeIoProbe> probe) {
    JournalOwner owner{FileJournalAdapter{}}; // Reserve state before native Open.
    auto file = FileJournalAdapter::CreateNew(path, std::move(probe));
    if (!file) return std::unexpected(file.error());
    owner.state_->file = std::move(*file);
    return owner;
}
std::expected<JournalOwner, std::string> JournalOwner::ContinueFile(const std::filesystem::path& path) {
    JournalOwner owner{FileJournalAdapter{}};
    auto file = FileJournalAdapter::ContinueFile(path);
    if (!file) return std::unexpected(file.error());
    owner.state_->file = std::move(*file);
    return owner;
}
std::expected<JournalOwner, std::string> JournalOwner::ContinueOwnedPrefix(
    const std::filesystem::path& path, std::string_view prefix, const JournalFileAnchor& anchor) {
    JournalOwner owner{FileJournalAdapter{}};
    auto file = FileJournalAdapter::ContinueOwnedPrefix(path, prefix, anchor);
    if (!file) return std::unexpected(file.error());
    owner.state_->file = std::move(*file);
    return owner;
}
std::expected<JournalOwner, std::string> JournalOwner::ContinueCaptured(const JournalReadHandle& capture) {
    const auto anchor = capture.native_anchor();
    if (!capture || !anchor) return std::unexpected("recovery.anchor_missing");
    return ContinueOwnedPrefix(capture.path(), capture.bytes(), *anchor);
}
std::expected<JournalReadHandle, std::string> JournalOwner::CaptureExisting(
    const std::filesystem::path& path, std::optional<std::size_t> max_bytes, bool follow_path) {
    return FileJournalAdapter::CaptureExisting(path, max_bytes, follow_path);
}
JournalOwner::AppendLease::AppendLease(State* state, std::unique_lock<std::mutex> lock,
    JournalOwnerUnconfirmed receipt) : state_(state), lock_(std::move(lock)), receipt_(std::move(receipt)) {}
JournalOwner::AppendLease::AppendLease(AppendLease&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)), lock_(std::move(other.lock_)),
      receipt_(std::move(other.receipt_)), complete_(other.complete_) {}
JournalOwner::AppendLease& JournalOwner::AppendLease::operator=(AppendLease&& other) noexcept {
    if (this != &other) {
        Retire();
        state_ = std::exchange(other.state_, nullptr);
        lock_ = std::move(other.lock_);
        receipt_ = std::move(other.receipt_);
        complete_ = other.complete_;
    }
    return *this;
}
JournalOwner::AppendLease::~AppendLease() { Retire(); }
void JournalOwner::AppendLease::UnconfirmSemantic() noexcept {
    if (!state_ || complete_ || receipt_.native.status != JournalAppendStatus::Committed) return;
    receipt_.phase = JournalOwnerUnconfirmed::Phase::SemanticCompletion;
    if (!state_->first_unconfirmed) state_->first_unconfirmed.emplace(std::move(receipt_));
    complete_ = true;
}
void JournalOwner::AppendLease::Complete() noexcept { complete_ = true; }
void JournalOwner::AppendLease::Retire() noexcept {
    if (state_ && !complete_) UnconfirmSemantic();
    state_ = nullptr;
    if (lock_.owns_lock()) lock_.unlock();
}
JournalOwner::AppendLease JournalOwner::AppendLine(
    std::string_view line, Durability durability, JournalAppendIdentity identity) {
    if (!state_) {
        JournalOwnerUnconfirmed receipt;
        receipt.identity = std::move(identity);
        receipt.native.requested_durability = durability;
        receipt.native.rejection = JournalBeforeIoReason::Closed;
        return AppendLease(nullptr, {}, std::move(receipt));
    }
    std::unique_lock lock(state_->mutex);
    JournalOwnerUnconfirmed receipt;
    identity.canonical_bytes = line.size(); // Actual request length, not a caller claim.
    receipt.identity = std::move(identity);
    if (state_->closed || state_->first_unconfirmed) {
        receipt.native.requested_durability = durability;
        receipt.native.rejection = state_->first_unconfirmed ? JournalBeforeIoReason::Broken : JournalBeforeIoReason::Closed;
    } else {
        receipt.native = state_->file.AppendLine(line, durability);
        if (receipt.native.status == JournalAppendStatus::Unconfirmed && !state_->first_unconfirmed)
            state_->first_unconfirmed.emplace(std::move(receipt));
        // Read the frozen fixed native receipt, without copying request strings.
        if (state_->first_unconfirmed) receipt.native = state_->first_unconfirmed->native;
    }
    return AppendLease(state_.get(), std::move(lock), std::move(receipt));
}
JournalCloseReceipt JournalOwner::CloseDetailed() noexcept {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    if (!state_->first_close) {
        state_->closed = true;
        state_->first_close = state_->file.Close();
    }
    return *state_->first_close;
}
const std::filesystem::path& JournalOwner::path() const {
    static const std::filesystem::path empty;
    return state_ ? state_->file.path() : empty;
}
std::optional<JournalOwnerUnconfirmed> JournalOwner::first_unconfirmed() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->first_unconfirmed;
}
bool JournalOwner::semantic_unconfirmed() const noexcept {
    if (!state_) return false;
    std::lock_guard lock(state_->mutex);
    return state_->first_unconfirmed &&
        state_->first_unconfirmed->phase == JournalOwnerUnconfirmed::Phase::SemanticCompletion;
}
std::optional<JournalAppendReceipt> JournalOwner::first_unconfirmed_append() const noexcept {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->file.first_native_unconfirmed();
}
std::expected<JournalReadHandle, std::string> JournalOwner::Capture(std::optional<std::size_t> max_bytes) const {
    if (!state_) return std::unexpected("recovery.anchor_missing");
    std::lock_guard lock(state_->mutex);
    return CaptureExisting(state_->file.path(), max_bytes);
}

} // namespace lubancode::trajectory
