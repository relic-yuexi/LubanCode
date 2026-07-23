#include "cli/todo_render.hpp"

#include <algorithm>

#include "cli/i18n.hpp"

namespace lubancode::cli {

namespace {
constexpr const char* kCheckedBox = "\xE2\x98\x91";    // ☑ U+2611
constexpr const char* kInProgressMark = "\xE2\x96\xB8";  // ▸ U+25B8
constexpr const char* kEmptyBox = "\xE2\x98\x90";        // ☐ U+2610
}  // namespace

std::string FormatTodoList(const std::vector<tools::TodoItem>& items, const Theme& theme,
                           const std::vector<std::size_t>& highlighted_indices) {
    if (items.empty()) {
        return tr("todo.empty") + "\n";
    }
    const bool plain = theme.reset.empty();
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
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
        const bool highlighted = !plain &&
                                 std::find(highlighted_indices.begin(), highlighted_indices.end(), i) !=
                                     highlighted_indices.end();
        out += highlighted ? (theme.prompt + item.content + theme.reset) : item.content;
        out += "\n";
    }
    return out;
}

}  // namespace lubancode::cli
