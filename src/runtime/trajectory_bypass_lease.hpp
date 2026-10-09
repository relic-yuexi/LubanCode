// Private preparation only: requires contract-first adoption and real Journal tests.
#pragma once

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "runtime/trajectory_bypass_bridge.hpp"

namespace lubancode::runtime {

// One scene owns admission; each proxy owns exactly one mutable sampling bridge.
// Only recorder callbacks borrow the scene. No lock spans a Backend invocation.
class TrajectoryBypassLeaseOwner {
    struct Slot {
        explicit Slot(std::unique_ptr<TrajectoryBypassBridge> value) : recorder(std::move(value)) {}
        std::unique_ptr<TrajectoryBypassBridge> recorder;
    };
    struct State {
        std::mutex mutex;
        bool retired = false;
        std::vector<std::weak_ptr<Slot>> slots;
    };
    class Proxy final : public agent::LoopBoundaryRecorder {
    public:
        Proxy(std::shared_ptr<State> state, std::shared_ptr<Slot> slot)
            : state_(std::move(state)), slot_(std::move(slot)) {}

        std::string OnRequestPrepared(const api::Request& request,
                                      const agent::RequestPreparedContext& context) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return {};
            return slot_->recorder->OnRequestPrepared(request, context);
        }
        bool OnRequestSent(const std::string& request_id) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return false;
            return slot_->recorder->OnRequestSent(request_id);
        }
        void OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                             bool reported_by_provider, const std::string& provider_response_id,
                             int cache_epoch = 0, bool prefix_append_only = true,
                             bool cache_read_reported_by_provider = false,
                             bool cache_creation_reported_by_provider = false,
                             const std::string& usage_anomaly = std::string()) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return;
            slot_->recorder->OnUsageRecorded(request_id, usage, reported_by_provider,
                provider_response_id, cache_epoch, prefix_append_only,
                cache_read_reported_by_provider, cache_creation_reported_by_provider, usage_anomaly);
        }
        bool OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                               const std::string& stop_reason,
                               const std::string& provider_response_id) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return false;
            return slot_->recorder->OnOutputCompleted(request_id, assistant, stop_reason,
                                                     provider_response_id);
        }
        void OnOutputFailed(const std::string& request_id, const std::string& reason) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return;
            slot_->recorder->OnOutputFailed(request_id, reason);
        }
        void OnOutputCancelled(const std::string& request_id, agent::OutputCancelSource source) override {
            std::lock_guard lock(state_->mutex);
            if (state_->retired || !slot_->recorder) return;
            slot_->recorder->OnOutputCancelled(request_id, source);
        }
    private:
        std::shared_ptr<State> state_;
        std::shared_ptr<Slot> slot_;
    };
public:
    enum class BindError { MissingRecorder, Retired };
    TrajectoryBypassLeaseOwner() : state_(std::make_shared<State>()) {}
    ~TrajectoryBypassLeaseOwner() { Retire(); }
    TrajectoryBypassLeaseOwner(const TrajectoryBypassLeaseOwner&) = delete;
    TrajectoryBypassLeaseOwner& operator=(const TrajectoryBypassLeaseOwner&) = delete;
    TrajectoryBypassLeaseOwner(TrajectoryBypassLeaseOwner&&) = delete;
    TrajectoryBypassLeaseOwner& operator=(TrajectoryBypassLeaseOwner&&) = delete;

    // Connected but unavailable is a non-null rejection recorder. Passing
    // nullptr to SampleModel would authorize an unrecorded model request.
    static std::unique_ptr<agent::LoopBoundaryRecorder> Rejected() {
        auto state = std::make_shared<State>();
        state->retired = true;
        auto slot = std::make_shared<Slot>(nullptr);
        return std::make_unique<Proxy>(std::move(state), std::move(slot));
    }

    // A missing/revoked connected recorder is an explicit rejection, never
    // permission to pass nullptr into SampleModel and send without recording.
    using BridgeFactory = std::function<std::unique_ptr<TrajectoryBypassBridge>()>;
    std::expected<std::unique_ptr<agent::LoopBoundaryRecorder>, BindError>
    Bind(const BridgeFactory& make_recorder) {
        if (!make_recorder) return std::unexpected(BindError::MissingRecorder);
        std::lock_guard lock(state_->mutex);
        if (state_->retired) return std::unexpected(BindError::Retired);
        // The private factory only freezes/constructs a real bridge. It must not
        // send a model request, start a thread, or re-enter this scene owner.
        // Gate raw ledger access before the factory, not after it returns.
        auto recorder = make_recorder();
        if (!recorder) return std::unexpected(BindError::MissingRecorder);
        auto slot = std::make_shared<Slot>(std::move(recorder));
        auto proxy = std::make_unique<Proxy>(state_, slot);
        std::erase_if(state_->slots, [](const auto& old) { return old.expired(); });
        state_->slots.emplace_back(slot);
        return std::unique_ptr<agent::LoopBoundaryRecorder>(std::move(proxy));
    }

    // Idempotent barrier: callbacks finish before their real bridge is destroyed.
    // A proxy may outlive this owner; it then rejects/no-ops through owned State.
    // The trusted TrajectoryBypassBridge destructor does not call the old ledger.
    void Retire() {
        std::lock_guard lock(state_->mutex);
        state_->retired = true;
        for (const auto& weak : state_->slots) {
            if (auto slot = weak.lock(); slot && slot->recorder) {
                slot->recorder->RetireRecording();
                slot->recorder.reset();
            }
        }
        state_->slots.clear();
    }
private:
    std::shared_ptr<State> state_;
};

}  // namespace lubancode::runtime
