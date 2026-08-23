// IdleWakeCoordinator(loop 单第 1 期)实现。

#include "runtime/idle_wake.hpp"

namespace lubancode::runtime {

struct IdleWakeCoordinator::Subscription::Impl {
    IdleWakeCoordinator* owner = nullptr;
    std::string name;
};

IdleWakeCoordinator::Subscription::Subscription() = default;

IdleWakeCoordinator::Subscription::~Subscription() { reset(); }

IdleWakeCoordinator::Subscription::Subscription(Subscription&& other) noexcept
    : impl_(std::move(other.impl_)) {
    other.impl_.reset();
}

IdleWakeCoordinator::Subscription& IdleWakeCoordinator::Subscription::operator=(
    Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        impl_ = std::move(other.impl_);
        other.impl_.reset();
    }
    return *this;
}

void IdleWakeCoordinator::Subscription::reset() {
    if (impl_ != nullptr) {
        impl_->owner->RemoveByName(impl_->name, impl_.get());
        impl_.reset();
    }
}

IdleWakeCoordinator::Subscription IdleWakeCoordinator::AddSource(std::string name,
                                                                 std::function<bool()> ready) {
    Subscription sub;
    sub.impl_ = std::make_unique<Subscription::Impl>();
    sub.impl_->owner = this;
    sub.impl_->name = name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sources_.push_back(Source{std::move(name), std::move(ready), sub.impl_.get()});
    }
    return sub;
}

bool IdleWakeCoordinator::AnyReady() const {
    std::vector<std::function<bool()>> ready_calls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_calls.reserve(sources_.size());
        for (const Source& s : sources_) {
            if (s.ready) {
                ready_calls.push_back(s.ready);
            }
        }
    }
    // 锁外调 ready:源实现可能自己拿锁(子代理台账),锁内回调会死锁。
    for (const auto& ready : ready_calls) {
        if (ready()) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> IdleWakeCoordinator::SourceNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(sources_.size());
    for (const Source& s : sources_) {
        names.push_back(s.name);
    }
    return names;
}

void IdleWakeCoordinator::RemoveByName(const std::string& name, Subscription::Impl* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        if (it->name == name && it->token == token) {
            sources_.erase(it);
            return;
        }
    }
}

}  // namespace lubancode::runtime
