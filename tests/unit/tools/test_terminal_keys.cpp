// ParseTerminalKeys(terminal 工具按键记号解析)单测:纯函数,两平台都跑。
#include <doctest/doctest.h>

#include <string>

#include "tools/terminal_keys.hpp"

using lubancode::tools::ParseTerminalKeys;

namespace {

std::string Keys(const std::string& text, bool app = false) {
    const auto parsed = ParseTerminalKeys(text, app);
    CAPTURE(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("ParseTerminalKeys: 普通文本原样,回车/制表/退格/空格记号") {
    CHECK(Keys("abc") == "abc");
    CHECK(Keys("ls -la<Enter>") == "ls -la\r");
    CHECK(Keys("<enter><CR><Return>") == "\r\r\r");
    CHECK(Keys("<Tab><S-Tab>") == "\t\x1b[Z");
    CHECK(Keys("<BS><Backspace>") == "\x7f\x7f");
    CHECK(Keys("a<Space>b") == "a b");
    CHECK(Keys("").empty());
}

TEST_CASE("ParseTerminalKeys: Esc 与 vim 典型序列") {
    CHECK(Keys("<Esc>") == "\x1b");
    CHECK(Keys("ihello<Esc>:wq<Enter>") == "ihello\x1b:wq\r");
}

TEST_CASE("ParseTerminalKeys: Ctrl 组合不分大小写,含 C0 符号键") {
    CHECK(Keys("<C-c>") == "\x03");
    CHECK(Keys("<C-D>") == "\x04");
    CHECK(Keys("<c-z>") == "\x1a");
    CHECK(Keys("<C-a><C-e>") == "\x01\x05");
    CHECK(Keys("<C-[>") == "\x1b");
    CHECK(Keys("<C-\\>") == "\x1c");
    CHECK(Keys("<C-]>") == "\x1d");
    CHECK(Keys("<C-@>") == std::string(1, '\0'));
    CHECK(Keys("<C-Space>") == std::string(1, '\0'));
}

TEST_CASE("ParseTerminalKeys: 方向键随光标键应用模式切换编码") {
    CHECK(Keys("<Up><Down><Right><Left>") == "\x1b[A\x1b[B\x1b[C\x1b[D");
    CHECK(Keys("<Up><Down><Right><Left>", true) == "\x1bOA\x1bOB\x1bOC\x1bOD");
    CHECK(Keys("<Home><End>") == "\x1b[H\x1b[F");
    CHECK(Keys("<Home><End>", true) == "\x1bOH\x1bOF");
}

TEST_CASE("ParseTerminalKeys: 编辑键与功能键") {
    CHECK(Keys("<PageUp><PageDown>") == "\x1b[5~\x1b[6~");
    CHECK(Keys("<Insert><Delete><Del>") == "\x1b[2~\x1b[3~\x1b[3~");
    CHECK(Keys("<F1><F4>") == "\x1bOP\x1bOS");
    CHECK(Keys("<F5><F10><F12>") == "\x1b[15~\x1b[21~\x1b[24~");
}

TEST_CASE("ParseTerminalKeys: Alt/Meta 组合发 ESC 前缀") {
    CHECK(Keys("<M-x>") == "\x1bx");
    CHECK(Keys("<A-b>") == "\x1b" "b");
    CHECK(Keys("<M-.>") == "\x1b.");
}

TEST_CASE("ParseTerminalKeys: 字面 < 的几种写法") {
    CHECK(Keys("<lt>") == "<");
    CHECK(Keys("<lt>div<gt>") == "<div>");
    CHECK(Keys("a < b") == "a < b");
    CHECK(Keys("cat <<EOF > out.txt") == "cat <<EOF > out.txt");
    CHECK(Keys("x<>y") == "x<>y");
    CHECK(Keys("<Enter") == "<Enter");  // 不闭合:当字面
    CHECK(Keys("if (a<b && c>d)") == "if (a<b && c>d)");  // 体里有空格:不像记号
}

TEST_CASE("ParseTerminalKeys: 不认识的记号直接报错并带原文") {
    for (const char* bad : {"<Entr>", "echo <div>", "<F13>", "<C-1>", "<Fx>"}) {
        CAPTURE(bad);
        const auto parsed = ParseTerminalKeys(bad);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().find("<lt>") != std::string::npos);
    }
    const auto parsed = ParseTerminalKeys("ls<Entr>");
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().find("<Entr>") != std::string::npos);
}
