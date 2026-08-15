// 子代理面板的纯逻辑层(0.28.x"面板移到输入框上方"一单):条目数据、
// 按键状态机(选择/查看/停止/两段确认)、窗口化布局、输入框上横线右端的
// 代理短标签,全收在这一个不碰终端的模块里,单测钉在 tests/test_agent_panel.cpp。
// 终端层(console_input.cpp)只做三件事:把 platform 按键翻成 PanelKey(唯一
// 的键位缝,日后接 keymap 只动那一处)、每拍调 LayoutAgentPanel 拿行、把
// 状态机的动作转给应用层接好的 AgentTool 正式接口。
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::cli {

// 面板条目。main 不在列表里(固定画成第 0 项);provider 只交后台子代理。
// 轻量:列表每 100ms 拉一次,这里只放列表行要用的字段——详情(完整任务
// 说明、工具流水、未送达介入消息)另走 detail provider 按需取,别让每拍
// 刷新都复制全部工具输出。
struct AgentPanelEntry {
    std::string name;   // 如 "general-purpose #4"
    std::string title;  // 真正短标题(AgentTaskSnapshot.title;旧任务空串,显示层退"未命名")
    std::string state;  // 状态摘要行("运行中(3 次工具调用 · …)")
    bool running = false;
    bool failed = false;  // 失败或取消
    int task_id = 0;      // AgentTool 任务号;main/无 = 0
};

// 面板动作,由应用层(InteractiveSession)接线到 AgentTool 的正式取消/清理
// 接口。终端层不直接碰 AgentTool。
struct AgentPanelActions {
    // 停止一只运行中的任务(置取消信号;面板等线程报终态再改灯,不先抹行)。
    std::function<bool(int task_id)> cancel_task;
    // 把一条已进终态的任务从面板清掉(顺带清未送达的介入消息)。
    std::function<bool(int task_id)> clear_task;
    // 停止全部运行中任务,返回发了停止信号的任务数。
    std::function<int()> cancel_all;
};

// -----------------------------------------------------------------------
// 按键状态机
// -----------------------------------------------------------------------

// 语义按键(与具体键位解耦的"动作 id")。console_input 里那张
// platform::KeyInput -> PanelKey 的小映射表是唯一的键位缝。
enum class PanelKey {
    Up,
    Down,
    EnterView,     // Enter:进/出查看态
    Esc,
    StopEntry,     // x:停止当前运行中条目 / 清除终态条目
    StopAllArm,    // Ctrl+X:两段确认第一段
    StopAllConfirm,// Ctrl+K:两段确认第二段
    Other,
};

// 空闲 composer 与流式监听共用的面板焦点/查看态/停止全部两段确认。纯逻辑,
// 不知道终端。选择按稳定 task id 记(0 = main),任务增删/重排后按 id 修正,
// 不靠易漂移的数组下标;取消/清除的动作目标也以 task id 交出(Outcome 里
// 的 stop_current_task_id),绝不让调用方按下标回查。规矩(规格三、六节):
//   - 正文非空时不抢上下/Enter,普通字母 x 只进 composer;
//   - Enter 进查看态(同时把 composer 收件目标切到这只子代理),Esc 先退
//     查看态、再退代理焦点;
//   - x 只在焦点落在一只子代理上时消费:运行中 = 停止,终态 = 清除;main
//     行不接停止/清除;
//   - Ctrl+X -> Ctrl+K 两段确认有时限,超时/Esc/别键撤销;
//   - main 固定第 0 项,总条目数 = 1 + 子代理数。
class AgentPanelController {
public:
    struct Outcome {
        bool consumed = false;      // 键被面板吃掉,不进 composer
        bool redraw = false;        // 面板状态变了,要重画
        bool stop_current = false;  // 对当前选中条目执行 停止/清除(应用层按条目状态分派)
        int stop_current_task_id = 0;  // 稳定任务号;x 的动作目标;0 = 无
        bool stop_all = false;      // 两段确认完成,停止全部运行中任务
    };

    // agent_task_ids:当前代理条目的稳定任务号表(main 不在表里,总条目数 =
    // ids.size()+1)。composer_empty:此刻 composer 是否空(半句正文永远
    // 优先归 composer)。
    Outcome HandleKey(PanelKey key, const std::vector<int>& agent_task_ids, bool composer_empty,
                      std::chrono::steady_clock::time_point now);

    // 条目增减(任务起停/清理/重排)后的修正:按 task id 找回选中;id 没了
    // 就落到相邻条目(下标钳回界内),全没了收干净。查看态里目标条目还在
    // 就保留(任务从 running 变终态不清标签——消息投递由 AgentTool 拒收);
    // 目标被清掉则强制收起,收件目标回 main。
    void OnEntriesChanged(const std::vector<int>& agent_task_ids);
    // 查看态目标条目被清理/会话收场:强制收起,收件目标回 main。
    void CloseView();
    // 全清(焦点/查看态/两段确认)。
    void Reset();

    bool focused() const { return focus_; }
    bool detail_open() const { return detail_; }
    bool stop_all_armed() const { return armed_; }
    // 当前选中的稳定任务号;0 = main。id 已不在表里时(增删后未修正的空档)
    // 按 main 算。
    int selected_task_id() const { return selected_task_id_; }
    // 选中条目在"main + ids"全表里的下标(0 = main),给窗口化布局用。
    int selected_index(const std::vector<int>& agent_task_ids) const;
    // 查看态里的收件目标稳定任务号;查看态关着或停在 main 上 = nullopt。
    std::optional<int> target_task_id() const;

    // 两段确认的时限;超时由调用方每拍调这个摘掉(首行提示跟着收回)。
    static constexpr std::chrono::milliseconds kStopAllWindow{2000};
    bool ExpireArmed(std::chrono::steady_clock::time_point now);

private:
    bool focus_ = false;
    bool detail_ = false;
    bool armed_ = false;
    int selected_task_id_ = 0;  // 稳定任务号;0 = main
    int selected_ = 0;          // 下标缓存(相邻回退与布局用),真实选择以 id 为准
    std::chrono::steady_clock::time_point arm_time_{};
};

// 会话级面板状态机外壳(规格三):空闲 composer(ReadLineKeyByKey)与流式
// 监听线程(TurnInputListener)共用同一份选择/焦点/详情/两段确认,流式转
// 空闲自然保得住状态,不靠两边各自记账。键处理只在持有 ConsoleReadMutex
// 的线程上发生(空闲读键/监听线程抢到读锁),绘制线程(footer 重画)另走
// 快照口——内部一把小锁把两边隔开。
class AgentPanelSession {
public:
    using Outcome = AgentPanelController::Outcome;

    Outcome HandleKey(PanelKey key, const std::vector<int>& agent_task_ids, bool composer_empty,
                      std::chrono::steady_clock::time_point now);
    void OnEntriesChanged(const std::vector<int>& agent_task_ids);
    void CloseView();
    void Reset();
    bool ExpireArmed(std::chrono::steady_clock::time_point now);

    // 绘制侧快照:选中下标按调用方给的 ids 现算(0 = main)。
    struct Snapshot {
        bool focused = false;
        bool detail_open = false;
        bool stop_all_armed = false;
        int selected_index = 0;                 // main 计入的下标
        int selected_task_id = 0;               // 稳定任务号;0 = main
        std::optional<int> target_task_id;      // 查看态收件目标;nullopt = main
    };
    Snapshot SnapshotFor(const std::vector<int>& agent_task_ids) const;

private:
    mutable std::mutex mutex_;
    AgentPanelController controller_;
};

// -----------------------------------------------------------------------
// 窗口化布局(纯函数)
// -----------------------------------------------------------------------

struct AgentPanelLayout {
    std::vector<std::string> lines;  // 整块面板要画的行(首行是操作提示)
    int visible_first = 0;           // 窗口里第一条的下标(0 = main)
    int visible_count = 0;           // 窗口里摆了几条(main 计入)
    int total_count = 0;             // 全表几条(main 计入)
    int hidden_above = 0;
    int hidden_below = 0;
};

// agents:后台子代理条目(main 由这里补成第 0 项);selected:选中下标;
// focused:未聚焦时不高亮(选中标记不画);detail_open/detail_lines:查看态
// 详情行(调用方按需从 detail provider 取,这里不复制工具流水);
// max_visible_entries:窗口最多摆几条条目(<=0 = 不限);
// max_total_rows:整块面板(含首行提示/计数行/详情)最多占几行(<=0 = 不限)
// ——锚点上方空间不够时在这里就开窗/截详情,首行操作提示永不丢;
// width:终端列宽,每行按它截断;armed_stop_all:两段确认第一段已按下,
// 首行提示换成确认话。
// 条目多于窗口时围着 selected 开窗,顶上写清总数与上下未展示数;详情超
// 预算保留头部(任务说明优先),末行写清未展示行数。
// streaming=true 时首行操作提示用流式版文案(规格四:屏上提示跟当前层
// 同步,流式里 Esc 的分层语义与空闲不同)。
AgentPanelLayout LayoutAgentPanel(const std::vector<AgentPanelEntry>& agents, int selected, bool focused,
                                  bool detail_open, const std::vector<std::string>& detail_lines,
                                  int max_visible_entries, int max_total_rows, int width, bool armed_stop_all,
                                  bool streaming = false);

// -----------------------------------------------------------------------
// 输入框上横线右端的代理短标签(纯函数)
// -----------------------------------------------------------------------

// 满宽横线右端挂一枚紧凑标签(查看态的子代理短述)。tag 先按"横线至少留
// kMinRuleCols 列"截宽,塞不下就整个退回无标签横线——先保住横线与提示符,
// 窄窗宁可缩标签。stats/reset 是主题色(plain 主题两串皆空,输出纯文本,
// 不夹 ANSI)。
constexpr int kMinRuleCols = 8;
std::string BuildRuleWithTag(const std::string& stats, const std::string& reset, const std::string& tag,
                             int width);

// 标签截断(测试也直接用):terminal 太窄时按 display width 截,保前弃后。
std::string TruncatePanelTag(const std::string& tag, int max_width);

}  // namespace lubancode::cli
