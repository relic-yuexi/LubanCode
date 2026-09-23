// TUI 排版批 6(常驻面板):面板行级语义档的合同册。
//   - PanelRowToneColor(cli/panel_chrome.hpp)钉死"档 -> Theme 字段"映射:
//     标题 frame_title、淡色附注 row_muted、键提示 key_hint、警示 tool_line、
//     正文空串(不包色);
//   - plain/T3 降级:全部档位空串,宿主包上 reset 也是空操作,零转义字节;
//   - 宿主按行位取档所依赖的帧结构契约:context-window 帧首行是标题、
//     尾两行是键位底栏;provider 帧首行标题、次行当前端、末行键提示。
//     (纯帧拼装在 provider_panel_core/context_window_panel_core,本册只钉
//     行位契约,不重钉内容——内容各归 test_provider_panel/test_context_window。)

#include <doctest/doctest.h>

#include <string>

#include "cli/context_window_panel.hpp"  // BuildContextWindowPanelFrame(纯帧拼装)
#include "cli/i18n.hpp"
#include "cli/panel_chrome.hpp"
#include "cli/provider_panel_core.hpp"
#include "cli/theme.hpp"

using namespace lubancode::cli;

TEST_CASE("面板行档:Title/Muted/Hint/Warning 各走 Theme 语义字段,Body 不包色") {
    for (const char* name : {"dark", "light"}) {
        const Theme theme = BuiltinTheme(name);
        CHECK(&PanelRowToneColor(theme, PanelRowTone::Title) == &theme.frame_title);
        CHECK(&PanelRowToneColor(theme, PanelRowTone::Muted) == &theme.row_muted);
        CHECK(&PanelRowToneColor(theme, PanelRowTone::Hint) == &theme.key_hint);
        CHECK(&PanelRowToneColor(theme, PanelRowTone::Warning) == &theme.tool_line);
        CHECK(PanelRowToneColor(theme, PanelRowTone::Body).empty());
        // 标题档在内置主题里与 banner 同值:TTY 宿主从 banner 换到
        // frame_title 是零视觉变化的语义收口。
        CHECK(PanelRowToneColor(theme, PanelRowTone::Title) == theme.banner);
    }
}

TEST_CASE("面板行档:plain/T3 降级全档空串,宿主包色零转义") {
    const Theme plain = BuiltinTheme("plain");
    for (const PanelRowTone tone :
         {PanelRowTone::Body, PanelRowTone::Title, PanelRowTone::Muted, PanelRowTone::Hint,
          PanelRowTone::Warning}) {
        CHECK(PanelRowToneColor(plain, tone).empty());
    }
    const Theme degraded = ResolveTheme("dark", /*enable_colors=*/false);
    for (const PanelRowTone tone :
         {PanelRowTone::Title, PanelRowTone::Muted, PanelRowTone::Hint, PanelRowTone::Warning}) {
        CHECK(PanelRowToneColor(degraded, tone).empty());
    }
}

TEST_CASE("context-window 帧行位契约:首行标题,尾两行键位底栏") {
    SetLanguage("zh-CN");
    ContextWindowPanelView view;
    view.model_title = "glm-5.3";
    view.window.values = {256000};
    view.window.current_window = 256000;
    view.window.limit_known = true;
    view.window.declared_limit = 256000;
    view.window_index = 0;
    view.original_window_index = 0;
    view.effort.options.clear();  // 不可调:走 current+note 行
    view.effort.current_effort = "high";
    view.effort.control = ThinkEffortControl::Adjustable;
    view.focus = 0;

    const ContextWindowPanelFrame frame = BuildContextWindowPanelFrame(view, 80);
    REQUIRE(frame.lines.size() >= 5);
    CHECK(frame.lines.front() == "glm-5.3");  // 宿主按 r==0 取 Title 档
    CHECK(!frame.lines[frame.lines.size() - 2].empty());  // footer_nav
    CHECK(!frame.lines.back().empty());                   // footer_save
}

TEST_CASE("provider 帧行位契约:首行标题,次行当前端,末行键提示") {
    SetLanguage("zh-CN");
    ProviderPanelView view;
    view.entries = {ProviderPanelEntry{"one", true, ""}, ProviderPanelEntry{"two", false, ""}};
    view.cursor = 0;
    view.offset = 0;
    view.visible_capacity = 0;

    const ProviderPanelFrame frame = BuildProviderPanelFrame(view, 80);
    REQUIRE(frame.lines.size() >= 6);
    CHECK(frame.lines[0] == std::string(tr("provider_panel.title")));       // r==0 Title
    CHECK(frame.lines[1].find(std::string(tr("cmd.provider.current"))) !=   // r==1 Muted
          std::string::npos || frame.lines[1] == std::string(tr("provider_panel.current_none")));
    CHECK(frame.lines.back() == std::string(tr("provider_panel.footer")));  // 末行 Hint
}
