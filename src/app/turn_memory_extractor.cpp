// TurnMemoryExtractor 的实现(记忆回合总结异步化单)。头注释是行为账:
// 异步、单飞、主线程收货、取消与退出的边界全在那边。

#include "app/turn_memory_extractor.hpp"

#include <chrono>
#include <exception>
#include <type_traits>
#include <utility>

#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
#include "app/turn_memory_extractor_test_hooks.hpp"
#endif

#include "accounting/purpose.hpp"  // RequestPurpose(旁路桥的 purpose 门)
#include "runtime/trajectory_session.hpp"  // TrajectoryBypassBridge(Token 账本单 A1)

namespace lubancode::app {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
namespace testing {
namespace {
thread_local std::function<void()> worker_start_hook;
thread_local MemoryWorkerExecutionHook worker_execution_hook;
}
std::function<void()> ExchangeMemoryWorkerStartHook(std::function<void()> hook) {
    return std::exchange(worker_start_hook, std::move(hook));
}
MemoryWorkerExecutionHook ExchangeMemoryWorkerExecutionHook(MemoryWorkerExecutionHook hook) {
    return std::exchange(worker_execution_hook, std::move(hook));
}
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
    auto work_hook = testing::worker_execution_hook;  // Worker owns this snapshot, not TLS.
#endif
    // 闭包不引用本对象；值材料与 shared 槽自持。旁路仍借 ledger，
    // 这份借用尚欠可撤销口，不能凭 shared 槽宣称 detach 晚归安全。
    auto run =
        [shared, outcome = std::move(outcome), worker_failure = std::move(worker_failure),
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
         work_hook = std::move(work_hook),
#endif
         backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), system_prompt = std::move(inputs.system_prompt),
         transcript = std::move(inputs.transcript),
         trajectory = inputs.trajectory, trajectory_wire = std::move(inputs.trajectory_wire),
         provider = std::move(inputs.provider)]() mutable noexcept {
            auto fail = [&]() noexcept {
                outcome.ok = false;
                outcome.extraction = MemoryExtraction{};
                outcome.error = std::move(worker_failure);
                // Accounting and invocation flag keep only facts already returned.
            };
            try {
#ifdef LUBANCODE_PRIVATE_MEMORY_WORKER_TEST_HOOKS
                if (work_hook) work_hook(testing::MemoryWorkerPhase::BeforeWork);
#endif
                // Token 账本单 A1:本线程自铸旁路桥(purpose=memory_extract)。
                // recorder 提交全程持锁,与主线程的写在盘上串行;桥随本栈
                // 生灭；所借 ledger、writer、簿和 observer 仍须活着。
                // 可撤销借用另交。没接轨迹(空)一笔不落。
                // purpose 必须显式传 MemoryExtract:v3 工厂的 purpose 门只认
                // memory_extract/title_refine 一类白名单,漏传走默认
                // OtherHostRequest 会被拒成 nullptr——采样本体照发,但抽取的
                // prompt/assistant 消息与 prepared/usage 细账一笔不落。
                std::unique_ptr<lubancode::runtime::TrajectoryBypassBridge> bypass;
                if (trajectory != nullptr) {
                    lubancode::runtime::TrajectoryTurnBridge::Identity identity{provider, trajectory_wire,
                                                                                "host"};
                    bypass = trajectory->NewBypassBridge(
                        std::move(identity), lubancode::accounting::RequestPurpose::MemoryExtract);
                }
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
