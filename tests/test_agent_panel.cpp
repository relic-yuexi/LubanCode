// 子代理导航坞纯逻辑(0.29.x"导航贴底并整帧去重"一单):折叠+窗口化布局、
// 结构化行三列渲染(身份/中段/右状态)、按键状态机(x 停止/清除四分支、
// Ctrl+X Ctrl+K 两段确认、闲置汇总展开/收起)、输入框上横线右端的代理短
// 标签截断、上方行进帧后的 diff 行为。全部不碰终端——终端层的接线由
// screen driver(手动)与 test_repaint_coord(协调层)各管各的。

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
        entry.title = "任务 " + std::to_string(i);
        entry.state = "运行中(0 次工具调用 · 0 tokens · 1s)";
        entry.running = true;
        out.push_back(std::move(entry));
    }
    return out;
}

// 闲置(完成)条目:不 running、不 failed——折叠规矩只认这种。
std::vector<AgentPanelEntry> MakeIdleAgents(int count) {
    std::vector<AgentPanelEntry> out;
    for (int i = 1; i <= count; ++i) {
        AgentPanelEntry entry;
        entry.task_id = i;
        entry.name = "general-purpose #" + std::to_string(i);
        entry.title = "任务 " + std::to_string(i);
        entry.state = "完成(3 次工具调用 · 1k tokens · 5s)";
        entry.running = false;
        entry.failed = false;
        out.push_back(std::move(entry));
    }
    return out;
}

PHalf Now() { return std::chrono::steady_clock::now(); }

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += line + "\n";
    }
    return joined;
}

// ids 表:task id 不必连续(清理/混排后常有空洞),按给定顺序就是列表顺序。
std::vector<int> Ids(std::initializer_list<int> ids) { return std::vector<int>(ids); }

}  // namespace

// -----------------------------------------------------------------------
// 布局:0/1/3 只、窗口化、计数行
// -----------------------------------------------------------------------

TEST_CASE("坞布局:0 只子代理整坞消失,没有孤零零的操作提示") {
    SetLanguage("zh");
    const auto layout = LayoutAgentDock({}, 0, false, 0, 0, 80, false, false, false);
    CHECK(layout.rows.empty());
    CHECK(layout.navigation_ids.size() == 1);  // 只剩 main
}

TEST_CASE("坞布局:1/3 只全显,main 固定导航表第 0 项") {
    SetLanguage("zh");
    const auto one = LayoutAgentDock(MakeAgents(1), 0, false, 0, 0, 80, false, false, false);
    REQUIRE(one.rows.size() == 3);  // 提示行 + main + 1 只
    CHECK(one.navigation_ids == Ids({0, 1}));
    const auto one_lines = RenderAgentDockLines(one, 80);
    CHECK(Contains(one_lines[1], "main"));

    const auto three = LayoutAgentDock(MakeAgents(3), 0, false, 0, 0, 80, false, false, false);
    REQUIRE(three.rows.size() == 5);  // 提示行 + main + 3 只
    CHECK(three.total_count == 4);
    CHECK(three.hidden_above == 0);
    CHECK(three.hidden_below == 0);
}

TEST_CASE("坞布局:16 只不限窗口时全显,没有未展示计数") {
    SetLanguage("zh");
    const auto layout = LayoutAgentDock(MakeAgents(16), 0, false, 0, 0, 80, false, false, false);
    CHECK(layout.rows.size() == 1 + 17);
    CHECK(layout.total_count == 17);
    CHECK(layout.hidden_above == 0);
    CHECK(layout.hidden_below == 0);
}

TEST_CASE("坞布局:常态窗口最多单列 5 只,围着选中开,选中永不消失") {
    SetLanguage("zh");
    const auto agents = MakeAgents(16);
    // 选中 0:main + 窗口贴顶的 5 只代理。
    auto head = LayoutAgentDock(agents, 0, true, 5, 0, 80, false, false, false);
    CHECK(head.visible_first == 0);
    CHECK(head.visible_count == 6);  // main + 5 只代理
    CHECK(head.hidden_above == 0);
    CHECK(head.hidden_below == 11);
    const auto head_lines = RenderAgentDockLines(head, 80);
    REQUIRE(head_lines.size() >= 2);
    CHECK(Contains(head_lines[1], "17"));  // 总数写在窗口计数行
    // 选中 16(最后一只):窗口贴底。
    auto tail = LayoutAgentDock(agents, 16, true, 5, 0, 80, false, false, false);
    CHECK(tail.visible_first == 0);  // main 恒在
    CHECK(tail.hidden_above == 11);
    CHECK(tail.hidden_below == 0);
    // 选中 8:窗口居中跟着走,窗口里必含选中那条。
    auto middle = LayoutAgentDock(agents, 8, true, 5, 0, 80, false, false, false);
    CHECK(middle.hidden_above == 5);
    CHECK(middle.hidden_below == 6);
    const auto middle_lines = RenderAgentDockLines(middle, 80);
    // 行序:提示(0)/计数(1)/main(2)/代理自 3 起,窗口自导航 6 到 10。
    CHECK(Contains(middle_lines[3 + (8 - 6)], "\xE2\x9D\xAF"));  // ❯ 选中标记
}

TEST_CASE("坞布局:选中标记只在焦点态画;查看态不往坞里长详情") {
    SetLanguage("zh");
    const auto agents = MakeAgents(2);
    const auto unfocused = LayoutAgentDock(agents, 1, false, 0, 0, 80, false, false, false);
    for (const auto& line : RenderAgentDockLines(unfocused, 80)) {
        CHECK_FALSE(Contains(line, "\xE2\x9D\xAF"));  // ❯
    }
    // 查看态:坞仍只有 提示+main+条目 三段行——详情(任务说明/工具流水)由
    // 上方会话视口承接,导航坞绝不向下生长(规格"现场一")。
    const auto viewing = LayoutAgentDock(agents, 2, true, 0, 0, 80, false, false, false, 2);
    const auto viewing_lines = RenderAgentDockLines(viewing, 80);
    CHECK(viewing_lines.size() == 1 + 3);  // 提示 + main/2 只,一行不多
    CHECK(JoinLines(viewing_lines).find("任务说明") == std::string::npos);
    CHECK(JoinLines(viewing_lines).find("工具调用流水") == std::string::npos);
}

TEST_CASE("坞布局:正在查看的行换实心灯 ◉,不靠颜色") {
    SetLanguage("zh");
    const auto agents = MakeAgents(2);
    const auto viewing = LayoutAgentDock(agents, 2, true, 0, 0, 80, false, false, false, 2);
    const auto lines = RenderAgentDockLines(viewing, 80);
    REQUIRE(lines.size() >= 3);
    CHECK(Contains(lines[3], "\xE2\x97\x89"));  // ◉ 第 2 只正在查看
    CHECK_FALSE(Contains(lines[2], "\xE2\x97\x89"));  // 第 1 只仍是 ◌ 运行中
}

TEST_CASE("坞布局:两段确认第一段按下时,首行提示换成确认话") {
    SetLanguage("zh");
    const auto agents = MakeAgents(1);
    const auto calm = LayoutAgentDock(agents, 0, false, 0, 0, 80, false, false, false);
    const auto armed = LayoutAgentDock(agents, 0, false, 0, 0, 80, true, false, false);
    const auto calm_lines = RenderAgentDockLines(calm, 80);
    const auto armed_lines = RenderAgentDockLines(armed, 80);
    CHECK(calm_lines[0] != armed_lines[0]);
    CHECK(Contains(armed_lines[0], "Ctrl+K"));
}

TEST_CASE("坞布局:提示行随焦点收放;窄屏摘掉'停止全部'低频长文案") {
    SetLanguage("zh");
    const auto agents = MakeAgents(1);
    const auto idle = LayoutAgentDock(agents, 0, false, 0, 0, 100, false, false, false);
    const auto idle_lines = RenderAgentDockLines(idle, 100);
    CHECK(Contains(idle_lines[0], "Ctrl+X Ctrl+K"));  // 宽屏未聚焦:全套
    const auto idle_narrow = RenderAgentDockLines(
        LayoutAgentDock(agents, 0, false, 0, 0, 60, false, false, false), 60);
    CHECK_FALSE(Contains(idle_narrow[0], "Ctrl+X Ctrl+K"));
    const auto focused = RenderAgentDockLines(
        LayoutAgentDock(agents, 1, true, 0, 0, 100, false, false, false), 100);
    CHECK(Contains(focused[0], "Esc"));  // 已选中才添 Esc 返回
    const auto stream = RenderAgentDockLines(
        LayoutAgentDock(agents, 0, false, 0, 0, 100, false, true, false), 100);
    const auto idle_hint = RenderAgentDockLines(
        LayoutAgentDock(agents, 0, false, 0, 0, 100, false, false, false), 100);
    CHECK(stream[0] != idle_hint[0]);  // 流式版提示与空闲版不同
    SetLanguage("en");
    const auto stream_en = RenderAgentDockLines(
        LayoutAgentDock(agents, 0, false, 0, 0, 100, false, true, false), 100);
    CHECK(Contains(stream_en[0], "Esc"));
    SetLanguage("zh");
}

TEST_CASE("坞布局:列表行只认真正 title,prompt 片段绝不出现(歪路封死)") {
    SetLanguage("zh");
    AgentPanelEntry entry;
    entry.task_id = 1;
    entry.name = "general-purpose #1";
    entry.title = "项目记忆升级一期";
    entry.state = "运行中(7 次工具调用 · 17.0k tokens · 43.4s)";
    entry.running = true;
    const auto layout = LayoutAgentDock({entry}, 1, true, 0, 0, 100, false, false, false);
    const auto joined = JoinLines(RenderAgentDockLines(layout, 100));
    CHECK(Contains(joined, "项目记忆升级一期"));
    CHECK(joined.find("你在一个 C++ 项目的隔离") == std::string::npos);
}

TEST_CASE("坞布局:矮屏预算开窗,首行提示永不丢") {
    SetLanguage("zh");
    const auto agents = MakeAgents(6);
    // 总预算 7 行:提示(1) + 条目(5) + 计数(1)——6 只代理放不满,数清就行。
    const auto windowed = LayoutAgentDock(agents, 0, false, 0, 7, 80, false, false, false);
    CHECK(windowed.rows.size() == 7);
    const auto windowed_lines = RenderAgentDockLines(windowed, 80);
    CHECK(Contains(windowed_lines[0], "Enter"));  // 提示行保住
    CHECK(windowed.visible_count == 5);
    CHECK(windowed.hidden_below == 2);
    CHECK(Contains(windowed_lines[1], "7"));  // 总数写明

    // 预算更紧(4 行):提示 + main + 2 只,绝无第四段。
    const auto tight = LayoutAgentDock(agents, 0, false, 0, 4, 80, false, false, false);
    CHECK(tight.rows.size() == 4);
    CHECK(Contains(RenderAgentDockLines(tight, 80)[0], "Enter"));

    // 连提示行都摆不下(预算 < 2):整块不画,不挤输入框。
    const auto none = LayoutAgentDock(agents, 0, false, 0, 1, 80, false, false, false);
    CHECK(none.rows.empty());
}

// -----------------------------------------------------------------------
// 闲置与终态收纳
// -----------------------------------------------------------------------

TEST_CASE("导航表:闲置(完成)最多单列三只,更多折成汇总哨兵;展开全回") {
    SetLanguage("zh");
    const auto agents = MakeIdleAgents(6);
    const auto collapsed = DockNavigationIds(agents, /*idle_expanded=*/false, 0);
    // 前三只闲置 + 汇总哨兵(表不含 main,main 隐式算第 0 项)。
    CHECK(collapsed == Ids({1, 2, 3, kIdleSummaryTaskId}));
    const auto expanded = DockNavigationIds(agents, /*idle_expanded=*/true, 0);
    CHECK(expanded == Ids({1, 2, 3, 4, 5, 6}));
    // 三只以内不折。
    CHECK(DockNavigationIds(MakeIdleAgents(3), false, 0) == Ids({1, 2, 3}));
}

TEST_CASE("导航表:运行中/失败/正在查看的行永不折叠") {
    SetLanguage("zh");
    // 5 只闲置(本该只留 3 只 + 汇总),#5 正在查看:保住单列;折掉的只有 #4。
    std::vector<AgentPanelEntry> agents = MakeIdleAgents(5);
    const auto ids = DockNavigationIds(agents, false, /*viewed=*/5);
    CHECK(ids == Ids({1, 2, 3, kIdleSummaryTaskId, 5}));
    // 运行中/失败同例:6 只里 #2 运行、#4 失败,它们之外的闲置仍只留前三。
    auto mixed = MakeIdleAgents(6);
    mixed[1].running = true;
    mixed[3].failed = true;
    const auto mixed_ids = DockNavigationIds(mixed, false, 0);
    CHECK(mixed_ids == Ids({1, 2, 3, 4, 5, kIdleSummaryTaskId}));
}

TEST_CASE("坞布局:6 只闲置只列前三只与一行汇总,汇总不占 5 行窗口的代理位") {
    SetLanguage("zh");
    const auto agents = MakeIdleAgents(6);
    const auto layout = LayoutAgentDock(agents, 0, false, 5, 0, 80, false, false, false);
    CHECK(layout.idle_summary);
    CHECK(layout.hidden_idle == 3);
    const auto lines = RenderAgentDockLines(layout, 80);
    int summary_count = 0;
    for (const auto& line : lines) {
        if (Contains(line, "另有 3 只闲置代理")) {
            ++summary_count;
        }
    }
    CHECK(summary_count == 1);  // 汇总行至多一份
    CHECK(Contains(JoinLines(lines), "Enter 展开"));
    // 展开后:全量在列,没有汇总行。
    const auto open = LayoutAgentDock(agents, 0, false, 0, 0, 80, false, false, true);
    const auto open_lines = RenderAgentDockLines(open, 80);
    CHECK(open_lines.size() == 1 + 1 + 6);
    CHECK(JoinLines(open_lines).find("另有") == std::string::npos);
}

// -----------------------------------------------------------------------
// 三列渲染:身份/中段/右状态
// -----------------------------------------------------------------------

TEST_CASE("渲染:三列起点稳定——耗时刷新不改身份列与标题起点") {
    SetLanguage("zh");
    auto agents = MakeAgents(3);
    const auto before = LayoutAgentDock(agents, 0, false, 0, 0, 120, false, false, false);
    agents[1].state = "运行中(9 次工具调用 · 99.9k tokens · 512s)";
    const auto after = LayoutAgentDock(agents, 0, false, 0, 0, 120, false, false, false);
    const auto lines_before = RenderAgentDockLines(before, 120);
    const auto lines_after = RenderAgentDockLines(after, 120);
    REQUIRE(lines_before.size() == lines_after.size());
    for (std::size_t i = 0; i < lines_before.size(); ++i) {
        // 每行里身份名字与标题的起点(字节位)不动——状态变长只往右顶状态列,
        // 中段起点由 identity_width 定死,不随耗时抖动。
        CHECK(lines_before[i].find("任务 " + std::to_string(static_cast<int>(i))) ==
              lines_after[i].find("任务 " + std::to_string(static_cast<int>(i))));
    }
    CHECK(before.identity_width == after.identity_width);
}

TEST_CASE("渲染:右状态贴右,同行三段都在,整行不超屏宽") {
    SetLanguage("zh");
    const auto agents = MakeAgents(3);
    const auto layout = LayoutAgentDock(agents, 1, true, 0, 0, 100, false, false, false);
    const auto lines = RenderAgentDockLines(layout, 100);
    REQUIRE(lines.size() == 5);  // 提示 + main + 3 只
    const std::string& row = lines[2];  // 第 1 只
    CHECK(Contains(row, "general-purpose #1"));
    CHECK(Contains(row, "任务 1"));
    CHECK(Contains(row, "运行中"));
    CHECK(DisplayWidthUtf8(row) <= 99);
}

TEST_CASE("渲染:超长短标题按显示宽截断,宽字符/窄屏不撑破") {
    SetLanguage("zh");
    AgentPanelEntry entry;
    entry.task_id = 1;
    entry.name = "general-purpose #1";
    entry.title = "这一段任务说明写得特别长特别长,长得把整行都快挤没了还得继续写下去";
    entry.state = "运行中(1 次工具调用 · 1k tokens · 2s)";
    entry.running = true;
    for (const int width : {30, 60, 100}) {
        const auto layout = LayoutAgentDock({entry}, 1, true, 0, 0, width, false, false, false);
        for (const auto& line : RenderAgentDockLines(layout, width)) {
            CHECK(DisplayWidthUtf8(line) <= width - 1);  // UTF-8 不截裂,宽度不破
        }
    }
}

TEST_CASE("渲染:身份列钳位 4~28,长名字吞不掉全屏") {
    SetLanguage("zh");
    AgentPanelEntry entry;
    entry.task_id = 1;
    entry.name = "general-purpose-with-a-very-long-name #1234567890";
    entry.title = "标题";
    entry.state = "运行中";
    entry.running = true;
    const auto layout = LayoutAgentDock({entry}, 1, true, 0, 0, 120, false, false, false);
    CHECK(layout.identity_width <= 28);
    CHECK(layout.identity_width >= 4);
}

// -----------------------------------------------------------------------
// 按键状态机
// -----------------------------------------------------------------------

TEST_CASE("状态机:上下进入焦点并环绕,Enter 进查看态,Esc 两层退出") {
    SetLanguage("zh");
    AgentPanelController c;
    const auto ids = Ids({11, 22, 33});  // main + 3 只,task id 不连续

    auto out = c.HandleKey(PanelKey::Down, ids, true, Now());
    CHECK(out.consumed);
    CHECK(c.focused());
    CHECK(c.selected_index(ids) == 1);
    CHECK(c.selected_task_id() == 11);

    out = c.HandleKey(PanelKey::Up, ids, true, Now());
    CHECK(c.selected_index(ids) == 0);  // 环绕回 main
    CHECK(c.selected_task_id() == 0);
    out = c.HandleKey(PanelKey::Up, ids, true, Now());
    CHECK(c.selected_index(ids) == 3);
    CHECK(c.selected_task_id() == 33);

    out = c.HandleKey(PanelKey::EnterView, ids, true, Now());
    CHECK(out.consumed);
    CHECK(c.viewed_task_id() == 33);  // Enter 只切视图:设置 viewed_task_id
    REQUIRE(c.target_task_id().has_value());
    CHECK(*c.target_task_id() == 33);
    // 正在看的那只再按 Enter = 刷新,不 toggle、不清目标。
    out = c.HandleKey(PanelKey::EnterView, ids, true, Now());
    CHECK(out.consumed);
    CHECK(c.viewed_task_id() == 33);

    out = c.HandleKey(PanelKey::Esc, ids, true, Now());  // 先退查看态
    CHECK(out.consumed);
    CHECK(c.viewed_task_id() == 0);
    CHECK_FALSE(c.target_task_id().has_value());
    CHECK(c.focused());

    out = c.HandleKey(PanelKey::Esc, ids, true, Now());  // 再退焦点
    CHECK(out.consumed);
    CHECK_FALSE(c.focused());
    CHECK(c.selected_task_id() == 0);
}

TEST_CASE("状态机:正文非空时上下/Enter 都还给 composer,字母 x 只进 composer") {
    SetLanguage("zh");
    AgentPanelController c;
    const auto ids = Ids({1, 2});
    c.HandleKey(PanelKey::Down, ids, true, Now());  // 先正常进焦点、选中 1

    const auto up = c.HandleKey(PanelKey::Up, ids, /*composer_empty=*/false, Now());
    CHECK_FALSE(up.consumed);
    const auto enter = c.HandleKey(PanelKey::EnterView, ids, false, Now());
    CHECK_FALSE(enter.consumed);  // Enter 切视图不得顺手提交 composer 正文(issue #24245)
    const auto stop = c.HandleKey(PanelKey::StopEntry, ids, false, Now());
    CHECK_FALSE(stop.consumed);
    CHECK_FALSE(stop.stop_current);
    // 没有任何子代理时(只有 main),面板任何键都不消费。
    AgentPanelController alone;
    CHECK_FALSE(alone.HandleKey(PanelKey::Down, Ids({}), true, Now()).consumed);
}

TEST_CASE("状态机:x 四分支——运行中停止、终态清除(应用层分派)、main 不接、打字不接") {
    SetLanguage("zh");
    AgentPanelController c;
    const auto ids = Ids({7, 8});
    // main 行:x 不消费(落回 composer,变成打了一个 x)。
    c.HandleKey(PanelKey::Down, ids, true, Now());  // 选中 7
    c.HandleKey(PanelKey::Up, ids, true, Now());    // 回 main(0)
    const auto on_main = c.HandleKey(PanelKey::StopEntry, ids, true, Now());
    CHECK_FALSE(on_main.consumed);
    // 子代理行:x 消费,动作目标按稳定 task id 交出(不按下标回查)。
    c.HandleKey(PanelKey::Down, ids, true, Now());  // 选中 7
    const auto on_agent = c.HandleKey(PanelKey::StopEntry, ids, true, Now());
    CHECK(on_agent.consumed);
    CHECK(on_agent.stop_current);
    CHECK(on_agent.stop_current_task_id == 7);
    CHECK(c.selected_task_id() == 7);  // 停止不清选中:等线程报终态再改灯
}

TEST_CASE("状态机:闲置汇总哨兵——Enter 展开、Esc 收起,不接停止/查看/收件") {
    SetLanguage("zh");
    AgentPanelController c;
    const auto ids = Ids({0});  // 占位,下面用带哨兵的表
    const auto nav = Ids({1, 2, 3, kIdleSummaryTaskId});
    (void)ids;
    c.HandleKey(PanelKey::Down, nav, true, Now());   // 选中 1
    c.HandleKey(PanelKey::Down, nav, true, Now());   // 2
    c.HandleKey(PanelKey::Down, nav, true, Now());   // 3
    c.HandleKey(PanelKey::Down, nav, true, Now());   // 汇总哨兵
    CHECK(c.selected_task_id() == kIdleSummaryTaskId);
    // 哨兵不接停止/清除。
    const auto stop = c.HandleKey(PanelKey::StopEntry, nav, true, Now());
    CHECK_FALSE(stop.consumed);
    CHECK_FALSE(stop.stop_current);
    // Enter 只展开,不开查看态、不设收件目标。
    const auto enter = c.HandleKey(PanelKey::EnterView, nav, true, Now());
    CHECK(enter.consumed);
    CHECK(c.idle_expanded());
    CHECK(c.viewed_task_id() == 0);
    CHECK_FALSE(c.target_task_id().has_value());
    CHECK(c.selected_task_id() == kIdleSummaryTaskId);  // 展开不改选中
    // Esc 收起汇总(还在焦点里)。
    const auto esc = c.HandleKey(PanelKey::Esc, nav, true, Now());
    CHECK(esc.consumed);
    CHECK_FALSE(c.idle_expanded());
    CHECK(c.focused());
}

TEST_CASE("状态机:Ctrl+X Ctrl+K 两段确认——成功、超时、Esc 撤销、错键撤销") {
    SetLanguage("zh");
    // 成功。
    AgentPanelController c;
    const auto ids = Ids({5});
    const auto arm = c.HandleKey(PanelKey::StopAllArm, ids, true, Now());
    CHECK(arm.consumed);
    CHECK(c.stop_all_armed());
    const auto confirm = c.HandleKey(PanelKey::StopAllConfirm, ids, true, Now());
    CHECK(confirm.consumed);
    CHECK(confirm.stop_all);
    CHECK_FALSE(c.stop_all_armed());

    // 超时:2.5 秒后 Ctrl+K 不再是确认。
    AgentPanelController slow;
    slow.HandleKey(PanelKey::StopAllArm, ids, true, Now());
    CHECK(slow.ExpireArmed(Now() + std::chrono::milliseconds(2500)));
    CHECK_FALSE(slow.stop_all_armed());
    const auto late = slow.HandleKey(PanelKey::StopAllConfirm, ids, true, Now());
    CHECK_FALSE(late.stop_all);

    // Esc 撤销第一段。
    AgentPanelController esc;
    esc.HandleKey(PanelKey::StopAllArm, ids, true, Now());
    const auto esc_out = esc.HandleKey(PanelKey::Esc, ids, true, Now());
    CHECK_FALSE(esc.stop_all_armed());
    CHECK_FALSE(esc_out.stop_all);

    // 别键(比如上下)也撤销第一段,且那枚键按原语义走。
    AgentPanelController wrong;
    wrong.HandleKey(PanelKey::StopAllArm, ids, true, Now());
    const auto wrong_out = wrong.HandleKey(PanelKey::Down, ids, true, Now());
    CHECK_FALSE(wrong.stop_all_armed());
    CHECK(wrong_out.consumed);  // Down 仍是选择
    CHECK_FALSE(wrong_out.stop_all);

    // 正文非空时第一段也不启(组合键只在面板可控制态生效)。
    AgentPanelController typing;
    const auto typing_arm = typing.HandleKey(PanelKey::StopAllArm, ids, false, Now());
    CHECK_FALSE(typing_arm.consumed);
    CHECK_FALSE(typing.stop_all_armed());
}

TEST_CASE("状态机:按稳定 task id 选择——任务重排不丢,选中项消失落相邻,目标被清强制收起") {
    SetLanguage("zh");
    AgentPanelController c;
    // 选中 22 并进查看态。
    c.HandleKey(PanelKey::Down, Ids({11, 22, 33}), true, Now());
    c.HandleKey(PanelKey::Down, Ids({11, 22, 33}), true, Now());
    REQUIRE(c.selected_task_id() == 22);
    c.HandleKey(PanelKey::EnterView, Ids({11, 22, 33}), true, Now());
    REQUIRE(c.viewed_task_id() == 22);
    REQUIRE(c.target_task_id().has_value());

    // 列表重排(33 排到 22 前面):按 id 找回,选择/查看态都不丢。
    c.OnEntriesChanged(Ids({33, 11, 22}));
    CHECK(c.selected_task_id() == 22);
    CHECK(c.selected_index(Ids({33, 11, 22})) == 3);
    CHECK(c.target_task_id().has_value());

    // 选中项(非查看目标)结束不清选择;被清掉才落相邻。
    AgentPanelController d;
    d.HandleKey(PanelKey::Down, Ids({11, 22, 33}), true, Now());  // 选中 11
    REQUIRE(d.selected_task_id() == 11);
    d.OnEntriesChanged(Ids({22, 33}));  // 11 被清:落相邻(旧下标 1 -> 22)
    CHECK(d.selected_task_id() == 22);
    // 全没了:状态收干净。
    d.OnEntriesChanged(Ids({}));
    CHECK_FALSE(d.focused());

    // 查看态目标被清:强制收起,收件目标回 main。
    AgentPanelController e;
    e.HandleKey(PanelKey::Down, Ids({11, 22}), true, Now());
    e.HandleKey(PanelKey::EnterView, Ids({11, 22}), true, Now());
    REQUIRE(e.target_task_id().has_value());
    e.OnEntriesChanged(Ids({22}));  // 11 被清
    CHECK(e.viewed_task_id() == 0);
    CHECK_FALSE(e.target_task_id().has_value());
    CHECK(e.selected_task_id() == 0);
}

TEST_CASE("会话级 AgentPanelSession:快照与键处理共用同一份状态,线程安全外壳") {
    SetLanguage("zh");
    AgentPanelSession session;
    const auto ids = Ids({9, 10});
    const auto out = session.HandleKey(PanelKey::Down, ids, true, Now());
    CHECK(out.consumed);
    session.HandleKey(PanelKey::EnterView, ids, true, Now());
    const auto snapshot = session.SnapshotFor(ids);
    CHECK(snapshot.focused);
    CHECK(snapshot.viewed_task_id == 9);
    CHECK(snapshot.selected_task_id == 9);
    REQUIRE(snapshot.target_task_id.has_value());
    CHECK(*snapshot.target_task_id == 9);
    CHECK(snapshot.selected_index == 1);
    session.CloseView();
    CHECK(session.SnapshotFor(ids).viewed_task_id == 0);
    session.Reset();
    CHECK_FALSE(session.SnapshotFor(ids).focused);
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
