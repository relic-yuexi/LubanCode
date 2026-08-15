#include "cli/bottom_chrome.hpp"

#include <functional>

namespace lubancode::cli {

std::string BottomChromeFingerprint(const BottomChromeFrame& frame) {
    std::string value;
    value += "q:";
    for (const auto& row : frame.queue_rows) {
        value += row + "\n";
    }
    value += "c:" + std::to_string(frame.composer_rows) + "\n";
    value += "d:";
    for (const auto& row : frame.agent_dock_rows) {
        value += row + "\n";
    }
    value += "t:";
    for (const auto& row : frame.transient_rows) {
        value += row + "\n";
    }
    value += "#" + std::to_string(frame.selected_task_id) + "\n";
    return value;
}

std::uint64_t BottomChromeRevision(const BottomChromeFrame& frame) {
    return std::hash<std::string>{}(BottomChromeFingerprint(frame));
}

}  // namespace lubancode::cli
