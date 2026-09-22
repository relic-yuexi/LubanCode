// TUI 排版批 2(tui优化todo.todo P2:单子记作 /workspace 的 /plugin 全族)
// 的输出形状册。
//   - inspect:头部键值对框(六行权限真账按 "标签: 值" 拆两列、key 列全表
//     对齐)+ 工具清单列表框;
//   - 长清单(单子验收:结果行数 ≥50 时不破对齐)——55 件工具的 manifest
//     钉列表行全部在框内、label 列起始列一致;
//   - trust/untrust/reload 反馈与 not_found 提示全进 frame;
//   - plain 主题(空 theme 指针与 BuiltinTheme("plain")两路)钉零转义。
//
// 走真 HandlePluginCommand(ParsePluginManifest 造清单 + TermPort 改道
// 捕获)。单子"现状证据"表的 /workspace 行号(274/368/497/592/618/622)实为
// 本文件 HandlePluginCommand 的 inspect/doctor/test/trust/reload/enable
// 分支——全仓没有 /workspace 命令,按实际命令族办。

#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "app/commands/workspace_commands.hpp"
#include "app/tool_runtime.hpp"  // PluginMountInfo
#include "cli/line_editor.hpp"   // DisplayWidthUtf8:对齐断言按显示列量
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "runtime/plugin_contract.hpp"

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

// 剥掉 CSI 序列(与 test_memory_commands_frame.cpp 同一把手写的尺)。
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
constexpr const char* kBullet = "\xe2\x80\xa2";           // •

std::string MakeV2ManifestText(int tool_count) {
    std::string tools;
    for (int i = 0; i < tool_count; ++i) {
        // 名字长短不一(3/5/7 位):对齐断言要的就是不等宽的 label。
        std::string suffix = std::to_string(i);
        while (suffix.size() < 3) {
            suffix = "0" + suffix;
        }
        const std::string name = "tool_" + suffix;
        tools += std::string(i == 0 ? "" : ", ") + "{\"name\": \"" + name +
                 "\", \"entry\": \"go\", \"description\": \"d\", \"input_schema\": {\"type\": \"object\"}}";
    }
    return R"json({
  "manifest_version": 2, "id": "frame-probe", "version": "0.9.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "probe.lua"},
  "permissions": {
    "network": [
      {"scheme": "https", "host": "api.example.com", "port": 443, "methods": ["POST"]}
    ],
    "secrets": [{"id": "api_key", "env": "FRAME_PROBE_KEY", "required": false}]
  },
  "limits": {"http_request_bytes": 65536, "http_response_bytes": 262144, "http_timeout_ms": 10000},
  "tools": [)json" + tools + R"json(]
})json";
}

std::vector<std::shared_ptr<const runtime::PluginManifest>> MakeManifests(int tool_count) {
    const std::string text = MakeV2ManifestText(tool_count);
    auto parsed = runtime::ParsePluginManifest(text, std::filesystem::path("/test/frame-probe"));
    if (!parsed.has_value()) {
        return {};
    }
    std::vector<std::shared_ptr<const runtime::PluginManifest>> manifests;
    manifests.push_back(std::make_shared<const runtime::PluginManifest>(std::move(*parsed)));
    return manifests;
}

std::string Run(const std::string& args, int tool_count, const cli::Theme* theme) {
    OutputCapture capture;
    app::HandlePluginCommand(args, {}, MakeManifests(tool_count), std::string(), nullptr, theme);
    return capture.text();
}

}  // namespace

TEST_CASE("inspect: 头部键值对框,六行真账 key 列全表对齐") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    const std::string out = Run("inspect frame-probe", /*tool_count=*/2, &theme);

    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(out, theme.row_label));
    const std::string plain = StripAnsiLight(out);
    // 标题句进框顶(既有 header 文案,含 id/版本/runtime/language)。
    CHECK(Contains(plain, "frame-probe"));
    CHECK(Contains(plain, "0.9.0"));
    // 六行真账按 "标签: 值" 拆列:句内不再有 "runtime: xxx" 冒号连排。
    CHECK(plain.find("runtime: ") == std::string::npos);
    CHECK(plain.find("entry: ") == std::string::npos);

    // key 列对齐:不同 key(runtime/secrets/limits...)的行,value 起始
    // 显示列一致(排除标题行——它自带 "runtime=embedded-lua" 连排)。
    std::istringstream lines(plain);
    std::string line;
    int runtime_col = -1;
    int limits_col = -1;
    while (std::getline(lines, line)) {
        if (Contains(line, "embedded-lua") && !Contains(line, "runtime=")) {
            runtime_col = ValueStartCol(line, "runtime");
        }
        if (Contains(line, "request 64 KiB")) limits_col = ValueStartCol(line, "limits");
    }
    CHECK(runtime_col > 0);
    CHECK(limits_col > 0);
    CHECK(runtime_col == limits_col);
}

TEST_CASE("inspect: 55 件工具的长清单列对齐不破(单子验收 ≥50 行)") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    constexpr int kToolCount = 55;
    const std::string out = Run("inspect frame-probe", kToolCount, &theme);
    const std::string plain = StripAnsiLight(out);

    // 工具清单独立成列表框(bullet 走 Project 档),标题是既有 tools 句。
    CHECK(Contains(out, theme.list_bullet_project));
    CHECK(Contains(plain, "工具 55 件"));

    // 55 行全在框里:以竖线收边的行数 ≥ 工具数;首尾框线齐整。
    int framed_rows = 0;
    std::istringstream lines(plain);
    std::string line;
    int label_col = -1;
    bool col_consistent = true;
    int seen_tools = 0;
    while (std::getline(lines, line)) {
        if (line.find("tool_") == std::string::npos) {
            continue;
        }
        ++seen_tools;
        if (Contains(line, kBoxLightVert)) {
            ++framed_rows;
        }
        const int col = DisplayColOf(line, "tool_");
        if (label_col < 0) {
            label_col = col;
        } else if (col != label_col) {
            col_consistent = false;
        }
    }
    CHECK(seen_tools == kToolCount);
    CHECK(framed_rows == kToolCount);  // 一行都不掉出框
    CHECK(col_consistent);             // label 列起始显示列处处一致
    CHECK(label_col > 0);
}

TEST_CASE("not_found/reload/enable 反馈进 frame") {
    const cli::Theme theme = cli::BuiltinTheme("dark");
    {
        const std::string out = Run("inspect no-such-plugin", 1, &theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "找不到插件 no-such-plugin"));
    }
    {
        const std::string out = Run("reload frame-probe", 1, &theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsiLight(out), "重启"));
    }
    {
        const std::string out = Run("disable frame-probe", 1, &theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
    }
}

TEST_CASE("plain 主题: theme 空指针与 BuiltinTheme(plain) 两路零转义") {
    {
        const std::string out = Run("inspect frame-probe", 3, nullptr);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        CHECK(!Contains(out, kBullet));
        CHECK(Contains(out, "embedded-lua"));  // 信息一字不少
    }
    const cli::Theme plain_theme = cli::BuiltinTheme("plain");
    {
        const std::string out = Run("inspect frame-probe", 3, &plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(Contains(out, "secrets"));
    }
}
