#include "sdk/event_queue.hpp"

#include <deque>
#include <exception>
#include <utility>

namespace lubancore::detail {
namespace {
thread_local const EventDeliveryOwner* provider_owner = nullptr;
struct ProviderScope {
    const EventDeliveryOwner* previous;
    explicit ProviderScope(const EventDeliveryOwner* owner, bool external = true)
        : previous(provider_owner) { if (external) provider_owner = owner; }
    ~ProviderScope() { provider_owner = previous; }
};
Error Failure(const char* code) { return {code, {}}; }
Error ProviderFailure(const Error& error, bool close = false) {
    return {close ? "sdk.events.provider_close_failed" : "sdk.events.provider_failed",
        error.code + (error.message.empty() ? "" : ": " + error.message)};
}
Error Thrown(const std::exception& error, bool close = false) {
    return {close ? "sdk.events.provider_close_failed" : "sdk.events.provider_failed", error.what()};
}

// The previous EventStream queue algorithm, including delivery/Close separation.
class DefaultEventQueue final : public events::v1::EventQueue {
public:
    explicit DefaultEventQueue(std::size_t limit) : capacity(limit) {}
    Result<void> Push(Event event) override {
        std::lock_guard lock(mutex);
        if (closed) return {};
        const auto cost = event.text.size() + event.payload_json.size() +
            (event.approval ? event.approval->input_json.size() : 0);
        if (events.size() >= capacity || cost > 16 * 1024 * 1024 || bytes > 16 * 1024 * 1024 - cost) {
            error = "sdk.events.overflow";
            closed = true;
            events.clear();
            bytes = 0;
            cv.notify_all();
            return std::unexpected(Error{error, {}});
        } else { events.push_back(event); bytes += cost; }
        cv.notify_all();
        return {};
    }
    Result<std::optional<Event>> Next(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex);
        ++active_reads;
        cv.wait_for(lock, timeout, [&] { return closed || !events.empty(); });
        --active_reads;
        cv.notify_all();
        if (closed) return std::unexpected(Error{error, {}});
        if (events.empty()) return std::optional<Event>{};
        Event event = std::move(events.front());
        events.pop_front();
        bytes -= event.text.size() + event.payload_json.size() + (event.approval ? event.approval->input_json.size() : 0);
        return std::optional<Event>{std::move(event)};
    }
    Result<void> Close() override {
        std::unique_lock lock(mutex);
        closed = true;
        cv.notify_all();
        cv.wait(lock, [&] { return active_reads == 0; });
        events.clear();
        bytes = 0;
        return {};
    }
private:
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Event> events;
    std::size_t capacity, active_reads = 0, bytes = 0;
    bool closed = false;
    std::string error = "sdk.events.closed";
};
}

bool InEventProvider() noexcept { return provider_owner != nullptr; }
EventDeliveryOwner::EventDeliveryOwner(std::unique_ptr<events::v1::EventSink> sink) : sink_(std::move(sink)) {}
void EventDeliveryOwner::RememberClose(const Error& error) {
    std::lock_guard lock(mutex_);
    if (!close_error_) close_error_ = error;
}
void EventDeliveryOwner::QueueRetired() {
    std::lock_guard lock(mutex_);
    --queues_;
    cv_.notify_all();
}
void EventDeliveryOwner::BeginClose() { std::lock_guard lock(mutex_); closing_ = true; }
Result<std::shared_ptr<EventQueueState>> EventDeliveryOwner::CreateQueue(std::size_t capacity) {
    {
        std::lock_guard lock(mutex_);
        if (closing_) return std::unexpected(Failure("sdk.session.closed"));
        ++factories_;
    }
    struct FactoryExit {
        EventDeliveryOwner& owner;
        ~FactoryExit() { std::lock_guard lock(owner.mutex_); --owner.factories_; owner.cv_.notify_all(); }
    } factory_exit{*this};
    const bool external = sink_ != nullptr; // sink is pinned until factory/queue retirement
    std::unique_ptr<events::v1::EventQueue> queue;
    try {
        if (external) {
            ProviderScope scope(this);
            auto result = sink_->CreateQueue(capacity);
            if (!result) return std::unexpected(ProviderFailure(result.error()));
            queue = std::move(*result);
        } else queue = std::make_unique<DefaultEventQueue>(capacity);
        if (!queue) return std::unexpected(Failure("sdk.events.provider_invalid"));
        auto state = std::make_shared<EventQueueState>(shared_from_this(), std::move(queue), external);
        bool late;
        { std::lock_guard lock(mutex_); late = closing_; }
        if (late) {
            (void)state->CloseChecked();
            return std::unexpected(Failure("sdk.session.closed"));
        }
        return state;
    } catch (const std::exception& error) {
        if (queue) {
            try { ProviderScope scope(this, external); auto closed = queue->Close();
                if (!closed) RememberClose(ProviderFailure(closed.error(), true)); }
            catch (...) { RememberClose(Failure("sdk.events.provider_close_failed")); }
            { ProviderScope scope(this, external); queue.reset(); }
        }
        return std::unexpected(Thrown(error));
    } catch (...) {
        if (queue) {
            try { ProviderScope scope(this, external); auto closed = queue->Close();
                if (!closed) RememberClose(ProviderFailure(closed.error(), true)); }
            catch (...) { RememberClose(Failure("sdk.events.provider_close_failed")); }
            { ProviderScope scope(this, external); queue.reset(); }
        }
        return std::unexpected(Failure("sdk.events.provider_failed"));
    }
}
Result<void> EventDeliveryOwner::CloseAndWait() {
    std::unique_ptr<events::v1::EventSink> sink;
    std::optional<Error> error;
    {
        std::unique_lock lock(mutex_);
        closing_ = true;
        cv_.wait(lock, [&] { return factories_ == 0 && queues_ == 0; });
        error = close_error_;
        sink = std::move(sink_);
    }
    { ProviderScope scope(this); sink.reset(); }
    return error ? Result<void>(std::unexpected(*error)) : Result<void>{};
}

EventQueueState::EventQueueState(std::shared_ptr<EventDeliveryOwner> owner,
    std::unique_ptr<events::v1::EventQueue> queue, bool external)
    : owner_(std::move(owner)), queue_(std::move(queue)), external_(external) {
    std::lock_guard lock(owner_->mutex_);
    ++owner_->queues_;
}
EventQueueState::~EventQueueState() { (void)Retire(); }
void EventQueueState::Push(const Event& event) {
    events::v1::EventQueue* queue;
    { std::lock_guard lock(mutex_); if (closed_) return; ++calls_; queue = queue_.get(); }
    Result<void> result;
    try { ProviderScope scope(owner_.get(), external_); result = queue->Push(event); }
    catch (const std::exception& error) { result = std::unexpected(Thrown(error)); }
    catch (...) { result = std::unexpected(Failure("sdk.events.provider_failed")); }
    {
        std::lock_guard lock(mutex_);
        --calls_;
        if (!result && !closed_ && !delivery_error_) { delivery_error_ = external_ ? ProviderFailure(result.error()) : result.error(); closed_ = true; }
        cv_.notify_all();
    }
    if (!result) (void)CloseChecked();
}
Result<std::optional<Event>> EventQueueState::Next(std::chrono::milliseconds timeout) {
    if (InEventProvider()) return std::unexpected(Failure("sdk.events.reentrant"));
    if (timeout.count() < 0) return std::unexpected(Failure("sdk.timeout.invalid"));
    events::v1::EventQueue* queue;
    { std::lock_guard lock(mutex_);
        if (closed_) return std::unexpected(delivery_error_.value_or(Failure("sdk.events.closed")));
        ++calls_; queue = queue_.get(); }
    Result<std::optional<Event>> result;
    try { ProviderScope scope(owner_.get(), external_); result = queue->Next(timeout); }
    catch (const std::exception& error) { result = std::unexpected(Thrown(error)); }
    catch (...) { result = std::unexpected(Failure("sdk.events.provider_failed")); }
    bool failed = !result;
    {
        std::lock_guard lock(mutex_);
        --calls_;
        if (failed && !closed_ && !delivery_error_) { delivery_error_ = external_ ? ProviderFailure(result.error()) : result.error(); closed_ = true; }
        if (closed_) result = std::unexpected(delivery_error_.value_or(Failure("sdk.events.closed")));
        cv_.notify_all();
    }
    if (failed) (void)CloseChecked();
    return result;
}
Result<void> EventQueueState::CloseChecked() {
    if (InEventProvider()) return std::unexpected(Failure("sdk.events.reentrant"));
    return Retire();
}
Result<void> EventQueueState::Retire() {
    events::v1::EventQueue* queue;
    {
        std::unique_lock lock(mutex_);
        if (close_inflight_) cv_.wait(lock, [&] { return close_done_; });
        if (close_done_) return close_error_ ? Result<void>(std::unexpected(*close_error_)) : Result<void>{};
        closed_ = true;
        close_inflight_ = true;
        queue = queue_.get();
    }
    Result<void> result;
    try { ProviderScope scope(owner_.get(), external_); result = queue->Close();
        if (!result && external_) result = std::unexpected(ProviderFailure(result.error(), true)); }
    catch (const std::exception& error) { result = std::unexpected(Thrown(error, true)); }
    catch (...) { result = std::unexpected(Failure("sdk.events.provider_close_failed")); }
    std::unique_ptr<events::v1::EventQueue> retired;
    {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return calls_ == 0; });
        if (!result) close_error_ = result.error();
        retired = std::move(queue_);
    }
    { ProviderScope scope(owner_.get(), external_); retired.reset(); }
    if (!result) owner_->RememberClose(result.error());
    owner_->QueueRetired();
    {
        std::lock_guard lock(mutex_);
        close_done_ = true;
        cv_.notify_all();
    }
    return result;
}
} // namespace lubancore::detail
