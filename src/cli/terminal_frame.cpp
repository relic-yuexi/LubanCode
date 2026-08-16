#include "cli/terminal_frame.hpp"

#include <algorithm>

#include "cli/line_editor.hpp"

namespace lubancode::cli {

WrappedComposerLayout LayoutComposerRows(const std::vector<std::u32string>& logical_lines,
                                         std::size_t cursor_row, std::size_t cursor_col,
                                         int first_width, int continuation_width) {
    const std::vector<std::u32string> fallback{std::u32string()};
    const auto& lines = logical_lines.empty() ? fallback : logical_lines;
    const std::size_t safe_cursor_row = (std::min)(cursor_row, lines.size() - 1);
    const std::size_t safe_cursor_col = (std::min)(cursor_col, lines[safe_cursor_row].size());

    WrappedComposerLayout layout;
    std::vector<std::pair<std::size_t, std::size_t>> row_ranges(lines.size());
    for (std::size_t logical = 0; logical < lines.size(); ++logical) {
        const std::u32string& line = lines[logical];
        const std::size_t first_row = layout.rows.size();
        if (line.empty()) {
            layout.rows.push_back(WrappedComposerRow{{}, logical, 0, 0, 0});
        } else {
            std::size_t begin = 0;
            while (begin < line.size()) {
                const bool first_physical_row = layout.rows.empty();
                const int capacity = (std::max)(1, first_physical_row ? first_width : continuation_width);
                std::size_t end = begin;
                int width = 0;
                while (end < line.size()) {
                    const int char_width = CharDisplayWidth(line[end]);
                    if (end > begin && width + char_width > capacity) {
                        break;
                    }
                    width += char_width;
                    ++end;
                    if (width >= capacity) {
                        break;
                    }
                }
                layout.rows.push_back(
                    WrappedComposerRow{line.substr(begin, end - begin), logical, begin, end, width});
                begin = end;
            }
        }
        row_ranges[logical] = {first_row, layout.rows.size()};
    }

    const auto [first_row, row_end] = row_ranges[safe_cursor_row];
    for (std::size_t physical = first_row; physical < row_end; ++physical) {
        const WrappedComposerRow& row = layout.rows[physical];
        const bool last = physical + 1 == row_end;
        if (safe_cursor_col < row.source_end || last) {
            layout.cursor_row = physical;
            const std::size_t relative = safe_cursor_col >= row.source_begin
                                             ? safe_cursor_col - row.source_begin
                                             : 0;
            layout.cursor_col = static_cast<int>(DisplayWidth(row.text.substr(0, relative)));
            break;
        }
    }
    return layout;
}

InlineFrameDiffStats QueueInlineFrameDiff(platform::TerminalBatch& batch,
                                          const InlineFrame* previous,
                                          const InlineFrame& next, int origin_y) {
    InlineFrameDiffStats stats;
    const std::size_t old_size = previous == nullptr ? 0 : previous->rows.size();
    stats.compared_rows = (std::max)(old_size, next.rows.size());

    for (std::size_t i = 0; i < stats.compared_rows; ++i) {
        const InlineFrameRow* old_row = i < old_size ? &previous->rows[i] : nullptr;
        const InlineFrameRow* new_row = i < next.rows.size() ? &next.rows[i] : nullptr;
        if (old_row != nullptr && new_row != nullptr && *old_row == *new_row) {
            continue;
        }

        ++stats.changed_rows;
        int clear_x = new_row != nullptr ? new_row->x : old_row->x;
        int clear_end = clear_x + (new_row != nullptr ? new_row->clear_width : old_row->clear_width);
        bool hard_clear = (new_row != nullptr && new_row->hard_clear) ||
                          (old_row != nullptr && old_row->hard_clear);
        if (old_row != nullptr) {
            clear_x = (std::min)(clear_x, old_row->x);
            clear_end = (std::max)(clear_end, old_row->x + old_row->clear_width);
        }
        if (hard_clear) {
            batch.ClearRowHardFrom(clear_x, origin_y + static_cast<int>(i), clear_end - clear_x);
        } else {
            batch.ClearRowFrom(clear_x, origin_y + static_cast<int>(i), clear_end - clear_x);
        }
        if (new_row != nullptr && !new_row->text.empty()) {
            batch.MoveTo(new_row->x, origin_y + static_cast<int>(i));
            batch.Write(new_row->text);
        }
    }

    stats.cursor_changed = previous == nullptr || previous->cursor_x != next.cursor_x ||
                           previous->cursor_row != next.cursor_row;
    stats.emitted = stats.changed_rows > 0 || stats.cursor_changed;
    if (stats.emitted) {
        batch.MoveTo(next.cursor_x, origin_y + next.cursor_row);
    }
    return stats;
}

ViewportRevealPlan ComputeViewportReveal(int buffer_height, int viewport_y, int viewport_height, int top_row,
                                         int rows_needed) {
    ViewportRevealPlan plan;
    if (rows_needed <= 0 || buffer_height <= 0) {
        return plan;
    }
    const int window_rows = viewport_height > 0 ? viewport_height : buffer_height;
    int viewport_bottom = viewport_y + window_rows - 1;
    if (viewport_bottom > buffer_height - 1) {
        viewport_bottom = buffer_height - 1;  // 防御:窗口报得比缓冲还长,按缓冲算
    }
    const int needed_bottom = top_row + rows_needed - 1;
    int overflow = needed_bottom - viewport_bottom;
    if (overflow <= 0) {
        return plan;  // 已经在可视区里,一笔都不动
    }
    const int room_below = buffer_height - 1 - viewport_bottom;
    plan.pan_rows = room_below > 0 ? (std::min)(overflow, room_below) : 0;
    plan.scroll_rows = overflow - plan.pan_rows;
    return plan;
}

}  // namespace lubancode::cli
