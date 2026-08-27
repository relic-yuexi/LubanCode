// 渲染边界补钉(显示 bug 扫荡单):把"只有真终端才看得见"的边界形状钉到
// 纯函数层——超长单词不折断、CJK/emoji 不切半个宽字、20 列窄窗不塌、
// 150 条工具条目、深嵌套 markdown、空/纯空白消息、UTF-8 坏字节进渲染层
// (上游清洗之外的兜),以及多轮帧差分不累计。铁律与既有册一致:所有输出
// 行显示宽 ≤ width-1;内容要么完整保留要么带明确省略号,不许悄悄丢。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/bottom_chrome.hpp"
#include "cli/diff.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8/WrapUtf8ToDisplayWidth
#include "cli/markdown.hpp"
#include "cli/terminal_frame.hpp"
#include "platform/terminal_batch.hpp"
#include "cli/theme.hpp"
#include "cli/transcript.hpp"

using lubancode::cli::BottomChromeModel;
using lubancode::cli::BuildBottomChromeLayout;
using lubancode::cli::BuildToolTitle;
using lubancode::cli::BuiltinTheme;
using lubancode::cli::ComposerMode;
using lubancode::cli::ComposerViewModel;
using lubancode::cli::ComputeLineDiff;
using lubancode::cli::DiffLine;
using lubancode::cli::DiffLineKind;
using lubancode::cli::DisplayWidthUtf8;
using lubancode::cli::FormatDiff;
using lubancode::cli::FormatTranscriptItem;
using lubancode::cli::FormatTranscriptItems;
using lubancode::cli::InlineFrame;
using lubancode::cli::InlineFrameRow;
using lubancode::cli::QueueInlineFrameDiff;
using lubancode::cli::RenderMarkdown;
using lubancode::cli::TranscriptItem;
using lubancode::cli::TranscriptKind;
using lubancode::cli::TranscriptStatus;
using lubancode::cli::WrapUtf8ToDisplayWidth;
using lubancode::platform::TerminalBatch;

namespace {

// 剥掉一行里的全部 ANSI 转义序列(\x1b[ ... 终止字母),量显示宽度用。
std::string StripAnsi(const std::string& line) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            i += 2;
            while (i < line.size() &&
                   !((line[i] >= 'a' && line[i] <= 'z') || (line[i] >= 'A' && line[i] <= 'Z'))) {
                ++i;
            }
            if (i < line.size()) {
                ++i;  // 终止字母本身
            }
            continue;
        }
        out += line[i];
        ++i;
    }
    return out;
}

std::vector<std::string> SplitLocal(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (true) {
        const std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
}

int VisibleWidth(const std::string& line) {
    return static_cast<int>(DisplayWidthUtf8(StripAnsi(line)));
}

bool AllLinesWithin(const std::vector<std::string>& lines, int width) {
    for (const std::string& line : lines) {
        if (VisibleWidth(line) > width) {
            return false;
        }
    }
    return true;
}

std::string Join(const std::vector<std::string>& pieces) {
    std::string out;
    for (const std::string& piece : pieces) {
        out += piece;
    }
    return out;
}

TranscriptItem MakeToolItem(int id, const std::string& title) {
    TranscriptItem item;
    item.id = id;
    item.kind = TranscriptKind::Tool;
    item.tool_name = "read_file";
    item.title = title;
    item.status = TranscriptStatus::Ok;
    item.summary_lines = {"读取 3 行"};
    return item;
}

}  // namespace

// ---------------------------------------------------------------------------
// 一、超长行 / 超长单词(没有断点的 base64、URL、路径)
// ---------------------------------------------------------------------------

TEST_CASE("WrapUtf8ToDisplayWidth: 200 字符不可断长词硬折行,内容一字不丢") {
    const std::string word(200, 'A');
    const auto lines = WrapUtf8ToDisplayWidth(word, 20);
    CHECK(lines.size() >= 10);
    CHECK(AllLinesWithin(lines, 20));
    CHECK(Join(lines) == word);  // 硬折不等于丢内容:拼回去逐字节相等
}

TEST_CASE("WrapUtf8ToDisplayWidth: 单个码点比 max_width 还宽时独占一行,不丢") {
    // emoji 宽 2,给它 max_width=1:行会超宽,但码点原子保留。
    const auto lines = WrapUtf8ToDisplayWidth("\xF0\x9F\x98\x80", 1);  // 😀
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "\xF0\x9F\x98\x80");
}

TEST_CASE("RenderMarkdown: 300 字符长词段落,width=20 每行不越界") {
    const std::string text = "hash " + std::string(300, 'x') + " 尾巴";
    const auto lines = RenderMarkdown(text, BuiltinTheme("plain"), 20);
    REQUIRE(!lines.empty());
    CHECK(AllLinesWithin(lines, 19));  // 锚点铁律:截到 width-1
}

TEST_CASE("FormatTranscriptItem: 300 字符路径标题按宽截断加省略,不劈多字节") {
    const std::string long_title = "read_file(" + std::string(300, 'p') + ")";
    TranscriptItem item = MakeToolItem(1, long_title);
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 40, false);
    const auto lines = SplitLocal(out);
    REQUIRE(lines.size() >= 1);
    CHECK(VisibleWidth(lines.front()) <= 40);
    // 省略要有明确记号("..."),不许静默截。
    CHECK(lines.front().find("...") != std::string::npos);
}

TEST_CASE("FormatDiff: 超长行按显示宽截断,width=20 不越界") {
    std::vector<DiffLine> diff;
    DiffLine del;
    del.kind = DiffLineKind::Del;
    del.text = std::string(120, 'z');
    del.old_no = 1;
    diff.push_back(del);
    DiffLine add;
    add.kind = DiffLineKind::Add;
    add.text = std::string(120, 'y');
    add.new_no = 1;
    diff.push_back(add);
    // FormatDiff 自己的截宽契约是 ≤ width(与 markdown 的 width-1 铁律不同:
    // 调用方 BuildFileDiffPreview 传参时已让出 5/10 列)。这里按 API 契约钉。
    const std::string out = FormatDiff(diff, BuiltinTheme("plain"), 20, 0, 0, "a.txt");
    const auto lines = SplitLocal(out);
    CHECK(AllLinesWithin(lines, 20));
}

// ---------------------------------------------------------------------------
// 二、CJK 折行 / emoji 双宽占位
// ---------------------------------------------------------------------------

TEST_CASE("WrapUtf8ToDisplayWidth: CJK 折行不切半个宽字,拼回原文") {
    // 13 个汉字,宽 26;max_width=5:每行 2 字,末行 1 字。
    const std::string text = "一二三四五六七八九十甲乙丙";
    const auto lines = WrapUtf8ToDisplayWidth(text, 5);
    CHECK(AllLinesWithin(lines, 5));
    CHECK(Join(lines) == text);
    for (const std::string& line : lines) {
        CHECK(DisplayWidthUtf8(line) % 2 == 0);  // 每行凑整数个宽字
    }
}

TEST_CASE("WrapUtf8ToDisplayWidth: 奇数宽时宽字符不硬塞,行宽 ≤ max_width") {
    // "ab" + 汉字:max_width=3 时第三列装不下宽字,汉字挪下一行。
    const auto lines = WrapUtf8ToDisplayWidth("ab\xe4\xb8\x80", 3);  // ab一
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "ab");
    CHECK(lines[1] == "\xe4\xb8\x80");
}

TEST_CASE("WrapUtf8ToDisplayWidth: emoji(4 字节双宽)按整码点折行") {
    const std::string emoji = "\xF0\x9F\x98\x80";                        // 😀
    const std::string text = emoji + emoji + emoji + "x" + emoji + emoji;
    const auto lines = WrapUtf8ToDisplayWidth(text, 4);
    CHECK(AllLinesWithin(lines, 4));
    CHECK(Join(lines) == text);
}

TEST_CASE("RenderMarkdown: CJK 列表项窄窗折行,圆点行首不丢,不切半个字") {
    const std::string text = "- \xe5\xb1\xb1\xe4\xb8\xad\xe7\x9a\x84\xe5\x9b\x9b\xe5\xad\xa3"
                             "\xe6\x9c\x89\xe5\xa5\xbd\xe5\xa4\x84";  // - 山中的四季有好处的尾巴
    const auto lines = RenderMarkdown(text, BuiltinTheme("plain"), 10);
    CHECK(AllLinesWithin(lines, 9));
    bool bullet_seen = false;
    for (const std::string& line : lines) {
        if (line.find("\xe2\x80\xa2") != std::string::npos) {  // •
            bullet_seen = true;
        }
    }
    CHECK(bullet_seen);
}

// ---------------------------------------------------------------------------
// 三、20 列级窄窗:transcript / 底栏 / 状态行
// ---------------------------------------------------------------------------

TEST_CASE("FormatTranscriptItem: width=20 标题/摘要全在界内,状态灯仍可见") {
    TranscriptItem item = MakeToolItem(7, "read_file(C:\\very\\long\\path\\to\\output.txt)");
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 20, false);
    const auto lines = SplitLocal(out);
    CHECK(AllLinesWithin(lines, 19));
    CHECK(out.find("[OK]") != std::string::npos);
}

TEST_CASE("FormatTranscriptItem: 子代理条目 20 列下缩进不越界") {
    TranscriptItem item = MakeToolItem(8, "read_file(C:\\sub\\agent\\nested\\deep\\path\\x.txt)");
    item.kind = TranscriptKind::SubTool;
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 20, false);
    CHECK(AllLinesWithin(SplitLocal(out), 19));
}

TEST_CASE("BuildBottomChromeLayout: 20 列窄窗长队列/长状态行/长坞行全在界内") {
    BottomChromeModel model;
    model.activity_rows = {"\xe2\x80\xa2 \xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad (12s)"};  // • 思考中 (12s)
    model.queue_rows = {"\xe6\xad\xa3\xe5\x9c\xa8\xe7\xbc\x96\xe8\xbe\x91\xe6\x8e\x92\xe9\x98\x9f\xe6\xb6\x88\xe6\x81\xaf"
                        "\xc2\xb7" "Enter\xe5\x8e\x9f\xe4\xbd\x8d\xe6\x9b\xbf\xe6\x8d\xa2"};  // 长队列标题
    model.composer.prompt = "> ";
    model.composer.placeholder = "\xe9\x94\xae\xe5\x85\xa5\xe5\xb9\xb6\xe5\x9b\x9e\xe8\xbd\xa6 \xe6\x8e\x92\xe9\x98\x9f"
                                 "\xe4\xb8\x8b\xe4\xb8\x80\xe6\x9d\xa1";  // 键入并回车 排队下一条
    model.composer.mode = ComposerMode::BusyQueue;
    model.status_rows = {"\xe2\x8f\xb5\xe2\x8f\xb5 \xe7\xa1\xae\xe8\xae\xa4\xe6\xa8\xa1\xe5\xbc\x8f (shift+tab "
                         "\xe5\x88\x87\xe6\x8d\xa2) \xc2\xb7 fake-model \xc2\xb7 "
                         "C:\\very\\long\\cwd\\path \xc2\xb7 context 12% \xc2\xb7 30.8k/256k"};
    model.agent_dock_rows = {"  \xe2\x97\x8f general-purpose #12  \xe6\xbc\x94\xe7\xa4\xba\xe4\xbb\xbb\xe5\x8a\xa1"
                             "12 \xe5\xae\x8c\xe6\x88\x90(2 \xe6\xac\xa1\xe5\xb7\xa5\xe5\x85\xb7)"};  // 长坞行
    model.transient_rows = {"  /effort \xe5\x90\x8c /think"};
    const auto layout = BuildBottomChromeLayout(model, BuiltinTheme("plain"), 20);
    std::vector<std::string> rows;
    for (const auto& row : layout.frame.rows) {
        rows.push_back(row.text);
    }
    REQUIRE(!rows.empty());
    CHECK(AllLinesWithin(rows, 19));
}

// ---------------------------------------------------------------------------
// 四、150 条工具条目 / 深嵌套 markdown / 空与纯空白消息
// ---------------------------------------------------------------------------

TEST_CASE("FormatTranscriptItems: 150 条工具条目全部渲染,无一越界") {
    std::vector<TranscriptItem> items;
    items.reserve(150);
    for (int i = 0; i < 150; ++i) {
        items.push_back(MakeToolItem(i + 1, "read_file(src/module" + std::to_string(i) + "/deep/path/file.cpp)"));
    }
    const std::string out = FormatTranscriptItems(items, BuiltinTheme("plain"), 60, false);
    CHECK(lubancode::cli::CountLines(out) >= 150);
    CHECK(AllLinesWithin(SplitLocal(out), 59));
    // 最后一条也在(没渲染到一半悄悄停)。
    CHECK(out.find("module149") != std::string::npos);
}

TEST_CASE("RenderMarkdown: 深嵌套——引用套表格、列表套代码围栏、表格套引用字面量") {
    const std::string text =
        "> \xe5\xbc\x95\xe7\x94\xa8\xe5\xa4\xb4\n"                             // > 引用头
        "\n"
        "- \xe5\xa4\x96\xe5\xb1\x82\n"                                          // - 外层
        "  - \xe5\x86\x85\xe5\xb1\x82\n"                                        //   - 内层
        "    - \xe6\xb7\xb1\xe5\xb1\x82\n"                                      //     - 深层
        "\n"
        "| \xe5\x88\x97\xe4\xb8\x80 | \xe5\x88\x97\xe4\xba\x8c |\n"             // | 列一 | 列二 |
        "| --- | --- |\n"
        "| \xe5\x80\xbc | > \xe4\xb8\x8d\xe6\x98\xaf\xe5\xbc\x95\xe7\x94\xa8 |\n"  // 值里带引用字面量
        "\n"
        "\xe6\xad\xa3\xe6\x96\x87\xe4\xb8\x8b\xe9\x9d\xa2\n";                   // 正文下面
    const auto lines = RenderMarkdown(text, BuiltinTheme("plain"), 40);
    CHECK(AllLinesWithin(lines, 39));
    CHECK(lines.size() >= 8);  // 结构都摆出来了
}

TEST_CASE("RenderMarkdown: 空文本与纯空白文本零行输出,不崩") {
    CHECK(RenderMarkdown("", BuiltinTheme("plain"), 80).empty());
    CHECK(RenderMarkdown(" \n\t \n  \n", BuiltinTheme("plain"), 80).empty());
}

TEST_CASE("LayoutUserPromptBlock: 空白消息不产生任何行(不铺空色块)") {
    const auto layout = lubancode::cli::LayoutUserPromptBlock("  \n\t ", BuiltinTheme("plain"), 80);
    CHECK(layout.rows.empty());
}

// ---------------------------------------------------------------------------
// 五、UTF-8 坏字节进渲染层(上游清洗之外,渲染层自己的兜)
// ---------------------------------------------------------------------------

TEST_CASE("WrapUtf8ToDisplayWidth: 坏字节跳过不崩,合法部分保真") {
    const std::string bad = "\xff\xfe\xe4\xb8\x80\xff";  // 两个坏首字节夹"一"
    const auto lines = WrapUtf8ToDisplayWidth(bad, 10);
    REQUIRE(!lines.empty());
    CHECK(lines[0] == "\xe4\xb8\x80");
}

TEST_CASE("RenderMarkdown: 坏字节混在标题/正文/表格里不崩,行宽守界") {
    const std::string text = std::string("## \xe6\xa0\x87") + "\xff" + "\xe9\xa2\x98\n"  // 标题夹坏字节
                             "\xff\xff \xe6\xad\xa3\xe6\x96\x87\n"
                             "| a\xff | b |\n| --- | --- |\n| 1 | 2\xff |\n";
    const auto lines = RenderMarkdown(text, BuiltinTheme("plain"), 30);
    CHECK(AllLinesWithin(lines, 29));
}

TEST_CASE("FormatTranscriptItem/BuildToolTitle: 坏字节进标题与摘要不崩不越界") {
    TranscriptItem item = MakeToolItem(3, "read_file(\xff\xfe bad \xe8\xb7\xaf\xe5\xbe\x84.txt)");
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 30, false);
    CHECK(AllLinesWithin(SplitLocal(out), 29));

    const nlohmann::json input = {{"path", std::string("\xff/bad/\xfe file.txt")}};
    const std::string title = BuildToolTitle("read_file", input);
    CHECK(!title.empty());  // 解码跳过坏字节,合法部分照拼
}

TEST_CASE("FormatDiff: 坏字节进删除/新增行不崩,行宽守界") {
    std::vector<DiffLine> diff;
    DiffLine del;
    del.kind = DiffLineKind::Del;
    del.text = "\xff\xe4\xb8\x80\xff";
    del.old_no = 1;
    diff.push_back(del);
    DiffLine add;
    add.kind = DiffLineKind::Add;
    add.text = "\xfe\xe4\xba\x8c";
    add.new_no = 1;
    diff.push_back(add);
    const std::string out = FormatDiff(diff, BuiltinTheme("plain"), 24, 0, 0, "x.txt");
    CHECK(AllLinesWithin(SplitLocal(out), 23));
}

// ---------------------------------------------------------------------------
// 六、多轮重铺:帧差分不累计、空帧零开销
// ---------------------------------------------------------------------------

namespace {

InlineFrame FrameOf(std::vector<std::string> texts) {
    InlineFrame frame;
    for (std::string& text : texts) {
        frame.rows.push_back(InlineFrameRow{0, 80, false, std::move(text)});
    }
    frame.cursor_row = static_cast<int>(frame.rows.size());
    return frame;
}

}  // namespace

TEST_CASE("QueueInlineFrameDiff: 50 轮交替重铺,变更行数恒定不累计") {
    const InlineFrame a = FrameOf({"one", "two", "three"});
    const InlineFrame b = FrameOf({"one", "TWO", "three"});
    TerminalBatch batch;  // 只攒不发(不 Flush 就不会真写终端)
    const InlineFrame* prev = &a;
    for (int round = 0; round < 50; ++round) {
        const InlineFrame& next = (round % 2 == 0) ? b : a;
        const auto stats = QueueInlineFrameDiff(batch, prev, next, 0);
        CHECK(stats.changed_rows == 1);  // 每轮恰好一行变更,不随轮数膨胀
        prev = &next;
    }
}

TEST_CASE("QueueInlineFrameDiff: 帧不变零输出(重铺同帧是 no-op)") {
    const InlineFrame frame = FrameOf({"x", "y"});
    TerminalBatch batch;
    const auto stats = QueueInlineFrameDiff(batch, &frame, frame, 0);
    CHECK(stats.changed_rows == 0);
    CHECK_FALSE(stats.emitted);
}
