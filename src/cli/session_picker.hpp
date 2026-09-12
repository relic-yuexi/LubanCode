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
#include <cstdint>
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
    // run_kind 账上没写(v3 老档,session.started 缺 runKind):行尾标
    // "(种类未知)",不暗当 main_session(R2)。
    bool run_kind_unknown = false;
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

// ---------------------------------------------------------------------------
// Ctrl+T 转录浮层的游标分页(P3 第二棒):一页行 + 两个方向的"还有货"。
// 取数归接线层(provider 回调,见 session_picker_panel.hpp);这里只有
// 中立数据形状与滚动账。v3 会话按 seq 游标翻页(不反复全量重读);
// v2 会话照旧头尾截断,一页给全,游标恒空(没有第二页)。
// ---------------------------------------------------------------------------

// 一页取数请求:两游标皆空 = 首开(从最新一页起);before_seq = 向更旧
// 翻(取 seq 更小的最近一页);after_seq = 向更新翻。游标一次给一枚。
struct SessionTranscriptPageQuery {
    std::string session_id;
    std::optional<std::uint64_t> before_seq;
    std::optional<std::uint64_t> after_seq;
    std::size_t max_lines = 40;  // 一页行数上限(0 = 不限)
};

// 一页转录行(旧→新,时间线原序)。lines 为空且无游标 = 没有可显示的
// 转录(接线层给空态画面)。
struct SessionTranscriptPage {
    std::vector<std::string> lines;
    bool has_older = false;
    bool has_newer = false;
    std::optional<std::uint64_t> oldest_seq;  // 本页首行 seq(向旧翻的游标)
    std::optional<std::uint64_t> newest_seq;  // 本页末行 seq(向新翻的游标)
};

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

// diagnostic 非空 = 数据源读取有障碍(空列表不是"真空"):空态画面报
// 读取失败而不是"还没有会话"(Resume 接入 v3 单 R2)。
SessionPickerFrame BuildSessionPickerFrame(const SessionPickerCore& core, int width,
                                            const std::string& diagnostic = std::string());

// 转录浮层一帧(Ctrl+T)。excerpt_lines 由接线层按需读档拼好(大文件取
// 头尾若干行);这里只排版:标题行 + 内容行 + 底栏。滚动归接线层
// (excerpt 已是当前窗口要显示的那段),浮层本身不记滚动账——看完
// Esc 回列表,选中行原样。
//
// P3 第二棒:行可滚可翻页后,底栏多一枚位置提示(更旧/更新还有货);
// hint 缺省 = 旧版定长摘要的画法(无滚动键提示),v2 会话不变样。
struct SessionTranscriptScrollHint {
    bool scrollable = false;  // 行已多于一屏(有滚动键提示)
    bool has_older = false;   // 向上翻还有更旧的可取
    bool has_newer = false;   // 向下翻还有更新的可取
};
SessionPickerFrame BuildSessionTranscriptFrame(const std::string& title_line,
                                               const std::vector<std::string>& excerpt_lines,
                                               int width,
                                               const SessionTranscriptScrollHint& hint = {});

// 转录浮层的滚动账(P3 第二棒):已取回的行(旧→新)+ 视口位置 + 两个
// 方向的"还有货"。面板层把 provider 取回的页喂进来(首开 LoadInitial,
// 触边补页 AppendOlder/AppendNewer),滚动键经 HandleKey 挪视口;要不要
// 补页由 NeedsOlderPage/NeedsNewerPage 暴露,取数(游标)归面板层。
// 前插补页时滚动位平移(画面钉在原顶行,不跳);后接补页时若原本贴底,
// 跟到新底。纯逻辑,测试钉在这层。
class SessionTranscriptScroller {
public:
    // 首开:整页从最新端来(接线层首查不带游标),视口钉在页底(先看
    // 最新的,与旧版"尾部摘要"顺眼一致)。空页也照收(空态画面)。
    void LoadInitial(const SessionTranscriptPage& page);
    // 换场/重开:清账。
    void Reset();
    // 视口行数变化(resize)。
    void SetViewportRows(std::size_t rows);

    // 滚动键(Up/Down/PageUp/PageDown/Home/End)挪视口;其余键返回
    // false(面板层放行给 core,浮层里 core 自有落空规矩)。
    bool HandleKey(KeyKind key);

    // 触边且 provider 说还有货 → 该向该方向补一页(游标见 OlderCursor/
    // NewerCursor;provider 回了空页就照喂 AppendXxx,账自会封口)。
    bool NeedsOlderPage() const;
    bool NeedsNewerPage() const;
    std::optional<std::uint64_t> OlderCursor() const;
    std::optional<std::uint64_t> NewerCursor() const;

    // 前插/后接一页。空页封掉该方向的"还有货"(不再追问);前插后滚动
    // 位平移 N(画面不跳),后接时若原本贴底则跟到新底。
    void AppendOlder(const SessionTranscriptPage& page);
    void AppendNewer(const SessionTranscriptPage& page);

    // 已取回的全部行(旧→新)与视口首行下标。
    const std::vector<std::string>& lines() const { return lines_; }
    std::size_t scroll() const { return scroll_; }
    // 当前视口要画的行(从 scroll_ 起切 viewport_rows 行)。
    std::vector<std::string> VisibleLines() const;
    // 画帧用的位置提示。
    SessionTranscriptScrollHint hint() const;

private:
    void ClampScroll();
    bool AtBottom() const;

    std::vector<std::string> lines_;
    bool has_older_ = false;
    bool has_newer_ = false;
    std::optional<std::uint64_t> oldest_seq_;
    std::optional<std::uint64_t> newest_seq_;
    std::size_t scroll_ = 0;      // 视口首行在 lines_ 里的下标
    std::size_t viewport_rows_ = 1;
};

// 相对时间(now 距 updated 的差):<60s "just now"、<60m "Nm ago"、
// <24h "Nh ago"、再久 "Nd ago"。相对时间只在渲染层算(单子"代码边界"
// 定的),协议层留稳定时间串。now_epoch/then_epoch 都是 epoch 秒。
std::string FormatSessionAgo(long long now_epoch, long long then_epoch);

// 百分比:选中第 selected(0 基)/ 共 total。空表给 0。
int SessionPickerScrollPercent(std::size_t selected, std::size_t total);

}  // namespace lubancode::cli
