// SessionTitleRefiner 的实现(实测问题 7)。头注释是行为账:异步、单飞、
// 主线程收货、取消与退出的边界全在那边。

#include "app/session_title_refiner.hpp"

#include <chrono>
#include <utility>

#include "app/session_title.hpp"  // RefineSessionTitle
#include "runtime/trajectory_session.hpp"  // TrajectoryBypassBridge(Token 账本单 A1)

namespace lubancode::app {
namespace {
// 退出兜底的有界等待窗:取消旗先行,后端听话就快回;真挂死(cpr 卡死
// 那类)到点 detach 放行,不冻退出——与 AgentTool 析构同一副方子。看门狗
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
    // 闭包只持值与 shared 槽:不引用本对象,detach 晚归也不悬垂。
    worker_ = std::thread(
        [shared, backend = std::move(inputs.backend), model = std::move(inputs.model),
         effort = std::move(inputs.effort), first_query = std::move(inputs.first_query),
         trajectory = inputs.trajectory, trajectory_wire = std::move(inputs.trajectory_wire),
         provider = std::move(inputs.provider), timeout_secs = inputs.timeout_secs]() mutable {
            Outcome outcome;
            outcome.model = model;
            outcome.generation = shared->generation;
            // Token 账本单 A1:本线程自铸旁路桥(purpose=title_refine)。
            // recorder 提交全程持锁,与主线程的写在盘上串行;桥随本栈
            // 生灭,detach 晚归也不悬垂。没接轨迹(空)一笔不落。
            // purpose 必须显式传 TitleRefine(标题触发提前单确诊):v3 工厂
            // 自 T11-A 加 purpose 门起只认 memory_extract/title_refine,漏传
            // 走默认 OtherHostRequest 会被拒成 nullptr——采样本体照发,但
            // title_refine 的 prompt/assistant 消息与 prepared/usage 细账
            // 一笔不落,v3 账本里看不到精炼的痕迹。
            std::unique_ptr<lubancode::runtime::TrajectoryBypassBridge> bypass;
            if (trajectory != nullptr) {
                lubancode::runtime::TrajectoryTurnBridge::Identity identity{provider, trajectory_wire,
                                                                            "host"};
                bypass = trajectory->NewBypassBridge(
                    std::move(identity), lubancode::accounting::RequestPurpose::TitleRefine);
            }
            // 看门狗(取消误报 ESC 单 Bug 1 后收编):超时交给 SampleModel
            // 的合并取消口(预算照进 timeout_secs,deadline 到点归因
            // local_deadline 并带预算数),会话拆除仍走 RequestCancel 的外
            // 部旗(升旗人申报 Internal)。本地看门狗线程退役。
            lubancode::agent::BackgroundCallAccounting accounting;
            const auto title = RefineSessionTitle(*backend, model, effort, first_query,
                                                  timeout_secs, &shared->cancel, &accounting,
                                                  bypass.get());
            // 失败半截也出账(旧口径:先记账再判错)。
            outcome.accounting = std::move(accounting);
            if (title.has_value() && !title->empty()) {
                outcome.ok = true;
                outcome.title = *title;
            } else if (!title.has_value()) {
                // 死因原样带回(2026-09-22 报明单):超时/网络错/空回各报
                // 各的,报明行拿它填 {0}——此前闭包只看 has_value,错误串
                // 扔在门口。has_value 且空的半档按契不会出现(空回走
                // unexpected),真出现就留空,报明行降级成无死因。
                outcome.error = title.error();
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

bool SessionTitleRefiner::Ready() const {
    // done 是 outcome 写完才立的收讫旗(见 Start 尾段):这里只读它,不
    // join、不锁、不动槽——ReadLine 的 100ms 拍在主线程问,零副作用。
    return shared_ != nullptr && shared_->done.load();
}

}  // namespace lubancode::app
