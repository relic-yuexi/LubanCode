// FormatTodoList:纯函数,把 tools::TodoItem 列表画成人看的文本——三种
// 状态各自的符号/着色对不对,plain 主题(不着色)下退化成 [x]/[>]/[ ],
// 空清单打"没有待办。"。

#include <doctest/doctest.h>

#include "cli/theme.hpp"
#include "cli/todo_render.hpp"

using lubancode::cli::BuiltinTheme;
using lubancode::cli::FormatTodoList;
using lubancode::tools::TodoItem;
using lubancode::tools::TodoStatus;

TEST_CASE("FormatTodoList: 空清单打\"没有待办。\"") {
    const auto theme = BuiltinTheme("dark");
    const std::string out = FormatTodoList({}, theme);
    CHECK(out == "没有待办。\n");
}

TEST_CASE("FormatTodoList: dark 主题下,三种状态各自的符号都出现") {
    const auto theme = BuiltinTheme("dark");
    const std::vector<TodoItem> items = {
        TodoItem{"已完成的事", TodoStatus::Completed},
        TodoItem{"进行中的事", TodoStatus::InProgress},
        TodoItem{"待办的事", TodoStatus::Pending},
    };
    const std::string out = FormatTodoList(items, theme);

    // ☑ U+2611, ▸ U+25B8, ☐ U+2610
    CHECK(out.find("\xE2\x98\x91") != std::string::npos);
    CHECK(out.find("\xE2\x96\xB8") != std::string::npos);
    CHECK(out.find("\xE2\x98\x90") != std::string::npos);
    CHECK(out.find("已完成的事") != std::string::npos);
    CHECK(out.find("进行中的事") != std::string::npos);
    CHECK(out.find("待办的事") != std::string::npos);
    // 完成项、进行中项各自套了主题颜色(dark 主题非空),reset 也该出现。
    CHECK(out.find(theme.stats) != std::string::npos);
    CHECK(out.find(theme.prompt) != std::string::npos);
    CHECK(out.find(theme.reset) != std::string::npos);
}

TEST_CASE("FormatTodoList: 每一项前面缩进两格") {
    const auto theme = BuiltinTheme("plain");
    const std::vector<TodoItem> items = {TodoItem{"单独一项", TodoStatus::Pending}};
    const std::string out = FormatTodoList(items, theme);
    CHECK(out.rfind("  [ ] 单独一项\n") == 0);
}

TEST_CASE("FormatTodoList: plain 主题退化成 [x]/[>]/[ ],不带任何颜色转义") {
    const auto theme = BuiltinTheme("plain");
    const std::vector<TodoItem> items = {
        TodoItem{"完成项", TodoStatus::Completed},
        TodoItem{"进行中项", TodoStatus::InProgress},
        TodoItem{"待办项", TodoStatus::Pending},
    };
    const std::string out = FormatTodoList(items, theme);

    CHECK(out.find("[x] 完成项") != std::string::npos);
    CHECK(out.find("[>] 进行中项") != std::string::npos);
    CHECK(out.find("[ ] 待办项") != std::string::npos);
    // 不该出现任何 box-drawing/emoji 符号残留
    CHECK(out.find("\xE2\x98\x91") == std::string::npos);
    CHECK(out.find("\xE2\x96\xB8") == std::string::npos);
    CHECK(out.find("\xE2\x98\x90") == std::string::npos);
}

TEST_CASE("FormatTodoList: 多项时逐行输出,顺序跟输入一致") {
    const auto theme = BuiltinTheme("plain");
    const std::vector<TodoItem> items = {
        TodoItem{"第一项", TodoStatus::Completed},
        TodoItem{"第二项", TodoStatus::InProgress},
        TodoItem{"第三项", TodoStatus::Pending},
    };
    const std::string out = FormatTodoList(items, theme);
    const std::size_t pos1 = out.find("第一项");
    const std::size_t pos2 = out.find("第二项");
    const std::size_t pos3 = out.find("第三项");
    REQUIRE(pos1 != std::string::npos);
    REQUIRE(pos2 != std::string::npos);
    REQUIRE(pos3 != std::string::npos);
    CHECK(pos1 < pos2);
    CHECK(pos2 < pos3);
}

TEST_CASE("FormatTodoList: 更新项点亮正文,其余项不额外着色") {
    const auto theme = BuiltinTheme("dark");
    const std::vector<TodoItem> items = {
        TodoItem{"刚完成", TodoStatus::Completed},
        TodoItem{"没有变化", TodoStatus::Pending},
    };
    const std::string out = FormatTodoList(items, theme, {0});
    CHECK(out.find(theme.prompt + std::string("刚完成") + theme.reset) != std::string::npos);
    CHECK(out.find(theme.prompt + std::string("没有变化") + theme.reset) == std::string::npos);

    const auto plain = BuiltinTheme("plain");
    CHECK(FormatTodoList(items, plain, {0}).find('\x1b') == std::string::npos);
}
