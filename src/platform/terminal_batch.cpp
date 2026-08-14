#include "platform/terminal_batch.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace lubancode::platform {

namespace {

constexpr std::string_view kSyncOutputBegin = "\x1b[?2026h";
constexpr std::string_view kSyncOutputEnd = "\x1b[?2026l";

}  // namespace

TerminalBatch::TerminalBatch(int viewport_x, int viewport_y, bool synchronized_output)
    : synchronized_output_(synchronized_output),
      viewport_x_((std::max)(0, viewport_x)),
      viewport_y_((std::max)(0, viewport_y)) {
    if (synchronized_output_) {
        bytes_.append(kSyncOutputBegin);
    }
}

void TerminalBatch::EnsureOpen() const {
    if (finished_) {
        throw std::logic_error("TerminalBatch is already finished");
    }
}

void TerminalBatch::MoveTo(int x, int y) {
    EnsureOpen();
    const int viewport_x = (std::max)(0, x - viewport_x_);
    const int viewport_y = (std::max)(0, y - viewport_y_);
    bytes_ += "\x1b[" + std::to_string(viewport_y + 1) + ";" +
              std::to_string(viewport_x + 1) + "H";
    has_commands_ = true;
}

void TerminalBatch::EraseCharacters(int count) {
    EnsureOpen();
    if (count <= 0) {
        return;
    }
    bytes_ += "\x1b[" + std::to_string(count) + "X";
    has_commands_ = true;
}

void TerminalBatch::ClearRowFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    MoveTo(x, y);
    EraseCharacters(count);
}

void TerminalBatch::ClearRowHardFrom(int x, int y, int count) {
    if (count <= 0) {
        return;
    }
    EnsureOpen();
    bytes_ += "\x1b[0m";
    MoveTo(x, y);
    EraseCharacters(count);
}

void TerminalBatch::Write(std::string_view text) {
    EnsureOpen();
    if (text.empty()) {
        return;
    }
    bytes_.append(text);
    has_commands_ = true;
}

void TerminalBatch::HideCursor() {
    Write("\x1b[?25l");
}

void TerminalBatch::ShowCursor() {
    Write("\x1b[?25h");
}

const std::string& TerminalBatch::Finish() {
    if (!finished_) {
        if (synchronized_output_) {
            bytes_.append(kSyncOutputEnd);
        }
        finished_ = true;
    }
    return bytes_;
}

void TerminalBatch::Flush() {
    const std::string& bytes = Finish();
    if (!has_commands_) {
        return;
    }
    std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    std::cout.flush();
}

}  // namespace lubancode::platform
