// TerminalScreen(terminal 工具的最小终端仿真器)单测:纯类,两平台都跑。
// 覆盖写字、换行、回车覆盖、退格、制表、光标定位、清行清屏、插删、滚屏、
// 滚动区、备用屏进出、应答、UTF-8 宽度与续接、未知序列不崩。
#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "tools/terminal_screen.hpp"

using lubancode::tools::TerminalCharWidth;
using lubancode::tools::TerminalScreen;

namespace {

std::string Line(const TerminalScreen& screen, std::size_t row) {
    const auto lines = screen.Lines();
    REQUIRE(row < lines.size());
    return lines[row];
}

}  // namespace

TEST_CASE("TerminalScreen: 写字推进光标,CRLF 换行,裸 LF 保留列") {
    TerminalScreen screen(5, 20);
    screen.Feed("hello");
    CHECK(Line(screen, 0) == "hello");
    CHECK(screen.cursor_row() == 0);
    CHECK(screen.cursor_col() == 5);

    screen.Feed("\r\nworld");
    CHECK(Line(screen, 1) == "world");

    screen.Feed("\nxy");  // 裸 LF:只下移,不回列首
    CHECK(Line(screen, 2) == "     xy");
    CHECK(screen.Snapshot() == "hello\nworld\n     xy");
}

TEST_CASE("TerminalScreen: 回车覆盖、退格、制表") {
    TerminalScreen screen(3, 20);
    screen.Feed("hello\rHE");
    CHECK(Line(screen, 0) == "HEllo");

    TerminalScreen bs(3, 20);
    bs.Feed("abc\b\bX");
    CHECK(Line(bs, 0) == "aXc");
    bs.Feed("\r\nxyz\b \b");  // shell 删字的惯用三连
    CHECK(Line(bs, 1) == "xy");
    CHECK(bs.cursor_col() == 2);

    TerminalScreen tab(3, 20);
    tab.Feed("a\tb");
    CHECK(Line(tab, 0) == "a       b");
}

TEST_CASE("TerminalScreen: CUP/CUU/CUD/CUF/CUB/CHA/VPA 定位且越界夹住") {
    TerminalScreen screen(10, 20);
    screen.Feed("\x1b[3;5HX");
    CHECK(Line(screen, 2) == "    X");

    screen.Feed("\x1b[5;5H\x1b[2A\x1b[3C\x1b[1B\x1b[2DZ");
    CHECK(screen.cursor_row() == 3);
    CHECK(Line(screen, 3) == "     Z");

    screen.Feed("\x1b[8G");
    CHECK(screen.cursor_col() == 7);
    screen.Feed("\x1b[9d");
    CHECK(screen.cursor_row() == 8);

    screen.Feed("\x1b[999;999H");
    CHECK(screen.cursor_row() == 9);
    CHECK(screen.cursor_col() == 19);
    screen.Feed("\x1b[H");
    CHECK(screen.cursor_row() == 0);
    CHECK(screen.cursor_col() == 0);
}

TEST_CASE("TerminalScreen: EL 三档与 ED 三档") {
    TerminalScreen el(3, 20);
    el.Feed("hello\x1b[3G\x1b[K");
    CHECK(Line(el, 0) == "he");
    el.Feed("\r\nhello\x1b[3G\x1b[1K");
    CHECK(Line(el, 1) == "   lo");
    el.Feed("\r\nhello\x1b[2K");
    CHECK(Line(el, 2).empty());

    TerminalScreen ed(4, 20);
    ed.Feed("aaa\r\nbbb\r\nccc\r\nddd");
    ed.Feed("\x1b[2;2H\x1b[J");
    CHECK(ed.Snapshot() == "aaa\nb");
    ed.Feed("\x1b[1;2H\x1b[1J");
    CHECK(Line(ed, 0) == "  a");
    ed.Feed("\x1b[2J");
    CHECK(ed.Snapshot().empty());
}

TEST_CASE("TerminalScreen: ICH/DCH/ECH 与 IL/DL") {
    TerminalScreen screen(4, 20);
    screen.Feed("abcdef\x1b[1G\x1b[2P");
    CHECK(Line(screen, 0) == "cdef");
    screen.Feed("\x1b[1G\x1b[2@");
    CHECK(Line(screen, 0) == "  cdef");
    screen.Feed("\x1b[3G\x1b[2X");
    CHECK(Line(screen, 0) == "    ef");

    TerminalScreen lines(4, 20);
    lines.Feed("1\r\n2\r\n3\r\n4");
    lines.Feed("\x1b[2H\x1b[L");
    CHECK(lines.Snapshot() == "1\n\n2\n3");
    lines.Feed("\x1b[M");
    CHECK(lines.Snapshot() == "1\n2\n3");
}

TEST_CASE("TerminalScreen: 满屏换行滚屏,顶行进滚动历史") {
    TerminalScreen screen(3, 10);
    screen.Feed("1\r\n2\r\n3\r\n4\r\n5");
    CHECK(screen.Snapshot() == "3\n4\n5");
    const auto history = screen.Scrollback(10);
    REQUIRE(history.size() == 2);
    CHECK(history[0] == "1");
    CHECK(history[1] == "2");
    CHECK(screen.Scrollback(1) == std::vector<std::string>{"2"});
}

TEST_CASE("TerminalScreen: 滚动历史有上限") {
    TerminalScreen screen(2, 10, 3);
    for (int i = 0; i < 10; ++i) {
        screen.Feed(std::to_string(i) + "\r\n");
    }
    const auto history = screen.Scrollback(100);
    REQUIRE(history.size() == 3);
    CHECK(history.back() == "8");
}

TEST_CASE("TerminalScreen: DECSTBM 滚动区内滚,区外不动,不进历史") {
    TerminalScreen screen(5, 10);
    screen.Feed("top\r\n\r\n\r\n\r\nbottom");
    screen.Feed("\x1b[2;4r");  // 第 2..4 行为滚动区,光标归位
    CHECK(screen.cursor_row() == 0);
    screen.Feed("\x1b[2;1Ha\r\nb\r\nc\r\nd");
    CHECK(Line(screen, 0) == "top");
    CHECK(Line(screen, 1) == "b");
    CHECK(Line(screen, 2) == "c");
    CHECK(Line(screen, 3) == "d");
    CHECK(Line(screen, 4) == "bottom");
    CHECK(screen.Scrollback(10).empty());

    // 顶端反向换行:区内下滚
    screen.Feed("\x1b[2;1H\x1bM");
    CHECK(Line(screen, 1).empty());
    CHECK(Line(screen, 2) == "b");
}

TEST_CASE("TerminalScreen: 反向换行在屏顶下滚一行") {
    TerminalScreen screen(3, 10);
    screen.Feed("a\x1bM");
    CHECK(Line(screen, 0).empty());
    CHECK(Line(screen, 1) == "a");
}

TEST_CASE("TerminalScreen: ?1049 备用屏进出,主屏与光标原样回来") {
    TerminalScreen screen(5, 20);
    screen.Feed("$ vim a.txt");
    const int col = screen.cursor_col();
    screen.Feed("\x1b[?1049h\x1b[H\x1b[2J");
    CHECK(screen.alternate_screen());
    CHECK(screen.Snapshot().empty());
    screen.Feed("~\r\n~\r\n\x1b[5;1H-- INSERT --");
    CHECK(Line(screen, 4) == "-- INSERT --");

    screen.Feed("\x1b[?1049l");
    CHECK_FALSE(screen.alternate_screen());
    CHECK(screen.Snapshot() == "$ vim a.txt");
    CHECK(screen.cursor_row() == 0);
    CHECK(screen.cursor_col() == col);

    // 备用屏里的滚动不进历史
    screen.Feed("\x1b[?1049h");
    for (int i = 0; i < 10; ++i) {
        screen.Feed("x\r\n");
    }
    screen.Feed("\x1b[?1049l");
    CHECK(screen.Scrollback(100).empty());

    // ?47 / ?1047 也认
    screen.Feed("\x1b[?47h");
    CHECK(screen.alternate_screen());
    screen.Feed("\x1b[?47l\x1b[?1047h");
    CHECK(screen.alternate_screen());
    screen.Feed("\x1b[?1047l");
    CHECK_FALSE(screen.alternate_screen());
}

TEST_CASE("TerminalScreen: 自动换行与延迟换行") {
    TerminalScreen screen(3, 5);
    screen.Feed("abcdefg");
    CHECK(Line(screen, 0) == "abcde");
    CHECK(Line(screen, 1) == "fg");

    // 恰好写满末列再 CRLF:不多出空行
    TerminalScreen exact(3, 5);
    exact.Feed("abcde\r\nx");
    CHECK(exact.Snapshot() == "abcde\nx");

    // ?7l 关自动换行:末列反复覆盖
    TerminalScreen nowrap(3, 5);
    nowrap.Feed("\x1b[?7labcdefg");
    CHECK(Line(nowrap, 0) == "abcdg");
    CHECK(Line(nowrap, 1).empty());
}

TEST_CASE("TerminalScreen: SGR 与未知序列忽略,OSC/DCS 吞净") {
    TerminalScreen screen(3, 40);
    screen.Feed("\x1b[1;31mred\x1b[0m \x1b[38;5;196mx\x1b[m");
    CHECK(Line(screen, 0) == "red x");

    screen.Feed("\r\n\x1b]0;window title\x07ok");
    CHECK(Line(screen, 1) == "ok");
    screen.Feed("\x1b]2;title\x1b\\!");
    CHECK(Line(screen, 1) == "ok!");
    screen.Feed("\x1bP+q544e\x1b\\?");
    CHECK(Line(screen, 1) == "ok!?");

    // 私有/带中间字节/离谱参数:不崩、不出字
    screen.Feed("\x1b[?999;12345z\x1b[>c\x1b[2 q\x1b[?u\x1b(B\x1b=\x1b>");
    screen.Feed("\x1b[" + std::string(500, '1') + "A");
    screen.Feed("\x1b[99999999999999999999B");
    CHECK(Line(screen, 1) == "ok!?");
    CHECK(screen.TakeReplies().empty());  // DA2 故意不答
}

TEST_CASE("TerminalScreen: 随机字节流不崩,光标始终在界内") {
    std::mt19937 rng(20261022);
    std::uniform_int_distribution<int> byte(0, 255);
    TerminalScreen screen(8, 30);
    for (int round = 0; round < 200; ++round) {
        std::string chunk;
        for (int i = 0; i < 64; ++i) {
            // 掺多些 ESC 与 '[',好撞进各类序列分支
            const int r = byte(rng);
            chunk.push_back(r < 20 ? '\x1b' : (r < 40 ? '[' : static_cast<char>(r)));
        }
        screen.Feed(chunk);
        REQUIRE(screen.cursor_row() >= 0);
        REQUIRE(screen.cursor_row() < screen.rows());
        REQUIRE(screen.cursor_col() >= 0);
        REQUIRE(screen.cursor_col() < screen.cols());
    }
    CHECK(screen.Lines().size() == 8);
    (void)screen.Snapshot();
    (void)screen.TakeReplies();
}

TEST_CASE("TerminalScreen: DSR 与 DA1 生成应答") {
    TerminalScreen screen(10, 40);
    screen.Feed("\x1b[2;3H\x1b[6n");
    CHECK(screen.TakeReplies() == "\x1b[2;3R");
    CHECK(screen.TakeReplies().empty());
    screen.Feed("\x1b[5n\x1b[c");
    CHECK(screen.TakeReplies() == "\x1b[0n\x1b[?1;2c");
}

TEST_CASE("TerminalScreen: ?1 光标键应用模式开关") {
    TerminalScreen screen(3, 10);
    CHECK_FALSE(screen.app_cursor_keys());
    screen.Feed("\x1b[?1h");
    CHECK(screen.app_cursor_keys());
    screen.Feed("\x1b[?1l");
    CHECK_FALSE(screen.app_cursor_keys());
}

TEST_CASE("TerminalScreen: UTF-8 中文按 2 列,跨 Feed 续接,坏字节记替换符") {
    TerminalScreen screen(4, 20);
    screen.Feed("中文ab");
    CHECK(Line(screen, 0) == "中文ab");
    CHECK(screen.cursor_col() == 6);

    // "中" = E4 B8 AD,切成两段喂
    screen.Feed("\r\n\xe4\xb8");
    screen.Feed("\xad!");
    CHECK(Line(screen, 1) == "中!");
    CHECK(screen.cursor_col() == 3);

    // 覆盖宽字符左半:右半一并清
    screen.Feed("\r\x1b[1Gx");
    CHECK(Line(screen, 1) == "x !");

    screen.Feed("\r\n\xff\xe4z");  // 孤立坏字节 + 被打断的序列
    CHECK(Line(screen, 2) == "\xef\xbf\xbd\xef\xbf\xbdz");

    // 组合附加符零宽丢弃
    screen.Feed("\r\ne\xcc\x81!");
    CHECK(Line(screen, 3) == "e!");
}

TEST_CASE("TerminalScreen: 宽字符落末列时折到下一行") {
    TerminalScreen screen(3, 5);
    screen.Feed("abcd中");
    CHECK(Line(screen, 0) == "abcd");
    CHECK(Line(screen, 1) == "中");
}

TEST_CASE("TerminalCharWidth: 宽度取舍") {
    CHECK(TerminalCharWidth(U'a') == 1);
    CHECK(TerminalCharWidth(0xE9) == 1);
    CHECK(TerminalCharWidth(0x4E2D) == 2);   // 中
    CHECK(TerminalCharWidth(0xAC00) == 2);   // 가
    CHECK(TerminalCharWidth(0xFF21) == 2);   // 全角 A
    CHECK(TerminalCharWidth(0x1F600) == 2);  // 😀
    CHECK(TerminalCharWidth(0x0301) == 0);   // 组合重音
    CHECK(TerminalCharWidth(0x200B) == 0);   // 零宽空格
}

TEST_CASE("TerminalScreen: ESC c 复位清屏,滚动历史保留") {
    TerminalScreen screen(2, 10);
    screen.Feed("1\r\n2\r\n3");
    screen.Feed("\x1b[?1h\x1b" "c");
    CHECK(screen.Snapshot().empty());
    CHECK_FALSE(screen.app_cursor_keys());
    CHECK(screen.Scrollback(10).size() == 1);
}
