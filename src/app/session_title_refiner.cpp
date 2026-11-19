// SessionTitleRefiner 的实现(实测问题 7)。头注释是行为账:异步、单飞、
// 主线程收货、取消与退出的边界全在那边。

#include "app/session_title_refiner.hpp"

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
#include "app/bypass_worker_test_hooks.hpp"
#include "app/session_title_refiner_test_hooks.hpp"
#endif

#include "app/session_title.hpp"  // RefineSessionTitle
#include "runtime/trajectory_session.hpp"  // TrajectoryBypassBridge(Token 账本单 A1)

namespace lubancode::app {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
namespace testing {
namespace { thread_local std::function<void()> title_start_hook; thread_local TitleWorkerHook title_execution_hook; }
std::function<void()> ExchangeTitleWorkerStartHook(std::function<void()> hook) {
    return std::exchange(title_start_hook,std::move(hook));
}
TitleWorkerHook ExchangeTitleWorkerExecutionHook(TitleWorkerHook hook) {
    return std::exchange(title_execution_hook,std::move(hook));
}
}
#endif
namespace {
// 退出时先取消,只把结果等待圈在七秒内;结果未归则 detach。
// 结果已发布后 join 另等线程退出与闭包析构,这段没有统一时限。看门狗
// 放宽到 30 秒后这窗不再覆盖整段预算:超窗的悬账被弃——本地标题已保住,
// usage 丢一笔可接受(2026-09-22 超时放宽单认可)。
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
    Outcome start_failure;
    start_failure.model = inputs.model;
    start_failure.generation = inputs.generation;
    start_failure.error = "标题精炼线程未能启动";
    shared->outcome = std::move(start_failure);
    Outcome binding_failure;
    binding_failure.model = inputs.model;
    binding_failure.generation = inputs.generation;
    binding_failure.error = "标题精炼旁路绑定失败";
    Outcome outcome; outcome.model = inputs.model; outcome.generation = inputs.generation;
    std::string execution_failure = "标题精炼后台执行失败";
    static_assert(std::is_nothrow_move_assignable_v<std::string>);
    static_assert(std::is_nothrow_move_assignable_v<std::optional<Outcome>>);
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
    auto bypass_hook = testing::SnapshotBypassWorkerHook();
    auto execution_hook = testing::title_execution_hook;
#endif
    std::unique_ptr<agent::LoopBoundaryRecorder> bypass;
    try {
        if (inputs.trajectory != nullptr) {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
            if (bypass_hook) bypass_hook(testing::BypassWorkerPurpose::Title,testing::BypassWorkerPhase::BeforeBinding);
#endif
            runtime::TrajectoryTurnBridge::Identity identity{inputs.provider, inputs.trajectory_wire, "host"};
            bypass = inputs.trajectory->NewLeasedBypassRecorder(std::move(identity), accounting::RequestPurpose::TitleRefine);
        }
    } catch (const std::exception&) {
        shared->outcome = std::move(binding_failure);
        inputs.backend.reset();
        shared->done.store(true);
        shared_ = std::move(shared);
        return true;
    } catch (...) {
        shared->outcome = std::move(binding_failure);
        inputs.backend.reset();
        shared->done.store(true);
        shared_ = std::move(shared);
        return true;
    }
    // Proxy/value captures own the callback gate. The ledger retires its real bridge before teardown.
    auto run =
        [shared, outcome = std::move(outcome), execution_failure = std::move(execution_failure),
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
         bypass_hook = std::move(bypass_hook), execution_hook = std::move(execution_hook),
#endif
         backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), first_query = std::move(inputs.first_query),
         bypass = std::move(bypass), timeout_secs = inputs.timeout_secs]() mutable noexcept {
            auto fail = [&]() noexcept {
                outcome.ok = false;
                outcome.title.clear();
                outcome.error = std::move(execution_failure);
                // Preserve only identity, invocation and accounting facts already returned.
            };
            try {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (execution_hook) execution_hook(testing::TitleWorkerPhase::BeforeWork);
                if (bypass_hook) bypass_hook(testing::BypassWorkerPurpose::Title, testing::BypassWorkerPhase::BeforeSampling);
#endif
                outcome.refinement_invoked = true;
                const auto title = RefineSessionTitle(*backend, model, effort, first_query,
                    timeout_secs, &shared->cancel, &outcome.accounting, bypass.get());
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (execution_hook) execution_hook(testing::TitleWorkerPhase::AfterRefinement);
#endif
                if (title.has_value() && !title->empty()) {
                    outcome.ok = true;
                    outcome.title = *title;
                } else if (!title.has_value()) {
                    outcome.error = title.error();
                }
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (execution_hook) execution_hook(testing::TitleWorkerPhase::BeforePublish);
#endif
            } catch (const std::exception&) {
                fail();
            } catch (...) {
                fail();
            }
            // One producer; readers acquire done before touching the slot.
            // The optional move is statically checked not to throw.
            shared->outcome = std::move(outcome);
            shared->done.store(true);
        };
    try {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
        if (testing::title_start_hook) testing::title_start_hook();
#endif
        worker_ = std::thread(std::move(run));
    } catch (const std::exception&) {
        shared->done.store(true);
    } catch (...) {
        shared->done.store(true);
    }
    shared_ = std::move(shared);
    return true;
}

std::optional<SessionTitleRefiner::Outcome> SessionTitleRefiner::TakeFinished() {
    if (shared_ == nullptr || !shared_->done.load()) {
        return std::nullopt;
    }
    if (worker_.joinable()) {
        worker_.join();  // done 表示结果已发布;这里还等线程退出与闭包析构
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

bool SessionTitleRefiner::Ready() const {
    // done 是 outcome 写完才立的收讫旗(见 Start 尾段):这里只读它,不
    // join、不锁、不动槽——ReadLine 的 100ms 拍在主线程问,零副作用。
    return shared_ != nullptr && shared_->done.load();
}

bool RecordTitleRefinementCall(agent::ModelUsageLedger& ledger,
                               const SessionTitleRefiner::Outcome& outcome) {
    if (!outcome.refinement_invoked) return false;
    ledger.Record(agent::ModelRole::Cheap,outcome.model,outcome.accounting.usage,
                  outcome.accounting.duration_ms,outcome.accounting.usage_reported);
    return true;
}
}  // namespace lubancode::app
