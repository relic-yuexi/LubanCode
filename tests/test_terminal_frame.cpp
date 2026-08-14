#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/terminal_frame.hpp"

using lubancode::cli::InlineFrame;
using lubancode::cli::InlineFrameRow;
using lubancode::cli::LayoutComposerRows;
using lubancode::cli::QueueInlineFrameDiff;
using lubancode::platform::TerminalBatch;

TEST_CASE("composer layout: first row uses prompt width and continuations use full width") {
    const auto layout = LayoutComposerRows({U"abcdefghij"}, 0, 10, 4, 6);
    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].text == U"abcd");
    CHECK(layout.rows[1].text == U"efghij");
    CHECK(layout.cursor_row == 1);
    CHECK(layout.cursor_col == 6);
}

TEST_CASE("composer layout: explicit lines and wide characters keep cursor on the right row") {
    const auto layout = LayoutComposerRows({U"ab中d", U"第二行"}, 1, 2, 4, 5);
    REQUIRE(layout.rows.size() == 4);
    CHECK(layout.rows[0].text == U"ab中");
    CHECK(layout.rows[1].text == U"d");
    CHECK(layout.rows[2].text == U"第二");
    CHECK(layout.rows[3].text == U"行");
    CHECK(layout.cursor_row == 3);
    CHECK(layout.cursor_col == 0);
}

TEST_CASE("inline frame diff: unchanged rows are skipped and all changes share one batch") {
    InlineFrame previous{{InlineFrameRow{2, 8, false, "old"},
                          InlineFrameRow{0, 10, true, "rule"}},
                         5, 0};
    InlineFrame next{{InlineFrameRow{2, 8, false, "new"},
                      InlineFrameRow{0, 10, true, "rule"}},
                     5, 0};

    TerminalBatch batch;
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 7);
    const std::string bytes = batch.Finish();
    CHECK(stats.compared_rows == 2);
    CHECK(stats.changed_rows == 1);
    CHECK(stats.emitted);
    CHECK(bytes.find("old") == std::string::npos);
    CHECK(bytes.find("new") != std::string::npos);
    CHECK(bytes.find("\x1b[8;3H") != std::string::npos);  // origin 7 + row 0, x=2
    CHECK(bytes.find("\x1b[?2026h") == 0);
    CHECK(bytes.ends_with("\x1b[?2026l"));
}

TEST_CASE("inline frame diff: removed rows are cleared without repainting survivors") {
    InlineFrame previous{{InlineFrameRow{2, 8, false, "same"},
                          InlineFrameRow{0, 10, true, "gone"}},
                         2, 0};
    InlineFrame next{{InlineFrameRow{2, 8, false, "same"}}, 2, 0};

    TerminalBatch batch;
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 3);
    const std::string bytes = batch.Finish();
    CHECK(stats.changed_rows == 1);
    CHECK(bytes.find("gone") == std::string::npos);
    CHECK(bytes.find("\x1b[0m\x1b[5;1H\x1b[10X") != std::string::npos);
}

TEST_CASE("inline frame diff: a large frame repaints only the changed row") {
    InlineFrame previous;
    for (int i = 0; i < 1000; ++i) {
        previous.rows.push_back(InlineFrameRow{0, 120, false, "row " + std::to_string(i)});
    }
    previous.cursor_x = 4;
    previous.cursor_row = 999;
    InlineFrame next = previous;
    next.rows[500].text = "changed";

    TerminalBatch batch;
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 0);
    const std::string bytes = batch.Finish();
    CHECK(stats.compared_rows == 1000);
    CHECK(stats.changed_rows == 1);
    CHECK(bytes.find("changed") != std::string::npos);
    CHECK(bytes.find("row 499") == std::string::npos);
    CHECK(bytes.size() < 128);
}

TEST_CASE("terminal batch converts screen-buffer coordinates to viewport coordinates") {
    TerminalBatch batch(/*viewport_x=*/4, /*viewport_y=*/7);
    batch.MoveTo(6, 9);
    const std::string bytes = batch.Finish();
    CHECK(bytes.find("\x1b[3;3H") != std::string::npos);
}
