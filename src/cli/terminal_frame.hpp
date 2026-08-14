#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "platform/terminal_batch.hpp"

namespace lubancode::cli {

struct WrappedComposerRow {
    std::u32string text;
    std::size_t logical_row = 0;
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    int display_width = 0;
};

struct WrappedComposerLayout {
    std::vector<WrappedComposerRow> rows;
    std::size_t cursor_row = 0;
    int cursor_col = 0;
};

// 第一物理行容下提示符后的窄区；往后的物理行统一走续行宽度。
WrappedComposerLayout LayoutComposerRows(const std::vector<std::u32string>& logical_lines,
                                         std::size_t cursor_row, std::size_t cursor_col,
                                         int first_width, int continuation_width);

struct InlineFrameRow {
    int x = 0;
    int clear_width = 0;
    bool hard_clear = false;
    std::string text;

    bool operator==(const InlineFrameRow&) const = default;
};

struct InlineFrame {
    std::vector<InlineFrameRow> rows;
    int cursor_x = 0;
    int cursor_row = 0;
};

struct InlineFrameDiffStats {
    std::size_t compared_rows = 0;
    std::size_t changed_rows = 0;
    bool cursor_changed = false;
    bool emitted = false;
};

// 行级双缓冲：没变的行一字不写，变化行整行清后重画；所有命令只进 batch。
InlineFrameDiffStats QueueInlineFrameDiff(platform::TerminalBatch& batch,
                                          const InlineFrame* previous,
                                          const InlineFrame& next, int origin_y);

}  // namespace lubancode::cli
