#include "cli/format_utils.hpp"

#include <cmath>

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

}  // namespace lubancode::cli
