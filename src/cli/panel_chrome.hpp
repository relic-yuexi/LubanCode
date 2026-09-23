// 常驻面板的行级语义档(TUI 排版批 6):TTY 面板宿主(provider/session/
// context-window/switch)逐行上色的统一查表。批 6 之前各宿主只给焦点行
// 上 confirm 色、其余行裸文本;批 6 起按行的结构角色取语义色——标题走
// frame_title、淡色附注走 row_muted、键提示/底栏走 key_hint、警示行走
// tool_line(黄警告档,与底栏全局通知同一把尺)。
//
// 规矩(与 docs/development/tui_style.md 同源):
//   - 只映射 cli::Theme 既有字段,不引入新样式字段;焦点色(confirm)是
//     交互语义,由宿主先行判定,不走这张表;
//   - plain 主题这些字段全空串,宿主包上 reset 也是空操作——T3 降级路径
//     零转义字节,不另判断;
//   - 纯查表,单测钉死映射(tests/unit/cli/test_context_window_panel_frame.cpp)。

#pragma once

#include "cli/theme.hpp"

namespace lubancode::cli {

enum class PanelRowTone {
    Body,     // 正文行:默认前景(空串,不包色)
    Title,    // 面板标题行:frame_title 档
    Muted,    // 淡色附注(当前端/筛选行/空态说明):row_muted 档
    Hint,     // 键提示/底栏操作说明:key_hint 档
    Warning,  // 警示行("! ..." 前缀的通知):tool_line 档(黄警告)
};

// 档 -> 主题字段。返回引用(Theme 字段本就是串),空串 = 不包色。
inline const std::string& PanelRowToneColor(const Theme& theme, PanelRowTone tone) {
    switch (tone) {
        case PanelRowTone::Title:
            return theme.frame_title;
        case PanelRowTone::Muted:
            return theme.row_muted;
        case PanelRowTone::Hint:
            return theme.key_hint;
        case PanelRowTone::Warning:
            return theme.tool_line;
        case PanelRowTone::Body:
            break;
    }
    static const std::string kEmpty;
    return kEmpty;
}

}  // namespace lubancode::cli
