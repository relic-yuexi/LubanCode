// 终端接线收尾单:transcript 控制器。
//
// 病灶一(用户查账原文):transcript 导航/查看态/模型转换住在大类
// (interactive_session)里。拆这一只控制器出去——导航键、查看态进出、
// 条目账本全归它;TurnItemView→TranscriptItem 的公共投影在
// cli::ProjectTurnItem(turn_renderer),这里只消费投影产物。
//
// 边界:住 cli 层,不引 app(横幅重画、查看态视口、ESC 急停、活历史、
// 轮视图存档都是会话侧的账,经 Hooks 回调借进来)。输出全走 TerminalPort
// (TermOut);锁规矩与原先一字不差(PrintViewedTranscript 自持
// StdoutWriteMutex,查看帧的擦账只有 console_input 那一本——见
// interactive_session 原注释,已随行搬来)。

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <vector>

#include "api/types.hpp"
#include "cli/console_input.hpp"  // UiKeyAction/查看态现值/锁
#include "cli/theme.hpp"
#include "cli/transcript.hpp"
#include "runtime/turn_view.hpp"  // Ctrl+L 重画优先用的轮视图存档

namespace lubancode::cli {

class TranscriptUiController {
public:
    // 会话侧借进来的五枚钩子。视口/横幅/急停给可调用体;历史与轮视图给
    // getter(返回指针,空 = 没有)。全部可缺——缺了的键路按"没有账"处理,
    // 与原先 nullptr 判定同款。
    struct Hooks {
        // 查看态(子代理视口)构建:task_id、宽度 -> 行组。BuildAgentTaskTranscriptLines 的接线。
        std::function<std::vector<std::string>(int task_id, int width)> build_task_transcript;
        // 会话横幅重画(PrintBanner 的接线;Ctrl+L 与退出查看态用)。
        std::function<void()> repaint_banner;
        // 空闲态 ESC 的 loop 急停:返回停了几只(0 = 没活 loop,键还给编辑器)。
        std::function<int()> stop_active_loops;
        // 轮次导航({ })要读的活历史;空 = 没起 loop,返回 nullptr 时键不消费。
        std::function<const std::vector<api::Message>*()> history;
        // Ctrl+L 重画优先用的轮视图存档(空 = 退回 transcript 快照重铺)。
        std::function<const std::vector<runtime::TurnView>*()> turn_views;
    };

    explicit TranscriptUiController(const Theme& theme);

    // 控制器常驻,钩子在会话装配齐之后灌(构造函数里 loop 还没建)。
    void SetHooks(Hooks hooks);

    // ---- 条目账本(ToolDisplay/RunTurn/通知入账共用同一份) ----
    std::vector<TranscriptItem>& items() { return items_; }
    const std::vector<TranscriptItem>& items() const { return items_; }

    // 紧凑/详细档开关的地址(TurnContext::transcript_expanded 指的就是它;
    // 监听线程与主线程跨线程读写,保持 atomic)。
    std::atomic<bool>* expanded_flag() { return &expanded_; }

    // ---- 按键与铺帧 ----
    // SetTranscriptUiHandler 的正主:UiKeyAction -> 导航/查看态/重画。
    // 返回 true = 键被消费(终端层按"铺了新内容"重锚)。
    bool HandleKey(UiKeyAction action);

    // 视图切换钩子(SetAgentViewSwitchHook)的正主:viewed_task_id 指向哪只
    // 子代理,就把它的 transcript 整块铺进上方;0 = 重铺 main 最近条目。
    // tail_rows>0 时只铺头三行+最近 N 行(实时流重铺拍,不刷滚屏)。
    void PrintViewedTranscript(int viewed_task_id, int tail_rows = 0);

    // 聚焦查看返回时的"简化重画":最近几条紧凑摘要(焦点标记照带)。
    void PrintRecentItems(std::size_t count);

    // ---- 会话侧的零星口 ----
    // 查看态的 Ctrl+O 档位:查看帧构建(子代理视口)时读,ToggleExpand 翻。
    bool agent_view_expanded() const { return agent_view_expanded_; }
    // 会话切走(resume/新轮起跑)时聚焦查看态复位:画面留在滚动历史里,
    // 不主动重铺(与原先 focus_view_active = false 一字不差)。
    void ExitFocusView() { focus_view_active_ = false; }

private:
    const Theme& theme_;

    // ---- 条目账与 UI 状态(原 TerminalSessionController 的六个成员搬来) ----
    std::vector<TranscriptItem> items_;
    // Ctrl+O 全局开关,RunTurn 里新条目也按它画。atomic<bool>:回合执行期间
    // TurnInputListener 的监听线程也会翻它(见原大类注释,随行搬来)。
    std::atomic<bool> expanded_{false};
    int focus_index_ = -1;          // 焦点条目下标,-1 = 无焦点
    int nav_turn_index_ = -1;       // { } 轮次导航的当前轮(-1 = 未开始)
    bool focus_view_active_ = false;  // 正在聚焦查看
    std::atomic<bool> expand_latest_{false};  // Ctrl+O:inline 展开最近一条
    // 子代理查看态的 Ctrl+O(查看帧里流式思考/正文尾巴的展开开关)。
    bool agent_view_expanded_ = false;

    Hooks hooks_;
};

}  // namespace lubancode::cli
