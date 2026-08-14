// 子代理面板纯逻辑(0.28.x"面板移到输入框上方"一单):窗口化布局、按键
// 状态机(x 停止/清除四分支、Ctrl+X Ctrl+K 两段确认)、输入框上横线右端的
// 代理短标签截断、上方行进帧后的 diff 行为。全部不碰终端——终端层的接线
// 由 screen driver(手动)与 test_repaint_coord(协调层)各管各的。

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "cli/agent_panel.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/terminal_frame.hpp"
#include "platform/terminal_batch.hpp"

using namespace lubancode::cli;
using PHalf = std::chrono::steady_clock::time_point;

namespace {

std::vector<AgentPanelEntry> MakeAgents(int count) {
    std::vector<AgentPanelEntry> out;
    for (int i = 1; i <= count; ++i) {
        AgentPanelEntry entry;
        entry.task_id = i;
        entry.name = "general-purpose #" + std::to_string(i);
        entry.description = "任务 " + std::to_string(i);
        entry.state = "运行中(0 次工具调用 · 0 tokens · 1s)";
        entry.running = true;
        out.push_back(std::move(entry));
    }
    return out;
}

PHalf Now() { return std::chrono::steady_clock::now(); }

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// -----------------------------------------------------------------------
// 布局:0/1/3/16 只、窗口化、计数行
// -----------------------------------------------------------------------

TEST_CASE("面板布局:0 只子代理没有面板行") {
    SetLanguage("zh");
    const auto layout = LayoutAgentPanel({}, 0, false, false, {}, 0, 0, 80, false);
    CHECK(layout.lines.empty());
}

TEST_CASE("面板布局:1/3 只全显,main 固定第 0 行") {
    SetLanguage("zh");
    const auto one = LayoutAgentPanel(MakeAgents(1), 0, false, false, {}, 0, 0, 80, false);
    REQUIRE(one.lines.size() == 3);  // 提示行 + main + 1 只
    CHECK(Contains(one.lines[1], "main"));

    const auto three = LayoutAgentPanel(MakeAgents(3), 0, false, false, {}, 0, 0, 80, false);
    REQUIRE(three.lines.size() == 5);  // 提示行 + main + 3 只
    CHECK(three.total_count == 4);
    CHECK(three.hidden_above == 0);
    CHECK(three.hidden_below == 0);
}

TEST_CASE("面板布局:16 只不限高时全显,没有未展示计数") {
    SetLanguage("zh");
    const auto layout = LayoutAgentPanel(MakeAgents(16), 0, false, false, {}, 0, 0, 80, false);
    CHECK(layout.lines.size() == 1 + 17);
    CHECK(layout.total_count == 17);
    CHECK(layout.hidden_above == 0);
    CHECK(layout.hidden_below == 0);
}

TEST_CASE("面板布局:窗口围着选中开,首尾都不丢") {
    SetLanguage("zh");
    const auto agents = MakeAgents(16);
    // 选中 0:窗口贴顶,下方写清还有几只。
    auto head = LayoutAgentPanel(agents, 0, true, false, {}, 5, 0, 80, false);
    CHECK(head.visible_first == 0);
    CHECK(head.visible_count == 5);
    CHECK(head.hidden_above == 0);
    CHECK(head.hidden_below == 12);
    REQUIRE(head.lines.size() >= 2);
    CHECK(Contains(head.lines[1], "17"));  // 总数写在窗口计数行
    // 选中 16(最后一只):窗口贴底。
    auto tail = LayoutAgentPanel(agents, 16, true, false, {}, 5, 0, 80, false);
    CHECK(tail.visible_first == 12);
    CHECK(tail.hidden_above == 12);
    CHECK(tail.hidden_below == 0);
    // 选中 8:窗口居中跟着走。
    auto middle = LayoutAgentPanel(agents, 8, true, false, {}, 5, 0, 80, false);
    CHECK(middle.visible_first == 6);
    CHECK(middle.hidden_above == 6);
    CHECK(middle.hidden_below == 6);
    // 窗口里必含选中那条:第 0 行是提示、第 1 行是计数,列表从第 2 行起。
    CHECK(Contains(middle.lines[2 + (8 - 6)], "\xE2\x9D\xAF"));  // ❯ 选中标记
}

TEST_CASE("面板布局:选中标记只在焦点态画;查看态详情行缀在列表后") {
    SetLanguage("zh");
    const auto agents = MakeAgents(2);
    const auto unfocused = LayoutAgentPanel(agents, 1, false, false, {}, 0, 0, 80, false);
    for (const auto& line : unfocused.lines) {
        CHECK_FALSE(Contains(line, "\xE2\x9D\xAF"));  // ❯
    }
    std::vector<std::string> detail{"任务说明: 查调用链", "工具 1: read_file"};
    const auto viewing = LayoutAgentPanel(agents, 2, true, true, detail, 0, 0, 80, false);
    bool saw_detail = false;
    for (const auto& line : viewing.lines) {
        if (Contains(line, "查调用链")) {
            saw_detail = true;
        }
    }
    CHECK(saw_detail);
    CHECK(viewing.lines.size() == 1 + 3 + 1 + 2);  // 提示 + main/2 只 + 详情提示 + 2 行
}

TEST_CASE("面板布局:两段确认第一段按下时,首行提示换成确认话") {
    SetLanguage("zh");
    const auto agents = MakeAgents(1);
    const auto calm = LayoutAgentPanel(agents, 0, false, false, {}, 0, 0, 80, false);
    const auto armed = LayoutAgentPanel(agents, 0, false, false, {}, 0, 0, 80, true);
    CHECK(calm.lines[0] != armed.lines[0]);
    CHECK(Contains(armed.lines[0], "Ctrl+K"));
}

TEST_CASE("面板布局:锚点上方空间不够时开窗截详情,首行提示永不丢") {
    SetLanguage("zh");
    const auto agents = MakeAgents(6);
    // 总预算 7 行:提示(1) + 条目(5) + 计数(1)——6 只代理放不满,数清就行。
    const auto windowed = LayoutAgentPanel(agents, 0, false, false, {}, 0, 7, 80, false);
    CHECK(windowed.lines.size() == 7);
    CHECK(Contains(windowed.lines[0], "Ctrl+X Ctrl+K"));  // 提示行保住
    CHECK(windowed.visible_count == 5);
    CHECK(windowed.hidden_below == 2);
    CHECK(Contains(windowed.lines[1], "7"));  // 总数写明

    // 详情超预算:条目让位给详情,详情保头部(任务说明优先),末行写清
    // 未展示行数,整块不超预算。
    std::vector<std::string> long_detail;
    for (int i = 0; i < 10; ++i) {
        long_detail.push_back("详情行 " + std::to_string(i));
    }
    const auto detail_view = LayoutAgentPanel(agents, 1, true, true, long_detail, 0, 12, 80, false);
    CHECK(Contains(detail_view.lines[0], "Ctrl+X Ctrl+K"));  // 提示行还在
    bool saw_head = false;
    for (const auto& line : detail_view.lines) {
        saw_head = saw_head || Contains(line, "详情行 0");
    }
    CHECK(saw_head);
    CHECK(detail_view.lines.back().find("未展示") != std::string::npos);
    CHECK(detail_view.lines.size() == 12);  // 不超预算

    // 连提示行都摆不下(预算 < 2):整块不画,不挤输入框。
    const auto none = LayoutAgentPanel(agents, 0, false, false, {}, 0, 1, 80, false);
    CHECK(none.lines.empty());
}

// -----------------------------------------------------------------------
// 按键状态机
// -----------------------------------------------------------------------

TEST_CASE("状态机:上下进入焦点并环绕,Enter 进查看态,Esc 两层退出") {
    SetLanguage("zh");
    AgentPanelController c;
    const int total = 4;  // main + 3 只

    auto out = c.HandleKey(PanelKey::Down, total, true, Now());
    CHECK(out.consumed);
    CHECK(c.focused());
    CHECK(c.selected() == 1);

    out = c.HandleKey(PanelKey::Up, total, true, Now());
    CHECK(c.selected() == 0);  // 环绕回 main
    out = c.HandleKey(PanelKey::Up, total, true, Now());
    CHECK(c.selected() == total - 1);

    out = c.HandleKey(PanelKey::EnterView, total, true, Now());
    CHECK(out.consumed);
    CHECK(c.detail_open());
    CHECK(c.target_index().has_value());
    CHECK(*c.target_index() == total - 1);

    out = c.HandleKey(PanelKey::Esc, total, true, Now());  // 先退查看态
    CHECK(out.consumed);
    CHECK_FALSE(c.detail_open());
    CHECK_FALSE(c.target_index().has_value());
    CHECK(c.focused());

    out = c.HandleKey(PanelKey::Esc, total, true, Now());  // 再退焦点
    CHECK(out.consumed);
    CHECK_FALSE(c.focused());
    CHECK(c.selected() == 0);
}

TEST_CASE("状态机:正文非空时上下/Enter 都还给 composer,字母 x 只进 composer") {
    SetLanguage("zh");
    AgentPanelController c;
    const int total = 3;
    c.HandleKey(PanelKey::Down, total, true, Now());  // 先正常进焦点、选中 1

    const auto up = c.HandleKey(PanelKey::Up, total, /*composer_empty=*/false, Now());
    CHECK_FALSE(up.consumed);
    const auto enter = c.HandleKey(PanelKey::EnterView, total, false, Now());
    CHECK_FALSE(enter.consumed);
    const auto stop = c.HandleKey(PanelKey::StopEntry, total, false, Now());
    CHECK_FALSE(stop.consumed);
    CHECK_FALSE(stop.stop_current);
    // 没有后台子代理时(只有 main),面板任何键都不消费。
    AgentPanelController alone;
    CHECK_FALSE(alone.HandleKey(PanelKey::Down, 1, true, Now()).consumed);
}

TEST_CASE("状态机:x 四分支——运行中停止、终态清除(应用层分派)、main 不接、打字不接") {
    SetLanguage("zh");
    AgentPanelController c;
    const int total = 3;
    // main 行:x 不消费(落回 composer,变成打了一个 x)。
    c.HandleKey(PanelKey::Down, total, true, Now());  // 选中 1
    c.HandleKey(PanelKey::Up, total, true, Now());    // 回 main(0)
    const auto on_main = c.HandleKey(PanelKey::StopEntry, total, true, Now());
    CHECK_FALSE(on_main.consumed);
    // 子代理行:x 消费并要求停止/清除当前条目。
    c.HandleKey(PanelKey::Down, total, true, Now());  // 选中 1
    const auto on_agent = c.HandleKey(PanelKey::StopEntry, total, true, Now());
    CHECK(on_agent.consumed);
    CHECK(on_agent.stop_current);
    CHECK(c.selected() == 1);  // 停止不清选中:等线程报终态再改灯
}

TEST_CASE("状态机:Ctrl+X Ctrl+K 两段确认——成功、超时、Esc 撤销、错键撤销") {
    SetLanguage("zh");
    // 成功。
    AgentPanelController c;
    const int total = 2;
    const auto arm = c.HandleKey(PanelKey::StopAllArm, total, true, Now());
    CHECK(arm.consumed);
    CHECK(c.stop_all_armed());
    const auto confirm = c.HandleKey(PanelKey::StopAllConfirm, total, true, Now());
    CHECK(confirm.consumed);
    CHECK(confirm.stop_all);
    CHECK_FALSE(c.stop_all_armed());

    // 超时:2.5 秒后 Ctrl+K 不再是确认。
    AgentPanelController slow;
    slow.HandleKey(PanelKey::StopAllArm, total, true, Now());
    CHECK(slow.ExpireArmed(Now() + std::chrono::milliseconds(2500)));
    CHECK_FALSE(slow.stop_all_armed());
    const auto late = slow.HandleKey(PanelKey::StopAllConfirm, total, true, Now());
    CHECK_FALSE(late.stop_all);

    // Esc 撤销第一段。
    AgentPanelController esc;
    esc.HandleKey(PanelKey::StopAllArm, total, true, Now());
    const auto esc_out = esc.HandleKey(PanelKey::Esc, total, true, Now());
    CHECK_FALSE(esc.stop_all_armed());
    CHECK_FALSE(esc_out.stop_all);

    // 别键(比如上下)也撤销第一段,且那枚键按原语义走。
    AgentPanelController wrong;
    wrong.HandleKey(PanelKey::StopAllArm, total, true, Now());
    const auto wrong_out = wrong.HandleKey(PanelKey::Down, total, true, Now());
    CHECK_FALSE(wrong.stop_all_armed());
    CHECK(wrong_out.consumed);  // Down 仍是选择
    CHECK_FALSE(wrong_out.stop_all);

    // 正文非空时第一段也不启(组合键只在面板可控制态生效)。
    AgentPanelController typing;
    const auto typing_arm = typing.HandleKey(PanelKey::StopAllArm, total, false, Now());
    CHECK_FALSE(typing_arm.consumed);
    CHECK_FALSE(typing.stop_all_armed());
}

TEST_CASE("状态机:条目增减修正选中;目标被清理强制收起") {
    SetLanguage("zh");
    AgentPanelController c;
    c.HandleKey(PanelKey::Down, 5, true, Now());  // 选中 1
    c.HandleKey(PanelKey::EnterView, 5, true, Now());
    REQUIRE(c.target_index().has_value());
    // 任务从 4 只收到 2 只(total=3):选中收回界内,查看态保得住(条目还在)。
    c.OnEntriesChanged(3);
    CHECK(c.selected() <= 2);
    // 子代理全没了:面板消失,状态收干净。
    c.OnEntriesChanged(1);
    CHECK_FALSE(c.focused());
    CHECK_FALSE(c.detail_open());
    CHECK_FALSE(c.target_index().has_value());
    // 目标条目被 x 清掉:CloseView 强制收起。
    AgentPanelController d;
    d.HandleKey(PanelKey::Down, 3, true, Now());
    d.HandleKey(PanelKey::EnterView, 3, true, Now());
    d.CloseView();
    CHECK_FALSE(d.detail_open());
    CHECK_FALSE(d.target_index().has_value());
}

// -----------------------------------------------------------------------
// 上横线右端的代理短标签
// -----------------------------------------------------------------------

TEST_CASE("标签:挂在不短于保底的横线右端,整行恰为 width-1 列") {
    const std::string line = BuildRuleWithTag("", "", "修 ask_user 被遮挡", 80);
    CHECK(Contains(line, "修 ask_user 被遮挡"));
    CHECK(line.find('\x1b') == std::string::npos);  // plain 主题不夹 ANSI
    CHECK(DisplayWidthUtf8(line) == 79);
}

TEST_CASE("标签:超长短述按宽截断,先保横线与提示符") {
    const std::string long_tag = "这一段任务说明写得特别长特别长,长得把横线都快挤没了还得继续写";
    const std::string line = BuildRuleWithTag("", "", long_tag, 40);
    CHECK(DisplayWidthUtf8(line) == 39);
    // 整段塞不下:尾巴的字被截掉,不撑破行宽。
    CHECK_FALSE(Contains(line, "还得继续写"));
    // 横线至少还剩 kMinRuleCols 格(第一个空格之前全是横线)。
    const std::size_t space = line.find(' ');
    REQUIRE(space != std::string::npos);
    CHECK(DisplayWidthUtf8(line.substr(0, space)) >= static_cast<std::size_t>(kMinRuleCols));
}

TEST_CASE("标签:窄终端塞不下就退回无标签横线;无标签时就是普通满宽横线") {
    const std::string narrow = BuildRuleWithTag("", "", "修遮挡", 10);
    CHECK(narrow.find("修遮挡") == std::string::npos);
    CHECK(DisplayWidthUtf8(narrow) == 9);
    const std::string empty_tag = BuildRuleWithTag("", "", "", 80);
    CHECK(DisplayWidthUtf8(empty_tag) == 79);
    CHECK(Contains(empty_tag, "-"));
}

TEST_CASE("标签:彩色主题横线带色码,标签本身不夹 ANSI,截断仍按显示宽算") {
    const std::string stats = "\x1b[90m";
    const std::string reset = "\x1b[0m";
    const std::string line = BuildRuleWithTag(stats, reset, "查调用链", 60);
    CHECK(Contains(line, stats));
    // ANSI 之外的实际显示宽度仍按 width-1 收口(去掉色码再量)。
    std::string plain;
    bool in_escape = false;
    for (char c : line) {
        if (c == '\x1b') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if (c == 'm') {
                in_escape = false;
            }
            continue;
        }
        plain += c;
    }
    CHECK(DisplayWidthUtf8(plain) == 59);
}

// -----------------------------------------------------------------------
// 上方行进帧后的 diff:面板增减不重写未变的 composer 行
// -----------------------------------------------------------------------

TEST_CASE("帧 diff:面板行加在帧顶,composer 行未变就不重写") {
    InlineFrame small;
    small.rows.push_back(InlineFrameRow{0, 80, true, "----"});
    small.rows.push_back(InlineFrameRow{0, 80, false, "> "});
    small.cursor_x = 2;
    small.cursor_row = 1;
    lubancode::platform::TerminalBatch diff_batch(0, 0);
    InlineFrame grown = small;
    grown.rows.insert(grown.rows.begin(), InlineFrameRow{0, 80, false, "agent #1"});
    grown.cursor_row = 2;
    const auto stats = QueueInlineFrameDiff(diff_batch, &small, grown, 10);
    REQUIRE(stats.emitted);
    CHECK(stats.changed_rows <= 3);  // 新增面板行 + 整体下移,没有多余重写
}

TEST_CASE("帧 diff:面板收走时旧行被清,不留残骸") {
    InlineFrame grown;
    grown.rows.push_back(InlineFrameRow{0, 80, false, "agent #1"});
    grown.rows.push_back(InlineFrameRow{0, 80, true, "----"});
    grown.cursor_x = 2;
    grown.cursor_row = 1;
    InlineFrame small;
    small.rows.push_back(InlineFrameRow{0, 80, true, "----"});
    small.cursor_x = 2;
    small.cursor_row = 0;
    lubancode::platform::TerminalBatch diff_batch(0, 0);
    const auto stats = QueueInlineFrameDiff(diff_batch, &grown, small, 10);
    CHECK(stats.emitted);
    CHECK(stats.changed_rows >= 1);  // 第 0 行(agent)必须被清
}
