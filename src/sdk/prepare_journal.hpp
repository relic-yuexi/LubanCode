#pragma once

#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "platform/sha256.hpp"
#include "trajectory/journal_owner.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore::detail {

// One host-serialized SDK opening, not a writer lease or an authorization token.
// Keep each module's validation order: constructing this value performs no I/O.
class SessionPrepareJournal final {
public:
    using LedgerView = std::shared_ptr<const lubancode::trajectory::v3::V3Ledger>;
    using ReadResult = std::expected<LedgerView, std::string>;
    SessionPrepareJournal(std::string workspace_key, std::string session_id)
        : workspace_key_(std::move(workspace_key)), session_id_(std::move(session_id)) {}

    ReadResult Read(const std::filesystem::path& stream) {
        if (stream_ && *stream_ != stream) return std::unexpected("recovery.prepare_source_changed");
        if (exception_) std::rethrow_exception(exception_);
        if (ledger_) return *ledger_;
        stream_ = stream;
        try {
            ledger_ = Capture(stream);
            return *ledger_;
        } catch (...) {
            exception_ = std::current_exception();
            throw;
        }
    }

    std::optional<lubancode::trajectory::RecoveryMainExpectation> expectation() const {
        return exception_ ? std::nullopt : expectation_;
    }
    // Observes calls to the actual File capture, not module requests or a fake
    // factory. Native tests still verify the real source and locked handoff.
    std::size_t capture_attempts() const noexcept { return capture_attempts_; }

private:
    ReadResult Capture(const std::filesystem::path& stream) {
        if (workspace_key_.empty() || session_id_.empty())
            return std::unexpected("recovery.prepare_scope_invalid");
        ++capture_attempts_;
        auto captured = lubancode::trajectory::JournalOwner::CaptureExisting(stream, std::nullopt);
        if (!captured) return std::unexpected(captured.error());
        try {
            auto ledger = lubancode::trajectory::v3::ReadV3LedgerCaptured(*captured);
            if (!ledger) {
                try { (void)captured->Close(); } catch (...) {}
                return std::unexpected(std::move(ledger.error()));
            }
            const auto closed = captured->Close();
            if (!closed) return std::unexpected(closed.error());
            // Scope/binding refusal belongs to each original module. In
            // particular Action's soft probe must inspect saved bindings before
            // deciding whether a foreign/malformed scene makes it strict.
            auto owned = std::make_shared<const lubancode::trajectory::v3::V3Ledger>(std::move(*ledger));
            expectation_ = lubancode::trajectory::RecoveryMainExpectation{
                workspace_key_, session_id_, "main", captured->bytes().size(),
                lubancode::platform::Sha256Hex(captured->bytes())};
            return owned;
        } catch (...) {
            try { (void)captured->Close(); } catch (...) {}
            throw;
        }
    }

    std::string workspace_key_, session_id_;
    std::optional<std::filesystem::path> stream_;
    std::optional<ReadResult> ledger_;
    std::optional<lubancode::trajectory::RecoveryMainExpectation> expectation_;
    std::exception_ptr exception_;
    std::size_t capture_attempts_ = 0;
};

inline SessionPrepareJournal::ReadResult ReadPrepareJournal(
    const std::filesystem::path& stream, SessionPrepareJournal* shared) {
    if (shared) return shared->Read(stream);
    // Optional direct module callers retain the original File read semantics.
    auto ledger = lubancode::trajectory::v3::ReadV3Ledger(stream);
    if (!ledger) return std::unexpected(std::move(ledger.error()));
    return std::make_shared<const lubancode::trajectory::v3::V3Ledger>(std::move(*ledger));
}

} // namespace lubancore::detail
