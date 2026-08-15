#include "cli/agent_panel.hpp"

#include <algorithm>
#include <utility>

#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {

// 单列闲置代理上限:超过三只折成一行汇总(规格"闲置与终态收纳")。
constexpr int kMaxIdleRows = 3;
// 身份列宽钳位(照 Claude Code 的密度:最窄 4 列、最宽 28 列)。
constexpr int kIdentityMinCols = 4;
constexpr int kIdentityMaxCols = 28;
// 提示行在这一列宽以上才摆"Ctrl+X Ctrl+K 停止全部"这类低频长文案。
constexpr int kStopAllHintMinCols = 90;

std::string PadRightTo(const std::string& text, int width) {
    const int current = static_cast<int>(DisplayWidthUtf8(text));
    if (current >= width || width <= 0) {
        return text;
    }
    return text + std::string(static_cast<std::size_t>(width - current), ' ');
}

}  // namespace

AgentPanelController::Outcome AgentPanelController::HandleKey(PanelKey key,
                                                              const std::vector<int>& agent_task_ids,
                                                              bool composer_empty,
                                                              std::chrono::steady_clock::time_point now) {
    Outcome out;
    const int total_entries = static_cast<int>(agent_task_ids.size()) + 1;
    const bool panel_alive = total_entries > 1;
    // 两段确认窗口内的任何别的键都撤销第一段(免得误杀),随后按原语义走。
    if (armed_ && key != PanelKey::StopAllArm && key != PanelKey::StopAllConfirm) {
        armed_ = false;
        out.redraw = true;
    }
    if (!panel_alive) {
        // 面板不存在(没有任何子代理):全部键还给 composer,状态收干净。
        if (focus_ || detail_) {
            Reset();
            out.redraw = true;
        }
        return out;
    }
    const int index = selected_index(agent_task_ids);

    switch (key) {
        case PanelKey::Up:
        case PanelKey::Down: {
            if (!composer_empty) {
                return out;  // 半句正文优先:上下键归 composer 历史
            }
            const int delta = key == PanelKey::Up ? -1 : 1;
            focus_ = true;
            detail_ = false;  // 换选择 = 离开旧查看态,标签跟着换
            const int next = (index + delta + total_entries) % total_entries;
            selected_ = next;
            selected_task_id_ = next == 0 ? 0 : agent_task_ids[static_cast<std::size_t>(next - 1)];
            out.consumed = true;
            out.redraw = true;
            return out;
        }
        case PanelKey::EnterView: {
            if (!focus_ || !composer_empty) {
                return out;  // 未聚焦时 Enter 归 composer(提交);打字时更不抢
            }
            if (selected_task_id_ == kIdleSummaryTaskId) {
                // 汇总行:Enter 只展开闲置列表,不开查看态、不改收件目标。
                idle_expanded_ = true;
                out.consumed = true;
                out.redraw = true;
                return out;
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
            if (idle_expanded_) {
                idle_expanded_ = false;  // 再收起闲置汇总(规格:Esc 收起)
                out.consumed = true;
                out.redraw = true;
                return out;
            }
            if (focus_) {
                focus_ = false;
                selected_ = 0;
                selected_task_id_ = 0;
                out.consumed = true;
                out.redraw = true;
                return out;
            }
            return out;  // 没在面板态:Esc 还给编辑器(清空输入老语义)
        }
        case PanelKey::StopEntry: {
            // 正在输入正文时普通字母 x 绝不触发代理操作;main 行与闲置汇总
            // 哨兵也不接停止/清除(哨兵不是真任务)。
            if (!focus_ || !composer_empty || index <= 0 || selected_task_id_ == kIdleSummaryTaskId) {
                return out;
            }
            out.consumed = true;
            out.redraw = true;
            out.stop_current = true;
            out.stop_current_task_id = selected_task_id_;
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

int AgentPanelController::selected_index(const std::vector<int>& agent_task_ids) const {
    if (selected_task_id_ == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < agent_task_ids.size(); ++i) {
        if (agent_task_ids[i] == selected_task_id_) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;  // id 已不在表里(增删后的空档):按 main 算,下一拍 OnEntriesChanged 修正
}

void AgentPanelController::OnEntriesChanged(const std::vector<int>& agent_task_ids) {
    if (agent_task_ids.empty()) {
        // 子代理全没了:面板消失,状态收干净(main 之外没有可聚焦的条目)。
        Reset();
        return;
    }
    // 查看态的目标被清掉:强制收起,收件目标回 main(规格七)。
    if (detail_ && target_task_id().has_value() &&
        std::find(agent_task_ids.begin(), agent_task_ids.end(), selected_task_id_) == agent_task_ids.end()) {
        CloseView();
        return;
    }
    const int found = selected_index(agent_task_ids);
    if (found > 0) {
        selected_ = found;  // id 还在:选择原样保住(哪怕列表重排)
        return;
    }
    // id 没了:落到相邻条目(旧下标钳回界内),没有相邻就回 main。
    selected_ = (std::min)(selected_, static_cast<int>(agent_task_ids.size()));
    selected_task_id_ = selected_ == 0 ? 0 : agent_task_ids[static_cast<std::size_t>(selected_ - 1)];
}

void AgentPanelController::CloseView() {
    detail_ = false;
    focus_ = false;
    selected_ = 0;
    selected_task_id_ = 0;
    armed_ = false;
    idle_expanded_ = false;
}

void AgentPanelController::Reset() {
    focus_ = false;
    detail_ = false;
    selected_ = 0;
    selected_task_id_ = 0;
    armed_ = false;
    idle_expanded_ = false;
}

std::optional<int> AgentPanelController::target_task_id() const {
    if (!detail_ || selected_task_id_ <= 0) {
        return std::nullopt;
    }
    return selected_task_id_;
}

// ---- AgentPanelSession:会话级外壳,一把小锁把键处理与绘制快照隔开 ----

AgentPanelController::Outcome AgentPanelSession::HandleKey(PanelKey key, const std::vector<int>& agent_task_ids,
                                                           bool composer_empty,
                                                           std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return controller_.HandleKey(key, agent_task_ids, composer_empty, now);
}

void AgentPanelSession::OnEntriesChanged(const std::vector<int>& agent_task_ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_.OnEntriesChanged(agent_task_ids);
}

void AgentPanelSession::CloseView() {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_.CloseView();
}

void AgentPanelSession::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    controller_.Reset();
}

bool AgentPanelSession::ExpireArmed(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return controller_.ExpireArmed(now);
}

AgentPanelSession::Snapshot AgentPanelSession::SnapshotFor(const std::vector<int>& agent_task_ids) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot out;
    out.focused = controller_.focused();
    out.detail_open = controller_.detail_open();
    out.stop_all_armed = controller_.stop_all_armed();
    out.idle_expanded = controller_.idle_expanded();
    out.selected_index = controller_.selected_index(agent_task_ids);
    out.selected_task_id = controller_.selected_task_id();
    out.target_task_id = controller_.target_task_id();
    return out;
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

std::vector<int> DockNavigationIds(const std::vector<AgentPanelEntry>& agents, bool idle_expanded,
                                   int viewed_task_id) {
    std::vector<int> ids;
    ids.reserve(agents.size() + 1);
    ids.push_back(0);  // main 固定第 0 项
    int idle_rows = 0;
    bool summary_slot = false;
    for (const auto& entry : agents) {
        const bool idle = !entry.running && !entry.failed;
        const bool viewed = viewed_task_id == entry.task_id;
        if (idle && !idle_expanded && idle_rows >= kMaxIdleRows && !viewed) {
            if (!summary_slot) {
                ids.push_back(kIdleSummaryTaskId);
                summary_slot = true;
            }
            continue;
        }
        if (idle) {
            ++idle_rows;
        }
        ids.push_back(entry.task_id);
    }
    return ids;
}

AgentDockLayout LayoutAgentDock(const std::vector<AgentPanelEntry>& agents, int selected, bool focused,
                                bool detail_open, const std::vector<std::string>& detail_lines,
                                int max_visible_entries, int max_total_rows, int width, bool armed_stop_all,
                                bool streaming, bool idle_expanded, int viewed_task_id) {
    AgentDockLayout out;
    out.navigation_ids.push_back(0);  // main 固定第 0 项(空表也一样)
    if (agents.empty() || width <= 0) {
        return out;  // 没有任何子代理:整坞不画(不得只剩一条孤零零的操作提示)
    }
    // 连首行提示都摆不下:整块不画(坞绝不把 composer 挤走,宁可不出场)。
    if (max_total_rows > 0 && max_total_rows < 2) {
        return out;
    }

    // ---- 折叠(规格"闲置与终态收纳") ----
    // 导航表只有一份来源(DockNavigationIds),这里按它对齐出条目指针表,
    // 折起来的数目用"总数 - 保留数"倒推——布局与按键状态机永远同一本账。
    out.navigation_ids = DockNavigationIds(agents, idle_expanded, detail_open ? viewed_task_id : 0);
    std::vector<const AgentPanelEntry*> nav_entries;  // 与 navigation_ids 对齐(main/哨兵 = nullptr)
    nav_entries.reserve(out.navigation_ids.size());
    std::size_t agent_cursor = 0;
    int kept_entries = 0;
    for (const int id : out.navigation_ids) {
        if (id == 0 || id == kIdleSummaryTaskId) {
            nav_entries.push_back(nullptr);
            continue;
        }
        while (agent_cursor < agents.size() && agents[agent_cursor].task_id != id) {
            ++agent_cursor;  // 被折起来的条目:跳过
        }
        nav_entries.push_back(agent_cursor < agents.size() ? &agents[agent_cursor] : nullptr);
        if (agent_cursor < agents.size()) {
            ++agent_cursor;
            ++kept_entries;
        }
    }
    out.hidden_idle = static_cast<int>(agents.size()) - kept_entries;
    out.idle_summary = out.hidden_idle > 0;
    out.total_count = static_cast<int>(out.navigation_ids.size());

    const int safe_selected = selected >= 0 && selected < out.total_count ? selected : 0;

    // ---- 窗口化:常态最多单列 5 只代理,围着 selected 开窗,选中永不消失 ----
    const int entry_cap = max_visible_entries > 0 ? max_visible_entries : out.total_count;
    const int detail_size = detail_open && safe_selected != 0 ? static_cast<int>(detail_lines.size())
                        : (detail_open ? 1 : 0);  // main 详情固定 1 行
    int visible_count = (std::min)(out.total_count, entry_cap);
    int detail_shown = detail_size;
    bool detail_truncated = false;
    // ---- 矮屏预算:提示(永不丢)> 至少一条条目(含选中)> 详情 > 更多条目 ----
    if (max_total_rows > 0) {
        int chosen = -1;
        for (int vis = visible_count; vis >= 1; --vis) {
            const int note = vis < out.total_count ? 1 : 0;
            const int detail_hint = detail_open ? 1 : 0;
            const int base = 1 + note + vis + detail_hint;
            if (base > max_total_rows) {
                continue;
            }
            const int detail_room = max_total_rows - base;
            int shown = detail_size;
            bool truncated = false;
            if (detail_open && detail_size > detail_room) {
                if (detail_room >= 2) {
                    shown = detail_room - 1;
                    truncated = true;
                } else if (detail_room == 1) {
                    shown = 0;
                    truncated = true;
                } else {
                    continue;
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

    // ---- 提示行:随焦点/闲置展开态收放,窄屏(<90 列)摘掉低频长文案 ----
    const bool on_summary =
        out.idle_summary && out.navigation_ids[static_cast<std::size_t>(safe_selected)] == kIdleSummaryTaskId;
    const char* hint_key;
    if (armed_stop_all) {
        hint_key = "agent_panel.hint_armed";
    } else if (focused && on_summary) {
        hint_key = streaming ? "agent_panel.stream_hint_idle_expanded" : "agent_panel.hint_idle_expanded";
    } else if (focused) {
        const bool wide = width >= kStopAllHintMinCols;
        hint_key = streaming ? (wide ? "agent_panel.stream_hint_focused" : "agent_panel.stream_hint_focused_short")
                             : (wide ? "agent_panel.hint_focused" : "agent_panel.hint_focused_short");
    } else {
        const bool wide = width >= kStopAllHintMinCols;
        hint_key = streaming ? (wide ? "agent_panel.stream_hint" : "agent_panel.stream_hint_short")
                             : (wide ? "agent_panel.hint" : "agent_panel.hint_short");
    }
    AgentDockRow hint_row;
    hint_row.kind = AgentDockRow::Kind::Hint;
    hint_row.text = tr(hint_key);
    out.rows.push_back(std::move(hint_row));

    if (out.hidden_above > 0 || out.hidden_below > 0) {
        AgentDockRow note_row;
        note_row.kind = AgentDockRow::Kind::Note;
        note_row.text = trf("agent_panel.window_note", out.total_count, out.hidden_above, out.hidden_below);
        out.rows.push_back(std::move(note_row));
    }

    // ---- 条目行(main + 代理 + 闲置汇总哨兵) ----
    int identity_max = 4;  // "main"
    int status_max = 0;
    const auto entry_row_at = [&](int nav_index) {
        const int id = out.navigation_ids[static_cast<std::size_t>(nav_index)];
        const AgentPanelEntry* entry = nav_entries[static_cast<std::size_t>(nav_index)];
        AgentDockRow row;
        row.kind = id == kIdleSummaryTaskId ? AgentDockRow::Kind::IdleSummary : AgentDockRow::Kind::Entry;
        row.task_id = id;
        row.selected = focused && nav_index == safe_selected;
        row.marker = row.selected ? "\xE2\x9D\xAF " : "  ";  // ❯
        if (id == 0) {
            row.lamp = "\xE2\x97\x8F ";  // ● main
            row.identity = "main";
            row.middle = tr("agent_panel.main");
        } else if (id == kIdleSummaryTaskId) {
            row.lamp = "  ";
            row.identity.clear();
            row.middle = trf("agent_panel.idle_summary", out.hidden_idle);
        } else {
            if (detail_open && viewed_task_id == id) {
                row.lamp = "\xE2\x97\x89 ";  // ◉ 正在查看(实心标记,不靠颜色)
            } else if (entry->running) {
                row.lamp = "\xE2\x97\x8C ";  // ◌ 运行中
            } else {
                row.lamp = entry->failed ? "\xC3\x97 " : "\xE2\x97\x8B ";  // × 失败 / ○ 完成
            }
            row.identity = entry->name;
            row.middle = entry->title;
            row.status = entry->state;
            identity_max = (std::max)(identity_max, static_cast<int>(DisplayWidthUtf8(entry->name)));
            status_max = (std::max)(status_max, static_cast<int>(DisplayWidthUtf8(entry->state)));
        }
        return row;
    };
    std::vector<AgentDockRow> entry_rows;
    entry_rows.reserve(static_cast<std::size_t>(visible_count));
    for (int nav_index = first; nav_index < first + visible_count; ++nav_index) {
        entry_rows.push_back(entry_row_at(nav_index));
    }
    out.identity_width = (std::min)(kIdentityMaxCols, (std::max)(kIdentityMinCols, identity_max));
    out.status_width = status_max;
    for (auto& row : entry_rows) {
        out.rows.push_back(std::move(row));
    }

    // ---- 查看态详情(缀在树后面,预算内保头部) ----
    if (detail_open) {
        AgentDockRow hint;
        hint.kind = AgentDockRow::Kind::DetailHint;
        hint.text = tr("agent_panel.detail_hint");
        out.rows.push_back(std::move(hint));
        if (safe_selected == 0) {
            AgentDockRow row;
            row.kind = AgentDockRow::Kind::DetailLine;
            row.text = "  " + tr("agent_panel.main_detail");
            out.rows.push_back(std::move(row));
        } else {
            const int shown = detail_truncated ? detail_shown : detail_size;
            for (int i = 0; i < shown && i < static_cast<int>(detail_lines.size()); ++i) {
                AgentDockRow row;
                row.kind = AgentDockRow::Kind::DetailLine;
                row.text = "  " + detail_lines[static_cast<std::size_t>(i)];
                out.rows.push_back(std::move(row));
            }
            if (detail_truncated) {
                AgentDockRow row;
                row.kind = AgentDockRow::Kind::DetailLine;
                row.text = trf("agent_panel.detail_more_note",
                               detail_lines.size() - static_cast<std::size_t>(shown));
                out.rows.push_back(std::move(row));
            }
        }
    }
    return out;
}

std::string RenderAgentDockRow(const AgentDockLayout& layout, const AgentDockRow& row, int width) {
    const int room = (std::max)(0, width - 1);
    if (row.kind != AgentDockRow::Kind::Entry && row.kind != AgentDockRow::Kind::IdleSummary) {
        return TruncateUtf8ToDisplayWidth(row.text, room);
    }
    // 三列:标记+灯(定宽 4) → 身份(identity_width) → 中段(优先吃宽、优先
    // 截断)→ 右状态(status_width,右对齐)。窄屏先缩状态、再截中段,身份
    // 列起点永不动——耗时刷新时整行不左右乱跳。
    const int left = 4 + layout.identity_width + 2;
    if (room < left + 4) {
        // 极窄:退回"标记+灯+名字+标题"单串截断,绝不撑破行宽。
        return TruncateUtf8ToDisplayWidth(row.marker + row.lamp + row.identity + "  " + row.middle, room);
    }
    int status_room = 0;
    if (!row.status.empty()) {
        status_room = (std::min)(layout.status_width, room - left - 4);
    }
    const int middle_room = room - left - (status_room > 0 ? status_room + 2 : 0);
    std::string line = row.marker + row.lamp + PadRightTo(row.identity, layout.identity_width) + "  " +
                       TruncateUtf8ToDisplayWidth(row.middle, (std::max)(0, middle_room));
    if (status_room > 0) {
        const int used = 4 + layout.identity_width + 2 + static_cast<int>(DisplayWidthUtf8(
                            TruncateUtf8ToDisplayWidth(row.middle, (std::max)(0, middle_room))));
        const int status_start = room - status_room;
        const int gap = (std::max)(2, status_start - used);
        line += std::string(static_cast<std::size_t>(gap), ' ') +
                TruncateUtf8ToDisplayWidth(row.status, status_room);
    }
    return line;
}

std::vector<std::string> RenderAgentDockLines(const AgentDockLayout& layout, int width) {
    std::vector<std::string> out;
    out.reserve(layout.rows.size());
    for (const auto& row : layout.rows) {
        out.push_back(RenderAgentDockRow(layout, row, width));
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
