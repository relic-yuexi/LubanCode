// 思考流中预览单(逐帧露尾与完毕自折叠)的显示测试:
//   - 状态机纯函数(五态 + Ctrl+O 往返,非法转移原地不动);
//   - 露尾排版 ThinkingPreviewRows(取末尾、按视觉行、宽度/清洗安全);
//   - FormatTranscriptItem 的思考档位(自动预览/折叠/用户展开);
//   - ToolDisplay 数据面(建块/收折/两段隔开/用户档/空正文);
//   - pipe 降级(只落收定一行,无 ANSI、无逐帧伪刷新);
//   - 按帧合并(UiEventPump:千枚 thinking delta 不按枚画);
//   - 四家 wire fixture 回放进同一组显示测试(协议差异不漏进 UI)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/anthropic/events.hpp"
#include "api/chat/events.hpp"
#include "api/gemini/events.hpp"
#include "api/responses/events.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"
#include "app/terminal_turn_sink.hpp"
#include "api_fixture.hpp"
#include "cli/live_transcript.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"

using lubancode::cli::BuiltinTheme;
using lubancode::cli::FormatTranscriptItem;
using lubancode::cli::NextThinkingPhase;
using lubancode::cli::ProviderContentKind;
using lubancode::cli::ThinkingHasVisibleText;
using lubancode::cli::ThinkingPhase;
using lubancode::cli::ThinkingPreviewRows;
using lubancode::cli::ThinkingSignal;
using lubancode::cli::TranscriptItem;
using lubancode::cli::TranscriptKind;
using lubancode::cli::TranscriptStatus;
using lubancode::cli::ToolDisplay;
using lubancode::cli::kThinkingPreviewMaxRows;

namespace {

TranscriptItem MakeThinking(ThinkingPhase phase, TranscriptStatus status) {
    TranscriptItem item;
    item.id = 1;
    item.kind = TranscriptKind::Thinking;
    item.tool_name = "thinking";
    item.status = status;
    item.thinking_phase = phase;
    return item;
}

// 一段渲染文本里有几行(以 \n 计,末截也算一行)。
int LineCountOf(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    int count = 0;
    for (const char c : text) {
        if (c == '\n') {
            ++count;
        }
    }
    if (text.back() != '\n') {
        ++count;
    }
    return count;
}

// 剥掉 ANSI 后的显示宽度(截断检查用)。
std::size_t VisibleWidth(const std::string& line) {
    std::string visible;
    bool in_esc = false;
    for (const char c : line) {
        if (c == '\x1b') {
            in_esc = true;
            continue;
        }
        if (in_esc) {
            if (c == 'm') {
                in_esc = false;
            }
            continue;
        }
        visible += c;
    }
    return lubancode::cli::DisplayWidthUtf8(visible);
}

}  // namespace

// ---- 状态机(单上五态 + Ctrl+O 往返) ---------------------------------------

TEST_CASE("思考状态机:默认路 Hidden->AutoPreview->CollapsedDone,展开路保持展开") {
    // 主链:首枚 delta 起自动预览,完毕自折叠。
    CHECK(NextThinkingPhase(ThinkingPhase::Hidden, ThinkingSignal::FirstDelta) ==
          ThinkingPhase::AutoPreviewRunning);
    CHECK(NextThinkingPhase(ThinkingPhase::AutoPreviewRunning, ThinkingSignal::Done) ==
          ThinkingPhase::CollapsedDone);
    // 用户展开:运行中展开 -> 完毕保持展开(不自动收折)。
    CHECK(NextThinkingPhase(ThinkingPhase::AutoPreviewRunning, ThinkingSignal::ToggleExpand) ==
          ThinkingPhase::ExplicitExpandedRunning);
    CHECK(NextThinkingPhase(ThinkingPhase::ExplicitExpandedRunning, ThinkingSignal::Done) ==
          ThinkingPhase::ExplicitExpandedDone);
    // 收起/再展开的往返:运行中与收定后都走得通。
    CHECK(NextThinkingPhase(ThinkingPhase::ExplicitExpandedRunning, ThinkingSignal::ToggleCollapse) ==
          ThinkingPhase::CollapsedRunning);
    CHECK(NextThinkingPhase(ThinkingPhase::CollapsedRunning, ThinkingSignal::ToggleExpand) ==
          ThinkingPhase::ExplicitExpandedRunning);
    CHECK(NextThinkingPhase(ThinkingPhase::CollapsedDone, ThinkingSignal::ToggleExpand) ==
          ThinkingPhase::ExplicitExpandedDone);
    CHECK(NextThinkingPhase(ThinkingPhase::ExplicitExpandedDone, ThinkingSignal::ToggleCollapse) ==
          ThinkingPhase::CollapsedDone);
}

TEST_CASE("思考状态机:表外组合原地不动,不开第二条隐路") {
    // Done 只许从两个运行态收口;其余相位(含 Hidden、收定态)原地不动。
    const auto frozen_on_done = {ThinkingPhase::Hidden, ThinkingPhase::CollapsedRunning,
                                 ThinkingPhase::CollapsedDone, ThinkingPhase::ExplicitExpandedDone};
    for (const auto phase : frozen_on_done) {
        CHECK(NextThinkingPhase(phase, ThinkingSignal::Done) == phase);
    }
    // ToggleCollapse 只许从两个展开态出发;没伸过手的块不吃"收起"。
    const auto frozen_on_collapse = {ThinkingPhase::Hidden, ThinkingPhase::AutoPreviewRunning,
                                     ThinkingPhase::CollapsedRunning, ThinkingPhase::CollapsedDone};
    for (const auto phase : frozen_on_collapse) {
        CHECK(NextThinkingPhase(phase, ThinkingSignal::ToggleCollapse) == phase);
    }
    CHECK(NextThinkingPhase(ThinkingPhase::Hidden, ThinkingSignal::Done) == ThinkingPhase::Hidden);
    CHECK(NextThinkingPhase(ThinkingPhase::Hidden, ThinkingSignal::ToggleExpand) == ThinkingPhase::Hidden);
    CHECK(NextThinkingPhase(ThinkingPhase::CollapsedDone, ThinkingSignal::FirstDelta) ==
          ThinkingPhase::CollapsedDone);
    CHECK(NextThinkingPhase(ThinkingPhase::CollapsedDone, ThinkingSignal::Done) == ThinkingPhase::CollapsedDone);
    CHECK(NextThinkingPhase(ThinkingPhase::AutoPreviewRunning, ThinkingSignal::ToggleCollapse) ==
          ThinkingPhase::AutoPreviewRunning);  // 没伸过手,自动档不被"收起"误伤
    // 运行态不吃 FirstDelta(重开块是建新 item 的事,不是相位转移)。
    CHECK(NextThinkingPhase(ThinkingPhase::AutoPreviewRunning, ThinkingSignal::FirstDelta) ==
          ThinkingPhase::AutoPreviewRunning);
}

// ---- 露尾排版 --------------------------------------------------------------

TEST_CASE("ThinkingPreviewRows:取末尾不取开头,连续小 delta 随内容推进") {
    const std::string text = "第一行\n第二行\n第三行\n第四行";
    const auto rows = ThinkingPreviewRows(text, 80, kThinkingPreviewMaxRows);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == "第二行");  // 露尾:最早的一行滚出去
    CHECK(rows[1] == "第三行");
    CHECK(rows[2] == "第四行");

    // 内容长一截,画面跟着换:又来一行,最早的那行再滚出去。
    const auto rows2 = ThinkingPreviewRows(text + "\n第五行", 80, kThinkingPreviewMaxRows);
    REQUIRE(rows2.size() == 3);
    CHECK(rows2[0] == "第三行");
    CHECK(rows2[2] == "第五行");
}

TEST_CASE("ThinkingPreviewRows:一枚 5000 字大 delta 也只折三行,长行按宽折不撑破") {
    std::string one_big;
    for (int i = 0; i < 5000; i += 10) {
        one_big += "abcdefghij";  // 5000 列的无空格长串
    }
    for (const int width : {40, 80, 120, 200}) {
        const auto rows = ThinkingPreviewRows(one_big, width, kThinkingPreviewMaxRows);
        REQUIRE(rows.size() == 3);
        for (const std::string& row : rows) {
            CHECK(VisibleWidth(row) <= static_cast<std::size_t>(width - 3));
        }
        // 露的是末尾:最后一行的收尾还是原文的末尾。
        CHECK(rows.back().size() >= 3);
        CHECK(one_big.find(rows.back()) != std::string::npos);
    }
    // 与许多小 delta 得到同一幅画:同一段文本无论怎么切,预览一致。
    const auto whole = ThinkingPreviewRows("甲" + one_big + "乙", 80, 3);
    std::string joined;
    for (const auto& row : whole) {
        joined += row;
    }
    CHECK(joined.find("乙") != std::string::npos);
}

TEST_CASE("ThinkingPreviewRows:多行巨文取尾三条视觉行,行数恒定不超三") {
    std::string text;
    for (int i = 1; i <= 200; ++i) {
        text += "逻辑行" + std::to_string(i) + "\n";
    }
    text += "收尾一句";
    const auto rows = ThinkingPreviewRows(text, 80, kThinkingPreviewMaxRows);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == "逻辑行199");
    CHECK(rows[1] == "逻辑行200");
    CHECK(rows[2] == "收尾一句");
}

TEST_CASE("ThinkingPreviewRows:CJK/emoji 折行不切半个宽字,拼回去不丢尾段") {
    // 105 个汉字 = 210 显示列,80 列宽(折行宽 77)下恰折 38+38+29 三行,
    // 三行全保、一个字不少。(std::string(count, U'汉') 会把宽字截成窄字节,
    // 必须逐字拼 UTF-8。)
    const std::string han_utf8 = "\xE6\xB1\x89";  // 汉(U+6C49)
    std::string wide;
    for (int i = 0; i < 105; ++i) {
        wide += han_utf8;
    }
    const auto rows = ThinkingPreviewRows(wide, 80, 3);
    REQUIRE(rows.size() == 3);
    std::string joined;
    for (const auto& row : rows) {
        CHECK(VisibleWidth(row) <= 77);
        joined += row;
    }
    std::size_t han = 0;
    for (std::size_t i = 0; i + 3 <= joined.size(); i += 3) {
        if (joined.compare(i, 3, han_utf8) == 0) {
            ++han;
        }
    }
    CHECK(han == 105);

    // 更长的串掐头留尾:120 个汉字折四行,只留末三行(首行的 38 个让位)。
    std::string longer;
    for (int i = 0; i < 120; ++i) {
        longer += han_utf8;
    }
    const auto tail_rows = ThinkingPreviewRows(longer, 80, 3);
    REQUIRE(tail_rows.size() == 3);
    std::size_t tail_han = 0;
    for (const auto& row : tail_rows) {
        for (std::size_t i = 0; i + 3 <= row.size(); i += 3) {
            if (row.compare(i, 3, han_utf8) == 0) {
                ++tail_han;
            }
        }
    }
    CHECK(tail_han == 120 - 38);

    // emoji(4 字节码点)与组合字符不崩、不出坏字节。
    const std::string tricky = "e\xCC\x81\xF0\x9F\x9A\x80rocket \xF0\x9F\x92\xA1tail";
    const auto emoji_rows = ThinkingPreviewRows(tricky, 20, 3);
    for (const auto& row : emoji_rows) {
        CHECK(row.find("\x1b") == std::string::npos);
    }
    CHECK(emoji_rows.back().find("tail") != std::string::npos);
}

TEST_CASE("ThinkingPreviewRows:控制字符与 ANSI 注入只当文字或被剥掉,不能操纵终端") {
    const std::string dirty = "\x1b[31m红字\x1b[0m\x07\b\x1b[2J清屏攻击\tTAB";
    const auto rows = ThinkingPreviewRows(dirty, 80, 3);
    REQUIRE_FALSE(rows.empty());
    std::string joined;
    for (const auto& row : rows) {
        CHECK(row.find('\x1b') == std::string::npos);  // 转义序列整段剥掉
        CHECK(row.find('\x07') == std::string::npos);
        CHECK(row.find('\x08') == std::string::npos);
        joined += row;
    }
    CHECK(joined.find("红字") != std::string::npos);  // 可见文字留下
    CHECK(joined.find("清屏攻击") != std::string::npos);
    CHECK(joined.find(' ') != std::string::npos);  // TAB 折空格
}

TEST_CASE("ThinkingPreviewRows:多字节劈开/非法字节不吐坏字节,不崩") {
    // 半个"你"字的续字节(Utf8ToUtf32 跳过非法起始字节,不造乱码)。
    const std::string half = "好\xE4\xBD";  // "好" + "你"的前两字节
    const auto rows = ThinkingPreviewRows(half, 80, 3);
    for (const auto& row : rows) {
        CHECK(row.find("\xE4\xBD") == std::string::npos);  // 坏截不进画面
        CHECK(row.find("好") != std::string::npos);
    }
    // 下一枚 delta 把字补齐,正文完整(门在上游,显示面只认合法 UTF-8)。
    std::string full = "好\xE4\xBD";
    full += "\xA0";  // 补上"你"的末字节:现在是一段合法 UTF-8"好你"
    const auto healed = ThinkingPreviewRows(full, 80, 3);
    REQUIRE_FALSE(healed.empty());
    CHECK(healed.back().find("好你") != std::string::npos);
}

TEST_CASE("ThinkingPreviewRows:空文本/纯空白没有行,预览不露空框") {
    CHECK(ThinkingPreviewRows("", 80, 3).empty());
    // 纯空白文本折出的行只含空格;渲染档的 ThinkingHasVisibleText 闸
    // 保证它们不上屏(不铺三行空框)。
    const auto blank_rows = ThinkingPreviewRows(" \n\t\n  ", 80, 3);
    for (const auto& row : blank_rows) {
        CHECK(row.find_first_not_of(' ') == std::string::npos);
    }
    CHECK(ThinkingHasVisibleText("有字") == true);
    CHECK(ThinkingHasVisibleText(" \n\t\r ") == false);
    CHECK(ThinkingHasVisibleText("") == false);
    CHECK(ThinkingHasVisibleText("\x1b[31m\x1b[0m") == true);  // 转义字节本身算字节,不算空白
}

TEST_CASE("ThinkingPreviewRows:不跑 Markdown——星号反引点原样是文字") {
    const std::string md = "**加粗** `code` | 表格 | 列\n- 条目";
    const auto rows = ThinkingPreviewRows(md, 200, 3);
    REQUIRE_FALSE(rows.empty());
    // 原样文本,没有 markdown 渲染痕迹(没有 ANSI 色码)。
    CHECK(rows[0].find("**加粗**") != std::string::npos);
    CHECK(rows[0].find("`code`") != std::string::npos);
    for (const auto& row : rows) {
        CHECK(row.find('\x1b') == std::string::npos);
    }
}

TEST_CASE("ThinkingPreviewRows:resize 换宽重算,三行预算与宽度上限不变") {
    const std::string text = "短句\n" + std::string(100, 'x') + "\n再一句";
    for (const int width : {40, 80, 120, 200}) {
        const auto rows = ThinkingPreviewRows(text, width, kThinkingPreviewMaxRows);
        CHECK(rows.size() <= 3);
        for (const auto& row : rows) {
            CHECK(VisibleWidth(row) <= static_cast<std::size_t>(width - 3));
        }
    }
}

// ---- FormatTranscriptItem 的思考档位 ----------------------------------------

TEST_CASE("思考条目渲染:自动预览档 = 标题 + 弱色三行,折叠档只留标题") {
    const auto dark = BuiltinTheme("dark");
    TranscriptItem item = MakeThinking(ThinkingPhase::AutoPreviewRunning, TranscriptStatus::Running);
    item.title = "思考中… 7.4s";
    item.full_output = "行一\n行二\n行三\n行四";
    const std::string out = FormatTranscriptItem(item, dark, 120);
    CHECK(out.find("思考中… 7.4s\n") != std::string::npos);
    CHECK(out.find("行二") != std::string::npos);  // 露尾三行 = 行二三四
    CHECK(out.find("行一") == std::string::npos);
    CHECK(out.find("行四") != std::string::npos);
    CHECK(LineCountOf(out) == 4);  // 标题 + 三行预览
    // 预览行挂弱色(dark.stats),不抢正文层级。
    CHECK(out.find(dark.stats + "行四" + dark.reset) != std::string::npos);

    // plain 主题:同款结构,零 ANSI,首行带 [RUNNING] 状态词。
    const auto plain = FormatTranscriptItem(item, BuiltinTheme("plain"), 120);
    CHECK(LineCountOf(plain) == 4);
    CHECK(plain.find("[RUNNING] 思考中… 7.4s") == 0);
    CHECK(plain.find('\x1b') == std::string::npos);

    // 折叠运行档(CollapsedRunning):标题一行,正文一字不露。
    item.thinking_phase = ThinkingPhase::CollapsedRunning;
    const std::string collapsed = FormatTranscriptItem(item, BuiltinTheme("plain"), 120);
    CHECK(LineCountOf(collapsed) == 1);
    CHECK(collapsed.find("行四") == std::string::npos);
}

TEST_CASE("思考条目渲染:用户展开档不吃全局开关,标题带字数,正文全文") {
    TranscriptItem item = MakeThinking(ThinkingPhase::ExplicitExpandedRunning, TranscriptStatus::Running);
    item.title = "思考中… 7.4s";
    item.full_output = "想第一段\n想第二段";
    // 全局紧凑(expanded=false),本条自己的展开态照样铺全文。
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 120, /*expanded=*/false);
    CHECK(out.find("思考中… 7.4s · 9 字") != std::string::npos);  // 换行也算码点
    CHECK(out.find("想第一段") != std::string::npos);
    CHECK(out.find("想第二段") != std::string::npos);
}

TEST_CASE("思考条目渲染:收定折叠一行带 Ctrl+O 提示,空正文不露空框") {
    TranscriptItem done = MakeThinking(ThinkingPhase::CollapsedDone, TranscriptStatus::Ok);
    done.title = "思考 10.2s(Ctrl+O 展开)";
    done.full_output = "完整思考正文";
    const std::string out = FormatTranscriptItem(done, BuiltinTheme("plain"), 120);
    CHECK(LineCountOf(out) == 1);
    CHECK(out.find("Ctrl+O") != std::string::npos);
    CHECK(out.find("完整思考正文") == std::string::npos);  // 折叠态藏正文

    // 无正文(仅 signature/redacted):不铺空框,展开档也不给"无完整输出"占位。
    TranscriptItem empty = MakeThinking(ThinkingPhase::CollapsedDone, TranscriptStatus::Ok);
    empty.title = "思考 3.0s(未提供摘要)";
    empty.provider_content_kind = ProviderContentKind::Redacted;
    const std::string empty_out = FormatTranscriptItem(empty, BuiltinTheme("plain"), 120, /*expanded=*/true);
    CHECK(empty_out.find("未提供摘要") != std::string::npos);
    CHECK(empty_out.find("无完整输出") == std::string::npos);
}

// ---- ToolDisplay 数据面 ------------------------------------------------------

namespace {

// 数据面测试靶:console=false(不碰真终端),plain 主题。
// 主题必须落成具名成员——ToolDisplay 存的是 Theme&,拿临时量喂构造函数
// 会吊一根悬空引用,后面一读就崩(实测教训,别再犯)。
struct ThinkingHarness {
    const lubancode::cli::Theme theme{BuiltinTheme("plain")};
    std::vector<TranscriptItem> transcript;
    ToolDisplay display;
    ThinkingHarness() : display(transcript, theme, /*console=*/false, /*todo=*/nullptr, /*cancel=*/nullptr) {}
    // Ctrl+O 翻档走真通道(监听线程那只回调),锁规约与生产一致。
    void Toggle(bool expanded) {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        static_cast<void>(display.FormatSnapshotForToggleLocked(expanded));
    }
};

}  // namespace

TEST_CASE("ToolDisplay 思考:首枚 delta 起 AutoPreviewRunning,标题与首段可见") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("正在核对请求字段……\n");
    REQUIRE(h.transcript.size() == 1);
    const auto& item = h.transcript.front();
    CHECK(item.kind == TranscriptKind::Thinking);
    CHECK(item.thinking_phase == ThinkingPhase::AutoPreviewRunning);
    CHECK(item.status == TranscriptStatus::Running);
    CHECK(item.full_output == "正在核对请求字段……\n");
    CHECK(item.thinking_text_bytes == static_cast<int>(item.full_output.size()));
    CHECK(item.thinking_signature_bytes == 0);  // signature 只记协议账,引擎未透传
    CHECK(item.title.find("思考中") != std::string::npos);
    // 渲染出来标题 + 首段预览都在。
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 120);
    CHECK(out.find("思考中") != std::string::npos);
    CHECK(out.find("正在核对请求字段") != std::string::npos);
}

TEST_CASE("ToolDisplay 思考:5000 字大 delta 只露三行,完整 buffer 不丢") {
    ThinkingHarness h;
    std::string big;
    for (int i = 1; i <= 200; ++i) {
        big += "第" + std::to_string(i) + "段都要想清楚再动。\n";
    }
    REQUIRE(big.size() > 5000);
    h.display.OnThinkingDelta(big);
    const auto& item = h.transcript.front();
    CHECK(item.full_output == big);  // 一枚大 delta 不丢一个字节
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 80);
    CHECK(LineCountOf(out) == 1 + kThinkingPreviewMaxRows);  // 标题 + 恰三行
}

TEST_CASE("ToolDisplay 思考:done 默认自动收一行带提示;空正文文案稳定") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("先想\n再想");
    h.display.OnThinkingDone();
    REQUIRE(h.transcript.size() == 1);
    const auto& item = h.transcript.front();
    CHECK(item.thinking_phase == ThinkingPhase::CollapsedDone);
    CHECK(item.status == TranscriptStatus::Ok);
    CHECK(item.title.find("思考 ") == 0);
    CHECK(item.title.find("Ctrl+O 展开") != std::string::npos);
    CHECK(item.full_output == "先想\n再想");  // 收折不丢数据
    CHECK(h.display.HasActiveThinking() == false);
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 120);
    CHECK(LineCountOf(out) == 1);

    // 空白正文:未提供摘要,不造内容。
    ThinkingHarness empty_h;
    empty_h.display.OnThinkingDelta(" \n\t ");
    empty_h.display.OnThinkingDone();
    const auto& empty_item = empty_h.transcript.front();
    CHECK(empty_item.title.find("未提供摘要") != std::string::npos);
    CHECK(empty_item.provider_content_kind == ProviderContentKind::Unavailable);
    CHECK(empty_item.title.find("Ctrl+O") == std::string::npos);  // 没正文可展开,不挂提示
}

TEST_CASE("ToolDisplay 思考:运行中展开后续 delta 可见,完毕不强折") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("起头");
    h.Toggle(/*expanded=*/true);  // 用户 Ctrl+O 展开
    h.display.OnThinkingDelta("\n中段");
    h.display.OnThinkingDelta("\n收尾");
    {
        const auto& item = h.transcript.front();
        REQUIRE(item.thinking_phase == ThinkingPhase::ExplicitExpandedRunning);
        CHECK(item.full_output == "起头\n中段\n收尾");  // 展开态照收存
        const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 120, /*expanded=*/false);
        CHECK(out.find("收尾") != std::string::npos);  // 后续 delta 按帧可见(本条展开态不吃全局紧凑)
    }
    h.display.OnThinkingDone();
    const auto& done = h.transcript.front();
    CHECK(done.thinking_phase == ThinkingPhase::ExplicitExpandedDone);  // 完毕不自动收折
    CHECK(done.title.find("Ctrl+O 展开") != std::string::npos);
    // 全局展开档(此刻 Ctrl+O 是开的)展开渲染见全文。
    const std::string out = FormatTranscriptItem(done, BuiltinTheme("plain"), 120, /*expanded=*/true);
    CHECK(out.find("收尾") != std::string::npos);
}

TEST_CASE("ToolDisplay 思考:主动折叠后续 delta 仍收存,画面只留标题") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("第一段");
    h.Toggle(true);   // 展开
    h.Toggle(false);  // 再按 Ctrl+O 收起
    h.display.OnThinkingDelta("\n第二段还在攒");
    const auto& item = h.transcript.front();
    CHECK(item.thinking_phase == ThinkingPhase::CollapsedRunning);
    CHECK(item.full_output == "第一段\n第二段还在攒");  // 数据照收
    const std::string out = FormatTranscriptItem(item, BuiltinTheme("plain"), 120);
    CHECK(LineCountOf(out) == 1);  // 画面只留标题
    CHECK(out.find("第二段") == std::string::npos);
}

TEST_CASE("ToolDisplay 思考:隐式收口幂等——text/tool/done 三触发不双收") {
    // sink 在 TextDelta/ToolStart/UsageUpdated 三处都会补调 OnThinkingDone;
    // 显示面只认一次:重复调用是空操作,条目不重复、状态不翻车。
    ThinkingHarness h;
    h.display.OnThinkingDelta("一段");
    h.display.OnThinkingDone();  // 触发一:显式 done
    h.display.OnThinkingDone();  // 触发二:补调
    h.display.OnThinkingDone();  // 触发三:再补
    REQUIRE(h.transcript.size() == 1);
    CHECK(h.transcript.front().thinking_phase == ThinkingPhase::CollapsedDone);
    CHECK(h.display.HasActiveThinking() == false);
}

TEST_CASE("ToolDisplay 思考:两段思考被工具隔开,两条 item 不串 buffer 与时长") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("第一段思考");
    h.display.OnThinkingDone();
    h.display.OnToolStart("toolu_1", "run_command", nlohmann::json{{"command", "ping"}});
    h.display.OnToolDone("toolu_1", "run_command",
                         lubancode::tools::Tool::Result{"[退出码 0]\nok", false});
    h.display.OnThinkingDelta("第二段思考");
    h.display.OnThinkingDone();

    REQUIRE(h.transcript.size() == 3);  // 思考、工具、思考
    CHECK(h.transcript[0].kind == TranscriptKind::Thinking);
    CHECK(h.transcript[0].full_output == "第一段思考");
    CHECK(h.transcript[1].kind == TranscriptKind::Tool);
    CHECK(h.transcript[2].kind == TranscriptKind::Thinking);
    CHECK(h.transcript[2].full_output == "第二段思考");
    CHECK(h.transcript[2].thinking_phase == ThinkingPhase::CollapsedDone);
    // 时长各记各的:第二段的 start_time 不早于第一段的 end_time。
    CHECK(h.transcript[2].start_time >= h.transcript[0].end_time);
}

TEST_CASE("ToolDisplay 思考:compact/expanded 往返 100 次,全文、字数、时长不变") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("完整正文一个字都不能丢。\n第二行。");
    h.display.OnThinkingDone();
    const auto& item = h.transcript.front();
    const std::string body = item.full_output;
    const int chars = lubancode::cli::CountUtf8Codepoints(body);
    const std::string title = item.title;
    for (int i = 0; i < 100; ++i) {
        h.Toggle(i % 2 == 0);
    }
    CHECK(h.transcript.front().full_output == body);
    CHECK(lubancode::cli::CountUtf8Codepoints(h.transcript.front().full_output) == chars);
    CHECK(h.transcript.front().title == title);
    CHECK(h.transcript.front().thinking_phase == ThinkingPhase::CollapsedDone);  // 收定态不因翻档复活
}

TEST_CASE("ToolDisplay 思考:Toggle 快照当场按用户档翻相位,流中即时生效") {
    ThinkingHarness h;
    h.display.OnThinkingDelta("流中正文");
    std::string compact;
    {
        std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
        compact = h.display.FormatSnapshotForToggleLocked(/*expanded=*/false);
    }
    // 刚按下的"收起"在这份重打里立刻生效:只留标题,不带三行预览。
    const std::string first_line = compact.substr(0, compact.find('\n'));
    CHECK(first_line.find("思考中") != std::string::npos);
    CHECK(compact.find("流中正文") == std::string::npos);
    // 本尊下一枚 delta 起走 CollapsedRunning。
    h.display.OnThinkingDelta("\n追加");
    CHECK(h.transcript.front().thinking_phase == ThinkingPhase::CollapsedRunning);
}

// ---- pipe 降级 --------------------------------------------------------------

TEST_CASE("pipe 降级:思考只有收定一行,无 ANSI、无逐帧伪刷新") {
    lubancode::cli::TermPort().Reset();
    std::stringstream captured;
    lubancode::cli::TermPort().Redirect(&captured, nullptr);
    std::vector<TranscriptItem> transcript;
    std::string mid_stream;
    const lubancode::cli::Theme pipe_theme{BuiltinTheme("plain")};
    {
        ToolDisplay display(transcript, pipe_theme, /*console=*/false,
                            /*todo=*/nullptr, /*cancel=*/nullptr);
        display.OnThinkingDelta("中途的半截思考不该落进日志\n");
        display.OnThinkingDelta("第二帧也不该\n");
        mid_stream = captured.str();
        display.OnThinkingDone();
    }
    lubancode::cli::TermPort().Reset();
    CHECK(mid_stream.empty());  // 运行中一个字节都不写(pipe 无瞬时预览)
    const std::string out = captured.str();
    CHECK(out.find("思考中") == std::string::npos);  // 不发逐帧伪刷新
    CHECK(out.find("半截思考") == std::string::npos);
    CHECK(out.find('\x1b') == std::string::npos);
    // 收定恰好一行"思考 X.Xs"。
    const std::size_t line_at = out.find("思考 ");
    CHECK(line_at != std::string::npos);
    CHECK(out.find("Ctrl+O") == std::string::npos);  // pipe 里没有 Ctrl+O 可按
    CHECK(LineCountOf(out.substr(line_at)) == 1);
}

// ---- 按帧合并 ----------------------------------------------------------------

TEST_CASE("UiEventPump:千枚 thinking delta 合并成少数几批,不按枚落屏") {
    std::atomic<int> render_calls{0};
    std::mutex text_mutex;
    std::string rendered_text;
    {
        lubancode::app::UiEventPump pump(
            [&](const lubancode::runtime::ServerEvent& event) {
                render_calls.fetch_add(1);
                std::lock_guard<std::mutex> lock(text_mutex);
                rendered_text += event.text;
            },
            std::chrono::milliseconds(50));
        for (int i = 0; i < 1000; ++i) {
            lubancode::runtime::ServerEvent event;
            event.kind = lubancode::runtime::ServerEventKind::ItemDelta;
            event.item_id = "think_1";
            event.item_kind = lubancode::runtime::ItemKind::Thinking;
            event.text = "字";
            pump.PostDelta(event);
        }
        pump.StopAndDrain();
    }
    CHECK(rendered_text.size() == 3000);  // 一个"字"都不少
    CHECK(render_calls.load() < 100);     // 落屏次数受帧率约束,不随 delta 数线性涨
}

// ---- 协议入口:四家 wire fixture 回放,同一组显示测试 --------------------------

namespace {

// 按各家 wire 把 fixture 实录解成中立事件(与 test_api_fixture_replay 同一喂法)。
std::vector<lubancode::api::StreamEvent> ReplayWhole(const lubancode_test::ApiFixture& fixture) {
    using namespace lubancode;
    std::vector<api::StreamEvent> events;
    std::vector<api::SseFrame> frames;
    api::SseFramer framer;
    for (auto& frame : framer.feed(fixture.stream)) {
        frames.push_back(std::move(frame));
    }
    // chat/gemini 的 parser 有状态:一只 parser 吃完全部帧,再 Finish。
    api::chat::EventParser chat_parser;
    api::gemini::EventParser gemini_parser;
    for (const auto& frame : frames) {
        const api::SseFrame f{frame.event.empty() ? std::string("message") : frame.event, frame.data};
        if (fixture.wire == "anthropic-messages") {
            if (auto event = api::anthropic::parse_event(f); event.has_value()) {
                events.push_back(*event);
            }
        } else if (fixture.wire == "openai-responses") {
            if (auto event = api::responses::parse_event(f); event.has_value()) {
                events.push_back(*event);
            }
        } else if (fixture.wire == "openai-chat-completions") {
            for (auto& event : chat_parser.Consume(f)) {
                events.push_back(std::move(event));
            }
        } else {
            for (auto& event : gemini_parser.Consume(f)) {
                events.push_back(std::move(event));
            }
        }
    }
    if (fixture.wire == "openai-chat-completions") {
        for (auto& event : chat_parser.Finish()) {
            events.push_back(std::move(event));
        }
    } else if (fixture.wire != "anthropic-messages" && fixture.wire != "openai-responses") {
        for (auto& event : gemini_parser.Finish()) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

// 一家 fixture 走完显示半边:thinking delta 逐枚喂、done 收口,返回账本。
struct WirePicture {
    std::string thinking_text;   // 四家各自拼出的思考正文
    std::string collapsed_title; // 收定折叠标题
    std::vector<std::string> preview_while_running;
    int items = 0;
    int signature_bytes = 0;
};

WirePicture FeedFixture(const lubancode_test::ApiFixture& fixture) {
    WirePicture picture;
    const lubancode::cli::Theme theme{BuiltinTheme("plain")};
    std::vector<TranscriptItem> transcript;
    ToolDisplay display(transcript, theme, /*console=*/false,
                        /*todo=*/nullptr, /*cancel=*/nullptr);
    bool sampled_preview = false;
    for (const auto& event : ReplayWhole(fixture)) {
        if (const auto* thinking = std::get_if<lubancode::api::ThinkingDelta>(&event); thinking != nullptr) {
            picture.thinking_text += thinking->text;
            picture.signature_bytes += static_cast<int>(thinking->signature.size());
            display.OnThinkingDelta(thinking->text);
            if (!sampled_preview && display.HasActiveThinking()) {
                const auto& item = transcript.back();
                picture.preview_while_running = ThinkingPreviewRows(item.full_output, 80, 3);
                sampled_preview = true;
            }
        }
    }
    display.OnThinkingDone();
    picture.items = static_cast<int>(transcript.size());
    if (!transcript.empty()) {
        picture.collapsed_title = transcript.front().title;
    }
    return picture;
}

}  // namespace

TEST_CASE("四家 wire fixture:ThinkingDelta 喂同一组显示测试,画面一致、协议差异不漏进 UI") {
    const struct {
        const char* wire_dir;
        const char* id;
    } sources[] = {
        {"anthropic_messages", "manual_thinking_enabled_stream"},
        {"openai_chat", "vllm_qwen_reasoning_delta"},
        {"openai_responses", "manual_reasoning_summary_stream"},
        {"google_generate_content", "internal_thought_part_stream"},
    };
    for (const auto& source : sources) {
        // doctest 的 CAPTURE 只收一枚实参(同 test_api_fixture_replay 的雷)。
        CAPTURE(source.wire_dir);
        CAPTURE(source.id);
        const auto fixture = lubancode_test::LoadApiFixture(source.wire_dir, source.id);
        REQUIRE(fixture.has_value());
        const WirePicture picture = FeedFixture(*fixture);

        INFO("thinking text: ", picture.thinking_text);
        // 有正文、成了一条思考条目、收定折叠一行。
        CHECK_FALSE(picture.thinking_text.empty());
        REQUIRE(picture.items == 1);
        CHECK(picture.collapsed_title.find("思考 ") == 0);
        CHECK(picture.collapsed_title.find("Ctrl+O 展开") != std::string::npos);
        // 预览行只含正文文本:索引/signature/协议字段一个都不漏进来。
        for (const std::string& row : picture.preview_while_running) {
            CHECK(row.find('\x1b') == std::string::npos);
            CHECK(row.find("rs_") == std::string::npos);  // responses 的 item id
            CHECK(row.find("msg_") == std::string::npos); // anthropic 的 message id
        }
        // signature 只记协议账:显示侧的签名字节账是独立字段(thinking_signature_
        // bytes),正文/预览/字数一概不吃它——anthropic 这册 signature 恒空串,
        // 未来的非空 signature 也走同一只字段,不漏进 UI 文案。
        CHECK(picture.signature_bytes == 0);
    }

    // 四家的收定画面形状一致:同一只 formatter,同一组文案 key。
    {
        const auto load = [](const char* wire, const char* id) {
            const auto fixture = lubancode_test::LoadApiFixture(wire, id);
            REQUIRE(fixture.has_value());
            return FeedFixture(*fixture);
        };
        const WirePicture a = load("anthropic_messages", "manual_thinking_enabled_stream");
        const WirePicture r = load("openai_responses", "manual_reasoning_summary_stream");
        const WirePicture g = load("google_generate_content", "internal_thought_part_stream");
        const WirePicture c = load("openai_chat", "vllm_qwen_reasoning_delta");
        // "思考 <时长>(Ctrl+O 展开)":时长各家不同,骨架逐字节一致。
        const auto skeleton = [](const std::string& title) {
            const std::size_t at = title.find('(');
            return at == std::string::npos ? title : title.substr(0, at);
        };
        CHECK(skeleton(a.collapsed_title).find("思考 ") == 0);
        CHECK(skeleton(r.collapsed_title).find("思考 ") == 0);
        CHECK(skeleton(g.collapsed_title).find("思考 ") == 0);
        CHECK(skeleton(c.collapsed_title).find("思考 ") == 0);
        CHECK(a.collapsed_title.find("(Ctrl+O 展开)") != std::string::npos);
        CHECK(r.collapsed_title.find("(Ctrl+O 展开)") != std::string::npos);
        CHECK(g.collapsed_title.find("(Ctrl+O 展开)") != std::string::npos);
        CHECK(c.collapsed_title.find("(Ctrl+O 展开)") != std::string::npos);
        // 多行正文的两家,预览露的是末尾(取尾不取头)。
        REQUIRE_FALSE(a.preview_while_running.empty());
        CHECK(a.thinking_text.find(a.preview_while_running.back()) != std::string::npos);
        REQUIRE_FALSE(r.preview_while_running.empty());
        CHECK(r.thinking_text.find(r.preview_while_running.back()) != std::string::npos);
    }
}

// ---- sink 接线:显式收口(wire 的 content_block_stop)立刻自折叠 --------------

TEST_CASE("TerminalTurnSink:thinking ItemCompleted 立刻自折叠,delta 只投队列") {
    const lubancode::cli::Theme theme{BuiltinTheme("plain")};
    std::vector<TranscriptItem> transcript;
    ToolDisplay display(transcript, theme, /*console=*/false,
                        /*todo=*/nullptr, /*cancel=*/nullptr);
    lubancode::cli::StreamBodyTracker body(theme, /*enabled=*/false);
    lubancode::app::TerminalTurnSink::Ingredients ingredients;
    ingredients.display = &display;
    ingredients.body_tracker = &body;
    lubancode::app::TerminalTurnSink sink(ingredients);

    lubancode::runtime::ServerEvent delta;
    delta.kind = lubancode::runtime::ServerEventKind::ItemDelta;
    delta.item_id = "think_1";
    delta.item_kind = lubancode::runtime::ItemKind::Thinking;
    delta.text = "流中一段";
    sink.Emit(delta);  // 流内事件:只投队列,泵线程按帧画
    sink.StopUiPump(); // 排干:此刻显示面必已吃到
    REQUIRE(display.HasActiveThinking());

    lubancode::runtime::ServerEvent done;
    done.kind = lubancode::runtime::ServerEventKind::ItemCompleted;
    done.item_id = "think_1";
    done.item_kind = lubancode::runtime::ItemKind::Thinking;
    done.outcome = lubancode::runtime::Outcome::Succeeded;
    sink.Emit(done);  // 控制路:就地画,立刻自折叠
    CHECK(display.HasActiveThinking() == false);
    REQUIRE(transcript.size() == 1);
    CHECK(transcript.front().thinking_phase == lubancode::cli::ThinkingPhase::CollapsedDone);
    CHECK(transcript.front().full_output == "流中一段");
}
