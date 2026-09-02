// SessionPicker(会话管理器单第二、三步)的纯逻辑层:焦点轮换、搜索词、
// 筛选/排序切换、选中保持与视口翻页;Ctrl+T 转录查看、Ctrl+E 展开详情、
// Ctrl+O 紧凑/舒展三种查看态也是本层的状态机。不碰终端、不碰磁盘、
// 不知道存档在哪——数据由调用方从 workspace 会话索引 摘好喂进来,
// resume 结果只是一枚 id。没有 delete;转录内容(excerpt 行)同样由
// 调用方按需读好带进来(大文件按需读的"按需"归接线层管)。
//
// 终端绘制(TTY 面板)在 app/commands/session_commands.cpp,与
// provider_switch 同一层路数:platform 原语画帧、行级清重画,不手写
// 转义序列。测试钉在这层(tests/unit/sessions/test_session_picker.cpp)。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "cli/line_editor.hpp"  // KeyEvent/KeyKind(纯枚举,不拖终端)

namespace lubancode::cli {

// 列表一行的展示数据(中立形状:cli 不反向依赖 agent,由接线层从
// 索引摘要 转过来;相对时间文字也由接线层算好带进来)。
struct SessionPickerEntry {
    std::string id;
    std::string title;           // 没设标题就空,展示层回退 preview
    std::string preview;         // 镇句预览(截不截由渲染层做,这里给原文)
    std::string cwd;             // 原样;窄窗时先收这格
    std::string updated_ago;     // 相对时间文字("1m ago" 那类,接线层算)
    std::string created_ago;     // 同上(排序为 Created 时用)
    bool damaged = false;        // 坏档:行尾标 damaged,照样能选(Enter 后
                                 // 由 resume 路报"认不得格式",不在这拦)
    // Ctrl+E 展开详情(接线层从 SessionSummary 直转,不用额外读盘):
    // created/updated 用存档侧稳定串,模型名原样;空串由渲染层回退占位。
    std::string created_at;      // "yyyy-mm-dd HH:MM:SS"
    std::string updated_at;
    std::string model;
    std::size_t message_count = 0;
};

// 筛选/排序两枚开关。
enum class SessionPickerScope { Cwd, All };
enum class SessionPickerSort { Updated, Created };

// Tab 轮换的三格焦点。
enum class SessionPickerFocus { Search, Filter, Sort };

// 查看态(第三步):紧凑/舒展只改画法;展开钉在"当前选中行";转录是
// 一块独立浮层(看完原路回列表,选中行不动)。
enum class SessionPickerLayout { Compact, Comfortable };

// 搜索命中规则:title/preview/id/cwd 四路,ASCII 不分大小写,中文按原字
// (与 索引查询口径 同一口径;这里对着喂进来的行数据再筛
// 一遍,免得接线层漏筛)。
bool SessionPickerMatches(const SessionPickerEntry& entry, const std::string& search);

// 控制器状态机。只持 query 形状(焦点/搜索/筛选/排序/选中 id/视口),
// 不持有数据——数据变化(重扫、翻页取回)由接线层调 SetEntries 重装,
// 选中项按 id 尽量留住(单子"产品定案二":它消失了才落到最近一行)。
class SessionPickerCore {
public:
    struct State {
        SessionPickerFocus focus = SessionPickerFocus::Search;
        std::string search;
        SessionPickerScope scope = SessionPickerScope::Cwd;
        SessionPickerSort sort = SessionPickerSort::Updated;
        SessionPickerLayout layout = SessionPickerLayout::Compact;  // Ctrl+O
        bool expanded = false;   // Ctrl+E:选中行的详情摊开(再按收起)
        bool transcript_open = false;  // Ctrl+T:转录浮层开着
        bool submitted = false;
        bool cancelled = false;
    };

    // visible_capacity:视口能摆几行(高度/resize 变了调 SetCapacity)。
    SessionPickerCore(std::size_t visible_capacity);
    const State& state() const { return state_; }
    State& state() { return state_; }  // 接线层设初值用(筛选/排序初值)

    // 装数据。prefer_id 非空时选中它(不在命中里就落到最近一行);
    // 重装后若命中为空,选位归零。返回当前选中的条目(没有命中给 nullptr)。
    const SessionPickerEntry* SetEntries(std::vector<SessionPickerEntry> entries,
                                         const std::string& prefer_id = std::string());
    // 视口容量变化(resize)。
    void SetCapacity(std::size_t visible_capacity);

    // 命中表(筛选/搜索后的行,顺序即展示序)。
    const std::vector<SessionPickerEntry>& matches() const { return matches_; }
    // 选中行在 matches 里的下标;命中为空时无意义(selected_ 恒 0)。
    std::size_t selected() const { return selected_; }
    // 视口首行在 matches 里的下标(窗口账)。
    std::size_t viewport_top() const { return viewport_top_; }
    // 选中条目(没有命中给 nullptr)。
    const SessionPickerEntry* SelectedEntry() const;

    // 喂一个键。search 焦点下可打字/退格;filter/sort 焦点下左右改选项;
    // Tab/ShiftTab 轮焦点;上下浏览;PageUp/PageDown 翻页;Home/End 到头尾;
    // Enter 提交;Esc/Ctrl+C/Ctrl+D 取消。查看态:Ctrl+O 切紧凑/舒展(只改
    // 画法,不动筛选与选中);Ctrl+E 摊开/收起选中行详情;Ctrl+T 开转录
    // 浮层(转录开着时 Esc/Ctrl+T/Ctrl+E 收浮层回列表,Enter 仍提交,
    // 其余键落空——浏览键不动选中行,看完回原行)。
    const State& HandleKey(const KeyEvent& event);

    // 视口行:从 matches 切出 [viewport_top_, viewport_top_+capacity)。
    std::vector<std::size_t> VisibleRows() const;

private:
    void Refilter(const std::string& prefer_id);
    void ClampSelection();
    void MoveSelection(std::size_t index);

    State state_;
    std::vector<SessionPickerEntry> entries_;  // 全量(当前 scope 下接线层给的)
    std::vector<SessionPickerEntry> matches_;  // 筛选/搜索后
    std::size_t selected_ = 0;
    std::size_t viewport_top_ = 0;
    std::size_t capacity_ = 1;
};

// ---------------------------------------------------------------------------
// 渲染(纯文本行,不夹 ANSI;宽度截断归 TruncateUtf8ToDisplayWidth)
// ---------------------------------------------------------------------------

// 一帧的行。行序:标题、搜索行、Filter/Sort 行、列表行(含空态)、
// 底栏(键位 + 序号/总数/百分比)。selected/highlight 非空时由终端层
// 上色——这里只管文本与列宽。width 是可用列数。
struct SessionPickerFrame {
    std::vector<std::string> lines;
    std::vector<std::size_t> row_match_index;  // 列表区每行对应 matches 下标;
                                               // 非列表行用 kNoMatch
    static constexpr std::size_t kNoMatch = static_cast<std::size_t>(-1);
};

SessionPickerFrame BuildSessionPickerFrame(const SessionPickerCore& core, int width);

// 转录浮层一帧(Ctrl+T)。excerpt_lines 由接线层按需读档拼好(大文件取
// 头尾若干行);这里只排版:标题行 + 内容行 + 底栏。滚动归接线层
// (excerpt 已是当前窗口要显示的那段),浮层本身不记滚动账——看完
// Esc 回列表,选中行原样。
SessionPickerFrame BuildSessionTranscriptFrame(const std::string& title_line,
                                               const std::vector<std::string>& excerpt_lines,
                                               int width);

// 相对时间(now 距 updated 的差):<60s "just now"、<60m "Nm ago"、
// <24h "Nh ago"、再久 "Nd ago"。相对时间只在渲染层算(单子"代码边界"
// 定的),协议层留稳定时间串。now_epoch/then_epoch 都是 epoch 秒。
std::string FormatSessionAgo(long long now_epoch, long long then_epoch);

// 百分比:选中第 selected(0 基)/ 共 total。空表给 0。
int SessionPickerScrollPercent(std::size_t selected, std::size_t total);

}  // namespace lubancode::cli
