// TurnMemoryExtractor 的实现(记忆回合总结异步化单)。头注释是行为账:
// 异步、单飞、主线程收货、取消与退出的边界全在那边。

#include "app/turn_memory_extractor.hpp"

#include <chrono>
#include <utility>

#include "accounting/purpose.hpp"  // RequestPurpose(旁路桥的 purpose 门)
#include "runtime/trajectory_session.hpp"  // TrajectoryBypassBridge(Token 账本单 A1)

namespace lubancode::app {
namespace {
// 退出兜底的有界等待窗:取消旗已拉(cpr 的合并取消口应速断),等不起
// 看门狗的 45 秒全预算——到点 detach 放行,不冻退出。与
// SessionTitleRefiner 析构同一副方子(它的窗按自己 5 秒看门狗配的,
// 这里按"取消生效后应速断"配 5 秒)。
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
    // 闭包只持值与 shared 槽:不引用本对象,detach 晚归也不悬垂。
    worker_ = std::thread(
        [shared, backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), system_prompt = std::move(inputs.system_prompt),
         transcript = std::move(inputs.transcript), task_type = std::move(inputs.task_type),
         trajectory = inputs.trajectory, trajectory_wire = std::move(inputs.trajectory_wire),
         provider = std::move(inputs.provider)]() mutable {
            Outcome outcome;
            outcome.model = model;
            outcome.task_type = task_type;
            outcome.session_generation = shared->session_generation;
            outcome.turn_id = shared->turn_id;
            // Token 账本单 A1:本线程自铸旁路桥(purpose=memory_extract)。
            // recorder 提交全程持锁,与主线程的写在盘上串行;桥随本栈
            // 生灭,detach 晚归也不悬垂。没接轨迹(空)一笔不落。
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
            const auto extraction =
                RunMemoryExtraction(*backend, model, system_prompt, transcript,
                                    kMemoryExtractTimeoutSecs, effort, &outcome.accounting,
                                    bypass.get(), &shared->cancel);
            outcome.extract_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - extract_started)
                                          .count();
            if (extraction.has_value()) {
                outcome.ok = true;
                outcome.extraction = std::move(*extraction);
            } else {
                outcome.error = extraction.error();
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
