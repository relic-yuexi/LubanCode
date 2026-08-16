// 子代理导航坞的纯逻辑层(0.29.x"导航贴底并整帧去重"一单):条目数据、
// 按键状态机(选择/查看/停止/两段确认/闲置汇总展开)、折叠与窗口化布局、
// 结构化行渲染(身份/中段/右状态三列)、输入框上横线右端的代理短标签,
// 全收在这一个不碰终端的模块里,单测钉在 tests/test_agent_panel.cpp。
// 终端层(console_input.cpp)只做三件事:把 platform 按键翻成 PanelKey(唯一
// 的键位缝,日后接 keymap 只动那一处)、每拍调 LayoutAgentDock 拿行、把
// 状态机的动作转给应用层接好的 AgentTool 正式接口。
//
// 层级规矩(规格"固定布局"):导航坞画在 composer 下横线与状态栏之后、
// 贴终端底部;待发队列仍属 composer 上方。布局层只产行与导航表,不知道
// 自己被摆在哪——"向上长"的旧假设已删净,调用方决定底部次序。
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
// 轻量:列表每 100ms 拉一次,这里只放列表行要用的字段——查看态的长正文
// (完整任务说明、工具流水、结论)由应用层按 viewed_task_id 从任务台账
// 现取,整块换进上方会话视口,别让每拍刷新都复制全部工具输出。
struct AgentPanelEntry {
    std::string name;   // 如 "general-purpose #4"
    std::string title;  // 真正短标题(AgentTaskSnapshot.title;旧任务空串,显示层退"未命名")
    std::string state;  // 状态摘要行("运行中(3 次工具调用 · …)")
    bool running = false;
    bool failed = false;  // 失败或预算耗尽(留短错,等查看或清理)
    int task_id = 0;      // AgentTool 任务号;main/无 = 0
    // 活动坞的退场账(规格"现场一"新规矩):done 且结果已交回 main,或被
    // 用户中止——都从活动导航坞退场。退场只是不进导航表,台账与详情照查。
    bool done_delivered = false;
    bool cancelled = false;
    // 内容修订号(追加需求"查看态实时思考流"):这只任务的消息账(事件/思考/
    // 正文增量/阶段翻页)每动一笔 +1。空闲 composer 的 100ms 拍拿它判断
    // "正查看的运行中子代理又在出活",到 1s 节流拍就重铺查看帧——终端层
    // 只比数字,不复制内容。0 = 无实时流(演示假代理/终态)。
    std::uint64_t content_revision = 0;
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

// 闲置汇总行的哨兵任务号:折叠后"另有 N 只闲置代理"占一个可导航位,Enter
// 展开、Esc 收起。它不是真任务——x/停止/清除/收件目标对它一律无效。
constexpr int kIdleSummaryTaskId = -1;

// 空闲 composer 与流式监听共用的面板焦点/查看态/停止全部两段确认。纯逻辑,
// 不知道终端。选择按稳定 task id 记(0 = main,-1 = 闲置汇总哨兵),任务
// 增删/重排后按 id 修正,
// 不靠易漂移的数组下标;取消/清除的动作目标也以 task id 交出(Outcome 里
// 的 stop_current_task_id),绝不让调用方按下标回查。规矩(规格三、六节):
//   - 正文非空时不抢上下/Enter,普通字母 x 只进 composer;
//   - Enter 消费按键、设置 viewed_task_id(切上方会话视口与 composer 收件
//     目标),Esc 先清 viewed 回 main、再退代理焦点;
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

    // 条目增减(任务起停/清理/重排/完成退场)后的修正:按 task id 找回选中;
    // id 没了就落到相邻条目(下标钳回界内),全没了收干净。查看态里目标条目
    // 还在就保留(任务从 running 变终态不清标签——消息投递由 AgentTool 拒
    // 收);目标不在导航表里了(完成退场/被清)则只退查看态:视口与收件目
    // 标回 main,选择落相邻运行项,不整份归零(规格"现场一")。
    void OnEntriesChanged(const std::vector<int>& agent_task_ids);
    // 查看态目标条目被清理/会话收场:强制收起,收件目标回 main。
    void CloseView();
    // 全清(焦点/查看态/两段确认)。
    void Reset();

    bool focused() const { return focus_; }
    bool stop_all_armed() const { return armed_; }
    // 会话层唯一真状态:正看哪只会话(0 = main)。上方 transcript 的数据源、
    // composer 收件目标、导航坞里被查看行的 ◉ 标记,三样全由这一枚 id 推出,
    // 不许各记一份(规格"现场一")。
    int viewed_task_id() const { return viewed_task_id_; }
    // 闲置汇总行是否处于展开态(Enter 展开、Esc 收起;展开/收起不改任何
    // 真任务的 task id 与消息目标)。
    bool idle_expanded() const { return idle_expanded_; }
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
    int viewed_task_id_ = 0;  // 正看哪只会话;0 = main(会话层唯一真状态)
    bool armed_ = false;
    bool idle_expanded_ = false;
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
        int viewed_task_id = 0;                 // 正看哪只会话;0 = main
        bool stop_all_armed = false;
        bool idle_expanded = false;
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
// 导航坞布局(纯函数):折叠 + 窗口化 + 结构化行
// -----------------------------------------------------------------------

// 坞里的一行。先算好身份列宽/状态列宽,再把余宽交给中段——任务状态每秒
// 跳一次,标题起点也不会左右乱晃(规格"整帧重画"一节)。
struct AgentDockRow {
    enum class Kind {
        Hint,         // 首行操作提示(随焦点/宽度收放)
        Note,         // 窗口计数行("共 N 只 · 上方未展示 …")
        Entry,        // main 或一只子代理(三列结构)
        IdleSummary,  // "另有 N 只闲置代理 · Enter 展开"(可导航哨兵)
    };
    Kind kind = Kind::Entry;
    int task_id = 0;   // Entry:0=main/任务号;IdleSummary:kIdleSummaryTaskId
    bool selected = false;  // pointer 标记(❯),未聚焦不画
    std::string marker;     // "❯ " 或 "  "
    std::string lamp;       // "● " main / "◌ " 运行 / "○ " 完成 / "× " 失败 / "◉ " 正在查看
    std::string identity;   // 身份列:"main" / "general-purpose #2"
    std::string middle;     // 中段:真正短标题(优先吃宽、优先截断)
    std::string status;     // 右列:状态摘要(耗时/token/queued),右对齐
    std::string text;       // Hint/Note 的整行文本
};

struct AgentDockLayout {
    std::vector<AgentDockRow> rows;  // 渲染顺序:提示 →[计数]→ main+条目(+汇总)
    std::vector<int> navigation_ids;  // 导航表:[0(main), 任务号…, -1(汇总哨兵,若有)]
    int visible_first = 0;           // 窗口里第一条的导航下标(0 = main)
    int visible_count = 0;           // 窗口里摆了几条(main 计入)
    int total_count = 0;             // 导航表几条(main 计入)
    int hidden_above = 0;
    int hidden_below = 0;
    int identity_width = 0;  // 本帧身份列显示宽(4~28 钳位)
    int status_width = 0;    // 本帧右状态列显示宽
    bool idle_summary = false;  // 折叠中的闲置汇总行在导航表里
    int hidden_idle = 0;        // 折进汇总行的闲置(完成)代理数
};

// 活动坞退场判定(纯函数,DockNavigationIds 与 LayoutAgentDock 共用一本账):
// done+delivered 与 cancelled 退场——底栏只管正在干活的会话,任务坟场不归
// 它管(规格"现场一"新规矩)。退场只是不进导航表,TaskDetail/历史台账照查。
bool DockEntryRetired(const AgentPanelEntry& entry);

// agents:后台子代理条目(main 由这里补成导航表第 0 项);selected:导航表
// 下标(0 = main,可落在 kIdleSummaryTaskId 哨兵上);focused:未聚焦不画
// 选中标记;max_visible_entries:窗口最多摆几条代理行(<=0 = 不限,
// 常态 5);max_total_rows:整坞(提示/计数/条目)最多占几行(<=0 =
// 不限,矮屏开窗预算);width:终端列宽;armed_stop_all:两段确认第一段;
// streaming:提示行用流式版文案;idle_expanded:闲置汇总是否展开;
// viewed_task_id:正查看的任务号(0 = main),该行画 ◉ 且永不折叠。
//
// 折叠与退场(规格"现场一"新规矩):退场条目(done+delivered/cancelled)
// 根本不进导航表;其余条目里 running/failed/正在查看的行永不折叠,done
// 未投递的过渡行最多单列三只,更多折成一行汇总哨兵(kIdleSummaryTaskId,
// Enter 展开、Esc 收起,展开/收起不改任何 task id)。条目多于窗口时围着
// selected 开窗,选中行永不因开窗消失。
// 导航表(纯函数):退场过滤 + 闲置折叠后的可导航 id 序列(不含 main——
// 控制器契约与旧 PanelEntryIds 一致,main 隐式算第 0 项)。布局渲染与按键
// 状态机共用这一份,选择永远落不进被折起来/退场的区域。
//
// 导航坞只放导航:行、状态与提示。完整 prompt、工具调用流水、结论与错误
// 全在上方会话视口里看(查看态由应用层的 transcript 换源负责,规格"现场
// 一"),绝不向坞下方生长。
std::vector<int> DockNavigationIds(const std::vector<AgentPanelEntry>& agents, bool idle_expanded,
                                   int viewed_task_id);

AgentDockLayout LayoutAgentDock(const std::vector<AgentPanelEntry>& agents, int selected, bool focused,
                                int max_visible_entries, int max_total_rows, int width, bool armed_stop_all,
                                bool streaming, bool idle_expanded, int viewed_task_id = 0);

// 按本帧列宽把一行渲染成纯文本(不含 ANSI;宽度不够先截中段、再缩状态,
// 绝不撑破 width)。三列起点由 layout.identity_width/status_width 定死。
std::string RenderAgentDockRow(const AgentDockLayout& layout, const AgentDockRow& row, int width);

// 整坞渲染成纯文本行(空闲 composer 与流式 footer 共用这一份)。
std::vector<std::string> RenderAgentDockLines(const AgentDockLayout& layout, int width);

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
