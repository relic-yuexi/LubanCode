// TurnMemoryExtractor 的实现(记忆回合总结异步化单)。头注释是行为账:
// 异步、单飞、主线程收货、取消与退出的边界全在那边。

#include "app/turn_memory_extractor.hpp"

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
#include "app/turn_memory_extractor_test_hooks.hpp"
#include "app/bypass_worker_test_hooks.hpp"
#endif

#include "accounting/purpose.hpp"  // RequestPurpose(旁路桥的 purpose 门)
#include "runtime/trajectory_session.hpp"  // TrajectoryBypassBridge(Token 账本单 A1)

namespace lubancode::app {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
namespace testing {
namespace {
thread_local std::function<void()> worker_start_hook;
thread_local MemoryWorkerExecutionHook worker_execution_hook;
thread_local BypassWorkerHook bypass_worker_hook;
}
std::function<void()> ExchangeMemoryWorkerStartHook(std::function<void()> hook) {
    return std::exchange(worker_start_hook, std::move(hook));
}
MemoryWorkerExecutionHook ExchangeMemoryWorkerExecutionHook(MemoryWorkerExecutionHook hook) {
    return std::exchange(worker_execution_hook, std::move(hook));
}
BypassWorkerHook ExchangeBypassWorkerHook(BypassWorkerHook hook) {
    return std::exchange(bypass_worker_hook, std::move(hook));
}
BypassWorkerHook SnapshotBypassWorkerHook() { return bypass_worker_hook; }
}  // namespace testing
#endif
namespace {
// 退出兜底的有界等待窗:取消旗已拉(cpr 的合并取消口应速断),等不起
// 看门狗的 45 秒全预算——到点 detach 放行,不冻退出。与
// SessionTitleRefiner 析构同一副方子(它那窗 7 秒,已与自己的 30 秒看门狗
// 预算脱钩;这里按"取消生效后应速断"配 5 秒)。
constexpr auto kShutdownGrace = std::chrono::seconds(5);
}  // namespace

TurnMemoryExtractor::~TurnMemoryExtractor() {
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

bool TurnMemoryExtractor::Start(Inputs&& inputs) {
    if (inputs.backend == nullptr || inputs.model.empty() || Busy()) {
        return false;
    }
    auto shared = std::make_shared<Shared>();
    shared->session_generation = inputs.session_generation;
    shared->turn_id = inputs.turn_id;
    // Freeze the failure receipt before any input enters the worker closure.
    Outcome start_failure;
    start_failure.model = inputs.model;
    start_failure.task_type = inputs.task_type;
    start_failure.session_generation = inputs.session_generation;
    start_failure.turn_id = inputs.turn_id;
    start_failure.error.code = ExtractionErrorCode::WorkerStartFailed;
    start_failure.error.message = "记忆抽取线程未能启动";
    shared->outcome = std::move(start_failure);
    // Freeze identity and failure text on the front thread, before moving inputs.
    Outcome outcome;
    outcome.model = inputs.model;
    outcome.task_type = inputs.task_type;
    outcome.session_generation = inputs.session_generation;
    outcome.turn_id = inputs.turn_id;
    ExtractionError worker_failure;
    worker_failure.code = ExtractionErrorCode::WorkerExecutionFailed;
    worker_failure.message = "记忆抽取后台执行失败";
    static_assert(std::is_nothrow_default_constructible_v<MemoryExtraction>);
    static_assert(std::is_nothrow_move_assignable_v<MemoryExtraction>);
    static_assert(std::is_nothrow_move_assignable_v<ExtractionError>);
    static_assert(std::is_nothrow_move_assignable_v<std::optional<Outcome>>);
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
    auto bypass_hook = testing::SnapshotBypassWorkerHook();
    auto work_hook = testing::worker_execution_hook;  // Worker owns this snapshot, not TLS.
#endif
    // Freeze and bind the real recorder while the front thread still owns this scene.
    std::unique_ptr<agent::LoopBoundaryRecorder> bypass;
    // Binding moved to the front thread. Preserve the former worker-setup
    // exception receipt instead of introducing a new unhandled CLI exit.
    auto fail_binding = [&]() noexcept {
        outcome.error = std::move(worker_failure);
        shared->outcome = std::move(outcome);
        bypass.reset();
        inputs.backend.reset();  // The accepted setup failure owns disposal too.
        shared->done.store(true);
        shared_ = std::move(shared);
    };
    try {
        if (inputs.trajectory != nullptr) {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
            if (bypass_hook) bypass_hook(testing::BypassWorkerPurpose::Memory,
                                         testing::BypassWorkerPhase::BeforeBinding);
#endif
            runtime::TrajectoryTurnBridge::Identity identity{inputs.provider, inputs.trajectory_wire, "host"};
            bypass = inputs.trajectory->NewLeasedBypassRecorder(std::move(identity), accounting::RequestPurpose::MemoryExtract);
        }
    } catch (const std::exception&) {
        fail_binding();
        return true;
    } catch (...) {
        fail_binding();
        return true;
    }
    // The closure owns its proxy. Each callback borrows through the revocable scene gate.
    // Backend execution remains outside that gate; detach is not proof it has stopped.
    auto run =
        [shared, outcome = std::move(outcome), worker_failure = std::move(worker_failure),
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
         work_hook = std::move(work_hook), bypass_hook = std::move(bypass_hook),
#endif
         backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), system_prompt = std::move(inputs.system_prompt),
         transcript = std::move(inputs.transcript), bypass = std::move(bypass)]() mutable noexcept {
            auto fail = [&]() noexcept {
                outcome.ok = false;
                outcome.extraction = MemoryExtraction{};
                outcome.error = std::move(worker_failure);
                // Accounting and invocation flag keep only facts already returned.
            };
            try {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (bypass_hook) bypass_hook(testing::BypassWorkerPurpose::Memory, testing::BypassWorkerPhase::BeforeSampling);
                if (work_hook) work_hook(testing::MemoryWorkerPhase::BeforeWork);
#endif
                // 抽取墙钟(§10.3):发起到采样返回的墙钟,与旧同步路同一跨度
                //(旧在主线程量,晚不了多少;usage 的 duration 在 accounting 里)。
                const auto extract_started = std::chrono::steady_clock::now();
                outcome.extraction_invoked = true;
                auto extraction =
                    RunMemoryExtraction(*backend, model, system_prompt, transcript,
                                        kMemoryExtractTimeoutSecs, effort, &outcome.accounting,
                                        bypass.get(), &shared->cancel);
                outcome.extract_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - extract_started)
                                              .count();
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (work_hook) work_hook(testing::MemoryWorkerPhase::AfterExtraction);
#endif
                if (extraction.has_value()) {
                    outcome.ok = true;
                    outcome.extraction = std::move(*extraction);
                } else {
                    outcome.error = extraction.error();
                }
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (work_hook) work_hook(testing::MemoryWorkerPhase::BeforePublish);
#endif
            } catch (const std::exception&) {
                fail();
            } catch (...) {
                fail();
            }
            // Sole producer; readers touch the slot only after acquiring done.
            // Optional move is statically checked not to throw; no lock/allocation here.
            shared->outcome = std::move(outcome);
            shared->done.store(true);  // outcome 写完才立收讫旗,主线程收货不抢跑
        };
    try {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
        if (testing::worker_start_hook) testing::worker_start_hook();
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

std::optional<TurnMemoryExtractor::Outcome> TurnMemoryExtractor::TakeFinished() {
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

void TurnMemoryExtractor::RequestCancel() {
    if (shared_ != nullptr) {
        shared_->cancel.store(true);
    }
}

bool TurnMemoryExtractor::Busy() const {
    return shared_ != nullptr;
}

bool TurnMemoryExtractor::Ready() const {
    // done 是 outcome 写完才立的收讫旗(见 Start 尾段):这里只读它,不
    // join、不锁、不动槽——ReadLine 的 100ms 拍在主线程问,零副作用。
    return shared_ != nullptr && shared_->done.load();
}

}  // namespace lubancode::app
