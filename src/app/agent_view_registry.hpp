// 按代理状态投影单 P1(身份与状态分账):会话级视图登记簿。
//
// 一场会话一只。三本账都在这:
//   1. 身份账:session_generation(/clear、/resume 换代)+ 当前查看页
//      (viewed_task_id,0 = main)+ view_epoch(每换一页 +1)。单子 §四.1:
//      main 也是合法成员,UI 边界把 task_id=0 映射进来;旧 generation 的
//      账换代整册作废。
//   2. main 活回合账(ledge):TurnCollector 的收账与绘制分离后,"账"的
//      权威在这里——每笔事件应用(工具配对/视图账/统计)经
//      ApplyToMainTurn 进锁、修订号 +1;离屏期间(main 不是当前页)绘制
//      被闸掉,账照走。重铺(切回 main/查看帧刷新)经 TakeMainLedge-
//      ForRepaint/MarkMainPrinted 成对取快照、钉打印水位——接续事件不
//      重放、不漏(单子 §四.2 的水位规矩)。
//   3. 每页交互状态账:AgentUiStateStore(滚动/展开/草稿),切走保留
//      切回还原。
//
// 线程与锁(单子 §四.3:先记账,再通知画屏):
//   - 一把小锁(mutex_)管全部状态。事件应用(泵消费线程/控制路线程,
//     均持泵画笔锁)→ 拿本锁改账;换页/重铺(监听线程)→ 拿本锁切身份
//     取快照。锁序恒为"泵画笔锁 → 本锁",本锁是叶子:锁内不做任何
//     I/O、不反拿画笔锁。
//   - 重铺的成对协议:重铺期间持有泵画笔锁(render_mutex_,RunTurn 在
//     BeginMainTurn 登记)——在飞的绘制/收账全部让路,快照与水位天然
//     成对。P2 收拢写者后这套换成切页事务 + FrameToken,本登记簿的
//     水位合同原样沿用。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "cli/agent_view_state.hpp"  // AgentViewKey/AgentUiStateStore
#include "runtime/turn_collector.hpp"
#include "runtime/turn_view.hpp"

namespace lubancode::app {

class AgentViewRegistry {
public:
    // main 活回合的成对快照:视图 + 修订号 + 是否仍在跑。view 为空 =
    // 没有可重铺的回合账(老会话/收口后清账)。
    struct MainTurnSnapshot {
        std::shared_ptr<const runtime::TurnView> view;
        std::uint64_t revision = 0;
        bool live = false;
    };

    // ---- 身份账 -----------------------------------------------------------

    // 会话换代(/clear、/resume、开场):generation +1,旧册作废(main
    // 回合账、每页交互状态、查看页回 main)。旧 generation 的迟到事件
    // 对不上新键,自然作废。
    void BeginNewSession() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++session_generation_;
        viewed_task_id_ = 0;
        ++view_epoch_;
        ++layout_revision_;
        ledge_view_.reset();
        ledge_revision_ = 0;
        main_revision_ = 0;
        printed_revision_ = 0;
        repaint_hold_ = false;
        ui_states_.DropGeneration(session_generation_ - 1);
    }

    std::uint64_t session_generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_generation_;
    }

    cli::AgentViewKey MainKey() const { return cli::AgentViewKey{session_generation(), 0}; }

    cli::AgentViewKey KeyFor(int task_id) const {
        return cli::AgentViewKey{session_generation(), task_id};
    }

    // 切换查看页(0 = main)。换页纪元 +1;同一页重复切是空操作。闸门
    // 立即生效:切走后 main 的绘制被拒(收账照走),切回后按打印水位
    // 接续。调用方(换页钩子)应在泵画笔锁内调(cli::SetViewSwitchGuard
    // 提供的那道护栏),防在飞的 main 绘制插进新页。
    void SwitchViewed(int viewed_task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (viewed_task_id == viewed_task_id_) {
            return;
        }
        viewed_task_id_ = viewed_task_id;
        ++view_epoch_;
    }

    int viewed_task_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return viewed_task_id_;
    }

    // 查看页对账:登记簿的 viewed 只随换页钩子动,空闲路的 Esc 兜底
    //(ResetAgentPanelSession)不走钩子——回合起跑前把面板状态机的现值
    // 同步进来,闸门才不会拿旧页当当前页(main 页被误闸)。
    void SyncViewed(int viewed_task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (viewed_task_id == viewed_task_id_) {
            return;
        }
        viewed_task_id_ = viewed_task_id;
        ++view_epoch_;
    }

    std::uint64_t view_epoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return view_epoch_;
    }

    // ---- 帧令牌(P2:切页事务与 FrameToken) -------------------------------

    // 此刻"画 target 这页"的令牌(锁外快照用)。target 的存在性由调用方
    //(面板台账)先核;这里只发号。
    cli::FrameToken TokenFor(int task_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cli::FrameToken{cli::AgentViewKey{session_generation_, task_id}, view_epoch_, layout_revision_};
    }

    // 写屏前的核对(统一提交锁内调):令牌三要素全对上当前态才放行。快速
    // A→B→A 的第一轮 A(旧 epoch)、resize 前算好的帧(旧 layout)都被
    // 这一句拦下——丢的是画面指令,事件照收账。
    bool TokenCurrent(const cli::FrameToken& token) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return token.view.session_generation == session_generation_ && token.view.task_id == viewed_task_id_ &&
               token.view_epoch == view_epoch_ && token.layout_revision == layout_revision_;
    }

    // 布局翻版(resize/Ctrl+L/Ctrl+O/展开档切换):layout_revision +1,
    // 在飞的旧布局帧全部失配。返回新版本号。
    std::uint64_t BumpLayoutRevision() {
        std::lock_guard<std::mutex> lock(mutex_);
        return ++layout_revision_;
    }

    // ---- main 活回合账(ledge)---------------------------------------------
    //
    // RunTurn 起回合时登记(collector 指针 + 泵画笔锁),收口时 EndMainTurn
    // 落最终视图,退场时 DetachMainTurn 摘画笔护栏。三步之外没人碰这几枚
    // 指针;指针只在本锁内读写。

    void BeginMainTurn(const runtime::TurnCollector* collector, std::recursive_mutex* render_mutex) {
        // render_mutex 参数实为统一提交锁(P2):调度器路径传
        // SessionUiDispatcher::commit_mutex,旧路传 sink 自持那把;护栏
        // WithMainRenderLock 拿它把换页事务与在飞渲染串起来。参数名保持
        // 旧签名,免动全部调用点。
        std::lock_guard<std::mutex> lock(mutex_);
        collector_ = collector;
        render_mutex_ = render_mutex;
        ledge_view_.reset();
        ledge_revision_ = 0;
        main_revision_ = 0;
        printed_revision_ = 0;
        repaint_hold_ = false;
    }

    // 收口(FinishTurn 之后):把最终视图连同修订号钉进 ledge——回合结束
    // 后切回 main,重铺用的就是这份终账。collector 摘掉(不再有应用),
    // 画笔护栏留到 DetachMainTurn(收口 chrome 还要写屏,换页还得拦)。
    void EndMainTurn(runtime::TurnView final_view) {
        std::lock_guard<std::mutex> lock(mutex_);
        collector_ = nullptr;
        ledge_view_ = std::make_shared<const runtime::TurnView>(std::move(final_view));
        ledge_revision_ = ++main_revision_;
    }

    // RunTurn 退场(收口 chrome 之后、sink 析构之前)摘画笔护栏。
    void DetachMainTurn() {
        std::lock_guard<std::mutex> lock(mutex_);
        collector_ = nullptr;
        render_mutex_ = nullptr;
    }

    // 事件应用(TerminalTurnSink 的收账半边):锁内改 collector 的账,
    // 修订号 +1 并返回。所有 collector 变更必须走这——重铺快照读的也是
    // 这把锁,两边对得上,快照永远不撕裂。
    template <class Mutate>
    std::uint64_t ApplyToMainTurn(Mutate&& mutate) {
        std::lock_guard<std::mutex> lock(mutex_);
        mutate();
        return ++main_revision_;
    }

    std::uint64_t MainRevision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return main_revision_;
    }

    bool MainTurnLive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return collector_ != nullptr;
    }

    // 只读快照(诊断/单测):有 collector 取现值,没有给 ledge 终账。
    MainTurnSnapshot MainLedgeSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (collector_ != nullptr) {
            return MainTurnSnapshot{std::make_shared<const runtime::TurnView>(collector_->view()),
                                    main_revision_, true};
        }
        return MainTurnSnapshot{ledge_view_, ledge_revision_, false};
    }

    // 绘制闸门(单子 §四.2 的水位判定):main 是当前页、这笔修订号过了
    // 打印水位、且没有重铺在进行,才许落笔。离屏期间收账照走、绘制被拒
    // ——被拒的内容都在 ledge 里,重铺接得上。
    bool MainShouldDraw(std::uint64_t event_revision) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return viewed_task_id_ == 0 && !repaint_hold_ && event_revision > printed_revision_;
    }

    // 收口 chrome(PrintTurnFooter/统计行/FinalizeRepaint)的可见性判定:
    // 静默档之外还要看"此刻看的是不是 main"。无登记簿时调用方按旧路
    // (!silent)处理。
    bool MainVisibleForChrome() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return viewed_task_id_ == 0;
    }

    // ---- 重铺的成对协议 ---------------------------------------------------

    // 泵画笔锁护栏(换页/重铺的擦旧帧+铺新帧全程持有,在飞的 main 绘制
    // 让路)。没有活回合(锁未登记)时直接执行。
    void WithMainRenderLock(const std::function<void()>& body) {
        std::recursive_mutex* render = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            render = render_mutex_;
        }
        if (render == nullptr) {
            body();
            return;
        }
        std::lock_guard<std::recursive_mutex> render_hold(*render);
        body();
    }

    // 重铺开拍:关绘制闸、取成对快照(有活回合取 collector 现值,否则
    // ledge 终账)。调用方随即在泵画笔锁内打印,完事 MarkMainPrinted。
    MainTurnSnapshot TakeMainLedgeForRepaint() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (collector_ != nullptr) {
            ledge_view_ = std::make_shared<const runtime::TurnView>(collector_->view());
            ledge_revision_ = main_revision_;
        }
        repaint_hold_ = true;
        return MainTurnSnapshot{ledge_view_, ledge_revision_, collector_ != nullptr};
    }

    // 重铺收拍:打印水位钉在快照的修订号上,开闸。此后修订号更大的事件
    // 接续落笔;更小的(已在快照里)不重放。
    void MarkMainPrinted(std::uint64_t snapshot_revision) {
        std::lock_guard<std::mutex> lock(mutex_);
        printed_revision_ = snapshot_revision;
        repaint_hold_ = false;
    }

    // ---- 每页交互状态 -----------------------------------------------------

    cli::AgentUiStateStore& ui_states() { return ui_states_; }

private:
    mutable std::mutex mutex_;
    std::uint64_t session_generation_ = 1;  // 从 1 起;0 留给"无登记簿"的单测默认键
    int viewed_task_id_ = 0;                // 正看哪页;0 = main
    std::uint64_t view_epoch_ = 1;
    std::uint64_t layout_revision_ = 1;     // 帧令牌第三要素:resize/Ctrl+L/Ctrl+O 翻版

    const runtime::TurnCollector* collector_ = nullptr;  // 活回合的账本(RunTurn 栈上)
    std::recursive_mutex* render_mutex_ = nullptr;        // 活回合的泵画笔锁(sink 持有)
    std::shared_ptr<const runtime::TurnView> ledge_view_;  // 离屏期间/收口后的回合账快照
    std::uint64_t ledge_revision_ = 0;
    std::uint64_t main_revision_ = 0;      // 活回合应用修订号(每笔事件 +1)
    std::uint64_t printed_revision_ = 0;   // 打印水位(重铺收拍时钉)
    bool repaint_hold_ = false;            // 重铺进行中(画笔锁之外的二道闸)

    cli::AgentUiStateStore ui_states_;
};

}  // namespace lubancode::app
