#include "cli/format_utils.hpp"

#include "cli/line_editor.hpp"  // Utf8ToUtf32/CharDisplayWidth:WrapStatusRows 的宽度账

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    // i18n:标签与提示分别进表。
    const std::string label = ConfirmModeLabel(mode);
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
                                  std::int64_t window_tokens, bool measured, const std::string& cache_note) {
    data.context_percent = context_percent;
    data.used_tokens = used_tokens;
    data.window_tokens = window_tokens;
    data.context_stale = !measured;
    data.cache_note = cache_note;
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
    // 工具调用后端档(PTC 单):非空恒挂一段,不进 items 配置(理由见
    // StatusPanelData::tools 注释)。纯文本 "tools <档>"。
    if (!data.tools.empty()) {
        out.push_back({"tools", "tools " + data.tools});
    }
    // Plan 模式标记(只读研究硬闸单):非空恒挂一段,不进 items 配置(理由
    // 见 StatusPanelData::plan_mode 注释)。纯文本 "plan"。
    if (!data.plan_mode.empty()) {
        out.push_back({"plan", data.plan_mode});
    }
    // goal/loop 会话状态段(goal 单合流):非空恒挂一段,不进 items 配置
    // (理由见 StatusPanelData::goal_loop 注释)。文字应用层拼好递进来。
    if (!data.goal_loop.empty()) {
        out.push_back({"goal_loop", data.goal_loop});
    }
    // 后台命令任务段(background 管理面单):非空恒挂一段,不进 items 配置
    //(理由见 StatusPanelData::background 注释)。文字应用层拼好递进来。
    if (!data.background.empty()) {
        out.push_back({"background", data.background});
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
                // 缓存注记(缓存诊断单):cached_tokens 有则"缓存命中 X(Y%)",
                // 没有则"缓存未报告"——不拿含糊的 0% 糊人。空串(一次实测都
                // 没有)整段不挂。
                text = std::string(stale_mark) + FormatTokenCount(data.used_tokens) + "/" +
                       FormatTokenCount(data.window_tokens);
                if (!data.cache_note.empty()) {
                    text += " · " + data.cache_note;
                }
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

std::string BuildCacheNote(const ContextTracker& tracker, bool last_usage_reported) {
    // 状态栏缓存注记:显示"最近一次请求"的命中,不显示会话累计——累计
    // 分母每轮都重复加进过去所有输入,数字越大越接近 100%,容易误读成
    // "每轮都 99%"。单轮口径才对应"现在这一下花了多少钱"。
    if (tracker.last_total_input_tokens() <= 0) {
        return {};  // 一次实测都没有:整段不挂
    }
    if (tracker.last_cache_read_tokens() > 0) {
        const int percent = tracker.last_cache_hit_percent();
        return trf("status.cache_note_hit", FormatTokenCount(tracker.last_cache_read_tokens()),
                   percent >= 0 ? std::to_string(percent) : std::string("?"));
    }
    if (!last_usage_reported) {
        return tr("status.cache_note_not_reported");
    }
    // 问题 9:零命中先看本地视角——最近一笔诊断说本地前缀稳定而 provider
    // 报零,明写"上游未命中",不让人误以为是自己多发几条冲掉了缓存。
    const auto& history = tracker.cache_request_history();
    if (!history.empty() && history.back().diagnostics_present &&
        history.back().miss_kind == ContextTracker::CacheMissKind::UpstreamMiss) {
        return tr("status.cache_note_upstream_miss");
    }
    const auto enabled = tracker.server_prefix_caching();
    if (enabled.has_value() && !*enabled) {
        return tr("status.cache_note_disabled");
    }
    return tr("status.cache_note_zero");
}

std::string CacheMissKindLabel(ContextTracker::CacheMissKind kind) {
    switch (kind) {
        case ContextTracker::CacheMissKind::Hit:
            return tr("cmd.context.cache_miss.hit");
        case ContextTracker::CacheMissKind::FirstRequest:
            return tr("cmd.context.cache_miss.first");
        case ContextTracker::CacheMissKind::EpochBreak:
            return tr("cmd.context.cache_miss.epoch_break");
        case ContextTracker::CacheMissKind::UpstreamMiss:
            return tr("cmd.context.cache_miss.upstream");
        case ContextTracker::CacheMissKind::Unreported:
            return tr("cmd.context.cache_miss.unreported");
        case ContextTracker::CacheMissKind::Unknown:
            return std::string();  // 诊断未随行:显示层另行说明,不猜
    }
    return std::string();
}

std::string BuildCacheDiagSegment(const ContextTracker::CacheRequestRecord& record) {
    // 问题 9 的本地前缀视角段:只拼 epoch/追加律/稳定前缀/分型/wire 字节
    // 这些"账",不碰任何正文。诊断没随行(单测/单发路径)整段只说一句
    // "诊断未随行",不拿默认值编故事。
    if (!record.diagnostics_present) {
        return tr("cmd.context.cache_diag_absent");
    }
    std::vector<std::string> parts;
    if (record.epoch_break_reason.empty()) {
        parts.push_back(trf("cmd.context.cache_diag_epoch", record.cache_epoch));
    } else {
        parts.push_back(trf("cmd.context.cache_diag_epoch_broken", record.cache_epoch,
                            record.epoch_break_reason));
    }
    parts.push_back(record.prefix_append_only ? tr("cmd.context.cache_diag_append")
                                              : tr("cmd.context.cache_diag_broken"));
    parts.push_back(trf("cmd.context.cache_diag_stable", record.stable_prefix_messages, record.total_messages));
    const std::string miss = CacheMissKindLabel(record.miss_kind);
    if (!miss.empty() && record.miss_kind != ContextTracker::CacheMissKind::Hit) {
        // 命中不必再念一遍(行首的"命中 X(Y%)"就是它);miss 分型才点名。
        parts.push_back(miss);
    }
    if (record.wire_common_prefix_bytes >= 0) {
        parts.push_back(trf("cmd.context.cache_diag_wire", record.wire_common_prefix_bytes));
    }
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += " · ";
        }
        out += part;
    }
    return out;
}

std::vector<std::string> BuildCacheRequestHistoryLines(const ContextTracker& tracker) {
    // 逐请求缓存命中(问题 5):一行 = 一次带回 usage 的 provider 请求,
    // 不是一轮用户问答。按外层用户轮次分组,"用户轮次""模型请求""工具
    // 调用"三种计数各叫各名。环形缓冲只留最近 kCacheHistorySize 次,总账
    // (total_model_requests)说总数。
    // 问题 9:每行再跟一段本地前缀视角(见 BuildCacheDiagSegment),窗口
    // 末尾添一行分型小计——命中率抖动时断在哪层一眼可辨。
    std::vector<std::string> lines;
    const auto& history = tracker.cache_request_history();
    if (history.empty()) {
        return lines;
    }
    // 窗口内覆盖的用户轮次数:历史按时间序,同 turn_id 连续,换号即换轮。
    std::size_t turn_count = 0;
    std::string previous_turn;
    bool has_turn = false;
    for (const auto& record : history) {
        if (!has_turn || record.turn_id != previous_turn) {
            ++turn_count;
            previous_turn = record.turn_id;
            has_turn = true;
        }
    }
    lines.push_back(std::string("  ") + trf("cmd.context.cache_history_header", history.size()));
    lines.push_back(std::string("  ") + trf("cmd.context.cache_history_counts", turn_count, history.size()));
    if (history.size() >= ContextTracker::kCacheHistorySize) {
        lines.push_back(std::string("  ") + trf("cmd.context.cache_history_capped",
                                                ContextTracker::kCacheHistorySize, tracker.total_model_requests()));
    }
    // 分组:同 turn_id 连续成组;标签查登记账,查不到按"未登记/轮次不明"
    // 措辞,不猜内容。
    std::string current_turn;
    bool in_group = false;
    for (const auto& record : history) {
        if (!in_group || record.turn_id != current_turn) {
            current_turn = record.turn_id;
            in_group = true;
            if (current_turn.empty()) {
                lines.push_back(std::string("    ") + tr("cmd.context.cache_history_turn_unknown"));
            } else {
                const ContextTracker::UserTurnLabel* label = tracker.FindTurnLabel(current_turn);
                if (label != nullptr) {
                    lines.push_back(std::string("    ") +
                                    trf("cmd.context.cache_history_turn", label->ordinal, label->turn_id,
                                        label->label.empty() ? tr("cmd.context.cache_history_turn_nolabel")
                                                             : label->label));
                } else {
                    lines.push_back(std::string("    ") + trf("cmd.context.cache_history_turn_plain", current_turn));
                }
            }
        }
        const std::string diag = BuildCacheDiagSegment(record);
        if (record.unreported || record.hit_percent() < 0) {
            lines.push_back(std::string("      ") +
                            trf("cmd.context.cache_history_row_unreported", record.step_index + 1, diag));
        } else {
            lines.push_back(std::string("      ") +
                            trf("cmd.context.cache_history_row", record.step_index + 1,
                                FormatTokenCount(record.input_tokens), FormatTokenCount(record.cache_read_tokens),
                                std::to_string(record.hit_percent()), diag));
        }
    }
    // 分型小计:窗口内有诊断账才出这行,各类计数用短词,断在本地还是
    // 上游一望即知。全部行都"诊断未随行"就不出,免得空账占屏。
    {
        std::size_t hits = 0;
        std::size_t firsts = 0;
        std::size_t breaks = 0;
        std::size_t upstream = 0;
        std::size_t unreported = 0;
        bool any_diag = false;
        for (const auto& record : history) {
            if (!record.diagnostics_present) {
                continue;
            }
            any_diag = true;
            switch (record.miss_kind) {
                case ContextTracker::CacheMissKind::Hit:
                    ++hits;
                    break;
                case ContextTracker::CacheMissKind::FirstRequest:
                    ++firsts;
                    break;
                case ContextTracker::CacheMissKind::EpochBreak:
                    ++breaks;
                    break;
                case ContextTracker::CacheMissKind::UpstreamMiss:
                    ++upstream;
                    break;
                case ContextTracker::CacheMissKind::Unreported:
                    ++unreported;
                    break;
                case ContextTracker::CacheMissKind::Unknown:
                    break;
            }
        }
        if (any_diag) {
            std::string tally;
            const auto append = [&tally](const std::string& label, std::size_t count) {
                if (count == 0) {
                    return;
                }
                if (!tally.empty()) {
                    tally += " · ";
                }
                tally += label + " " + std::to_string(count);
            };
            append(tr("cmd.context.cache_miss.hit"), hits);
            append(tr("cmd.context.cache_miss.first"), firsts);
            append(tr("cmd.context.cache_miss.epoch_break"), breaks);
            append(tr("cmd.context.cache_miss.upstream"), upstream);
            append(tr("cmd.context.cache_miss.unreported"), unreported);
            lines.push_back(std::string("  ") + trf("cmd.context.cache_history_tally", tally));
        }
    }
    return lines;
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
    // 占用卡片表头,与其他分组(── 缓存 ── / ── 结构与回收 ──)同一风格。
    lines.push_back("── " + tr("cmd.context.group.usage") + " ──(窗口 " + TokenText(window_tokens) + ")");
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

// ---- 回合视觉收束:耗时人话与 turn footer(纯函数,单测主战场) --------

std::string FormatTurnDuration(std::int64_t milliseconds) {
    if (milliseconds < 0) {
        milliseconds = 0;
    }
    // 十秒内留一位小数(9.4s);十至五十九秒取整(42s)。
    if (milliseconds < 10'000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(milliseconds) / 1000.0);
        return std::string(buf);
    }
    if (milliseconds < 60'000) {
        return std::to_string(milliseconds / 1000) + "s";
    }
    const std::int64_t total_seconds = milliseconds / 1000;
    const std::int64_t minutes = total_seconds / 60;
    const std::int64_t seconds_in_minute = total_seconds % 60;
    // 一小时以上 Xh Ym;一分钟以上 Xm Ys。
    if (minutes >= 60) {
        const std::int64_t hours = minutes / 60;
        return std::to_string(hours) + "h " + std::to_string(minutes % 60) + "m";
    }
    return std::to_string(minutes) + "m " + std::to_string(seconds_in_minute) + "s";
}

std::string FormatTurnFooterText(std::int64_t milliseconds, TurnFooterTone tone) {
    const std::string duration = FormatTurnDuration(milliseconds);
    switch (tone) {
        case TurnFooterTone::Stopped:
            return "Stopped after " + duration;
        case TurnFooterTone::Failed:
            return "Failed after " + duration;
        case TurnFooterTone::Worked:
            break;
    }
    return "Worked for " + duration;
}

namespace {

// 收口符:紧跟着前一个词的这些字符不许折在它们前头(P3-3 的右括号断行)。
bool IsWrapCloser(char32_t cp) {
    switch (cp) {
        case U')':
        case U']':
        case U'}':
        case U'>':
        case U'"':
        case U'%':
        case U'、':  // 、
        case U'。':  // 。
        case U'，':  // ，
        case U'：':  // ：
        case U'；':  // ；
        case U'！':  // ！
        case U'？':  // ？
        case U'〉':  // 〉
        case U'》':  // 》
        case U'】':  // 】
        case U'〕':  // 〕
        case U'”':  // ”
        case U'’':  // ’
            return true;
        default:
            return false;
    }
}

}  // namespace

std::vector<std::string> WrapStatusRows(const std::string& utf8, int width) {
    if (width <= 0 || utf8.empty()) {
        return {utf8};
    }
    // 先解码成"单元":ANSI 转义(零宽,黏在原位)、空格(唯一合法断点,
    // 断在它后面)、普通码点(按显示宽计)。收口符跟在非空格码点后时,
    // 断点只能挪到这串收口符再后面——贪心量行,放不下就退回上一个合法
    // 断点折行;一个合法断点都没有(词比整行宽)就不折,原样占一行。
    struct Unit {
        std::string bytes;
        int width = 0;
        bool is_space = false;
        bool break_after = false;  // 空格且其后不是收口符:这里可以折
    };
    std::vector<Unit> units;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        if (lead == 0x1b) {
            std::size_t end = i + 1;
            if (end < utf8.size() && utf8[end] == '[') {
                ++end;
                while (end < utf8.size() &&
                       (static_cast<unsigned char>(utf8[end]) < 0x40 ||
                        static_cast<unsigned char>(utf8[end]) > 0x7e)) {
                    ++end;
                }
                if (end < utf8.size()) {
                    ++end;  // 终止字节
                }
            }
            Unit unit;
            unit.bytes = utf8.substr(i, end - i);
            units.push_back(std::move(unit));
            i = end;
            continue;
        }
        std::size_t bytes = 1;
        if ((lead & 0xE0U) == 0xC0U) {
            bytes = 2;
        } else if ((lead & 0xF0U) == 0xE0U) {
            bytes = 3;
        } else if ((lead & 0xF8U) == 0xF0U) {
            bytes = 4;
        }
        bytes = (std::min)(bytes, utf8.size() - i);
        const std::string chunk = utf8.substr(i, bytes);
        const std::u32string decoded = Utf8ToUtf32(chunk);
        Unit unit;
        unit.bytes = chunk;
        if (!decoded.empty()) {
            unit.width = CharDisplayWidth(decoded[0]);
            unit.is_space = decoded[0] == U' ' || decoded[0] == U'　';
        }
        units.push_back(std::move(unit));
        i += bytes;
    }
    // 空格断点资格:其后紧跟收口符的空格不许当断点(收口符要跟上一个词)。
    for (std::size_t u = 0; u < units.size(); ++u) {
        if (!units[u].is_space) {
            continue;
        }
        const std::u32string next_cp = Utf8ToUtf32(u + 1 < units.size() ? units[u + 1].bytes : std::string());
        if (next_cp.empty() || !IsWrapCloser(next_cp[0])) {
            units[u].break_after = true;
        }
    }

    std::vector<std::string> rows;
    std::string current;
    int current_width = 0;
    // 最后一个可折空格在 current 里的字节下标、字节长与显示宽(全角空格
    // 占 3 字节 2 列,折行时整枚吃掉)。
    std::size_t last_break = std::string::npos;
    std::size_t last_break_bytes = 1;
    int last_break_width = 1;
    int width_at_break = 0;
    for (const Unit& unit : units) {
        if (current_width + unit.width > width && !current.empty()) {
            if (last_break != std::string::npos) {
                rows.push_back(current.substr(0, last_break));
                current = current.substr(last_break + last_break_bytes);
                current_width -= width_at_break + last_break_width;
                last_break = std::string::npos;
                // 折行后还超(词本身超宽):照加,不切。
            }
        }
        if (unit.is_space && current.empty()) {
            continue;  // 折行吃掉的行首空格不再补
        }
        if (unit.break_after) {
            last_break = current.size();
            last_break_bytes = unit.bytes.size();
            last_break_width = unit.width;
            width_at_break = current_width;
        }
        current += unit.bytes;
        current_width += unit.width;
    }
    if (!current.empty()) {
        rows.push_back(std::move(current));
    }
    if (rows.empty()) {
        rows.push_back(std::string());
    }
    return rows;
}

std::string FormatApprovalWaitNote(std::int64_t approval_wait_ms) {
    if (approval_wait_ms <= 0) {
        return std::string();
    }
    return " · waited " + FormatTurnDuration(approval_wait_ms) + " for approval";
}

}  // namespace lubancode::cli
