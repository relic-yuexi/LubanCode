#include "cli/todo_render.hpp"

namespace lubancode::cli {

namespace {
constexpr const char* kCheckedBox = "\xE2\x98\x91";    // ☑ U+2611
constexpr const char* kInProgressMark = "\xE2\x96\xB8";  // ▸ U+25B8
constexpr const char* kEmptyBox = "\xE2\x98\x90";        // ☐ U+2610
}  // namespace

std::string FormatTodoList(const std::vector<tools::TodoItem>& items, const Theme& theme) {
    if (items.empty()) {
        return "没有待办。\n";
    }
    const bool plain = theme.reset.empty();
    std::string out;
    for (const auto& item : items) {
        out += "  ";
        switch (item.status) {
            case tools::TodoStatus::Completed:
                out += plain ? "[x] " : (theme.stats + kCheckedBox + " " + theme.reset);
                break;
            case tools::TodoStatus::InProgress:
                out += plain ? "[>] " : (theme.prompt + kInProgressMark + " " + theme.reset);
                break;
            case tools::TodoStatus::Pending:
                out += plain ? "[ ] " : (std::string(kEmptyBox) + " ");
                break;
        }
        out += item.content;
        out += "\n";
    }
    return out;
}

}  // namespace lubancode::cli
