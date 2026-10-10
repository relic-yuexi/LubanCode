#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <utility>

#include "api/types.hpp"
#include "api/usage_event_projection.hpp"

namespace lubancode::api::usage_wire {

// Request-local numeric ownership, independent of material/event allocations.
// This is a callback-delivery checkpoint, never a durable or typed owner ACK.
class NumericDeliveryOwner {
public:
    void Own(const Snapshot& source) noexcept {
        numbers_ = Numbers(source);
        pending_ = true;
    }
    void Delivered(const StreamEvent& event) noexcept {
        if (std::holds_alternative<UsageSnapshot>(event)) pending_ = false;
        else if (const auto* done = std::get_if<MessageDone>(&event);
                 done && (done->usage_reported || done->usage_observation)) pending_ = false;
    }
    std::optional<UsageSnapshot> Pending() const noexcept {
        if (!pending_) return std::nullopt;
        UsageSnapshot result;
        result.usage = numbers_;
        result.usage_reported = true;
        // No copied material or invented identity. Every precise field remains
        // unknown until its actual observation reaches the sampling owner.
        return result;
    }
private:
    Usage numbers_;
    bool pending_ = false;
};

// Declared after the parser: on exception, the transport has already unwound
// (including its C callback), while parser and caller callback remain alive.
template <typename Parser>
class PendingUsageOnUnwind {
public:
    PendingUsageOnUnwind(Parser& parser, const std::function<void(const StreamEvent&)>& callback) noexcept
        : parser_(parser), callback_(callback), exceptions_(std::uncaught_exceptions()) {}
    PendingUsageOnUnwind(const PendingUsageOnUnwind&) = delete;
    PendingUsageOnUnwind& operator=(const PendingUsageOnUnwind&) = delete;
    ~PendingUsageOnUnwind() noexcept {
        if (callback_failed_ || std::uncaught_exceptions() <= exceptions_) return;
        try {
            if (auto pending = parser_.PendingUsage()) callback_(std::move(*pending));
        } catch (...) {} // A secondary callback fault must not replace the original exception.
    }
    void Emit(const StreamEvent& event) {
        try { callback_(event); }
        catch (...) { callback_failed_ = true; throw; }
        parser_.UsageDelivered(event);
    }
private:
    Parser& parser_;
    const std::function<void(const StreamEvent&)>& callback_;
    int exceptions_;
    bool callback_failed_ = false;
};

} // namespace lubancode::api::usage_wire
