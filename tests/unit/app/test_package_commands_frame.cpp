// TUI 排版批 4(/package 全族)的输出形状册。
//   - list:主表(id/scope/state,state 列 valid=pass 语义色)+ 明细表(id/
//     detail 长文本),标题句(尾冒号剥掉)嵌上边框;
//   - 长清单(单子验收:结果行数 ≥50 时不破对齐)——55 只包钉表格行全在
//     框内、id 列起始列一致;
//   - show:头部键值对框(id+版本作标题,状态/来源/启停 key 列对齐)+
//     组件清单框;
//   - trust/enable/disable/reload 反馈与"没找到包"错误全进键值对框;
//   - plain 主题(theme 空指针)钉零转义、无框。
//
// 走真 HandleSlashPackage(temp 主目录下造 user 层包,TermPort 改道捕获)。
// 官方层随测试 exe 走(build 树里没有 packages/),对账只认 probe 前缀的行。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/package_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/slash_commands.hpp"
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

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int DisplayColOf(const std::string& row, const std::string& needle) {
    const std::size_t at = row.find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, at)));
}

// 剥掉 CSI 序列(与 test_plugin_commands_frame.cpp 同一把手写的尺)。
std::string StripAnsiLight(const std::string& text) {
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

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(cli::DisplayWidthUtf8(row.substr(0, i)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

// temp 主目录 + user 层 N 只最小合法包(schema/id/version/name/description)。
struct PackageFixture {
    std::filesystem::path home;
    std::optional<std::string> home_value;

    explicit PackageFixture(int package_count) {
        std::error_code ec;
        home = std::filesystem::temp_directory_path() /
               ("lubancode-package-frame-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(home / "packages", ec);
        for (int i = 0; i < package_count; ++i) {
            std::string suffix = std::to_string(i);
            while (suffix.size() < 3) {
                suffix = "0" + suffix;
            }
            const std::string id = "probe.pkg" + suffix;
            const std::filesystem::path pkg = home / "packages" / id;
            std::filesystem::create_directories(pkg, ec);
            std::ofstream yaml(pkg / "package.yaml", std::ios::binary | std::ios::trunc);
            yaml << "schema: 1\n"
                 << "id: " << id << "\n"
                 << "version: 0.1." << (i % 10) << "\n"
                 << "name: probe " << suffix << "\n"
                 << "description: d\n";
            yaml.close();
        }
        home_value = [] (const std::filesystem::path& p) {
            const std::u8string raw = p.u8string();
            return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
        }(home);
    }

    ~PackageFixture() {
        std::error_code ec;
        std::filesystem::remove_all(home, ec);
    }

    PackageFixture(const PackageFixture&) = delete;
    PackageFixture& operator=(const PackageFixture&) = delete;

    std::string Run(const std::string& args, const cli::Theme* theme) {
        app::PackageCommandContext ctx;
        ctx.home_lubancode = &home_value;
        ctx.theme = theme;
        OutputCapture capture;
        const cli::ParsedSlashCommand parsed = cli::ParseSlashCommand("/package " + args);
        app::HandleSlashPackage(ctx, parsed);
        return capture.text();
    }
};

}  // namespace

TEST_CASE("list: 主表+明细表进 frame,state 列语义色(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    PackageFixture fixture(/*package_count=*/3);
    const std::string out = fixture.Run("list", &dark);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsiLight(out);
    // 标题句(五路说明,尾冒号剥掉)进上边框。
    CHECK(Contains(plain, "Package(五路: dev > project > store > user > official"));
    CHECK(plain.find("装架):") == std::string::npos);
    // 主表三列 schema 名与三只包都在册;valid 单元格走 table_pass 语义色。
    CHECK(Contains(plain, "probe.pkg000"));
    CHECK(Contains(plain, "probe.pkg002"));
    CHECK(Contains(plain, "scope"));
    CHECK(Contains(plain, "state"));
    CHECK(Contains(plain, "valid"));
    CHECK(Contains(out, dark.table_pass));
    // 明细表:组件计数长文本在第二张表里。
    CHECK(Contains(plain, "agents:0"));
    CHECK(Contains(plain, "detail"));
    // id 列对齐:主表两只包的 id 起始显示列一致。
    int col_a = -1;
    int col_b = -1;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("probe.pkg000") != std::string::npos && col_a < 0) {
            col_a = DisplayColOf(line, "probe.pkg000");
        }
        if (line.find("probe.pkg001") != std::string::npos && col_b < 0) {
            col_b = DisplayColOf(line, "probe.pkg001");
        }
    }
    CHECK(col_a > 0);
    CHECK(col_a == col_b);
}

TEST_CASE("list: 55 只包的长清单列对齐不破,一行不掉出框(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    PackageFixture fixture(/*package_count=*/55);
    const std::string out = fixture.Run("list", &dark);
    const std::string plain = StripAnsiLight(out);

    int framed_rows = 0;
    int id_col = -1;
    bool col_consistent = true;
    int seen = 0;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t at = line.find("probe.pkg");
        if (at == std::string::npos) {
            continue;
        }
        // 明细表也带 id,只对主表行计数:主表行含 scope 列(user)。
        if (line.find(" user ") == std::string::npos && line.find(" user") == std::string::npos) {
            continue;
        }
        ++seen;
        if (Contains(line, kBoxLightVert)) {
            ++framed_rows;
        }
        const int col = DisplayColOf(line, "probe.pkg");
        if (id_col < 0) {
            id_col = col;
        } else if (col != id_col) {
            col_consistent = false;
        }
    }
    CHECK(seen == 55);
    CHECK(framed_rows == 55);
    CHECK(col_consistent);
    CHECK(id_col > 0);
}

TEST_CASE("show: 头部键值对框 key 列对齐,标题带版本(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    PackageFixture fixture(/*package_count=*/1);
    const std::string out = fixture.Run("show probe.pkg000", &dark);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsiLight(out);
    // 标题 = id + 版本;头部键值对(状态/来源/启停/内容哈希)拆列。
    CHECK(Contains(plain, "probe.pkg000 0.1.0"));
    CHECK(Contains(plain, "状态"));
    CHECK(Contains(plain, "来源"));
    CHECK(Contains(plain, "启停"));
    CHECK(Contains(plain, "内容哈希"));
    CHECK(Contains(out, dark.row_label));
    // key 列对齐:状态/来源/启停的 value 起始显示列一致。
    int state_col = -1;
    int source_col = -1;
    int toggle_col = -1;
    std::istringstream lines(plain);
    std::string line;
    while (std::getline(lines, line)) {
        if (state_col < 0) state_col = ValueStartCol(line, "状态");
        if (source_col < 0) source_col = ValueStartCol(line, "来源");
        if (toggle_col < 0) toggle_col = ValueStartCol(line, "启停");
    }
    CHECK(state_col > 0);
    CHECK(source_col > 0);
    CHECK(toggle_col > 0);
    CHECK(state_col == source_col);
    CHECK(state_col == toggle_col);
    // 组件清单框:七组名与件数都在。
    CHECK(Contains(plain, "agents"));
    CHECK(Contains(plain, "channels"));
}

TEST_CASE("trust/enable/disable/reload 反馈与 not_found 进 frame(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    PackageFixture fixture(/*package_count=*/1);
    {
        const std::string out = fixture.Run("trust no-such-package", &dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(out, dark.error));
        CHECK(Contains(StripAnsiLight(out), "没找到包"));
    }
    {
        // enable/disable 的 not-found 路(错误进框;不碰真实主目录的启停账
        // ——包不存在,账务前就返回,只读)。
        const std::string out = fixture.Run("disable no-such-package", &dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "没找到包"));
    }
    {
        // reload:没接会话执行体,如实明说(进键值对框)。
        const std::string out = fixture.Run("reload", &dark);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "reload"));
    }
}

TEST_CASE("plain 主题(theme 空指针):零转义、无框,信息一字不少") {
    PackageFixture fixture(/*package_count=*/2);
    const std::string out = fixture.Run("list", nullptr);
    CHECK(out.find("\x1b") == std::string::npos);
    CHECK(!Contains(out, kBoxLightTopLeft));
    CHECK(!Contains(out, kBoxLightVert));
    CHECK(Contains(out, "probe.pkg000"));
    CHECK(Contains(out, "probe.pkg001"));
    CHECK(Contains(out, "valid"));
    CHECK(Contains(out, "agents:0"));
}
