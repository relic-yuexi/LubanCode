// session_picker.hpp 的实现:纯逻辑,不碰终端。

#include "cli/session_picker.hpp"

#include <algorithm>
#include <cmath>

#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"  // TruncateUtf8ToDisplayWidth/DisplayWidthUtf8

namespace lubancode::cli {

namespace {

std::string ToLowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void EraseLastUtf8(std::string& text) {
    if (text.empty()) {
        return;
    }
    std::size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    text.erase(pos);
}

SessionPickerFocus NextFocus(SessionPickerFocus focus) {
    switch (focus) {
        case SessionPickerFocus::Search: return SessionPickerFocus::Filter;
        case SessionPickerFocus::Filter: return SessionPickerFocus::Sort;
        case SessionPickerFocus::Sort: return SessionPickerFocus::Search;
    }
    return SessionPickerFocus::Search;
}

SessionPickerFocus PrevFocus(SessionPickerFocus focus) {
    switch (focus) {
        case SessionPickerFocus::Search: return SessionPickerFocus::Sort;
        case SessionPickerFocus::Filter: return SessionPickerFocus::Search;
        case SessionPickerFocus::Sort: return SessionPickerFocus::Filter;
    }
    return SessionPickerFocus::Search;
}

}  // namespace

bool SessionPickerMatches(const SessionPickerEntry& entry, const std::string& search) {
    if (search.empty()) {
        return true;
    }
    const std::string needle = ToLowerAscii(search);
    return ToLowerAscii(entry.title).find(needle) != std::string::npos ||
           ToLowerAscii(entry.preview).find(needle) != std::string::npos ||
           ToLowerAscii(entry.id).find(needle) != std::string::npos ||
           ToLowerAscii(entry.cwd).find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 控制器
// ---------------------------------------------------------------------------

SessionPickerCore::SessionPickerCore(std::size_t visible_capacity)
    : capacity_(visible_capacity == 0 ? 1 : visible_capacity) {}

void SessionPickerCore::Refilter(const std::string& prefer_id) {
    matches_.clear();
    for (const auto& entry : entries_) {
        if (SessionPickerMatches(entry, state_.search)) {
            matches_.push_back(entry);
        }
    }
    // 留住选中:prefer_id(或换筛选前的旧选中)还在命中里就守住它;
    // 没了落到最近一行(单子口径:按 id 留住,消失了才落到最近一行)。
    std::size_t want = 0;
    const std::string& target = prefer_id.empty() ? std::string() : prefer_id;
    if (!target.empty()) {
        for (std::size_t i = 0; i < matches_.size(); ++i) {
            if (matches_[i].id == target) {
                want = i;
                break;
            }
        }
    }
    selected_ = matches_.empty() ? 0 : (std::min)(want, matches_.size() - 1);
    viewport_top_ = matches_.empty() ? 0 : (std::min)(viewport_top_, matches_.size() - 1);
    ClampSelection();
}

void SessionPickerCore::ClampSelection() {
    if (matches_.empty()) {
        selected_ = 0;
        viewport_top_ = 0;
        return;
    }
    if (selected_ >= matches_.size()) {
        selected_ = matches_.size() - 1;
    }
    const std::size_t cap = capacity_ == 0 ? 1 : capacity_;
    if (selected_ < viewport_top_) {
        viewport_top_ = selected_;
    } else if (selected_ >= viewport_top_ + cap) {
        viewport_top_ = selected_ + 1 > cap ? selected_ + 1 - cap : 0;
    }
    if (viewport_top_ + cap > matches_.size()) {
        viewport_top_ = matches_.size() > cap ? matches_.size() - cap : 0;
    }
}

void SessionPickerCore::MoveSelection(std::size_t index) {
    if (matches_.empty()) {
        return;
    }
    selected_ = (std::min)(index, matches_.size() - 1);
    ClampSelection();
}

const SessionPickerEntry* SessionPickerCore::SetEntries(std::vector<SessionPickerEntry> entries,
                                                        const std::string& prefer_id) {
    entries_ = std::move(entries);
    Refilter(prefer_id);
    return SelectedEntry();
}

void SessionPickerCore::SetCapacity(std::size_t visible_capacity) {
    capacity_ = visible_capacity == 0 ? 1 : visible_capacity;
    ClampSelection();
}

const SessionPickerEntry* SessionPickerCore::SelectedEntry() const {
    if (selected_ >= matches_.size()) {
        return nullptr;
    }
    return &matches_[selected_];
}

const SessionPickerCore::State& SessionPickerCore::HandleKey(const KeyEvent& event) {
    if (state_.submitted || state_.cancelled) {
        return state_;
    }
    // 转录浮层开着:除 Enter(提交)、Esc/Ctrl+C/Ctrl+D/Ctrl+T/Ctrl+E(收
    // 浮层)外全部落空——上下浏览、翻页、打字都不动列表状态,看完回原行。
    if (state_.transcript_open) {
        switch (event.kind) {
            case KeyKind::Enter:
            case KeyKind::NewLine:
                state_.submitted = !matches_.empty();
                break;
            case KeyKind::Esc:
            case KeyKind::CtrlC:
            case KeyKind::CtrlD:
            case KeyKind::CtrlT:
            case KeyKind::CtrlE:
                state_.transcript_open = false;
                break;
            default:
                break;  // 浮层里其余键落空
        }
        return state_;
    }
    switch (event.kind) {
        case KeyKind::Tab:
            state_.focus = NextFocus(state_.focus);
            break;
        case KeyKind::ShiftTab:
            state_.focus = PrevFocus(state_.focus);
            break;
        case KeyKind::Up:
            if (selected_ > 0) {
                MoveSelection(selected_ - 1);
            }
            break;
        case KeyKind::Down:
            MoveSelection(selected_ + 1);
            break;
        case KeyKind::PageUp: {
            const std::size_t cap = capacity_ == 0 ? 1 : capacity_;
            MoveSelection(selected_ > cap ? selected_ - cap : 0);
            break;
        }
        case KeyKind::PageDown: {
            MoveSelection(selected_ + capacity_);
            break;
        }
        case KeyKind::Home:
            MoveSelection(0);
            break;
        case KeyKind::End:
            if (!matches_.empty()) {
                MoveSelection(matches_.size() - 1);
            }
            break;
        case KeyKind::Left:
            if (state_.focus == SessionPickerFocus::Filter) {
                state_.scope = state_.scope == SessionPickerScope::Cwd ? SessionPickerScope::All
                                                                       : SessionPickerScope::Cwd;
            } else if (state_.focus == SessionPickerFocus::Sort) {
                state_.sort = state_.sort == SessionPickerSort::Updated ? SessionPickerSort::Created
                                                                        : SessionPickerSort::Updated;
            }
            // Search 焦点下左右键不动搜索词(单行尾编辑,没有光标概念;
            // 免得左右一想挪光标却把筛选切了)。
            break;
        case KeyKind::Right:
            if (state_.focus == SessionPickerFocus::Filter) {
                state_.scope = state_.scope == SessionPickerScope::Cwd ? SessionPickerScope::All
                                                                       : SessionPickerScope::Cwd;
            } else if (state_.focus == SessionPickerFocus::Sort) {
                state_.sort = state_.sort == SessionPickerSort::Updated ? SessionPickerSort::Created
                                                                        : SessionPickerSort::Updated;
            }
            break;
        case KeyKind::Char:
            if (state_.focus == SessionPickerFocus::Search && event.ch >= 0x20 && event.ch != 0x7F &&
                !event.ctrl && !event.alt) {
                AppendUtf8(state_.search, event.ch);
            }
            break;
        case KeyKind::Paste:
            if (state_.focus == SessionPickerFocus::Search) {
                for (const char c : event.text) {
                    if (c == '\r' || c == '\n' || c == '\t') {
                        state_.search.push_back(' ');  // 搜索词是单行,换行折成空格
                    } else if (c != '\0') {
                        state_.search.push_back(c);
                    }
                }
            }
            break;
        case KeyKind::Backspace:
            if (state_.focus == SessionPickerFocus::Search) {
                EraseLastUtf8(state_.search);
            }
            break;
        case KeyKind::Enter:
        case KeyKind::NewLine:
            state_.submitted = !matches_.empty();  // 空表 Enter 不当提交
            break;
        case KeyKind::CtrlO:
            // 紧凑/舒展:只改画法,筛选结果与选中项一个不动(单子口径)。
            state_.layout = state_.layout == SessionPickerLayout::Compact
                                ? SessionPickerLayout::Comfortable
                                : SessionPickerLayout::Compact;
            break;
        case KeyKind::CtrlE:
            if (state_.transcript_open) {
                state_.transcript_open = false;  // 转录浮层里 Ctrl+E 也当收浮层
                break;
            }
            state_.expanded = !state_.expanded && !matches_.empty();
            break;
        case KeyKind::CtrlT:
            // 转录浮层开/收。开着时再按一次收掉;关着时开(空表开了也只
            // 是空浮层,接线层给"没有会话"的画面,不拦)。
            state_.transcript_open = !state_.transcript_open;
            break;
        case KeyKind::Esc:
            if (state_.transcript_open) {
                state_.transcript_open = false;  // 转录浮层:Esc 先收浮层,不当取消
                break;
            }
            state_.cancelled = true;
            break;
        case KeyKind::CtrlC:
        case KeyKind::CtrlD:
            if (state_.transcript_open) {
                state_.transcript_open = false;  // 同 Esc:收浮层,不当取消
                break;
            }
            state_.cancelled = true;
            break;
        default:
            break;  // 其余键(含 PageUp 之外的编辑键)落空
    }
    return state_;
}

std::vector<std::size_t> SessionPickerCore::VisibleRows() const {
    std::vector<std::size_t> rows;
    if (matches_.empty()) {
        return rows;
    }
    const std::size_t cap = capacity_ == 0 ? 1 : capacity_;
    for (std::size_t i = viewport_top_; i < matches_.size() && i < viewport_top_ + cap; ++i) {
        rows.push_back(i);
    }
    return rows;
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

std::string FormatSessionAgo(long long now_epoch, long long then_epoch) {
    const long long diff = now_epoch - then_epoch;
    if (diff < 0) {
        return std::string(tr("picker.ago.now"));  // 时钟倒拨/未来时间,按"刚刚"算
    }
    if (diff < 60) {
        return std::string(tr("picker.ago.now"));
    }
    if (diff < 3600) {
        return trf("picker.ago.minutes", diff / 60);
    }
    if (diff < 86400) {
        return trf("picker.ago.hours", diff / 3600);
    }
    return trf("picker.ago.days", diff / 86400);
}

int SessionPickerScrollPercent(std::size_t selected, std::size_t total) {
    if (total <= 1) {
        return 0;  // 空表或单条:没有可滚的区间,0%
    }
    const std::size_t index = (std::min)(selected, total - 1);
    return static_cast<int>(index * 100 / (total - 1));
}

SessionPickerFrame BuildSessionTranscriptFrame(const std::string& title_line,
                                               const std::vector<std::string>& excerpt_lines, int width) {
    (void)width;  // 宽度截断归终端层
    SessionPickerFrame frame;
    frame.lines.push_back(title_line);
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    frame.lines.push_back(std::string());
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    if (excerpt_lines.empty()) {
        frame.lines.push_back(std::string("  ") + tr("picker.transcript.empty"));
        frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    } else {
        for (const std::string& line : excerpt_lines) {
            frame.lines.push_back(line.empty() ? std::string(" ") : line);
            frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
        }
    }
    frame.lines.push_back(std::string());
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    frame.lines.push_back(tr("picker.transcript.footer"));
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    return frame;
}

SessionPickerFrame BuildSessionPickerFrame(const SessionPickerCore& core, int width) {
    SessionPickerFrame frame;
    const int usable = width > 4 ? width - 2 : 20;  // 左右各留一格,绝不贴死
    const auto& state = core.state();
    (void)usable;  // 宽度截断归终端层(TruncateUtf8ToDisplayWidth),这里先出整行

    // 标题。
    frame.lines.push_back(tr("picker.title"));
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);

    // 搜索行(焦点时前缀高亮,由终端层上色;这里给结构)。
    const char* search_prefix =
        state.focus == SessionPickerFocus::Search ? "> " : "  ";
    const std::string query =
        state.search.empty() ? std::string(tr("picker.search.placeholder")) : state.search;
    frame.lines.push_back(std::string(search_prefix) + query);
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);

    // Filter/Sort 行。
    const bool filter_focused = state.focus == SessionPickerFocus::Filter;
    const bool sort_focused = state.focus == SessionPickerFocus::Sort;
    const std::string filter_text =
        std::string(filter_focused ? "> " : "  ") + tr("picker.filter.label") + " [" +
        tr(state.scope == SessionPickerScope::Cwd ? "picker.filter.cwd" : "picker.filter.all") + "] " +
        tr(state.scope == SessionPickerScope::Cwd ? "picker.filter.all" : "picker.filter.cwd");
    const std::string sort_text =
        std::string(sort_focused ? "> " : "  ") + tr("picker.sort.label") + " [" +
        tr(state.sort == SessionPickerSort::Updated ? "picker.sort.updated" : "picker.sort.created") +
        "] " +
        tr(state.sort == SessionPickerSort::Updated ? "picker.sort.created" : "picker.sort.updated");
    frame.lines.push_back(filter_text + "   " + sort_text);
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    frame.lines.push_back(std::string());
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);

    // 列表区。
    const auto visible = core.VisibleRows();
    if (visible.empty()) {
        // 两种空态:全无 / 搜空(单子:空列表、搜索无命中各有画面)。
        // 搜索词非空且无命中 = 搜空;连数据都没有 = 全无。
        const bool searched = !state.search.empty();
        frame.lines.push_back(std::string("  ") +
                               trf(searched ? "picker.empty.search" : "picker.empty.none", state.search));
        frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    } else {
        for (const std::size_t index : visible) {
            const SessionPickerEntry& entry = core.matches()[index];
            const bool selected_row = index == core.selected();
            const std::string prefix = selected_row ? "> " : "  ";
            // 相对时间(排序为 Created 时给 created 那份)+ 首句预览。
            const std::string& ago = state.sort == SessionPickerSort::Updated ? entry.updated_ago
                                                                              : entry.created_ago;
            const std::string label =
                !entry.title.empty() ? entry.title : (entry.preview.empty() ? std::string(tr("picker.no_text")) : entry.preview);
            std::string line = prefix + ago + "    " + label;
            if (entry.damaged) {
                line += "  [" + std::string(tr("picker.damaged")) + "]";
            }
            // 舒展行(Ctrl+O):选中行与普通行都多一行 cwd(选中行多了才看
            // 得出"现在指着哪间房");展开详情(Ctrl+E)另算,见下。
            if (state.layout == SessionPickerLayout::Comfortable) {
                frame.lines.push_back(line);
                frame.row_match_index.push_back(index);
                const std::string cwd = entry.cwd.empty() ? std::string(tr("picker.unknown_dir"))
                                                          : entry.cwd;
                frame.lines.push_back(std::string("      ") + cwd);
                // 舒展行第二行也指回同一 match(上色/选中账用同一份)。
                frame.row_match_index.push_back(index);
                continue;
            }
            // 展开详情(Ctrl+E):只摊当前选中行,长标题(截断前的原文)、
            // cwd、id、模型、消息数、创建/更新时间各一行。
            if (state.expanded && selected_row) {
                frame.lines.push_back(line);
                frame.row_match_index.push_back(index);
                const std::string full_title =
                    label.empty() ? std::string(tr("picker.no_text")) : label;
                frame.lines.push_back(std::string("      ") + tr("picker.expand.title") + " " + full_title);
                frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
                frame.lines.push_back(std::string("      ") + tr("picker.expand.cwd") + " " +
                                      (entry.cwd.empty() ? std::string(tr("picker.unknown_dir")) : entry.cwd));
                frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
                frame.lines.push_back(std::string("      ") + tr("picker.expand.id") + " " + entry.id);
                frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
                frame.lines.push_back(
                    std::string("      ") + tr("picker.expand.model") + " " +
                    (entry.model.empty() ? std::string(tr("picker.unknown_model")) : entry.model) + " · " +
                    trf("picker.expand.messages", entry.message_count));
                frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
                frame.lines.push_back(std::string("      ") + tr("picker.expand.created") + " " +
                                      (entry.created_at.empty() ? std::string(tr("picker.unknown_time"))
                                                                : entry.created_at) +
                                      " · " + tr("picker.expand.updated") + " " +
                                      (entry.updated_at.empty() ? std::string(tr("picker.unknown_time"))
                                                                : entry.updated_at));
                frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
                continue;
            }
            frame.lines.push_back(line);
            frame.row_match_index.push_back(index);
        }
    }

    // 底栏:键位 + 序号/总数/百分比。
    frame.lines.push_back(std::string());
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    const std::size_t total = core.matches().size();
    std::string status;
    if (total == 0) {
        status = tr("picker.status.empty");
    } else {
        status = trf("picker.status", core.selected() + 1, total,
                     SessionPickerScrollPercent(core.selected(), total));
    }
    frame.lines.push_back(state.transcript_open ? tr("picker.transcript.footer") : tr("picker.footer"));
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    frame.lines.push_back(status);
    frame.row_match_index.push_back(SessionPickerFrame::kNoMatch);
    return frame;
}

}  // namespace lubancode::cli
