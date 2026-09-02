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
    value += "h:";
    for (const auto& row : frame.help_rows) {
        value += row + "\n";
    }
    value += "a:";
    for (const auto& row : frame.activity_rows) {
        value += row + "\n";
    }
    value += "q:";
    for (const auto& row : frame.queue_rows) {
        value += row + "\n";
    }
    value += "k:";
    for (const auto& row : frame.shortcut_rows) {
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

// 状态行由调用方拼好、自带主题色(与队列/坞行"外包色"的 tinted 路不同),
// 是最后一步落帧的行。宽度感知按约定在调用方,但渲染层给最后一道兜:
// 超宽时按显示宽截断——CSI 转义序列不占宽、永不被劈,截在色段中间就补
// 一枚 reset,别把开着的颜色漏到帧外;末列照铁律留白不写字。
std::string ClampAnsiRowToWidth(const std::string& row, int width) {
    if (width <= 1) {
        return row;
    }
    const int limit = width - 1;  // 末列不写字(锚点铁律,防隐式 wrap)
    std::string out;
    int used = 0;
    bool cut = false;
    for (std::size_t i = 0; i < row.size() && !cut;) {
        if (row[i] == '\x1b' && i + 1 < row.size() && row[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < row.size()) {
                const unsigned char ch = static_cast<unsigned char>(row[j++]);
                if (ch >= 0x40U && ch <= 0x7EU) {
                    break;
                }
            }
            out.append(row, i, j - i);  // 整段转义原样带走,不占宽
            i = j;
            continue;
        }
        // 解一个 UTF-8 码点,量宽。
        const unsigned char lead = static_cast<unsigned char>(row[i]);
        std::size_t len = 1;
        char32_t cp = lead;
        if ((lead & 0xE0U) == 0xC0U) {
            cp = lead & 0x1FU;
            len = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            cp = lead & 0x0FU;
            len = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            cp = lead & 0x07U;
            len = 4;
        }
        for (std::size_t k = 1; k < len && i + k < row.size(); ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(row[i + k]) & 0x3FU);
        }
        const int w = lubancode::cli::CharDisplayWidth(cp);
        if (used + w > limit) {
            cut = true;
            break;
        }
        out.append(row, i, len);
        used += w;
        i += len;
    }
    if (!cut) {
        return row;  // 没超宽:原样返回,不添一枚多余 reset。
    }
    if (used + 1 <= limit) {
        out += ".";  // 截断的省略记号(还剩格子才写)
    }
    out += "\x1b[0m";
    return out;
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

bool HelpOverlayNext(bool visible, HelpOverlayEvent event) {
    switch (event) {
        case HelpOverlayEvent::TogglePressed:
            return !visible;  // 头一按展开,再一按收起——同一枚键,同一个动作
        case HelpOverlayEvent::EscapePressed:
            return false;     // Esc 只收不开(改绑 help.show 后的确定出口)
        case HelpOverlayEvent::DraftFilled:
        case HelpOverlayEvent::SceneChanged:
            return false;     // 场景换了,帮助层不让位也得让位
    }
    return visible;
}

BottomChromeLayout BuildBottomChromeLayout(const BottomChromeModel& model, const Theme& theme,
                                            int terminal_width, int height_budget) {
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

    int top_padding = model.framed ? (std::max)(0, composer.top_padding_rows) : 0;
    int bottom_padding =
        model.framed
            ? (std::max)(0, composer.min_body_rows - top_padding - static_cast<int>(wrapped.rows.size()))
            : 0;

    // ---- 高度预算钳制(终端画面隔网单·战术二):"输入行必画得下"的硬约束 ----
    // 可选行(帮助/活动条/队列/快捷键/坞/提示)按 transient -> dock ->
    // shortcut -> queue -> activity -> help 的次序舍(即保的优先级相反:帮助层是用户刚显式要
    // 的,最保、最后舍;舍时保头——表头写着怎么收,丢了头用户就找不到门,
    // 装不下全表即面板内截断);可选行全舍了还装不下,composer 的物理行围
    // 光标开窗——窗口尾部贴光标,保底一行,留白跟着免掉。0 = 不限(单测
    // 与无终端环境的老行为)。
    std::size_t help_count = model.help_rows.size();
    std::size_t activity_count = model.activity_rows.size();
    std::size_t queue_count = model.queue_rows.size();
    std::size_t shortcut_count = model.shortcut_rows.size();
    std::size_t dock_count = model.agent_dock_rows.size();
    std::size_t transient_count = model.transient_rows.size();
    std::size_t window_first = 0;                          // composer 行窗(默认全量)
    std::size_t window_count = wrapped.rows.size();
    bool draw_rules = model.framed;
    bool draw_status = model.framed;
    int dropped_optional_rows = 0;
    if (height_budget > 0) {
        const int rules_rows = draw_rules ? 2 : 0;
        const int status_rows_n =
            draw_status ? (std::max)(1, static_cast<int>(model.status_rows.size())) : 0;
        const int composer_phys =
            top_padding + static_cast<int>(wrapped.rows.size()) + bottom_padding;
        const int core = rules_rows + status_rows_n + composer_phys;
        if (core <= height_budget) {
            // 常态:核心(横线+输入+状态)装得下,余量从高到低分给可选行。
            int room = height_budget - core;
            const auto take = [&room](std::size_t total) {
                const int granted = (std::min)((std::max)(0, room), static_cast<int>(total));
                room -= granted;
                return static_cast<std::size_t>(granted);
            };
            help_count = take(model.help_rows.size());
            activity_count = take(model.activity_rows.size());
            queue_count = take(model.queue_rows.size());
            shortcut_count = take(model.shortcut_rows.size());
            dock_count = take(model.agent_dock_rows.size());
            transient_count = take(model.transient_rows.size());
        } else {
            // 绝境:可选行一行不剩,composer 围光标开窗。窗口保底一行;
            // 连横线+状态行都容不下时它们也让位——"输入行必画得下"是底线,
            // 别的一切都排它后头。
            help_count = 0;
            activity_count = 0;
            queue_count = 0;
            shortcut_count = 0;
            dock_count = 0;
            transient_count = 0;
            int keep = height_budget - rules_rows - status_rows_n;
            if (keep < 1) {
                draw_rules = false;
                draw_status = false;
                keep = height_budget;
            }
            keep = (std::max)(1, (std::min)(keep, static_cast<int>(wrapped.rows.size())));
            int window_end = static_cast<int>(wrapped.cursor_row) + 1;  // 窗尾贴光标
            int window_begin = window_end - keep;
            if (window_begin < 0) {
                window_begin = 0;
                window_end = (std::min)(keep, static_cast<int>(wrapped.rows.size()));
            } else if (window_end > static_cast<int>(wrapped.rows.size())) {
                window_end = static_cast<int>(wrapped.rows.size());
                window_begin = (std::max)(0, window_end - keep);
            }
            window_first = static_cast<std::size_t>(window_begin);
            window_count = static_cast<std::size_t>(window_end - window_begin);
            // 窗口化是缩不是补:上下留白一并免掉,格子全留给输入正文。
            top_padding = 0;
            bottom_padding = 0;
        }
        dropped_optional_rows =
            static_cast<int>(model.help_rows.size() - help_count +
                             model.activity_rows.size() - activity_count +
                             model.queue_rows.size() - queue_count +
                             model.shortcut_rows.size() - shortcut_count +
                             model.agent_dock_rows.size() - dock_count +
                             model.transient_rows.size() - transient_count);
    }

    BottomChromeLayout layout;
    // 淡色行包装:plain 主题 stats/reset 都是空串,自动退化成纯文本,不用
    // 另判断(合流前 Busy 给队列/坞行包色、Idle 不包,这里统一成包——
    // 两条路从此同色)。
    const auto tinted = [&](const std::string& text) {
        return theme.stats + TruncateUtf8ToDisplayWidth(text, width - 1) + theme.reset;
    };
    // 坞行的监督色辅助(监督器单 P1-1 §十):颜色只作辅助——行文本自身已
    // 带阶段/静默龄/重连次数。映射只用既有主题档(黄=tool_line,青=
    // spinner,红=error),plain 主题这些全是空串,自动退回默认淡色,无色
    // 模式不丢信息。
    const auto dock_tint_of = [&theme, &model](std::size_t index) -> const std::string& {
        const AgentHealthTint tint = index < model.agent_dock_tints.size()
                                         ? model.agent_dock_tints[index]
                                         : AgentHealthTint::Normal;
        switch (tint) {
            case AgentHealthTint::Quiet:
                return theme.tool_line;  // 黄(两个内置主题里都是黄系)
            case AgentHealthTint::Recovering:
                return theme.spinner;    // 青
            case AgentHealthTint::Degraded:
                return theme.error;      // 红
            case AgentHealthTint::Normal:
                break;
        }
        return theme.stats;
    };
    const auto push = [&layout, width](bool hard, std::string text) {
        layout.frame.rows.push_back(InlineFrameRow{0, width, hard, std::move(text)});
    };

    for (std::size_t i = 0; i < help_count; ++i) {
        // 帮助层垫帧最顶:与队列同款淡色包装,超宽按屏宽截断。
        push(false, tinted(model.help_rows[i]));
    }
    for (std::size_t i = 0; i < activity_count; ++i) {
        push(false, model.activity_rows[i]);  // Working 行自带配色,布局只管摆位
    }
    for (std::size_t i = 0; i < queue_count; ++i) {
        push(false, tinted(model.queue_rows[i]));
    }
    // 常用键速览只在空 composer 出现,紧挨 skills 上方。slash/搜索/提及
    // 仍走 transient_rows 留在输入框下,两类提示不混位。
    for (std::size_t i = 0; i < shortcut_count; ++i) {
        push(false, tinted(model.shortcut_rows[i]));
    }
    // 模式行常驻输入框正上方,快捷键速览紧贴其上。thinking/activity 再在
    // 上面。状态行自带配色,窄屏由组行层先保右端信息,这里再作 ANSI 截断。
    if (draw_status) {
        for (const std::string& row : model.status_rows) {
            push(draw_rules, ClampAnsiRowToWidth(row, width));
        }
    }
    if (draw_rules) {
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
    layout.composer_row_count = static_cast<int>(window_count);
    // placeholder 只在主草稿真空时显示:一行空串且无更多逻辑行才算真空。
    const bool show_placeholder =
        wrapped.rows.size() == 1 && wrapped.rows[0].text.empty() && !composer.placeholder.empty();
    for (std::size_t i = window_first; i < window_first + window_count; ++i) {
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

    if (draw_rules) {
        push(true, BoxRuleLine(theme, width));
    }
    for (std::size_t i = 0; i < dock_count; ++i) {
        push(false, dock_tint_of(i) + TruncateUtf8ToDisplayWidth(model.agent_dock_rows[i], width - 1) +
                        theme.reset);
    }
    for (std::size_t i = 0; i < transient_count; ++i) {
        // slash 提示与搜索/提及行:调用方拼好的短命 UI,按屏宽截断(与
        // 合流前两条路的 hint 画法一致),不包色。
        push(false, TruncateUtf8ToDisplayWidth(model.transient_rows[i], width - 1));
    }

    // 光标来自软换行结果,不从 echo 字符串猜:首物理行在 prompt 之后,
    // 续行在两格缩进之后。cursor_row 记帧内绝对下标(行窗开着时按窗内
    // 相对下标折算)。
    const std::size_t cursor_in_window = static_cast<std::size_t>(wrapped.cursor_row) - window_first;
    layout.cursor_row = layout.composer_first_row + static_cast<int>(cursor_in_window);
    layout.cursor_x = wrapped.cursor_row == 0 ? prompt_width + wrapped.cursor_col
                                              : kContinuationIndent + wrapped.cursor_col;
    layout.frame.cursor_x = layout.cursor_x;
    layout.frame.cursor_row = layout.cursor_row;

    layout.painted_row_widths.reserve(layout.frame.rows.size());
    for (const InlineFrameRow& row : layout.frame.rows) {
        layout.painted_row_widths.push_back(PlainDisplayWidth(row.text));
    }

    BottomChromeFrame& chrome = layout.chrome;
    // 行数账记"真画出来的":预算钳掉的可选行不进账/指纹——同一窗口高下
    // 画面相同即指纹相同,跳帧判断不被"画不出来的行"搅动。
    chrome.help_rows.assign(model.help_rows.begin(), model.help_rows.begin() + help_count);
    chrome.activity_rows.assign(model.activity_rows.begin(), model.activity_rows.begin() + activity_count);
    chrome.queue_rows.assign(model.queue_rows.begin(), model.queue_rows.begin() + queue_count);
    chrome.shortcut_rows.assign(model.shortcut_rows.begin(),
                                model.shortcut_rows.begin() + shortcut_count);
    chrome.agent_dock_rows.assign(model.agent_dock_rows.begin(),
                                  model.agent_dock_rows.begin() + dock_count);
    chrome.transient_rows.assign(model.transient_rows.begin(),
                                 model.transient_rows.begin() + transient_count);
    chrome.composer_rows = top_padding + static_cast<int>(window_count) + bottom_padding;
    chrome.status_rows =
            draw_status ? (std::max)(1, static_cast<int>(model.status_rows.size())) : 0;
    chrome.rule_rows = draw_rules ? 2 : 0;
    chrome.composer_digest = BuildComposerDigest(composer);
    chrome.selected_task_id = model.selected_task_id;
    chrome.revision = BottomChromeRevision(chrome);
    layout.revision = chrome.revision;
    layout.dropped_optional_rows = dropped_optional_rows;
    return layout;
}

}  // namespace lubancode::cli
