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
        // 排版批 6:三态标色走语义档——完成 table_pass(绿)、进行中
        // spinner(青,活动色)、待办不包色;高亮行(刚更新的那一拍)走
        // confirm 焦点色,与菜单焦点同一档。plain 全空串,退 [x]/[>]/[ ]。
        switch (item.status) {
            case tools::TodoStatus::Completed:
                out += plain ? "[x] " : (theme.table_pass + kCheckedBox + " " + theme.reset);
                break;
            case tools::TodoStatus::InProgress:
                out += plain ? "[>] " : (theme.spinner + kInProgressMark + " " + theme.reset);
                break;
            case tools::TodoStatus::Pending:
                out += plain ? "[ ] " : (std::string(kEmptyBox) + " ");
                break;
        }
        const bool highlighted = !plain &&
                                 std::find(highlighted_indices.begin(), highlighted_indices.end(), i) !=
                                     highlighted_indices.end();
        out += highlighted ? (theme.confirm + item.content + theme.reset) : item.content;
        out += "\n";
    }
    return out;
}

}  // namespace lubancode::cli
