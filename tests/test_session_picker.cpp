// SessionPicker(会话管理器单第二步)的纯逻辑测试:
//   - 搜索命中四路(title/preview/id/cwd),ASCII 不分大小写;
//   - 焦点轮换 Search→Filter→Sort→Search(Shift+Tab 反向);
//   - Filter/Sort 焦点下左右改选项,Search 焦点下左右不动;
//   - 搜索词编辑(含中文/退格按码点删);
//   - 上下浏览边界、PageUp/PageDown 翻页、Home/End;
//   - 选中保持:重装数据按 id 留住,消失了落到最近一行;
//   - 视口窗口账:选中行始终在视口内;
//   - Enter 提交(空表不提交)、Esc 取消;
//   - 渲染行:帧结构/空态画面/坏档标记/底栏序号百分比;
//   - 相对时间与百分比纯函数。
//
// 终端帧(resize 不残行不闪屏)走 provider_switch 同款的 platform 原语
// 画法(单帧事务 + 整行清),自动测钉不到真终端,手测口径见提交说明:
// Windows Terminal/ConPTY 下开面板、拖宽拖窄、翻页,无残行无闪屏。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/i18n.hpp"
#include "cli/session_picker.hpp"

using namespace lubancode::cli;
using K = KeyKind;

namespace {

SessionPickerEntry Entry(std::string id, std::string title, std::string preview, std::string cwd,
                         bool damaged = false) {
    SessionPickerEntry e;
    e.id = std::move(id);
    e.title = std::move(title);
    e.preview = std::move(preview);
    e.cwd = std::move(cwd);
    e.updated_ago = "1h ago";
    e.created_ago = "2h ago";
    e.damaged = damaged;
    return e;
}

KeyEvent Char(char32_t c) { return KeyEvent::Char(c); }
KeyEvent Key(KeyKind kind) { return KeyEvent::Simple(kind); }

}  // namespace

TEST_CASE("搜索命中: title/preview/id/cwd 四路,ASCII 不分大小写") {
    const auto a = Entry("20260820-1-a", "甲的标题", "甲的首句", "D:/房甲");
    const auto b = Entry("20260820-2-b", "", "B 首句 with English", "E:/room2");
    CHECK(SessionPickerMatches(a, "标题"));
    CHECK(SessionPickerMatches(a, "首句"));
    CHECK(SessionPickerMatches(a, "房甲"));
    CHECK(SessionPickerMatches(a, "0260820-1"));     // id 子串
    CHECK(SessionPickerMatches(a, "20260820-1-A"));  // id 折小写对上
    CHECK(SessionPickerMatches(b, "with english"));  // ASCII 折小写
    CHECK(SessionPickerMatches(b, "ENGLISH"));
    CHECK_FALSE(SessionPickerMatches(b, "没这个词"));
    CHECK(SessionPickerMatches(a, ""));  // 空搜索 = 全命中
}

TEST_CASE("焦点轮换: Tab 正向三轮换,ShiftTab 反向") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "x", "c")});
    CHECK(core.state().focus == SessionPickerFocus::Search);

    core.HandleKey(Key(K::Tab));
    CHECK(core.state().focus == SessionPickerFocus::Filter);
    core.HandleKey(Key(K::Tab));
    CHECK(core.state().focus == SessionPickerFocus::Sort);
    core.HandleKey(Key(K::Tab));
    CHECK(core.state().focus == SessionPickerFocus::Search);

    core.HandleKey(Key(K::ShiftTab));
    CHECK(core.state().focus == SessionPickerFocus::Sort);
    core.HandleKey(Key(K::ShiftTab));
    CHECK(core.state().focus == SessionPickerFocus::Filter);
    core.HandleKey(Key(K::ShiftTab));
    CHECK(core.state().focus == SessionPickerFocus::Search);
}

TEST_CASE("Filter/Sort 焦点下左右改选项,Search 焦点下不动") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "x", "c")});

    // Search 焦点:左右键不改筛选/排序。
    core.HandleKey(Key(K::Right));
    CHECK(core.state().scope == SessionPickerScope::Cwd);
    CHECK(core.state().sort == SessionPickerSort::Updated);
    core.HandleKey(Key(K::Left));
    CHECK(core.state().scope == SessionPickerScope::Cwd);

    // Filter 焦点:左右切 Cwd/All。
    core.HandleKey(Key(K::Tab));
    core.HandleKey(Key(K::Right));
    CHECK(core.state().scope == SessionPickerScope::All);
    core.HandleKey(Key(K::Right));
    CHECK(core.state().scope == SessionPickerScope::Cwd);  // 两档来回翻
    core.HandleKey(Key(K::Left));
    CHECK(core.state().scope == SessionPickerScope::All);

    // Sort 焦点:左右切 Updated/Created。
    core.HandleKey(Key(K::Tab));
    core.HandleKey(Key(K::Left));
    CHECK(core.state().sort == SessionPickerSort::Created);
    core.HandleKey(Key(K::Left));
    CHECK(core.state().sort == SessionPickerSort::Updated);
}

TEST_CASE("搜索词编辑: 打字(含中文)、退格按码点删、粘贴;Enter 不当字符") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "甲乙丙", "c")});

    core.HandleKey(Char(U'灰'));
    core.HandleKey(Char(U'度'));
    CHECK(core.state().search == "灰度");
    core.HandleKey(Key(K::Backspace));
    CHECK(core.state().search == "灰");  // 中文整字退,不留半字节
    core.HandleKey(KeyEvent::Paste("abc\nx"));
    CHECK(core.state().search == "灰abc x");  // 换行换空格,NUL 不进
    core.HandleKey(Key(K::Enter));
    CHECK(core.state().search == "灰abc x");  // Enter 不是字符
    CHECK(core.state().submitted);
}

TEST_CASE("翻页与浏览边界: 上下到头停,PageUp/PageDown/Home/End") {
    SessionPickerCore core(3);
    std::vector<SessionPickerEntry> entries;
    for (int i = 0; i < 10; ++i) {
        entries.push_back(Entry("e" + std::to_string(i), "", "行" + std::to_string(i), "c"));
    }
    core.SetEntries(entries);
    REQUIRE(core.matches().size() == 10);
    CHECK(core.selected() == 0);
    CHECK(core.viewport_top() == 0);

    core.HandleKey(Key(K::Up));
    CHECK(core.selected() == 0);  // 到头停,不回绕

    core.HandleKey(Key(K::Down));
    core.HandleKey(Key(K::Down));
    CHECK(core.selected() == 2);
    CHECK(core.viewport_top() == 0);  // 第 3 行还在首屏

    core.HandleKey(Key(K::Down));  // 选中第 4 行(下标 3),视口开始跟
    CHECK(core.selected() == 3);
    CHECK(core.viewport_top() == 1);

    core.HandleKey(Key(K::PageDown));  // +3
    CHECK(core.selected() == 6);
    core.HandleKey(Key(K::PageDown));  // 越界钳到尾
    CHECK(core.selected() == 9);
    CHECK(core.viewport_top() == 7);  // 尾屏顶 = 10-3

    core.HandleKey(Key(K::End));
    CHECK(core.selected() == 9);
    core.HandleKey(Key(K::Home));
    CHECK(core.selected() == 0);
    CHECK(core.viewport_top() == 0);

    core.HandleKey(Key(K::PageUp));  // 首屏内 PageUp 钳 0
    CHECK(core.selected() == 0);
}

TEST_CASE("视口窗口账: 选中行始终落在视口内,VisibleRows 不越界") {
    SessionPickerCore core(2);
    std::vector<SessionPickerEntry> entries;
    for (int i = 0; i < 5; ++i) {
        entries.push_back(Entry("e" + std::to_string(i), "", "x", "c"));
    }
    core.SetEntries(entries);
    core.HandleKey(Key(K::End));
    CHECK(core.selected() == 4);
    const auto rows = core.VisibleRows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == 3);  // 尾屏 = [3,4]
    CHECK(rows[1] == 4);

    core.SetCapacity(5);  // resize 变高:一屏装下
    CHECK(core.VisibleRows().size() == 5);
    core.SetCapacity(1);  // 变矮:单行视口
    CHECK(core.VisibleRows().size() == 1);
    CHECK(core.VisibleRows()[0] == core.selected());  // 选中行仍可见
}

TEST_CASE("选中保持: 重装数据按 id 留住,消失了落到最近一行(顶部)") {
    SessionPickerCore core(4);
    std::vector<SessionPickerEntry> entries;
    for (int i = 0; i < 5; ++i) {
        entries.push_back(Entry("e" + std::to_string(i), "", "行" + std::to_string(i), "c"));
    }
    core.SetEntries(entries);
    core.HandleKey(Key(K::Down));
    core.HandleKey(Key(K::Down));
    REQUIRE(core.SelectedEntry() != nullptr);
    CHECK(core.SelectedEntry()->id == "e2");

    // 同一批数据重装,prefer 守住 e2。
    core.SetEntries(entries, "e2");
    CHECK(core.SelectedEntry()->id == "e2");

    // 新数据里没有 e2(换筛选后原选中不在新命中里):落到最近一行——
    // 排序后的首行(时间上最新的一场),单子口径与 Codex 同款。
    std::vector<SessionPickerEntry> shrunk(entries.begin(), entries.begin() + 2);
    core.SetEntries(shrunk, "e2");
    CHECK(core.SelectedEntry()->id == "e0");

    // 空表:选位归零,SelectedEntry 给 nullptr。
    core.SetEntries({}, "");
    CHECK(core.SelectedEntry() == nullptr);
    CHECK(core.matches().empty());
}

TEST_CASE("Enter 提交与空表、Esc/Ctrl+C 取消") {
    SessionPickerCore with_entries(4);
    with_entries.SetEntries({Entry("a", "", "x", "c")});
    with_entries.HandleKey(Key(K::Enter));
    CHECK(with_entries.state().submitted);

    SessionPickerCore empty(4);
    empty.SetEntries({});
    empty.HandleKey(Key(K::Enter));
    CHECK_FALSE(empty.state().submitted);  // 空表 Enter 不当提交
    CHECK_FALSE(empty.state().cancelled);

    SessionPickerCore cancel(4);
    cancel.SetEntries({Entry("a", "", "x", "c")});
    cancel.HandleKey(Key(K::Esc));
    CHECK(cancel.state().cancelled);
    CHECK_FALSE(cancel.state().submitted);

    SessionPickerCore ctrlc(4);
    ctrlc.SetEntries({Entry("a", "", "x", "c")});
    ctrlc.HandleKey(Key(K::CtrlC));
    CHECK(ctrlc.state().cancelled);
}

TEST_CASE("提交/取消后再喂键,状态不动") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "x", "c")});
    core.HandleKey(Key(K::Enter));
    const auto selected = core.selected();
    core.HandleKey(Key(K::Down));
    core.HandleKey(Char(U'z'));
    CHECK(core.state().submitted);
    CHECK(core.selected() == selected);
    CHECK(core.state().search.empty());
}

TEST_CASE("渲染帧: 标题/搜索/筛选行/列表/底栏,行序与 match 下标对齐") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "首句甲", "c"), Entry("b", "乙标题", "x", "c2", /*damaged=*/true)});
    const auto frame = BuildSessionPickerFrame(core, 80);

    REQUIRE(frame.lines.size() == frame.row_match_index.size());
    REQUIRE(frame.lines.size() >= 8);
    // 前四行是标题/搜索/筛选/空行,都不是列表行。
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(frame.row_match_index[i] == SessionPickerFrame::kNoMatch);
    }
    // 两条列表行对上 matches 下标。
    CHECK(frame.row_match_index[4] == 0);
    CHECK(frame.row_match_index[5] == 1);
    // 坏档行带标记。
    CHECK(frame.lines[5].find("damaged") != std::string::npos);
    // 底栏:序号 1/2 · 0%。
    CHECK(frame.lines.back().find("1 / 2") != std::string::npos);
    CHECK(frame.lines.back().find("0%") != std::string::npos);
    // 键位行常驻。
    bool has_footer = false;
    for (const auto& line : frame.lines) {
        if (line.find("enter resume") != std::string::npos) {
            has_footer = true;
        }
    }
    CHECK(has_footer);
}

TEST_CASE("渲染帧: 空列表与搜索无命中各有画面") {
    SessionPickerCore empty(4);
    empty.SetEntries({});
    auto frame = BuildSessionPickerFrame(empty, 80);
    bool empty_hint = false;
    for (const auto& line : frame.lines) {
        if (line.find("还没有会话存档") != std::string::npos) {
            empty_hint = true;
        }
    }
    CHECK(empty_hint);
    CHECK(frame.lines.back().find("0 / 0") != std::string::npos);

    SessionPickerCore no_hit(4);
    no_hit.HandleKey(Char(U'z'));
    no_hit.HandleKey(Char(U'z'));
    REQUIRE(no_hit.state().search == "zz");
    no_hit.SetEntries({Entry("a", "", "首句", "c")}, "a");
    REQUIRE(no_hit.matches().empty());  // 搜索词下重筛,无命中
    frame = BuildSessionPickerFrame(no_hit, 80);
    bool no_match_hint = false;
    for (const auto& line : frame.lines) {
        if (line.find("没有命中") != std::string::npos) {
            no_match_hint = true;
        }
    }
    CHECK(no_match_hint);
}

TEST_CASE("相对时间: 分钟/小时/天分档,未来时间按刚刚") {
    const long long now = 1000000;
    CHECK(FormatSessionAgo(now, now) == "just now");
    CHECK(FormatSessionAgo(now, now - 59) == "just now");
    CHECK(FormatSessionAgo(now, now - 60) == "1m ago");
    CHECK(FormatSessionAgo(now, now - 3599) == "59m ago");
    CHECK(FormatSessionAgo(now, now - 3600) == "1h ago");
    CHECK(FormatSessionAgo(now, now - 86399) == "23h ago");
    CHECK(FormatSessionAgo(now, now - 86400) == "1d ago");
    CHECK(FormatSessionAgo(now, now + 100) == "just now");  // 时钟倒拨
}

TEST_CASE("百分比: 首尾 0/100,单条 0,空表 0") {
    CHECK(SessionPickerScrollPercent(0, 1) == 0);   // 单条:0%
    CHECK(SessionPickerScrollPercent(0, 10) == 0);
    CHECK(SessionPickerScrollPercent(5, 10) == 55);  // 5/9 向下取整
    CHECK(SessionPickerScrollPercent(9, 10) == 100);
    CHECK(SessionPickerScrollPercent(0, 0) == 0);
    CHECK(SessionPickerScrollPercent(99, 10) == 100);  // 越界钳住
}

// ---------------------------------------------------------------------------
// 第三步:三种查看态(Ctrl+T 转录 / Ctrl+E 展开 / Ctrl+O 紧凑舒展)
// ---------------------------------------------------------------------------

TEST_CASE("Ctrl+O 切紧凑/舒展: 只改画法,筛选与选中不动") {
    SessionPickerCore core(4);
    core.SetEntries({Entry("a", "", "x", "c"), Entry("b", "", "y", "c2")});
    core.HandleKey(Key(K::Down));
    REQUIRE(core.selected() == 1);

    core.HandleKey(Key(K::CtrlO));
    CHECK(core.state().layout == SessionPickerLayout::Comfortable);
    // 筛选/排序/选中/视口一个没动。
    CHECK(core.state().scope == SessionPickerScope::Cwd);
    CHECK(core.state().sort == SessionPickerSort::Updated);
    CHECK(core.selected() == 1);
    CHECK(core.viewport_top() == 0);
    CHECK_FALSE(core.state().submitted);
    CHECK_FALSE(core.state().cancelled);

    core.HandleKey(Key(K::CtrlO));
    CHECK(core.state().layout == SessionPickerLayout::Compact);

    // 舒展帧:每行多一行 cwd,行数翻倍;row_match_index 两行同源。
    core.HandleKey(Key(K::CtrlO));
    const auto comfy = BuildSessionPickerFrame(core, 80);
    const auto compact = [&]() {
        core.HandleKey(Key(K::CtrlO));
        return BuildSessionPickerFrame(core, 80);
    }();
    CHECK(comfy.lines.size() == compact.lines.size() + 2);  // 两条各多一行 cwd
    // 舒展帧第 5 行(第一条的第二行)是 cwd,match 下标与第一行相同。
    CHECK(comfy.row_match_index[4] == 0);
    CHECK(comfy.row_match_index[4] == comfy.row_match_index[5]);
    bool cwd_seen = false;
    for (const auto& line : comfy.lines) {
        if (line.find("c2") != std::string::npos) {
            cwd_seen = true;  // 舒展行把第二条的 cwd(c2)也摆出来了
        }
    }
    CHECK(cwd_seen);
}

TEST_CASE("Ctrl+E 展开: 选中行摊出详情,再按收起;空表不开") {
    SessionPickerCore core(4);
    SessionPickerEntry a = Entry("20260820-1-a", "长标题", "x", "D:/房");
    a.model = "glm-5.2";
    a.message_count = 12;
    a.created_at = "2026-08-20 09:00:00";
    a.updated_at = "2026-08-21 10:00:00";
    core.SetEntries({a});

    core.HandleKey(Key(K::CtrlE));
    CHECK(core.state().expanded);
    const auto frame = BuildSessionPickerFrame(core, 80);
    // 展开帧多出详情:标题/cwd/id/模型/创建更新各一行。
    std::size_t detail_lines = 0;
    for (const auto& line : frame.lines) {
        if (line.find("id:") != std::string::npos || line.find("模型:") != std::string::npos ||
            line.find("创建:") != std::string::npos) {
            ++detail_lines;
        }
    }
    CHECK(detail_lines == 3);
    bool id_line = false;
    bool model_line = false;
    for (const auto& line : frame.lines) {
        if (line.find("20260820-1-a") != std::string::npos && line.find("id:") != std::string::npos) {
            id_line = true;
        }
        if (line.find("glm-5.2") != std::string::npos) {
            model_line = true;
        }
    }
    CHECK(id_line);
    CHECK(model_line);

    core.HandleKey(Key(K::CtrlE));
    CHECK_FALSE(core.state().expanded);

    // 空表按 Ctrl+E 不开。
    SessionPickerCore empty(4);
    empty.SetEntries({});
    empty.HandleKey(Key(K::CtrlE));
    CHECK_FALSE(empty.state().expanded);
}

TEST_CASE("Ctrl+T 转录浮层: 开/收,浏览键落空,Enter 仍提交,选中回原行") {
    SessionPickerCore core(3);
    std::vector<SessionPickerEntry> entries;
    for (int i = 0; i < 5; ++i) {
        entries.push_back(Entry("e" + std::to_string(i), "", "行", "c"));
    }
    core.SetEntries(entries);
    core.HandleKey(Key(K::Down));
    core.HandleKey(Key(K::Down));
    REQUIRE(core.selected() == 2);

    core.HandleKey(Key(K::CtrlT));
    CHECK(core.state().transcript_open);

    // 浮层里浏览/打字全落空:选中、搜索、筛选、视口一个不动。
    core.HandleKey(Key(K::Down));
    core.HandleKey(Key(K::Up));
    core.HandleKey(Key(K::PageDown));
    core.HandleKey(Key(K::Home));
    core.HandleKey(Char(U'灰'));
    core.HandleKey(Key(K::Tab));
    core.HandleKey(Key(K::Left));
    CHECK(core.selected() == 2);
    CHECK(core.state().search.empty());
    CHECK(core.state().scope == SessionPickerScope::Cwd);
    CHECK(core.state().focus == SessionPickerFocus::Search);
    CHECK(core.state().transcript_open);  // 浮层还开着

    // Esc 收浮层不当取消;浮层收掉后选中还是原行。
    core.HandleKey(Key(K::Esc));
    CHECK_FALSE(core.state().transcript_open);
    CHECK_FALSE(core.state().cancelled);
    CHECK(core.selected() == 2);

    // 再开一次,Ctrl+T 也收得了。
    core.HandleKey(Key(K::CtrlT));
    CHECK(core.state().transcript_open);
    core.HandleKey(Key(K::CtrlT));
    CHECK_FALSE(core.state().transcript_open);

    // 开着浮层时 Enter 直接提交(选中的就是浮层看的那场)。
    core.HandleKey(Key(K::CtrlT));
    core.HandleKey(Key(K::Enter));
    CHECK(core.state().submitted);

    // 开着浮层时 Ctrl+C/Ctrl+D 收浮层,不当取消。
    SessionPickerCore cc(3);
    cc.SetEntries({Entry("a", "", "x", "c")});
    cc.HandleKey(Key(K::CtrlT));
    cc.HandleKey(Key(K::CtrlC));
    CHECK_FALSE(cc.state().transcript_open);
    CHECK_FALSE(cc.state().cancelled);

    SessionPickerCore cd(3);
    cd.SetEntries({Entry("a", "", "x", "c")});
    cd.HandleKey(Key(K::CtrlT));
    cd.HandleKey(Key(K::CtrlD));
    CHECK_FALSE(cd.state().transcript_open);
    CHECK_FALSE(cd.state().cancelled);

    // 开着浮层时 Ctrl+E 也当收浮层(core 的第二个实例验)。
    SessionPickerCore ce(3);
    ce.SetEntries({Entry("a", "", "x", "c")});
    ce.HandleKey(Key(K::CtrlT));
    ce.HandleKey(Key(K::CtrlE));
    CHECK_FALSE(ce.state().transcript_open);
    CHECK_FALSE(ce.state().expanded);
}

TEST_CASE("转录浮层帧: 标题/内容/空态/底栏") {
    const auto frame = BuildSessionTranscriptFrame("转录 · 20260820-1-a",
                                                   {"  user · 你好", "  assistant · 在"}, 80);
    REQUIRE(frame.lines.size() == frame.row_match_index.size());
    CHECK(frame.lines.front().find("20260820-1-a") != std::string::npos);
    bool has_user_line = false;
    bool has_footer = false;
    for (const auto& line : frame.lines) {
        if (line.find("user · 你好") != std::string::npos) {
            has_user_line = true;
        }
        if (line.find("enter resume") != std::string::npos) {
            has_footer = true;
        }
    }
    CHECK(has_user_line);
    CHECK(has_footer);
    for (const auto& index : frame.row_match_index) {
        CHECK(index == SessionPickerFrame::kNoMatch);  // 浮层没有列表行
    }

    const auto empty_frame = BuildSessionTranscriptFrame("转录 · x", {}, 80);
    bool empty_hint = false;
    for (const auto& line : empty_frame.lines) {
        if (line.find("还没有可显示的转录") != std::string::npos) {
            empty_hint = true;
        }
    }
    CHECK(empty_hint);
}
