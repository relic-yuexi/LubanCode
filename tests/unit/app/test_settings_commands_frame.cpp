// TUI 排版批 5b(tui优化todo.todo:/update check)的输出形状册。
//   - usage 路径(参数不认得):用法句按句内冒号拆列进键值对框;
//   - "正在检查"流式行等网络,不进框(批 2 裁量 2),check 的网络结果面
//     (current/available/failed)依赖 GitHub 可达性,CI 里状态不定,本册
//     不钉——PR 标"真机未验";
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /update 无 --json/--format 分支(源码核实),字节级不变一条天然满足。

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "app/commands/settings_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"

using namespace lubancode;

namespace {

class OutputCapture {
public:
    OutputCapture() { cli::TermPort().Redirect(&stream_, nullptr); }
    ~OutputCapture() { cli::TermPort().Reset(); }
    std::string text() const { return stream_.str(); }

private:
    std::ostringstream stream_;
};

std::string StripAnsi(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

}  // namespace

TEST_CASE("usage 路径: 用法句拆列进键值对框,退出码仍为 false") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    OutputCapture capture;
    const bool ok = app::HandleUpdateCommand("bogus", /*connect_timeout_ms=*/1,
                                             /*request_timeout_secs=*/1, theme);
    const std::string out = capture.text();
    CHECK_FALSE(ok);  // 退出码语义不动(合同第 1 条)
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    CHECK(Contains(plain, "用法"));
    CHECK(Contains(plain, "/update"));
    // key 列走 row_label 色。
    CHECK(Contains(out, theme.row_label + "用法"));
}

TEST_CASE("plain 主题: usage 路径零转义字节、无框字形(T3/--no-color 路径)") {
    OutputCapture capture;
    const bool ok = app::HandleUpdateCommand("bogus", 1, 1, cli::BuiltinTheme("plain"));
    const std::string out = capture.text();
    CHECK_FALSE(ok);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!Contains(out, kBoxLightTopLeft));
    CHECK(!Contains(out, kBoxLightVert));
    CHECK(Contains(out, "/update"));  // 信息一字不少
}
