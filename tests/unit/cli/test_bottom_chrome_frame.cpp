// TUI 排版批 6(常驻面板):底栏横线/标签/键提示的基件收口形状册。
//   - BoxRuleLine/BuildRuleWithTag 走 divider::line + frame_border/frame_title
//     语义档(批 0 基件,约定见 docs/development/tui_style.md);
//   - 帮助层/速览行走 key_hint、队列走 row_muted(两档在内置主题同值,
//     钉的是"语义字段落位",不是新色);
//   - T3 降级(ResolveTheme 逼 plain):整帧零转义字节、横线退 ASCII '-'。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/agent_panel.hpp"    // BuildRuleWithTag/kMinRuleCols
#include "cli/bottom_chrome.hpp"  // BoxRuleLine/BuildBottomChromeLayout
#include "cli/divider.hpp"
#include "cli/line_editor.hpp"    // DisplayWidthUtf8
#include "cli/theme.hpp"

using namespace lubancode::cli;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 剥掉 CSI 序列后的可见文本(与 test_frame_helpers 同一把尺)。
std::string StripAnsi(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;
            }
            continue;
        }
        out += text[i++];
    }
    return out;
}

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

BottomChromeModel FramedModel(const RenderState& state, ComposerMode mode) {
    BottomChromeModel model;
    model.composer.editor = state;
    model.composer.prompt = "> ";
    model.composer.mode = mode;
    model.status_rows = {"status"};
    return model;
}

}  // namespace

// ---- BoxRuleLine:divider::line + frame_border -------------------------------

TEST_CASE("底栏横线:字符走 divider::line(Light),色走 frame_border 档") {
    for (const char* name : {"dark", "light"}) {
        const Theme theme = BuiltinTheme(name);
        const std::string rule = BoxRuleLine(theme, 40);
        CHECK(Contains(rule, theme.frame_border));
        CHECK(Contains(rule, theme.reset));
        // 可见文本恰是 39 格 U+2500(divider::line 的 Light 档)。
        const std::string visible = StripAnsi(rule);
        CHECK(visible == divider::line(divider::Style::Light, 39));
        CHECK(Contains(visible, "\xe2\x94\x80"));
        CHECK_FALSE(Contains(visible, "-"));
    }
}

TEST_CASE("底栏横线:plain/T3 降级退 ASCII 零转义,与旧口径同宽") {
    const Theme plain = BuiltinTheme("plain");
    const std::string rule = BoxRuleLine(plain, 40);
    CHECK(rule == std::string(39, '-'));
    CHECK(rule.find('\x1b') == std::string::npos);

    // --no-color 正路(ResolveTheme 逼 plain)同样零转义。
    const Theme degraded = ResolveTheme("dark", /*enable_colors=*/false);
    CHECK(BoxRuleLine(degraded, 40).find('\x1b') == std::string::npos);

    // 窄终端守界:width=1 时一格都不画(与 BuildDividerLine 同规)。
    CHECK(BoxRuleLine(plain, 1).empty());
}

// ---- BuildRuleWithTag:标签即嵌进上边框的标题 ---------------------------------

TEST_CASE("底栏标签:横线 frame_border、标签 frame_title,进帧后同款") {
    const Theme dark = BuiltinTheme("dark");
    BottomChromeModel model = FramedModel(ComposerState({U"正文"}, 0, 2), ComposerMode::Idle);
    model.rule_tag = "agent #2 重构";
    const auto layout = BuildBottomChromeLayout(model, dark, 40);
    // 行序:状态行(0)→ 上横线(1)。标签嵌在横线右端,带 frame_title 色。
    REQUIRE(layout.frame.rows.size() >= 2);
    CHECK(Contains(layout.frame.rows[1].text, dark.frame_border));
    CHECK(Contains(layout.frame.rows[1].text, dark.frame_title + "agent #2 重构" + dark.reset));
    // 可见宽度仍是满宽 39 列(标签占的格子从横线里扣)。
    CHECK(static_cast<int>(DisplayWidthUtf8(StripAnsi(layout.frame.rows[1].text))) == 39);
}

// ---- 帮助层/队列:键提示与淡色附注的语义档 -----------------------------------

TEST_CASE("底栏行色:帮助层/速览行走 key_hint,队列走 row_muted") {
    const Theme dark = BuiltinTheme("dark");
    BottomChromeModel model = FramedModel(ComposerState({U""}, 0, 0), ComposerMode::Idle);
    model.help_rows = {"/help  列出命令"};
    model.queue_rows = {"待发 2 条"};
    const auto layout = BuildBottomChromeLayout(model, dark, 40);
    REQUIRE(layout.frame.rows.size() >= 2);
    CHECK(Contains(layout.frame.rows[0].text, dark.key_hint));
    CHECK(Contains(layout.frame.rows[1].text, dark.row_muted));
    // T3 降级:整帧零转义。
    const Theme degraded = ResolveTheme("dark", false);
    const auto plain_layout = BuildBottomChromeLayout(model, degraded, 40);
    for (const auto& row : plain_layout.frame.rows) {
        CHECK(row.text.find('\x1b') == std::string::npos);
    }
}
