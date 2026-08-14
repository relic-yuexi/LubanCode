#include "cli/agent_panel.hpp"

#include <algorithm>
#include <utility>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

// 与旧版 FormatAgentPanel 同款行格式:❯ 选中标记 + 状态灯 + 名字 + 短述 + 摘要。
std::string BuildEntryRow(int index, const std::string& name, const std::string& description,
                          const std::string& state, bool active, bool running, bool failed) {
    std::string marker = active ? "\xE2\x9D\xAF " : "  ";  // ❯
    std::string lamp;
    if (index == 0) {
        lamp = "\xE2\x97\x8F ";  // ● main
    } else if (running) {
        lamp = "\xE2\x97\x8C ";  // ◌
    } else {
        lamp = failed ? "\xC3\x97 " : "\xE2\x97\x8F ";  // × / ●
    }
    return marker + lamp + name + (description.empty() ? std::string() : "  " + description) +
           (state.empty() ? std::string() : "  " + state);
}

}  // namespace

AgentPanelController::Outcome AgentPanelController::HandleKey(PanelKey key, int total_entries,
                                                              bool composer_empty,
                                                              std::chrono::steady_clock::time_point now) {
    Outcome out;
    const bool panel_alive = total_entries > 1;
    // 两段确认窗口内的任何别的键都撤销第一段(免得误杀),随后按原语义走。
    if (armed_ && key != PanelKey::StopAllArm && key != PanelKey::StopAllConfirm) {
        armed_ = false;
        out.redraw = true;
    }
    if (!panel_alive) {
        // 面板不存在(没有后台子代理):全部键还给 composer,状态收干净。
        if (focus_ || detail_) {
            Reset();
            out.redraw = true;
        }
        return out;
    }

    switch (key) {
        case PanelKey::Up:
        case PanelKey::Down: {
            if (!composer_empty) {
                return out;  // 半句正文优先:上下键归 composer 历史
            }
            const int delta = key == PanelKey::Up ? -1 : 1;
            focus_ = true;
            detail_ = false;  // 换选择 = 离开旧查看态,标签跟着换
            selected_ = (selected_ + delta + total_entries) % total_entries;
            out.consumed = true;
            out.redraw = true;
            return out;
        }
        case PanelKey::EnterView: {
            if (!focus_ || !composer_empty) {
                return out;  // 未聚焦时 Enter 归 composer(提交);打字时更不抢
            }
            detail_ = !detail_;
            out.consumed = true;
            out.redraw = true;
            return out;
        }
        case PanelKey::Esc: {
            if (detail_) {
                detail_ = false;  // 先退查看态,标签摘掉
                out.consumed = true;
                out.redraw = true;
                return out;
            }
            if (focus_) {
                focus_ = false;
                selected_ = 0;
                out.consumed = true;
                out.redraw = true;
                return out;
            }
            return out;  // 没在面板态:Esc 还给编辑器(清空输入老语义)
        }
        case PanelKey::StopEntry: {
            // 正在输入正文时普通字母 x 绝不触发代理操作;main 行也不接停止。
            if (!focus_ || !composer_empty || selected_ <= 0) {
                return out;
            }
            out.consumed = true;
            out.redraw = true;
            out.stop_current = true;
            return out;
        }
        case PanelKey::StopAllArm: {
            if (!composer_empty) {
                return out;  // 打字中途不启全局停止(组合键只在面板可控制态生效)
            }
            armed_ = !armed_;
            if (armed_) {
                arm_time_ = now;
            }
            out.consumed = true;
            out.redraw = true;
            return out;
        }
        case PanelKey::StopAllConfirm: {
            if (!armed_) {
                return out;  // 没按过第一段:Ctrl+K 不是确认,别拦
            }
            armed_ = false;
            out.consumed = true;
            out.redraw = true;
            out.stop_all = true;
            return out;
        }
        case PanelKey::Other:
            return out;
    }
    return out;
}

void AgentPanelController::OnEntriesChanged(int total_entries) {
    if (total_entries <= 1) {
        // 子代理全没了:面板消失,状态收干净(main 之外没有可聚焦的条目)。
        Reset();
        return;
    }
    if (selected_ >= total_entries) {
        selected_ = total_entries - 1;
    }
}

void AgentPanelController::CloseView() {
    detail_ = false;
    focus_ = false;
    selected_ = 0;
    armed_ = false;
}

void AgentPanelController::Reset() {
    focus_ = false;
    detail_ = false;
    selected_ = 0;
    armed_ = false;
}

std::optional<int> AgentPanelController::target_index() const {
    if (!detail_ || selected_ <= 0) {
        return std::nullopt;
    }
    return selected_;
}

bool AgentPanelController::ExpireArmed(std::chrono::steady_clock::time_point now) {
    if (!armed_) {
        return false;
    }
    if (now - arm_time_ > kStopAllWindow) {
        armed_ = false;
        return true;
    }
    return false;
}

AgentPanelLayout LayoutAgentPanel(const std::vector<AgentPanelEntry>& agents, int selected, bool focused,
                                  bool detail_open, const std::vector<std::string>& detail_lines,
                                  int max_visible_entries, int max_total_rows, int width, bool armed_stop_all) {
    AgentPanelLayout out;
    out.total_count = static_cast<int>(agents.size()) + 1;
    if (agents.empty() || width <= 0) {
        return out;
    }
    const int safe_selected = selected >= 0 && selected < out.total_count ? selected : 0;
    // 连首行提示都摆不下:整块不画(面板绝不把输入框挤走,宁可不出场)。
    if (max_total_rows > 0 && max_total_rows < 2) {
        return out;
    }

    // ---- 空间预算(规格:面板向上长,不把输入框顶出视口) ----
    // 优先级:首行操作提示(永不丢) > 至少一条条目(含选中) > 详情行(超
    // 预算保头部,任务说明优先,末行写清未展示数) > 更多条目。有整块预算
    // (max_total_rows>0)时从"最多条目"往下让,先让详情尾巴、再让条目数;
    // 没预算(不限高)就是全显。
    const int entry_cap = max_visible_entries > 0 ? max_visible_entries : out.total_count;
    const int detail_size = detail_open && safe_selected != 0 ? static_cast<int>(detail_lines.size())
                        : (detail_open ? 1 : 0);  // main 详情固定 1 行
    int visible_count = (std::min)(out.total_count, entry_cap);
    int detail_shown = detail_size;   // 详情正文摆几行(不含 detail_hint)
    bool detail_truncated = false;
    if (max_total_rows > 0) {
        // 从满条目往下让,找到第一个塞得下的组合;计数行(开窗才有)一并算。
        int chosen = -1;
        for (int vis = visible_count; vis >= 1; --vis) {
            const int note = vis < out.total_count ? 1 : 0;
            const int detail_hint = detail_open ? 1 : 0;
            const int base = 1 + note + vis + detail_hint;  // 提示+计数+条目+详情提示
            if (base > max_total_rows) {
                continue;
            }
            const int detail_room = max_total_rows - base;  // 详情正文还能摆几行
            int shown = detail_size;
            bool truncated = false;
            if (detail_open && detail_size > detail_room) {
                if (detail_room >= 2) {
                    shown = detail_room - 1;  // 正文 + 一行"另有 N 行未展示"
                    truncated = true;
                } else if (detail_room == 1) {
                    shown = 0;  // 只剩一行:总述"另有 N 行"
                    truncated = true;
                } else {
                    continue;  // 详情正文一行都摆不下,让一条条目再试
                }
            }
            chosen = vis;
            detail_shown = shown;
            detail_truncated = truncated;
            break;
        }
        if (chosen < 1) {
            return out;  // 连一条条目都摆不下:不出场
        }
        visible_count = chosen;
    }
    int first = 0;
    if (visible_count < out.total_count) {
        first = safe_selected - visible_count / 2;  // 选中尽量落窗口中部
        if (first < 0) {
            first = 0;
        }
        if (first + visible_count > out.total_count) {
            first = out.total_count - visible_count;
        }
    }
    out.visible_first = first;
    out.visible_count = visible_count;
    out.hidden_above = first;
    out.hidden_below = out.total_count - first - visible_count;

    out.lines.push_back(armed_stop_all ? tr("agent_panel.hint_armed") : tr("agent_panel.hint"));
    if (out.hidden_above > 0 || out.hidden_below > 0) {
        out.lines.push_back(trf("agent_panel.window_note", out.total_count, out.hidden_above, out.hidden_below));
    }

    const int room = width - 1;
    const auto push_row = [&](std::string row) {
        out.lines.push_back(TruncateUtf8ToDisplayWidth(std::move(row), room));
    };
    for (int index = first; index < first + visible_count; ++index) {
        if (index == 0) {
            push_row(BuildEntryRow(0, "main", tr("agent_panel.main"), std::string(),
                                   focused && index == safe_selected, true, false));
        } else {
            const AgentPanelEntry& entry = agents[static_cast<std::size_t>(index - 1)];
            push_row(BuildEntryRow(index, entry.name, entry.description, entry.state,
                                   focused && index == safe_selected, entry.running, entry.failed));
        }
    }

    if (detail_open) {
        out.lines.push_back(tr("agent_panel.detail_hint"));
        if (safe_selected == 0) {
            out.lines.push_back("  " + tr("agent_panel.main_detail"));
        } else {
            const int shown = detail_truncated ? detail_shown : detail_size;
            for (int i = 0; i < shown && i < static_cast<int>(detail_lines.size()); ++i) {
                out.lines.push_back("  " + TruncateUtf8ToDisplayWidth(detail_lines[static_cast<std::size_t>(i)],
                                                                      room - 2));
            }
            if (detail_truncated) {
                out.lines.push_back("  " + trf("agent_panel.detail_more_note",
                                               detail_lines.size() - static_cast<std::size_t>(shown)));
            }
        }
    }
    return out;
}

std::string TruncatePanelTag(const std::string& tag, int max_width) {
    if (max_width <= 0) {
        return std::string();
    }
    return TruncateUtf8ToDisplayWidth(tag, max_width);
}

std::string BuildRuleWithTag(const std::string& stats, const std::string& reset, const std::string& tag,
                             int width) {
    const bool plain = reset.empty();
    // 画 n 格横线。BuildDividerLine(w,·,w) 吐 w-1 格(BoxRuleLine 同款口径),
    // 这里想要精确 n 格,传 n+1 即可。
    const auto glyphs = [&](int n) {
        return stats + BuildDividerLine(n + 1, plain, n + 1) + reset;
    };
    if (width <= kMinRuleCols + 2 || tag.empty()) {
        return glyphs(width - 1);  // 与 BoxRuleLine(theme, width) 同宽
    }
    // 满宽横线(width-1 格)- 1 格间隙 - 标签,横线保底 kMinRuleCols 格。
    const int tag_room = width - 2 - kMinRuleCols;
    if (tag_room < 3) {
        return glyphs(width - 1);
    }
    const std::string truncated = TruncatePanelTag(tag, tag_room);
    const int tag_width = static_cast<int>(DisplayWidthUtf8(truncated));
    if (tag_width <= 0) {
        return glyphs(width - 1);
    }
    const int rule_width = width - 2 - tag_width;
    if (rule_width < kMinRuleCols) {
        return glyphs(width - 1);
    }
    return glyphs(rule_width) + " " + truncated;
}

}  // namespace lubancode::cli
