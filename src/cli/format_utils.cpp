#include "cli/format_utils.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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

StatusPanelData WithContextUpdate(StatusPanelData data, int context_percent, std::int64_t used_tokens,
                                  std::int64_t window_tokens, bool measured) {
    data.context_percent = context_percent;
    data.used_tokens = used_tokens;
    data.window_tokens = window_tokens;
    data.context_stale = !measured;
    return data;
}

std::vector<StatusPanelSegment> BuildStatusPanelSegments(
    const std::vector<std::string>& items, ConfirmMode mode, const StatusPanelData& data) {
    std::vector<StatusPanelSegment> out;
    out.reserve(items.size() + 2);
    // REC 标记:录制中恒挂第一段,不进 items 配置(见 StatusPanelData::rec 注释)。
    if (!data.rec.empty()) {
        out.push_back({"rec", data.rec});
    }
    // 旧值前缀:最近一次请求没带回 usage 时,context/tokens 两段的数字还是
    // 上一次的实测,前缀 ~ 提醒"不是本次新数"(见 StatusPanelData::context_stale)。
    const char* stale_mark = data.context_stale ? "~" : "";
    // WT 房名:住在隔离 worktree 里恒挂一段,不进 items 配置(理由见
    // StatusPanelData::worktree 注释)。纯文本 "WT <名字>"。
    if (!data.worktree.empty()) {
        out.push_back({"worktree", "WT " + data.worktree});
    }
    for (const std::string& key : items) {
        std::string text;
        if (key == "permission_mode") {
            text = StatusLineModeSegment(mode);
        } else if (key == "model") {
            text = data.model;
        } else if (key == "cwd") {
            text = data.cwd;
        } else if (key == "git_branch") {
            text = data.git_branch;
        } else if (key == "context") {
            text = std::string(stale_mark) + "context " + std::to_string(data.context_percent) + "%";
        } else if (key == "tokens") {
            if (data.used_tokens > 0) {
                text = std::string(stale_mark) + FormatTokenCount(data.used_tokens) + "/" +
                       FormatTokenCount(data.window_tokens);
            }
        } else if (key == "provider") {
            if (!data.provider.empty()) {
                text = "provider " + data.provider;
            }
        } else if (key == "effort") {
            if (!data.effort.empty()) {
                text = "effort " + data.effort;
            }
        }
        if (!text.empty()) {
            out.push_back({key, std::move(text)});
        }
    }
    return out;
}

std::string BuildStatusPanelText(const std::vector<std::string>& items,
                                 std::string_view separator, ConfirmMode mode,
                                 const StatusPanelData& data) {
    const auto segments = BuildStatusPanelSegments(items, mode, data);
    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += segments[i].text;
    }
    return out;
}

std::string CompactStatusPath(std::string_view path, int max_width) {
    if (max_width <= 0) {
        return {};
    }
    const std::string full(path);
    if (static_cast<int>(DisplayWidthUtf8(full)) <= max_width) {
        return full;
    }

    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const char separator = path.find('\\') != std::string_view::npos ? '\\' : '/';
    std::string root;
    if (path.size() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        root = std::string(path.substr(0, 3));
    } else if (!path.empty() && (path.front() == '/' || path.front() == '\\')) {
        root.assign(1, path.front());
    }

    std::string compact = root + "…" + separator + std::string(leaf);
    if (static_cast<int>(DisplayWidthUtf8(compact)) <= max_width) {
        return compact;
    }
    compact = "…" + std::string(1, separator) + std::string(leaf);
    return TruncateUtf8ToDisplayWidth(compact, max_width);
}

std::string StreamHintText(bool plain) {
    return tr(plain ? "stream.hint.plain" : "stream.hint");
}

std::string StreamFooterInterruptText(bool plain) {
    return tr(plain ? "stream.interrupt.plain" : "stream.interrupt");
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

std::vector<std::string> FormatContextBreakdown(std::size_t sys_tokens_in, std::size_t tools_tokens_in,
                                                 std::size_t history_tokens_est, std::int64_t cache_read_tokens,
                                                 std::size_t window_tokens, std::size_t measured_used_tokens,
                                                 const Theme& theme, int bar_width, int cache_hit_percent) {
    // plain 主题全部字段是空串,拿 reset 当探针(真主题 reset 恒非空)。
    const bool plain = theme.reset.empty();
    if (bar_width < 0) {
        bar_width = 0;
    }

    // 系统提示、工具定义每轮固定,token 由调用方按全库统一口径
    // (agent::EstimateUtf8Tokens:ASCII 4 字符约 1 token,非 ASCII 每字约
    // 1.5 token)算好传进来,数字前带 ~ 提醒是估的。
    const std::size_t sys_tokens = sys_tokens_in;
    const std::size_t tools_tokens = tools_tokens_in;

    // measured_used_tokens 是 tracker 实测(最近一次请求的 input+cache_read+
    // cache_creation+output),只要发过一轮请求就是精确值,是唯一该信的数。
    // 有实测:总量直接用实测(不带 ~),历史 = max(0, 实测总量 - 系统 - 工具)
    //   反推——这样三分项之和恒等于实测总量,不再各估各的打架。
    // 没实测(实测=0,如刚启动):三项全退回字符估,整体标 ~。
    const bool have_measured = measured_used_tokens > 0;
    const std::size_t sys_plus_tools = sys_tokens + tools_tokens;
    const std::size_t history_tokens =
        have_measured ? (measured_used_tokens > sys_plus_tools ? measured_used_tokens - sys_plus_tools : 0)
                      : history_tokens_est;
    const std::size_t used = have_measured ? measured_used_tokens : sys_plus_tools + history_tokens;

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

    // estimated=true 时数字前带 ~(字符估);false 是实测/反推的确定值。
    const auto tok_cell = [&](std::size_t tokens, bool estimated) {
        return PadRightCols(estimated ? "~" + TokenText(tokens) : TokenText(tokens), tok_cols);
    };
    const auto category_row = [&](const std::string& label, std::size_t tokens, bool estimated) {
        return "  " + PadRightCols(label, label_cols) + tok_cell(tokens, estimated) +
               BuildBar(tokens, window_tokens, bar_width, plain) + "  " +
               PadLeftCols(std::to_string(PercentOfWindow(tokens, window_tokens)), 3) + "%";
    };

    std::vector<std::string> lines;
    lines.push_back(trf("cmd.context.bd.header", TokenText(window_tokens)));
    // 系统提示/工具永远是字符估(可单独算,但口径仍是字符/3),带 ~。
    lines.push_back(category_row(label_sys, sys_tokens, /*estimated=*/true));
    lines.push_back(category_row(label_tools, tools_tokens, /*estimated=*/true));
    {
        // 历史:有实测是反推的确定值(不带 ~,行尾注明反推口径),无实测退字符估。
        std::string history_row = category_row(label_history, history_tokens, /*estimated=*/!have_measured);
        if (have_measured) {
            history_row += "  " + tr("cmd.context.bd.history_derived");
        }
        if (cache_read_tokens > 0) {
            // 命中率分母只取输入;没回报(-1)只摆命中量,不伪造 0%。
            history_row += cache_hit_percent >= 0
                               ? "   " + trf("cmd.context.bd.cache", FormatTokenCount(cache_read_tokens),
                                            std::to_string(cache_hit_percent))
                               : "   " + trf("cmd.context.bd.cache_no_ratio", FormatTokenCount(cache_read_tokens));
        }
        lines.push_back(std::move(history_row));
    }
    // 分隔线长度盖住"标签 + 数字 + 条形 + 百分比"那几列。
    const int sep_cols = static_cast<int>(label_cols + tok_cols) + bar_width + 6;
    lines.push_back("  " + RepeatGlyph(plain ? "-" : "─", sep_cols));
    {
        std::string used_row = "  " + PadRightCols(label_used, label_cols) +
                               tok_cell(used, /*estimated=*/!have_measured) +
                               std::string(static_cast<std::size_t>(bar_width), ' ') + "  " +
                               PadLeftCols(std::to_string(PercentOfWindow(used, window_tokens)), 3) + "%";
        if (have_measured) {
            used_row += "   " + tr("cmd.context.bd.measured");
        }
        lines.push_back(std::move(used_row));
    }
    const std::size_t threshold_tokens = window_tokens * kAutoCompactThresholdPercent / 100;
    lines.push_back("  " + PadRightCols(label_threshold, label_cols) + TokenText(threshold_tokens) + "(" +
                    std::to_string(kAutoCompactThresholdPercent) + "%)");
    const std::size_t remaining = window_tokens > used ? window_tokens - used : 0;
    lines.push_back("  " + PadRightCols(label_remaining, label_cols) + TokenText(remaining));
    lines.push_back("  " + tr(have_measured ? "cmd.context.bd.note.measured" : "cmd.context.bd.note.est"));
    return lines;
}

}  // namespace lubancode::cli
