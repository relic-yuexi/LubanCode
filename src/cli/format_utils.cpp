#include "cli/format_utils.hpp"

#include <algorithm>
#include <cmath>

#include "cli/context_tracker.hpp"  // kAutoCompactThresholdPercent
#include "cli/i18n.hpp"

namespace lubancode::cli {

std::string FormatTokenCount(std::int64_t n) {
    if (n < 10000) {
        return std::to_string(n);  // 负数也走这条:原样打出来,不猜
    }
    if (n < 1000000) {
        // 一位小数 k,四舍五入;尾随 .0 省略(10000 -> "10k",10500 -> "10.5k")。
        const std::int64_t tenths = std::llround(static_cast<double>(n) / 100.0);
        std::string out = std::to_string(tenths / 10);
        if (tenths % 10 != 0) {
            out += "." + std::to_string(tenths % 10);
        }
        return out + "k";
    }
    // 两位小数 M,四舍五入;尾随 0 逐位省略(1000000 -> "1M",1049999 ->
    // "1.05M",1500000 -> "1.5M")。
    const std::int64_t hundredths = std::llround(static_cast<double>(n) / 10000.0);
    std::string out = std::to_string(hundredths / 100);
    const std::int64_t frac = hundredths % 100;
    if (frac != 0) {
        out += ".";
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) {
            out += static_cast<char>('0' + frac % 10);
        }
    }
    return out + "M";
}

std::string StatusLineModeSegment(ConfirmMode mode) {
    // Confirm 档叫"确认模式"(spec 定的展示词),auto/yolo 沿用
    // ConfirmModeLabel 的英文小写,跟提示符前缀 [auto]/[yolo] 对得上。
    // i18n:两截文字都进表(status.mode.confirm / status.shift_tab_hint)。
    const std::string label = mode == ConfirmMode::Confirm ? tr("status.mode.confirm") : ConfirmModeLabel(mode);
    return "⏵⏵ " + label + " " + tr("status.shift_tab_hint");
}

std::string StatusLineInfoSegment(const std::string& model, int context_percent,
                                   std::int64_t used_tokens, std::int64_t window_tokens) {
    std::string out;
    if (!model.empty()) {
        out += " · " + model;
    }
    out += " · context " + std::to_string(context_percent) + "%";
    if (used_tokens > 0) {
        out += " (" + FormatTokenCount(used_tokens) + "/" + FormatTokenCount(window_tokens) + ")";
    }
    return out;
}

std::string BuildStatusLineText(ConfirmMode mode, const std::string& model, int context_percent,
                                 std::int64_t used_tokens, std::int64_t window_tokens) {
    return StatusLineModeSegment(mode) + StatusLineInfoSegment(model, context_percent, used_tokens, window_tokens);
}

namespace {

// 显示列宽:ASCII 一列,非 ASCII(这里的用途只有 CJK 标签,按全角)两列;
// UTF-8 续字节不算。够对齐几个中文标签用,不是通用 wcwidth。
std::size_t DisplayCols(const std::string& s) {
    std::size_t cols = 0;
    for (const unsigned char c : s) {
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        cols += c < 0x80 ? std::size_t{1} : std::size_t{2};
    }
    return cols;
}

std::string PadRightCols(const std::string& s, std::size_t cols) {
    const std::size_t w = DisplayCols(s);
    return w >= cols ? s : s + std::string(cols - w, ' ');
}

std::string PadLeftCols(const std::string& s, std::size_t cols) {
    const std::size_t w = DisplayCols(s);
    return w >= cols ? s : std::string(cols - w, ' ') + s;
}

// 占窗口百分比,四舍五入;窗口为 0 一律 0(不除零),超 100 钉在 100。
int PercentOfWindow(std::size_t part, std::size_t window) {
    if (window == 0) {
        return 0;
    }
    const int pct =
        static_cast<int>(static_cast<double>(part) / static_cast<double>(window) * 100.0 + 0.5);
    return pct > 100 ? 100 : pct;
}

std::string RepeatGlyph(const char* glyph, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += glyph;
    }
    return out;
}

// 条形图:实心格数 = 占窗口比例 * 宽度四舍五入,超宽截断打满;窗口为 0
// 全空。plain 用 #/-,别的主题用 █/░。
std::string BuildBar(std::size_t part, std::size_t window, int width, bool plain) {
    if (width <= 0) {
        return {};
    }
    int filled = 0;
    if (window > 0) {
        const double ratio = static_cast<double>(part) / static_cast<double>(window);
        filled = static_cast<int>(ratio * width + 0.5);
        filled = std::clamp(filled, 0, width);
    }
    return RepeatGlyph(plain ? "#" : "█", filled) + RepeatGlyph(plain ? "-" : "░", width - filled);
}

std::string TokenText(std::size_t tokens) { return FormatTokenCount(static_cast<std::int64_t>(tokens)); }

}  // namespace

std::vector<std::string> FormatContextBreakdown(std::size_t sys_chars, std::size_t tools_chars,
                                                 std::size_t history_chars, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width) {
    // plain 主题全部字段是空串,拿 reset 当探针(真主题 reset 恒非空)。
    const bool plain = theme.reset.empty();
    if (bar_width < 0) {
        bar_width = 0;
    }

    // 估算口径:字符数/3(中英混排的粗折算),数字前带 ~ 提醒是估的。
    const std::size_t sys_tokens = sys_chars / 3;
    const std::size_t tools_tokens = tools_chars / 3;
    const std::size_t history_tokens = history_chars / 3;
    const std::size_t est_used = sys_tokens + tools_tokens + history_tokens;
    // tracker 实测(最近一次请求 usage)比字符估算准;实测更大就用实测并标注。
    const bool use_measured = measured_used_tokens > est_used;
    const std::size_t used = use_measured ? measured_used_tokens : est_used;

    const std::string label_sys = tr("cmd.context.bd.system");
    const std::string label_tools = tr("cmd.context.bd.tools");
    const std::string label_history = tr("cmd.context.bd.history");
    const std::string label_used = tr("cmd.context.bd.used");
    const std::string label_threshold = tr("cmd.context.bd.threshold");
    const std::string label_remaining = tr("cmd.context.bd.remaining");

    std::size_t label_cols = 0;
    for (const auto* label : {&label_sys, &label_tools, &label_history, &label_used, &label_threshold,
                               &label_remaining}) {
        label_cols = (std::max)(label_cols, DisplayCols(*label));
    }
    label_cols += 2;                    // 标签与数字之间至少两个空格
    const std::size_t tok_cols = 9;     // "~204.8k" 这类数字列的宽度

    const auto category_row = [&](const std::string& label, std::size_t tokens) {
        return "  " + PadRightCols(label, label_cols) + PadRightCols("~" + TokenText(tokens), tok_cols) +
               BuildBar(tokens, window_tokens, bar_width, plain) + "  " +
               PadLeftCols(std::to_string(PercentOfWindow(tokens, window_tokens)), 3) + "%";
    };

    std::vector<std::string> lines;
    lines.push_back(trf("cmd.context.bd.header", TokenText(window_tokens)));
    lines.push_back(category_row(label_sys, sys_tokens));
    lines.push_back(category_row(label_tools, tools_tokens));
    {
        std::string history_row = category_row(label_history, history_tokens);
        if (cache_read_tokens > 0) {
            history_row += "   " + trf("cmd.context.bd.cache", FormatTokenCount(cache_read_tokens));
        }
        lines.push_back(std::move(history_row));
    }
    // 分隔线长度盖住"标签 + 数字 + 条形 + 百分比"那几列。
    const int sep_cols = static_cast<int>(label_cols + tok_cols) + bar_width + 6;
    lines.push_back("  " + RepeatGlyph(plain ? "-" : "─", sep_cols));
    {
        std::string used_row = "  " + PadRightCols(label_used, label_cols) +
                               PadRightCols(use_measured ? TokenText(used) : "~" + TokenText(used), tok_cols) +
                               std::string(static_cast<std::size_t>(bar_width), ' ') + "  " +
                               PadLeftCols(std::to_string(PercentOfWindow(used, window_tokens)), 3) + "%";
        if (use_measured) {
            used_row += "   " + tr("cmd.context.bd.measured");
        }
        lines.push_back(std::move(used_row));
    }
    const std::size_t threshold_tokens = window_tokens * kAutoCompactThresholdPercent / 100;
    lines.push_back("  " + PadRightCols(label_threshold, label_cols) + TokenText(threshold_tokens) + "(" +
                    std::to_string(kAutoCompactThresholdPercent) + "%)");
    const std::size_t remaining = window_tokens > used ? window_tokens - used : 0;
    lines.push_back("  " + PadRightCols(label_remaining, label_cols) + TokenText(remaining));
    lines.push_back("  " + tr("cmd.context.bd.note"));
    return lines;
}

}  // namespace lubancode::cli
