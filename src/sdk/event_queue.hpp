#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>

#include "lubancore/events.hpp"

namespace lubancore::detail {

// Actual provider invocation, including capture destruction. Never caller DTOs.
bool InEventProvider() noexcept;
class EventQueueState;
class EventDeliveryOwner : public std::enable_shared_from_this<EventDeliveryOwner> {
public:
    explicit EventDeliveryOwner(std::unique_ptr<events::v1::EventSink> sink);
    Result<std::shared_ptr<EventQueueState>> CreateQueue(std::size_t capacity);
    void BeginClose();
    Result<void> CloseAndWait();
private:
    friend class EventQueueState;
    void RememberClose(const Error& error);
    void QueueRetired();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<events::v1::EventSink> sink_;
    std::size_t factories_ = 0, queues_ = 0;
    bool closing_ = false;
    std::optional<Error> close_error_;
};

// No borrowed Session/Writer/backend. Once closed, only owned cached errors remain.
class EventQueueState {
public:
    EventQueueState(std::shared_ptr<EventDeliveryOwner> owner,
        std::unique_ptr<events::v1::EventQueue> queue, bool external);
    ~EventQueueState();
    void Push(const Event& event);
    Result<std::optional<Event>> Next(std::chrono::milliseconds timeout);
    Result<void> CloseChecked();
private:
    Result<void> Retire();
    std::shared_ptr<EventDeliveryOwner> owner_;
    std::unique_ptr<events::v1::EventQueue> queue_;
    bool external_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t calls_ = 0;
    bool closed_ = false, close_inflight_ = false, close_done_ = false;
    std::optional<Error> delivery_error_, close_error_;
};

} // namespace lubancore::detail
