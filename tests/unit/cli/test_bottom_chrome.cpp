// 底栏一本帧账(0.29.x):BottomChromeFrame 的行序/高度/指纹纯逻辑。空
// 闲 composer 与流式 footer 共认这一份——两条路的行序契约钉死在这儿,
// 谁也不许多拼一套。
//
// Composer 合流 P0/P1(终端Composer合流单):新增 BuildBottomChromeLayout
// 的同源布局测试。Idle 与 Busy 两边各造同一份 RenderState、截取纯布局结果
// 作 golden——两边 Composer 物理行、padding、cursor 必须一字不差;合流前
// Busy 只有"首行回显 + 另有 N 行"的单行会计,这些断言在那条老路上全红,
// 转绿即证明布局已同源。

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

#include "cli/bottom_chrome.hpp"
#include "cli/theme.hpp"

using namespace lubancode::cli;

namespace {
bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 拼一份可直接进 ComposerViewModel 的 RenderState:lines 为显式逻辑行,
// cursor 落 (row, col);line 同步拼好(布局只认 lines,指纹摘要认 line)。
RenderState ComposerState(std::vector<std::u32string> lines, std::size_t row, std::size_t col) {
    RenderState state;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            state.line.push_back(U'\n');
        }
        state.line += lines[i];
    }
    state.lines = std::move(lines);
    state.cursor_row = row;
    state.cursor_col = col;
    return state;
}

// 常态框模型:plain 主题、40 列、一颗状态行。Idle 与 Busy 的差别只许落在
// mode/placeholder/activity 这些数据上,画法零差别。
BottomChromeModel FramedModel(const RenderState& state, ComposerMode mode,
                              std::string placeholder = "") {
    BottomChromeModel model;
    model.composer.editor = state;
    model.composer.prompt = "> ";
    model.composer.mode = mode;
    model.composer.placeholder = std::move(placeholder);
    model.status_rows = {"status"};
    return model;
}

std::string PlainRule(int width) {
    return std::string(static_cast<std::size_t>(width - 1), '-');
}
}  // namespace

TEST_CASE("帧账:总行数 = 队列+横线+输入+状态+坞+提示") {
    BottomChromeFrame frame;
    CHECK(frame.TotalRows() == 4);  // 空队列 + 横线2 + 输入1 + 状态1 + 空坞 + 空提示
    frame.queue_rows = {"待发送消息 2 条", "> 第一条", "> 第二条"};
    frame.agent_dock_rows = {"↑/↓ 选择", "● main", "○ agent #1"};
    frame.transient_rows = {"  /help  列出命令"};
    frame.composer_rows = 3;  // 多行 composer
    CHECK(frame.TotalRows() == 3 + 3 + 2 + 3 + 1 + 1);
    // 坞首行 = 队列之后、框与状态之下(相对帧顶)。
    CHECK(frame.AgentDockFirstRow() == 3 + 3 + 2 + 1);
}

TEST_CASE("帧账:指纹认内容——行变/选择变必变,重排不变瞎报") {
    BottomChromeFrame a;
    a.queue_rows = {"队列"};
    a.agent_dock_rows = {"● main", "○ agent #1"};
    a.selected_task_id = 3;
    a.revision = BottomChromeRevision(a);
    BottomChromeFrame b = a;
    b.revision = BottomChromeRevision(b);
    CHECK(BottomChromeFingerprint(a) == BottomChromeFingerprint(b));
    CHECK(a.revision == b.revision);

    b.agent_dock_rows[1] = "○ agent #1  完成";  // 状态变了
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    b = a;
    b.selected_task_id = 0;  // 选择变了
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    b = a;
    b.composer_rows = 2;  // 单行变多行
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
}

TEST_CASE("帧账:指纹把各分区隔开,行内容跨区撞车也不误判相等") {
    BottomChromeFrame a;
    a.queue_rows = {"同一行字"};
    BottomChromeFrame b;
    b.agent_dock_rows = {"同一行字"};
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    CHECK(Contains(BottomChromeFingerprint(a), "q:"));
    CHECK(Contains(BottomChromeFingerprint(a), "d:"));
}

TEST_CASE("模式说明状态:5999ms 可见、6000ms 到期且连按重置") {
    using Clock = ModeNoticeState::Clock;
    const Clock::time_point t0{};
    ModeNoticeState state;
    LineEditorCore configured_editor;
    configured_editor.set_confirm_mode(ConfirmMode::Yolo);
    CHECK(configured_editor.confirm_mode() == ConfirmMode::Yolo);
    CHECK_FALSE(state.VisibleMode(t0).has_value());  // 启动/配置设档不调用 Show

    state.Show(ConfirmMode::AcceptEdits, t0);
    CHECK(state.VisibleMode(t0 + std::chrono::milliseconds(5999)) == ConfirmMode::AcceptEdits);
    CHECK_FALSE(state.VisibleMode(t0 + std::chrono::milliseconds(6000)).has_value());

    state.Show(ConfirmMode::Yolo, t0);
    state.Show(ConfirmMode::Auto, t0 + std::chrono::milliseconds(5000));
    CHECK(state.VisibleMode(t0 + std::chrono::milliseconds(10999)) == ConfirmMode::Auto);
    CHECK_FALSE(state.VisibleMode(t0 + std::chrono::milliseconds(11000)).has_value());
}

TEST_CASE("模式说明专位:状态行上方、与 transient 并存且安全裁剪") {
    Theme theme;
    theme.tool_line = "\x1b[33m";
    theme.reset = "\x1b[0m";
    BottomChromeModel model = FramedModel(ComposerState({U""}, 0, 0), ComposerMode::Idle);
    model.mode_notice_rows = {"YOLO：允许所有工具，无需确认。"};
    model.transient_rows = {"slash hint"};
    const BottomChromeLayout layout = BuildBottomChromeLayout(model, theme, 16);

    REQUIRE(layout.frame.rows.size() == 6);
    CHECK(Contains(layout.frame.rows[0].text, "YOLO"));
    CHECK(layout.frame.rows[1].text == "status");
    CHECK(layout.frame.rows.back().text == "slash hint");
    CHECK(layout.chrome.mode_notice_rows.size() == 1);
    CHECK(layout.chrome.transient_rows.size() == 1);
    CHECK(layout.painted_row_widths[0] <= 15);

    BottomChromeFrame without = layout.chrome;
    without.mode_notice_rows.clear();
    CHECK(BottomChromeRevision(layout.chrome) != BottomChromeRevision(without));
}

// ---------------------------------------------------------------------------
// Composer 合流 P0/P1:BuildBottomChromeLayout 同源布局。以下全按"同源布局"
// 测试矩阵展开——空串/单行/多逻辑行/软换行边界/光标各位置/padding 改值/
// placeholder 与各区增减/rule tag/三种主题/五档宽度。
// ---------------------------------------------------------------------------

TEST_CASE("同源布局:golden——两逻辑行 + 中英混排,行序/padding/cursor 钉死") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"第一行", U"second"}, 1, 6),
                                          ComposerMode::BusyQueue);
    const BottomChromeLayout layout = BuildBottomChromeLayout(model, plain, 40);
    REQUIRE(layout.frame.rows.size() == 5);
    CHECK(layout.frame.rows[0].text == "status");        // 模式行紧贴输入框上方
    CHECK(layout.frame.rows[1].text == PlainRule(40));   // 上横线
    CHECK(layout.frame.rows[2].text == "> 第一行");       // 首行带提示符
    CHECK(layout.frame.rows[3].text == "  second");       // 续行两格缩进,落在文本里
    CHECK(layout.frame.rows[4].text == PlainRule(40));   // 下横线
    CHECK(layout.composer_first_row == 2);
    CHECK(layout.composer_row_count == 2);
    CHECK(layout.cursor_row == 3);                       // 光标在第二物理行
    CHECK(layout.cursor_x == 2 + 6);                     // 缩进 2 + "second" 宽 6
    CHECK(layout.painted_row_widths ==
          std::vector<int>{6, 39, 8, 8, 39});            // 纯文本宽,转义字节不计
    CHECK(layout.chrome.composer_rows == 2);
    CHECK(layout.chrome.TotalRows() == 5);
}

TEST_CASE("同源布局:Idle 与 Busy 同拍——物理行/padding/cursor 必须相同") {
    const Theme plain;
    // 两边各造同一份 RenderState:Idle 不带占位提示,Busy 带(两边数据差异
    // 只许体现在 placeholder 上,画法必须同源)。
    const RenderState draft = ComposerState({U"你好", U"排队"}, 1, 2);
    const BottomChromeLayout idle =
        BuildBottomChromeLayout(FramedModel(draft, ComposerMode::Idle), plain, 40);
    const BottomChromeLayout busy = BuildBottomChromeLayout(
        FramedModel(draft, ComposerMode::BusyQueue, "键入并回车排队"), plain, 40);
    REQUIRE(idle.frame.rows.size() == busy.frame.rows.size());
    for (std::size_t i = 0; i < idle.frame.rows.size(); ++i) {
        // 有正文时连逐行文本都必须相同:placeholder 只在草稿真空时显示。
        CHECK(idle.frame.rows[i].text == busy.frame.rows[i].text);
    }
    CHECK(idle.chrome.composer_rows == busy.chrome.composer_rows);   // padding 同拍
    CHECK(idle.composer_row_count == busy.composer_row_count);
    CHECK(idle.cursor_row == busy.cursor_row);
    CHECK(idle.cursor_x == busy.cursor_x);

    // 空草稿:占位提示只在 Busy 侧显示,行数/padding/cursor 仍必须相同。
    const RenderState empty = ComposerState({U""}, 0, 0);
    const BottomChromeLayout idle_empty =
        BuildBottomChromeLayout(FramedModel(empty, ComposerMode::Idle), plain, 40);
    const BottomChromeLayout busy_empty = BuildBottomChromeLayout(
        FramedModel(empty, ComposerMode::BusyQueue, "键入并回车排队"), plain, 40);
    CHECK(idle_empty.frame.rows[idle_empty.composer_first_row].text == "> ");
    CHECK(busy_empty.frame.rows[busy_empty.composer_first_row].text == "> 键入并回车排队");
    CHECK(idle_empty.frame.rows.size() == busy_empty.frame.rows.size());
    CHECK(idle_empty.chrome.composer_rows == busy_empty.chrome.composer_rows);
    CHECK(idle_empty.cursor_row == busy_empty.cursor_row);
    CHECK(idle_empty.cursor_x == busy_empty.cursor_x);
    CHECK(idle_empty.cursor_x == 2);
}

TEST_CASE("同源布局:忙时输入两行中文,全部逻辑行可见,光标落末字后") {
    // 合流前 Busy 只有"首行 + …(还有 N 行)";这条就是当年的失败测试,
    // 单行捷径删干净后转绿。
    const Theme plain;
    BottomChromeModel model =
        FramedModel(ComposerState({U"第一句中文", U"第二句中文"}, 1, 6), ComposerMode::BusyQueue);
    const BottomChromeLayout layout = BuildBottomChromeLayout(model, plain, 40);
    REQUIRE(layout.composer_row_count == 2);
    CHECK(layout.frame.rows[layout.composer_first_row].text == "> 第一句中文");
    CHECK(layout.frame.rows[layout.composer_first_row + 1].text == "  第二句中文");
    for (const auto& row : layout.frame.rows) {
        CHECK_FALSE(Contains(row.text, "另有"));
        CHECK_FALSE(Contains(row.text, "…"));
    }
    // 光标落在末字之后:续行缩进 2 + 五个汉字 10 列。
    CHECK(layout.cursor_row == layout.composer_first_row + 1);
    CHECK(layout.cursor_x == 2 + 10);
}

TEST_CASE("同源布局:单行 ASCII 与单行中文、空串") {
    const Theme plain;
    const auto ascii = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"hello"}, 0, 5), ComposerMode::Idle), plain, 80);
    CHECK(ascii.composer_row_count == 1);
    CHECK(ascii.frame.rows[ascii.composer_first_row].text == "> hello");
    CHECK(ascii.cursor_x == 2 + 5);
    // 单行中文:显示宽按 2 列/字计,光标在末字后。
    const auto cjk = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"你好"}, 0, 2), ComposerMode::Idle), plain, 80);
    CHECK(cjk.frame.rows[cjk.composer_first_row].text == "> 你好");
    CHECK(cjk.cursor_x == 2 + 4);
    // 空串:一行空正文,靠 min body 撑住框高。
    const auto empty = BuildBottomChromeLayout(
        FramedModel(ComposerState({U""}, 0, 0), ComposerMode::Idle), plain, 80);
    CHECK(empty.composer_row_count == 1);
    CHECK(empty.frame.rows[empty.composer_first_row].text == "> ");
    CHECK(empty.chrome.composer_rows == kComposerMinBodyRows);
    CHECK(empty.cursor_row == empty.composer_first_row);
    CHECK(empty.cursor_x == 2);
}

TEST_CASE("同源布局:末行空与三行以上,续行缩进一致") {
    const Theme plain;
    const auto tail_empty = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"abc", U""}, 1, 0), ComposerMode::Idle), plain, 80);
    REQUIRE(tail_empty.composer_row_count == 2);
    CHECK(tail_empty.frame.rows[tail_empty.composer_first_row + 1].text == "  ");
    CHECK(tail_empty.cursor_row == tail_empty.composer_first_row + 1);
    CHECK(tail_empty.cursor_x == 2);  // 行首:缩进之后

    const auto three = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"一", U"二二", U"三三三"}, 2, 3), ComposerMode::Idle), plain,
        80);
    REQUIRE(three.composer_row_count == 3);
    CHECK(three.frame.rows[three.composer_first_row].text == "> 一");
    CHECK(three.frame.rows[three.composer_first_row + 1].text == "  二二");
    CHECK(three.frame.rows[three.composer_first_row + 2].text == "  三三三");
    CHECK(three.cursor_x == 2 + 6);
}

TEST_CASE("同源布局:软换行恰满、差一格、宽字符前只剩一格") {
    const Theme plain;
    // 40 列:首行容量 = 40 - 2 - 1 = 37。恰满 37 个 ASCII 不折。
    const std::u32string full(37, U'x');
    const auto exactly = BuildBottomChromeLayout(
        FramedModel(ComposerState({full}, 0, 37), ComposerMode::Idle), plain, 40);
    CHECK(exactly.composer_row_count == 1);
    CHECK(exactly.frame.rows[exactly.composer_first_row].text == "> " + Utf32ToUtf8(full));

    // 差一格(36):仍一行。
    const std::u32string almost(36, U'x');
    const auto short_one = BuildBottomChromeLayout(
        FramedModel(ComposerState({almost}, 0, 36), ComposerMode::Idle), plain, 40);
    CHECK(short_one.composer_row_count == 1);

    // 宽字符前只剩一格:36 个 ASCII 后跟一个汉字,汉字整个挪到续行,不切半。
    std::u32string wide(36, U'x');
    wide.push_back(U'汉');
    const auto split = BuildBottomChromeLayout(
        FramedModel(ComposerState({wide}, 0, 37), ComposerMode::Idle), plain, 40);
    REQUIRE(split.composer_row_count == 2);
    CHECK(split.frame.rows[split.composer_first_row].text == "> " + std::string(36, 'x'));
    CHECK(split.frame.rows[split.composer_first_row + 1].text == "  汉");
    CHECK(split.cursor_row == split.composer_first_row + 1);
    CHECK(split.cursor_x == 2 + 2);
}

TEST_CASE("同源布局:光标在首行/续行/行首/行中/行尾") {
    const Theme plain;
    const std::vector<std::pair<std::size_t, std::size_t>> spots = {
        {0, 0}, {0, 3}, {0, 6}, {1, 0}, {1, 4}};
    for (const auto& [row, col] : spots) {
        const auto layout = BuildBottomChromeLayout(
            FramedModel(ComposerState({U"abcdef", U"wxyz"}, row, col), ComposerMode::Idle), plain,
            40);
        if (row == 0) {
            CHECK(layout.cursor_row == layout.composer_first_row);
            CHECK(layout.cursor_x == 2 + static_cast<int>(col));
        } else {
            CHECK(layout.cursor_row == layout.composer_first_row + 1);
            CHECK(layout.cursor_x == 2 + static_cast<int>(col));
        }
    }
}

TEST_CASE("同源布局:top padding 与 min body 改值,Idle 与 Busy 同拍变化") {
    const Theme plain;
    const RenderState draft = ComposerState({U"正文"}, 0, 2);
    auto idle = FramedModel(draft, ComposerMode::Idle);
    auto busy = FramedModel(draft, ComposerMode::BusyQueue, "占位");
    idle.composer.min_body_rows = 5;
    idle.composer.top_padding_rows = 2;
    busy.composer.min_body_rows = 5;
    busy.composer.top_padding_rows = 2;
    const BottomChromeLayout idle_layout = BuildBottomChromeLayout(idle, plain, 40);
    const BottomChromeLayout busy_layout = BuildBottomChromeLayout(busy, plain, 40);
    // 单行正文:上留白 2 + 正文 1 + 下补 2 = 5。
    CHECK(idle_layout.chrome.composer_rows == 5);
    CHECK(busy_layout.chrome.composer_rows == 5);
    CHECK(idle_layout.frame.rows[2].text.empty());   // 上留白两行
    CHECK(idle_layout.frame.rows[3].text.empty());
    CHECK(idle_layout.frame.rows[4].text == "> 正文");
    CHECK(idle_layout.frame.rows[5].text.empty());   // 下补两行
    CHECK(idle_layout.frame.rows[6].text.empty());
    CHECK(idle_layout.frame.rows.size() == busy_layout.frame.rows.size());
    // 正文长过 min body 时框自然长高,不留下补。
    idle.composer.editor = ComposerState({U"一", U"二", U"三", U"四", U"五"}, 4, 1);
    busy.composer.editor = idle.composer.editor;
    const auto grown_idle = BuildBottomChromeLayout(idle, plain, 40);
    const auto grown_busy = BuildBottomChromeLayout(busy, plain, 40);
    CHECK(grown_idle.chrome.composer_rows == 2 + 5);
    CHECK(grown_busy.chrome.composer_rows == 2 + 5);
    CHECK(grown_idle.cursor_row == grown_busy.cursor_row);
    CHECK(grown_idle.cursor_x == grown_busy.cursor_x);
}

TEST_CASE("同源布局:placeholder 只在草稿真空时显示") {
    const Theme plain;
    BottomChromeModel model =
        FramedModel(ComposerState({U"/mod"}, 0, 4), ComposerMode::BusyQueue, "键入并回车排队");
    const auto filled = BuildBottomChromeLayout(model, plain, 40);
    CHECK(filled.frame.rows[filled.composer_first_row].text == "> /mod");
    model.composer.editor = ComposerState({U""}, 0, 0);
    const auto vacuum = BuildBottomChromeLayout(model, plain, 40);
    CHECK(vacuum.frame.rows[vacuum.composer_first_row].text == "> 键入并回车排队");
}

TEST_CASE("同源布局:activity/queue/hints/dock 单独增减,行序与锚点跟着走") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"正文"}, 0, 2), ComposerMode::BusyQueue);
    model.activity_rows = {"• Working (3s)"};
    model.queue_rows = {"待发 2 条", "> 第一条"};
    model.transient_rows = {"  /help  列出命令"};
    model.agent_dock_rows = {"● main", "○ agent #1"};
    const BottomChromeLayout layout = BuildBottomChromeLayout(model, plain, 40);
    // 行序:Working > 队列 > 模式行 > 上横线 > 输入 > 下横线 > 坞 > 提示。
    CHECK(layout.frame.rows[0].text == "• Working (3s)");
    CHECK(layout.frame.rows[1].text == "待发 2 条");
    CHECK(layout.frame.rows[2].text == "> 第一条");
    CHECK(layout.frame.rows[3].text == "status");
    CHECK(layout.frame.rows[4].text == PlainRule(40));
    CHECK(layout.composer_first_row == 5);
    CHECK(layout.frame.rows[6].text == PlainRule(40));
    CHECK(layout.frame.rows[7].text == "● main");
    CHECK(layout.frame.rows[8].text == "○ agent #1");
    CHECK(layout.frame.rows[9].text == "  /help  列出命令");
    CHECK(layout.frame.rows.size() == 10);
    CHECK(layout.chrome.TotalRows() == 10);
    CHECK(layout.chrome.AgentDockFirstRow() == 7);
    // 光标仍指输入区,不随区块增减漂移。
    CHECK(layout.cursor_row == layout.composer_first_row);
    // 摘掉 Working 与队列,输入区上移两行,其余不变。
    model.activity_rows.clear();
    model.queue_rows.clear();
    const auto lean = BuildBottomChromeLayout(model, plain, 40);
    CHECK(lean.composer_first_row == 2);  // 模式行、上横线之后便是输入行
    CHECK(lean.cursor_row == 2);
    CHECK(lean.frame.rows.size() == 7);
}

TEST_CASE("同源布局:rule tag 挂上横线右端,没有就整线") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"正文"}, 0, 2), ComposerMode::Idle);
    model.rule_tag = "agent #2 重构";
    const auto tagged = BuildBottomChromeLayout(model, plain, 40);
    CHECK(Contains(tagged.frame.rows[1].text, "agent #2 重构"));
    model.rule_tag.clear();
    const auto plain_rule = BuildBottomChromeLayout(model, plain, 40);
    CHECK(plain_rule.frame.rows[1].text == PlainRule(40));
}

TEST_CASE("同源布局:dark/light 着色,plain 不夹一个转义字节") {
    const RenderState draft = ComposerState({U"正文"}, 0, 2);
    for (const char* name : {"dark", "light"}) {
        const Theme theme = BuiltinTheme(name);
        REQUIRE(!theme.reset.empty());
        BottomChromeModel model = FramedModel(draft, ComposerMode::BusyQueue);
        model.queue_rows = {"队列行"};
        model.agent_dock_rows = {"● main"};
        model.composer.placeholder = "占位";
        model.composer.prompt = theme.prompt + "> " + theme.reset;
        const auto layout = BuildBottomChromeLayout(model, theme, 40);
        // 横线/队列/坞行带主题淡色;宽度账只记纯文本宽。
        CHECK(Contains(layout.frame.rows[0].text, theme.stats));
        CHECK(Contains(layout.frame.rows[2].text, theme.stats));
        CHECK(layout.painted_row_widths[0] == 6);   // 队列行"队列行" 三个汉字
        CHECK(layout.painted_row_widths[1] == 6);   // 模式行测试占位 "status"
        CHECK(layout.painted_row_widths[2] == 39);  // 其下才是满宽横线
        CHECK(layout.cursor_x == 6);                // 彩色 "> " 只占 2 列 + 正文 4 列
    }
    BottomChromeModel model = FramedModel(draft, ComposerMode::BusyQueue, "占位");
    model.queue_rows = {"队列行"};
    model.agent_dock_rows = {"● main"};
    const auto plain = BuildBottomChromeLayout(model, Theme{}, 40);
    for (const auto& row : plain.frame.rows) {
        CHECK(row.text.find('\x1b') == std::string::npos);
    }
}

TEST_CASE("同源布局:20/40/80/120/200 列,宽了行数递减、光标仍在末字后") {
    const Theme plain;
    const std::u32string long_line(30, U'汉');  // 60 列正文
    int previous_rows = 0;
    for (const int width : {20, 40, 80, 120, 200}) {
        const auto layout = BuildBottomChromeLayout(
            FramedModel(ComposerState({long_line}, 0, 30), ComposerMode::Idle), plain, width);
        if (previous_rows > 0) {
            CHECK(layout.composer_row_count <= previous_rows);
        }
        previous_rows = layout.composer_row_count;
        // 每行纯文本宽不越屏(行首提示符/缩进已计入)。
        for (int r = 0; r < layout.composer_row_count; ++r) {
            CHECK(layout.painted_row_widths[layout.composer_first_row + r] <= width - 1);
        }
        // 光标始终在末物理行(末字之后)。
        CHECK(layout.cursor_row == layout.composer_first_row + layout.composer_row_count - 1);
    }
    // 20 列窄屏:首行容量 17 → 8 个汉字;续行 17 → 8;末行 6,光标在其后。
    const auto narrow = BuildBottomChromeLayout(
        FramedModel(ComposerState({long_line}, 0, 30), ComposerMode::Idle), plain, 20);
    REQUIRE(narrow.composer_row_count == 4);
    CHECK(narrow.frame.rows[narrow.composer_first_row].text.size() == 2 + 24);  // "> " + 8 汉字
    CHECK(narrow.cursor_x == 2 + 12);  // 末行 6 个汉字 = 12 列
}

TEST_CASE("同源布局:指纹纳入 composer 内容/cursor/mode/placeholder") {
    const Theme plain;
    const RenderState base = ComposerState({U"草稿"}, 0, 2);
    const BottomChromeLayout anchor =
        BuildBottomChromeLayout(FramedModel(base, ComposerMode::Idle), plain, 40);

    const auto same = BuildBottomChromeLayout(FramedModel(base, ComposerMode::Idle), plain, 40);
    CHECK(same.revision == anchor.revision);
    CHECK(same.chrome.composer_digest == anchor.chrome.composer_digest);

    // 内容变(行数不变,合流前这类变化指纹看不见)。
    const auto edited = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"草稿改"}, 0, 3), ComposerMode::Idle), plain, 40);
    CHECK(edited.revision != anchor.revision);
    // 光标变。
    const auto moved = BuildBottomChromeLayout(
        FramedModel(ComposerState({U"草稿"}, 0, 1), ComposerMode::Idle), plain, 40);
    CHECK(moved.revision != anchor.revision);
    // 档位变。
    const auto busied =
        BuildBottomChromeLayout(FramedModel(base, ComposerMode::BusyQueue), plain, 40);
    CHECK(busied.revision != anchor.revision);
    // 占位提示变。
    const auto with_hint =
        BuildBottomChromeLayout(FramedModel(base, ComposerMode::Idle, "占位"), plain, 40);
    CHECK(with_hint.revision != anchor.revision);
    // 帧账层面的直接断言:digest 进指纹。
    BottomChromeFrame a;
    a.composer_digest = "x";
    BottomChromeFrame b = a;
    b.composer_digest = "y";
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
}

TEST_CASE("同源布局:无框读取退化——不画横线、不留白、光标仍准") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"向导输入"}, 0, 4), ComposerMode::Idle);
    model.framed = false;
    model.status_rows.clear();
    const auto layout = BuildBottomChromeLayout(model, plain, 40);
    REQUIRE(layout.frame.rows.size() == 1);
    CHECK(layout.frame.rows[0].text == "> 向导输入");
    CHECK(layout.composer_first_row == 0);
    CHECK(layout.chrome.composer_rows == 1);
    CHECK(layout.chrome.rule_rows == 0);
    CHECK(layout.chrome.status_rows == 0);
    CHECK(layout.cursor_x == 2 + 8);  // 四个汉字 8 列
    CHECK(layout.cursor_row == 0);
}

// ---------------------------------------------------------------------------
// 高度预算(终端画面隔网单·战术二):"输入行必画得下"的硬约束。可选行按
// transient -> dock -> queue -> activity 的次序舍;可选行全舍了还不够,
// composer 围光标开窗。预算 0 = 不限(老行为)。
// ---------------------------------------------------------------------------

TEST_CASE("高度预算:0 不限,整帧照旧(老行为不受扰)") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"一行"}, 0, 2), ComposerMode::BusyQueue);
    model.queue_rows = {"队列标题", "> 条目"};
    model.agent_dock_rows = {"↑/↓ 选择", "● main"};
    model.transient_rows = {"  /help"};
    model.activity_rows = {"• Working (3s)"};
    const auto unlimited = BuildBottomChromeLayout(model, plain, 40, 0);
    const auto legacy = BuildBottomChromeLayout(model, plain, 40);
    CHECK(unlimited.frame.rows.size() == legacy.frame.rows.size());
    CHECK(unlimited.dropped_optional_rows == 0);
}

TEST_CASE("高度预算:超窗先舍提示/坞,队列次之,输入行永远画得下") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"一行"}, 0, 2), ComposerMode::BusyQueue);
    model.queue_rows = {"队列标题", "> 条目"};
    model.agent_dock_rows = {"↑/↓ 选择", "● main"};
    model.transient_rows = {"  /help"};
    model.activity_rows = {"• Working (3s)"};
    // 整帧不限时 = 活动1 + 队列2 + 横线2 + 输入1 + 状态1 + 坞2 + 提示1 = 10 行。
    REQUIRE(BuildBottomChromeLayout(model, plain, 40).frame.rows.size() == 10);

    // 预算 7:先舍提示(1),再舍坞(2)——队列/活动条/输入全套保住。
    const auto tight = BuildBottomChromeLayout(model, plain, 40, 7);
    CHECK(tight.frame.rows.size() == 7);
    CHECK(tight.dropped_optional_rows == 3);
    bool has_dock = false;
    bool has_hint = false;
    bool has_queue = false;
    bool has_input = false;
    for (const auto& row : tight.frame.rows) {
        has_dock = has_dock || Contains(row.text, "main");
        has_hint = has_hint || Contains(row.text, "/help");
        has_queue = has_queue || Contains(row.text, "队列标题");
        has_input = has_input || (!row.text.empty() && row.text[0] == '>');
    }
    CHECK_FALSE(has_dock);
    CHECK_FALSE(has_hint);
    CHECK(has_queue);
    CHECK(has_input);
    // 行账与指纹记"真画出来的"(被钳的行不进指纹)。
    CHECK(tight.chrome.agent_dock_rows.empty());
    CHECK(tight.chrome.transient_rows.empty());
    CHECK(tight.chrome.queue_rows.size() == 2);
}

TEST_CASE("高度预算:绝境开窗——输入行比窗高也围光标画得下") {
    const Theme plain;
    // 十条逻辑行,光标在末行行尾。
    std::vector<std::u32string> lines;
    for (int i = 1; i <= 10; ++i) {
        lines.push_back(i == 10 ? U"第十行" : U"第" + std::u32string(1, U'0' + i) + U"行");
    }
    BottomChromeModel model =
        FramedModel(ComposerState(std::move(lines), 9, 3), ComposerMode::BusyQueue);
    model.queue_rows = {"队列标题"};
    model.agent_dock_rows = {"↑/↓ 选择", "● main"};
    // 预算 5:横线 2 + 状态 1 只剩 2 行给输入——十条逻辑行须开窗到 2 行,
    // 窗尾贴光标(第九/第十行),窗口之外一行不画。
    const auto windowed = BuildBottomChromeLayout(model, plain, 40, 5);
    CHECK(windowed.frame.rows.size() == 5);
    CHECK(windowed.composer_row_count == 2);
    CHECK(windowed.chrome.composer_rows == 2);
    CHECK(Contains(windowed.frame.rows[2].text, "第9行"));
    CHECK(Contains(windowed.frame.rows[3].text, "第十行"));
    CHECK(windowed.cursor_row == windowed.composer_first_row + 1);
    CHECK(windowed.frame.cursor_row == windowed.cursor_row);
    // 预算 1:横线/状态全让位,只剩光标那一行——硬约束的底线。
    const auto one_row = BuildBottomChromeLayout(model, plain, 40, 1);
    CHECK(one_row.frame.rows.size() == 1);
    CHECK(Contains(one_row.frame.rows[0].text, "第十行"));
    CHECK(one_row.cursor_row == 0);
    CHECK(one_row.cursor_x == 2 + 6);  // 缩进 2 + "第十行" 三字 6 列(光标在行尾)
}

TEST_CASE("高度预算:刚好装下不多舍,差一行才舍") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U"一行"}, 0, 2), ComposerMode::BusyQueue);
    model.agent_dock_rows = {"↑/↓ 选择", "● main"};
    model.transient_rows = {"  /help"};
    // 不限 = 横线2+输入1+状态1+坞2+提示1 = 7 行;预算 7 一行不少。
    REQUIRE(BuildBottomChromeLayout(model, plain, 40).frame.rows.size() == 7);
    const auto exact = BuildBottomChromeLayout(model, plain, 40, 7);
    CHECK(exact.frame.rows.size() == 7);
    CHECK(exact.dropped_optional_rows == 0);
    // 预算 6 舍提示一行。
    const auto one_less = BuildBottomChromeLayout(model, plain, 40, 6);
    CHECK(one_less.frame.rows.size() == 6);
    CHECK(one_less.dropped_optional_rows == 1);
}

// ---------------------------------------------------------------------------
// 场景帮助层(`?` 键位帮助只能展开不能收起单):帮助行是底栏帧最顶的一块
// retained 层——进帧、进指纹、受高度预算钳制(最保,保头舍尾);开合
// 状态机 HelpOverlayNext 纯逻辑钉死在这里,终端层各处只报事件。
// ---------------------------------------------------------------------------

TEST_CASE("帮助层开合状态机:同一枚键展开/收起,Esc 只收,场景换必收") {
    bool visible = false;
    // 头一按展开,再一按收起——同一枚键,同一个动作。
    visible = HelpOverlayNext(visible, HelpOverlayEvent::TogglePressed);
    CHECK(visible);
    visible = HelpOverlayNext(visible, HelpOverlayEvent::TogglePressed);
    CHECK_FALSE(visible);
    // Esc 只收不开:没开的时候按 Esc 不把帮助顶出来。
    visible = HelpOverlayNext(visible, HelpOverlayEvent::EscapePressed);
    CHECK_FALSE(visible);
    visible = HelpOverlayNext(visible, HelpOverlayEvent::TogglePressed);
    CHECK(visible);
    visible = HelpOverlayNext(visible, HelpOverlayEvent::EscapePressed);
    CHECK_FALSE(visible);
    // 草稿一有正文(打字/粘贴/取回),场景换了,帮助必收。
    visible = HelpOverlayNext(visible, HelpOverlayEvent::TogglePressed);
    CHECK(visible);
    visible = HelpOverlayNext(visible, HelpOverlayEvent::DraftFilled);
    CHECK_FALSE(visible);
    // 底栏让位/场景切换(搜索开、外部编辑器、转录导航、查看态切换)同款。
    visible = HelpOverlayNext(visible, HelpOverlayEvent::TogglePressed);
    CHECK(visible);
    visible = HelpOverlayNext(visible, HelpOverlayEvent::SceneChanged);
    CHECK_FALSE(visible);
}

TEST_CASE("帮助层布局:垫帧最顶,行序在活动条/队列之上,进指纹") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U""}, 0, 0), ComposerMode::Idle);
    model.help_rows = {"帮助表头", "? help.show", "帮助表尾"};
    model.activity_rows = {"• Working (3s)"};
    model.queue_rows = {"待发 1 条"};
    const auto layout = BuildBottomChromeLayout(model, plain, 40);
    // 行序:帮助 3 行 > 活动条 > 队列 > 模式行 > 上横线 > 输入 > 下横线。
    CHECK(layout.frame.rows[0].text == "帮助表头");
    CHECK(layout.frame.rows[1].text == "? help.show");
    CHECK(layout.frame.rows[2].text == "帮助表尾");
    CHECK(layout.frame.rows[3].text == "• Working (3s)");
    CHECK(layout.frame.rows[4].text == "待发 1 条");
    CHECK(layout.frame.rows[5].text == "status");
    CHECK(layout.frame.rows[6].text == PlainRule(40));
    CHECK(layout.composer_first_row == 7);
    CHECK(layout.chrome.help_rows.size() == 3);
    CHECK(layout.chrome.TotalRows() == static_cast<int>(layout.frame.rows.size()));
    // 指纹认帮助行:行变了指纹必变;与别区分区隔开,同文不误判相等。
    BottomChromeFrame a = layout.chrome;
    BottomChromeFrame b = a;
    CHECK(BottomChromeFingerprint(a) == BottomChromeFingerprint(b));
    b.help_rows[1] = "? chat.search_history";
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
    b.help_rows.clear();
    b.queue_rows = a.help_rows;  // 帮助行挪进队列区,分区标记得拦住"相等"
    CHECK(BottomChromeFingerprint(a) != BottomChromeFingerprint(b));
}

TEST_CASE("帮助层布局:高度预算里最保,装不下保头舍尾不挤提示符") {
    const Theme plain;
    BottomChromeModel model = FramedModel(ComposerState({U""}, 0, 0), ComposerMode::Idle);
    model.help_rows = {"表头", "行一", "行二", "行三", "行四", "表尾"};
    model.agent_dock_rows = {"↑/↓ 选择", "● main"};
    model.transient_rows = {"  /help"};
    // 不限 = 帮助6 + 横线2 + 输入1 + 状态1 + 坞2 + 提示1 = 13 行。
    REQUIRE(BuildBottomChromeLayout(model, plain, 40).frame.rows.size() == 13);
    // 预算 10(核心4 + 帮助6 恰好,坞/提示让位):帮助一行不少,提示符照画。
    const auto keeps_help = BuildBottomChromeLayout(model, plain, 40, 10);
    CHECK(keeps_help.frame.rows.size() == 10);
    CHECK(keeps_help.chrome.help_rows.size() == 6);
    CHECK(keeps_help.chrome.agent_dock_rows.empty());
    CHECK(keeps_help.chrome.transient_rows.empty());
    CHECK(keeps_help.dropped_optional_rows == 3);
    bool has_input = false;
    for (const auto& row : keeps_help.frame.rows) {
        if (!row.text.empty() && row.text[0] == '>') {
            has_input = true;
        }
    }
    CHECK(has_input);
    // 预算 7(核心4 + 帮助3):帮助保头舍尾——表头在,表尾让位。
    const auto truncated = BuildBottomChromeLayout(model, plain, 40, 7);
    CHECK(truncated.frame.rows.size() == 7);
    REQUIRE(truncated.chrome.help_rows.size() == 3);
    CHECK(truncated.chrome.help_rows[0] == "表头");
    CHECK(truncated.chrome.help_rows[2] == "行二");
    CHECK(truncated.dropped_optional_rows == 3 + 3);  // 帮助3 + 坞2 + 提示1
    // 绝境(预算 2,连核心都装不下):帮助一行不剩,输入行必画得下。
    const auto desperate = BuildBottomChromeLayout(model, plain, 40, 2);
    CHECK(desperate.frame.rows.size() == 1);
    CHECK(desperate.chrome.help_rows.empty());
    CHECK(desperate.composer_row_count == 1);
}
