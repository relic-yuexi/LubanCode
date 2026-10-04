#include <lubancore/events.hpp>
#include <lubancore/core.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lubancore_consumer {
namespace {
namespace sdk = lubancore;
namespace ev = sdk::events::v1;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(sdk::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(*value);
}
void Take(sdk::Result<void> value) { if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message); }
std::string Utf8(const fs::path& path) {
    const auto value = path.u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct Counts { std::atomic<unsigned> factory{0}, push{0}, close{0}, destroyed{0}, sink_destroyed{0}, models{0}; };
class Queue final : public ev::EventQueue {
public:
    Queue(std::shared_ptr<Counts> counts, std::size_t capacity) : counts_(std::move(counts)), capacity_(capacity) {}
    ~Queue() override { ++counts_->destroyed; }
    sdk::Result<void> Push(sdk::Event value) override {
        std::lock_guard lock(mutex_); ++counts_->push; if (closed_) return {};
        const auto cost = value.text.size() + value.payload_json.size() + (value.approval ? value.approval->input_json.size() : 0);
        if (queue_.size() >= capacity_ || cost > 16u * 1024u * 1024u || bytes_ > 16u * 1024u * 1024u - cost) {
            closed_ = true; queue_.clear(); bytes_ = 0; cv_.notify_all();
            return std::unexpected(sdk::Error{"consumer.overflow", {}});
        }
        bytes_ += cost; queue_.push_back(std::move(value)); cv_.notify_all(); return {};
    }
    sdk::Result<std::optional<sdk::Event>> Next(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(mutex_); cv_.wait_for(lock, timeout, [&] { return closed_ || !queue_.empty(); });
        if (closed_) return std::unexpected(sdk::Error{"consumer.closed", {}});
        if (queue_.empty()) return std::optional<sdk::Event>{};
        auto value = std::move(queue_.front()); queue_.pop_front();
        bytes_ -= value.text.size() + value.payload_json.size() + (value.approval ? value.approval->input_json.size() : 0);
        return std::optional<sdk::Event>{std::move(value)};
    }
    sdk::Result<void> Close() override {
        std::lock_guard lock(mutex_); ++counts_->close; closed_ = true; queue_.clear(); bytes_ = 0; cv_.notify_all(); return {};
    }
private:
    std::shared_ptr<Counts> counts_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<sdk::Event> queue_;
    std::size_t capacity_, bytes_ = 0;
    bool closed_ = false;
};
class Sink final : public ev::EventSink {
public:
    explicit Sink(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    ~Sink() override { ++counts_->sink_destroyed; }
    sdk::Result<std::unique_ptr<ev::EventQueue>> CreateQueue(std::size_t capacity) override {
        ++counts_->factory; return std::unique_ptr<ev::EventQueue>(new Queue(counts_, capacity));
    }
private:
    std::shared_ptr<Counts> counts_;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++counts_->models; return sdk::ModelReply{"installed-events", {}, sdk::Usage{3, 4}};
    }
private:
    std::shared_ptr<Counts> counts_;
};
}
void EventSink(const fs::path& base) {
    fs::create_directories(base / "event-project"); fs::create_directories(base / "event-resources");
    auto runtime = Take(sdk::Runtime::Create({Utf8(base / "event-data"), Utf8(base / "event-resources")}));
    auto counts = std::make_shared<Counts>(); sdk::SessionOptions options;
    options.cwd = Utf8(fs::canonical(base / "event-project")); options.model = "fixture"; options.system_prompt = "Installed events";
    options.backend = std::make_unique<Backend>(counts); options.event_sink = std::make_unique<Sink>(counts);
    auto session = Take(runtime->OpenSession(std::move(options))); auto stream = Take(session->Subscribe());
    const auto receipt = Take(session->Submit("event-sink-key", "question")); bool completed = false, turn_completed = false, text = false; unsigned delivered = 0;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!(completed && turn_completed) && delivered < 512 && std::chrono::steady_clock::now() < deadline) {
        auto value = Take(stream->Next(1s)); if (!value) continue;
        ++delivered; Check(value->session_id == session->id(), "event session owner mismatch");
        Check(value->operation_id == receipt.operation_id, "event operation owner mismatch");
        text = text || value->text == "installed-events";
        const bool completion = value->kind == "operation_completed";
        completed = completed || completion;
        turn_completed = turn_completed || value->kind == "turn.completed";
        if (completion) Check(value->text == "installed-events", "completion text changed");
    }
    Check(completed && turn_completed && text && delivered > 1, "actual event flow missing");
    const auto operation = Take(session->WaitResult(receipt.operation_id, 20s));
    Check(operation.state == sdk::OperationState::Succeeded && operation.result_persisted, "operation did not persist");
    Take(stream->CloseChecked()); Take(stream->CloseChecked()); Take(session->Close()); Take(runtime->Shutdown());
    Check(counts->factory == 1 && counts->close == 1 && counts->destroyed == 1 && counts->sink_destroyed == 1 && counts->models == 1,
        "owned event queue did not retire exactly once");
    Check(counts->push == delivered, "actual queue flow count changed");
    const auto closed = stream->Next(0ms); Check(!closed && closed.error().code == "sdk.events.closed", "closed stream cache changed");
    std::cout << "[sdk-event-sink-consumer] actual-owned-queue\n";
}
} // namespace lubancore_consumer
