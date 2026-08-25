#include "cli/bottom_chrome.hpp"

#include <algorithm>
#include <functional>

#include "cli/divider.hpp"

namespace lubancode::cli {

std::string BoxRuleLine(const Theme& theme, int console_width) {
    const bool plain = theme.reset.empty();
    return theme.stats + BuildDividerLine(console_width, plain, console_width) + theme.reset;
}

std::string BottomChromeFingerprint(const BottomChromeFrame& frame) {
    std::string value;
    value += "a:";
    for (const auto& row : frame.activity_rows) {
        value += row + "\n";
    }
    value += "q:";
    for (const auto& row : frame.queue_rows) {
        value += row + "\n";
    }
    value += "c:" + std::to_string(frame.composer_rows) + "\n";
    value += "e:" + frame.composer_digest + "\n";
    value += "d:";
    for (const auto& row : frame.agent_dock_rows) {
        value += row + "\n";
    }
    value += "t:";
    for (const auto& row : frame.transient_rows) {
        value += row + "\n";
    }
    value += "#" + std::to_string(frame.selected_task_id) + "\n";
    return value;
}

std::uint64_t BottomChromeRevision(const BottomChromeFrame& frame) {
    return std::hash<std::string>{}(BottomChromeFingerprint(frame));
}

namespace {

// 续行缩进两格:与合流前 RedrawEditArea 的老账一字不差,改这里等于同时
// 改 Idle 与 Busy 的续行画法——这正是"只有一个布局入口"的意义。
constexpr int kContinuationIndent = 2;

// 行文本里剥掉 ANSI CSI 序列再量显示宽。行由本文件拼装(部分带着调用方
// 给的主题色),painted_row_widths 只准记纯文本宽——footer 的 resize 旧帧
// 追踪拿它当"上一帧各逻辑行占多宽"的账,夹进转义字节就把宽算爆了。
int PlainDisplayWidth(const std::string& text) {
    std::string plain;
    plain.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size()) {
                const unsigned char ch = static_cast<unsigned char>(text[i++]);
                if (ch >= 0x40U && ch <= 0x7EU) {
                    break;
                }
            }
            continue;
        }
        plain.push_back(text[i++]);
    }
    return static_cast<int>(DisplayWidthUtf8(plain));
}

// 摘要只进指纹,不进画面:草稿全文 + 光标 + 档位 + 占位提示。宽度不进来
// ——宽度变化走 resize 整帧重画那条路,不归"内容指纹"管。
std::string BuildComposerDigest(const ComposerViewModel& composer) {
    std::string digest;
    digest += "m" + std::to_string(static_cast<int>(composer.mode));
    digest += "|p" + composer.prompt + "|" + composer.placeholder;
    digest += "|t" + Utf32ToUtf8(composer.editor.line);
    digest += "|c" + std::to_string(composer.editor.cursor_row) + ":" +
              std::to_string(composer.editor.cursor_col);
    return digest;
}

}  // namespace

BottomChromeLayout BuildBottomChromeLayout(const BottomChromeModel& model, const Theme& theme,
                                            int terminal_width) {
    const int width = (std::max)(1, terminal_width);
    const ComposerViewModel& composer = model.composer;
    // prompt 常带主题 ANSI 颜色码。这里只认屏上列宽；若拿原字符串量，
    // 转义字节会把光标凭空推到正文右边。
    const int prompt_width = PlainDisplayWidth(composer.prompt);

    // 软换行:首物理行容下提示符后的窄区,续行统一走续行宽度(与合流前
    // Idle 的 RedrawEditArea 同一把尺;Busy 此前没有这一步,只有单行截断)。
    const std::vector<std::u32string> fallback{std::u32string()};
    const std::vector<std::u32string>& lines =
        composer.editor.lines.empty() ? fallback : composer.editor.lines;
    const WrappedComposerLayout wrapped =
        LayoutComposerRows(lines, composer.editor.cursor_row, composer.editor.cursor_col,
                           width - prompt_width - 1, width - kContinuationIndent - 1);

    const int top_padding = model.framed ? (std::max)(0, composer.top_padding_rows) : 0;
    const int bottom_padding =
        model.framed
            ? (std::max)(0, composer.min_body_rows - top_padding - static_cast<int>(wrapped.rows.size()))
            : 0;

    BottomChromeLayout layout;
    // 淡色行包装:plain 主题 stats/reset 都是空串,自动退化成纯文本,不用
    // 另判断(合流前 Busy 给队列/坞行包色、Idle 不包,这里统一成包——
    // 两条路从此同色)。
    const auto tinted = [&](const std::string& text) {
        return theme.stats + TruncateUtf8ToDisplayWidth(text, width - 1) + theme.reset;
    };
    const auto push = [&layout, width](bool hard, std::string text) {
        layout.frame.rows.push_back(InlineFrameRow{0, width, hard, std::move(text)});
    };

    for (const std::string& row : model.activity_rows) {
        push(false, row);  // Working 行自带配色,布局只管摆位
    }
    for (const std::string& row : model.queue_rows) {
        push(false, tinted(row));
    }
    if (model.framed) {
        const std::string rule =
            model.rule_tag.empty()
                ? BoxRuleLine(theme, width)
                : BuildRuleWithTag(theme.stats, theme.reset, model.rule_tag, width);
        push(true, rule);
    }
    for (int i = 0; i < top_padding; ++i) {
        push(true, {});
    }

    layout.composer_first_row = static_cast<int>(layout.frame.rows.size());
    layout.composer_row_count = static_cast<int>(wrapped.rows.size());
    // placeholder 只在主草稿真空时显示:一行空串且无更多逻辑行才算真空。
    const bool show_placeholder =
        wrapped.rows.size() == 1 && wrapped.rows[0].text.empty() && !composer.placeholder.empty();
    for (std::size_t i = 0; i < wrapped.rows.size(); ++i) {
        std::string text = i == 0 ? composer.prompt + Utf32ToUtf8(wrapped.rows[i].text)
                                  : std::string(kContinuationIndent, ' ') +
                                        Utf32ToUtf8(wrapped.rows[i].text);
        if (i == 0 && show_placeholder) {
            text += theme.stats + TruncateUtf8ToDisplayWidth(composer.placeholder,
                                                             width - 1 - prompt_width) +
                    theme.reset;
        }
        // 续行缩进只落在文本里。若再把绘制起点右移两格,屏上会缩进四格,
        // 光标却仍按两格算,末字便被方块光标压住(5782e2a 教训,随布局
        // 一并搬来)。
        push(false, std::move(text));
    }
    for (int i = 0; i < bottom_padding; ++i) {
        push(true, {});
    }

    if (model.framed) {
        push(true, BoxRuleLine(theme, width));
        for (const std::string& row : model.status_rows) {
            push(true, row);
        }
    } else {
        for (const std::string& row : model.status_rows) {
            push(false, row);  // 无框读取常态没有状态行,防御性摆位
        }
    }
    for (const std::string& row : model.agent_dock_rows) {
        push(false, tinted(row));
    }
    for (const std::string& row : model.transient_rows) {
        // slash 提示与搜索/提及行:调用方拼好的短命 UI,按屏宽截断(与
        // 合流前两条路的 hint 画法一致),不包色。
        push(false, TruncateUtf8ToDisplayWidth(row, width - 1));
    }

    // 光标来自软换行结果,不从 echo 字符串猜:首物理行在 prompt 之后,
    // 续行在两格缩进之后。cursor_row 记帧内绝对下标。
    layout.cursor_row = layout.composer_first_row + static_cast<int>(wrapped.cursor_row);
    layout.cursor_x = wrapped.cursor_row == 0 ? prompt_width + wrapped.cursor_col
                                              : kContinuationIndent + wrapped.cursor_col;
    layout.frame.cursor_x = layout.cursor_x;
    layout.frame.cursor_row = layout.cursor_row;

    layout.painted_row_widths.reserve(layout.frame.rows.size());
    for (const InlineFrameRow& row : layout.frame.rows) {
        layout.painted_row_widths.push_back(PlainDisplayWidth(row.text));
    }

    BottomChromeFrame& chrome = layout.chrome;
    chrome.activity_rows = model.activity_rows;
    chrome.queue_rows = model.queue_rows;
    chrome.agent_dock_rows = model.agent_dock_rows;
    chrome.transient_rows = model.transient_rows;
    chrome.composer_rows = top_padding + static_cast<int>(wrapped.rows.size()) + bottom_padding;
    chrome.status_rows = model.framed ? (std::max)(1, static_cast<int>(model.status_rows.size())) : 0;
    chrome.rule_rows = model.framed ? 2 : 0;
    chrome.composer_digest = BuildComposerDigest(composer);
    chrome.selected_task_id = model.selected_task_id;
    chrome.revision = BottomChromeRevision(chrome);
    layout.revision = chrome.revision;
    return layout;
}

}  // namespace lubancode::cli
