// TUI 排版批 5b(tui优化todo.todo:/telemetry 全族)的输出形状册。
//   - status 空态/enable|disable 选项单(pause|resume|flush|spool|consent
//     未开回执/policy/认不得提示)全进 frame:键值对框为主,选项单走列表框
//     (标题=引导句剥尾冒号);
//   - 遥测未装配(telemetry_service=nullptr)路径全纯本地,不碰网络不碰盘;
//   - plain 主题(--no-color/T3 降级路径)钉零转义字节、无框字形——合同
//     第 3 条;
//   - /telemetry 无 --json/--format 分支(源码核实),字节级不变一条天然
//     满足。
//
// 走真 HandleSlashTelemetry(TermPort 改道捕获),断言只看形状与相对位置。

#include <doctest/doctest.h>

#include <sstream>
#include <string>

#include "app/commands/telemetry_commands.hpp"
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

struct TelemetryRig {
    app::TelemetryCommandContext ctx;
    cli::Theme theme{cli::BuiltinTheme("dark")};
    cli::Theme plain_theme{cli::BuiltinTheme("plain")};

    TelemetryRig() { ctx.theme = &theme; }

    std::string Run(const std::string& args, const cli::Theme& use_theme) {
        ctx.theme = &use_theme;
        cli::ParsedSlashCommand parsed;
        parsed.args = args;
        OutputCapture capture;
        app::HandleSlashTelemetry(ctx, parsed);
        return capture.text();
    }
};

}  // namespace

TEST_CASE("status 未装配: 空态进键值对框") {
    TelemetryRig rig;
    const std::string out = rig.Run("status", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    CHECK(Contains(StripAnsi(out), "遥测未开启"));
    CHECK(Contains(StripAnsi(out), "/telemetry enable session"));
}

TEST_CASE("enable 裸敲: 选项单走列表框,标题剥尾冒号") {
    TelemetryRig rig;
    const std::string out = rig.Run("enable", rig.theme);
    REQUIRE(Contains(out, kBoxLightTopLeft));
    const std::string plain = StripAnsi(out);
    // 引导句进框顶,引导下文的冒号不再连排(批 2 裁量 3)。
    CHECK(Contains(plain, "enable 只对当前进程还是写配置,须你挑一个(§24.2 不暗改文件)"));
    CHECK(plain.find("须你挑一个(§24.2 不暗改文件):") == std::string::npos);
    CHECK(Contains(plain, "/telemetry enable session"));
    CHECK(Contains(plain, "/telemetry enable config"));
    // 选项行项目符走 user 档色。
    CHECK(Contains(out, rig.theme.list_bullet_user));
}

TEST_CASE("未开遥测的各子命令回执进框,不再裸打印") {
    TelemetryRig rig;
    {
        const std::string out = rig.Run("pause", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有出口可pause"));
    }
    {
        const std::string out = rig.Run("flush", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没东西可 flush"));
    }
    {
        const std::string out = rig.Run("spool", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "没有 spool"));
    }
    {
        const std::string out = rig.Run("consent", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "consent 无从谈起"));
    }
}

TEST_CASE("policy 与认不得: policy 进框;认不得子命令上 error 色") {
    TelemetryRig rig;
    {
        const std::string out = rig.Run("policy", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        CHECK(Contains(StripAnsi(out), "属 T4"));
    }
    {
        const std::string out = rig.Run("bogus", rig.theme);
        REQUIRE(Contains(out, kBoxLightTopLeft));
        const std::string plain = StripAnsi(out);
        // 句内首冒号拆列:key="/telemetry bogus",value 走 error 色。
        CHECK(Contains(plain, "/telemetry bogus"));
        CHECK(Contains(plain, "认不得"));
        CHECK(Contains(out, rig.theme.error + "认不得"));
    }
}

TEST_CASE("plain 主题: 全族输出零转义字节、无框字形(T3/--no-color 路径)") {
    TelemetryRig rig;
    for (const std::string& args :
         {"status", "enable", "disable", "pause", "flush", "spool", "consent", "policy", "bogus"}) {
        CAPTURE(args);
        const std::string out = rig.Run(args, rig.plain_theme);
        CHECK(out.find("\x1b") == std::string::npos);
        CHECK(!Contains(out, kBoxLightTopLeft));
        CHECK(!Contains(out, kBoxLightVert));
        // 信息一字不少。
        if (args == "enable") {
            CHECK(Contains(out, "/telemetry enable session"));
            CHECK(Contains(out, "/telemetry enable config"));
        }
    }
}
