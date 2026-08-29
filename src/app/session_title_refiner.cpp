// SessionTitleRefiner 的实现(实测问题 7)。头注释是行为账:异步、单飞、
// 主线程收货、取消与退出的边界全在那边。

#include "app/session_title_refiner.hpp"

#include <chrono>
#include <utility>

#include "app/session_title.hpp"  // RefineSessionTitle/kTitleRefineTimeoutSecs

namespace lubancode::app {
namespace {
// 退出兜底的有界等待窗:看门狗 5 秒 + 收尾余量。挂死绝境(cpr 卡死那类)
// 到点 detach 放行,不冻退出——与 AgentTool 析构同一副方子。
constexpr auto kShutdownGrace = std::chrono::seconds(7);
}  // namespace

SessionTitleRefiner::~SessionTitleRefiner() {
    if (shared_ != nullptr) {
        shared_->cancel.store(true);
    }
    if (!worker_.joinable()) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + kShutdownGrace;
    while (std::chrono::steady_clock::now() < deadline && !shared_->done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (shared_->done.load()) {
        worker_.join();
    } else {
        worker_.detach();  // 挂死绝境:放线程走,闭包自持 shared 状态不悬垂
    }
}

bool SessionTitleRefiner::Start(Inputs&& inputs) {
    if (inputs.backend == nullptr || inputs.model.empty() || Busy()) {
        return false;
    }
    auto shared = std::make_shared<Shared>();
    shared->generation = inputs.generation;
    // 闭包只持值与 shared 槽:不引用本对象,detach 晚归也不悬垂。
    worker_ = std::thread(
        [shared, backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), first_query = std::move(inputs.first_query)]() mutable {
            Outcome outcome;
            outcome.model = model;
            outcome.generation = shared->generation;
            // 看门狗:到点拉取消旗。SampleModel 的口径是外部取消链优先、
            // 自带看门狗退位——超时必须自己管,5 秒是硬上限。
            std::atomic<bool> watch_done{false};
            std::thread watcher([&watch_done, &shared]() {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(kTitleRefineTimeoutSecs);
                while (!watch_done.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!watch_done.load()) {
                    shared->cancel.store(true);
                }
            });
            lubancode::agent::BackgroundCallAccounting accounting;
            const auto title = RefineSessionTitle(*backend, model, effort, first_query,
                                                  /*timeout_secs=*/0, &shared->cancel, &accounting);
            watch_done.store(true);
            watcher.join();
            // 失败半截也出账(旧口径:先记账再判错)。
            outcome.accounting = std::move(accounting);
            if (title.has_value() && !title->empty()) {
                outcome.ok = true;
                outcome.title = *title;
            }
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->outcome = std::move(outcome);
            }
            shared->done.store(true);  // outcome 写完才立收讫旗,主线程收货不抢跑
        });
    shared_ = std::move(shared);
    return true;
}

std::optional<SessionTitleRefiner::Outcome> SessionTitleRefiner::TakeFinished() {
    if (shared_ == nullptr || !shared_->done.load()) {
        return std::nullopt;
    }
    if (worker_.joinable()) {
        worker_.join();  // done 已立:线程已退场或正要退,join 立即回
    }
    std::optional<Outcome> out;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        out = std::move(shared_->outcome);
    }
    shared_.reset();
    return out;
}

void SessionTitleRefiner::RequestCancel() {
    if (shared_ != nullptr) {
        shared_->cancel.store(true);
    }
}

bool SessionTitleRefiner::Busy() const {
    return shared_ != nullptr;
}

}  // namespace lubancode::app
