// todo_write:整表替换语义(每次调用都是完整覆盖,不是增量更新)——测
// 首次写入、二次覆盖、空数组清空、status 枚举校验(非法值报 is_error 且
// 不改动原有状态)、确认文案里的计数是否对得上。

#include <doctest/doctest.h>

#include <memory>

#include "tools/todo_tool.hpp"

using lubancode::tools::ParseTodoStatus;
using lubancode::tools::TodoItem;
using lubancode::tools::TodoListState;
using lubancode::tools::TodoStatus;
using lubancode::tools::TodoStatusToString;
using lubancode::tools::TodoWriteKind;
using lubancode::tools::TodoWriteTool;

TEST_CASE("ParseTodoStatus: 三个合法值 + 不认得的字符串") {
    CHECK(ParseTodoStatus("pending") == TodoStatus::Pending);
    CHECK(ParseTodoStatus("in_progress") == TodoStatus::InProgress);
    CHECK(ParseTodoStatus("completed") == TodoStatus::Completed);
    CHECK_FALSE(ParseTodoStatus("Pending").has_value());  // 区分大小写
    CHECK_FALSE(ParseTodoStatus("done").has_value());
    CHECK_FALSE(ParseTodoStatus("").has_value());
}

TEST_CASE("TodoStatusToString: 跟 ParseTodoStatus 互为反函数") {
    CHECK(TodoStatusToString(TodoStatus::Pending) == "pending");
    CHECK(TodoStatusToString(TodoStatus::InProgress) == "in_progress");
    CHECK(TodoStatusToString(TodoStatus::Completed) == "completed");
}

TEST_CASE("todo_write: 首次写入,整表落进 state") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json input;
    input["items"] = nlohmann::json::array({
        {{"content", "数 cpp 文件"}, {"status", "pending"}},
        {{"content", "读 vcpkg.json"}, {"status", "pending"}},
    });
    const auto result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    REQUIRE(state->items.size() == 2);
    CHECK(state->items[0].content == "数 cpp 文件");
    CHECK(state->items[0].status == TodoStatus::Pending);
    CHECK(state->items[1].content == "读 vcpkg.json");
    CHECK(state->revision == 1);
    CHECK(state->last_write_kind == TodoWriteKind::Created);
    CHECK(state->last_changed_indices == std::vector<std::size_t>({0, 1}));
    CHECK(result.content.find("已创建") != std::string::npos);
}

TEST_CASE("todo_write: 二次调用整表覆盖,不是增量合并") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json first;
    first["items"] = nlohmann::json::array({
        {{"content", "步骤一"}, {"status", "pending"}},
        {{"content", "步骤二"}, {"status", "pending"}},
        {{"content", "步骤三"}, {"status", "pending"}},
    });
    tool.execute(first);
    REQUIRE(state->items.size() == 3);

    // 第二次只传两项,且状态改了——旧的第三项应该整个消失,不是"没提到
    // 的项保持原样"。
    nlohmann::json second;
    second["items"] = nlohmann::json::array({
        {{"content", "步骤一"}, {"status", "completed"}},
        {{"content", "步骤二"}, {"status", "in_progress"}},
    });
    const auto result = tool.execute(second);

    CHECK_FALSE(result.is_error);
    REQUIRE(state->items.size() == 2);
    CHECK(state->items[0].status == TodoStatus::Completed);
    CHECK(state->items[1].status == TodoStatus::InProgress);
    CHECK(state->revision == 2);
    CHECK(state->last_write_kind == TodoWriteKind::Updated);
    CHECK(state->last_changed_indices == std::vector<std::size_t>({0, 1, 2}));
    CHECK(result.content.find("已更新 3 项") != std::string::npos);
}

TEST_CASE("todo_write: 重复写入同一份清单,标成无变化") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);
    const nlohmann::json input = {
        {"items", nlohmann::json::array({{{"content", "步骤一"}, {"status", "in_progress"}}})},
    };
    tool.execute(input);
    const auto result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(state->revision == 2);
    CHECK(state->last_write_kind == TodoWriteKind::Unchanged);
    CHECK(state->last_changed_indices.empty());
    CHECK(result.content.find("没有变化") != std::string::npos);
}

TEST_CASE("todo_write: 空数组清空清单") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json first;
    first["items"] = nlohmann::json::array({{{"content", "某件事"}, {"status", "pending"}}});
    tool.execute(first);
    REQUIRE(state->items.size() == 1);

    nlohmann::json empty;
    empty["items"] = nlohmann::json::array();
    const auto result = tool.execute(empty);

    CHECK_FALSE(result.is_error);
    CHECK(state->items.empty());
    CHECK(state->revision == 2);
    CHECK(state->last_write_kind == TodoWriteKind::Cleared);
    CHECK(result.content.find("0 项") != std::string::npos);
    CHECK(result.content.find("已清空") != std::string::npos);
}

TEST_CASE("todo_write: status 传非法值,报 is_error,且不改动原有状态") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json first;
    first["items"] = nlohmann::json::array({{{"content", "原有项"}, {"status", "pending"}}});
    tool.execute(first);
    REQUIRE(state->items.size() == 1);

    nlohmann::json bad;
    bad["items"] = nlohmann::json::array({
        {{"content", "正常项"}, {"status", "pending"}},
        {{"content", "坏项"}, {"status", "done"}},  // 不认得的枚举值
    });
    const auto result = tool.execute(bad);

    CHECK(result.is_error);
    CHECK(result.content.find("done") != std::string::npos);
    // 半路校验失败,原有清单原封不动,不能留一份"改了一半"的状态。
    REQUIRE(state->items.size() == 1);
    CHECK(state->items[0].content == "原有项");
    CHECK(state->revision == 1);
}

TEST_CASE("todo_write: 缺 items 字段,或 items 不是数组,报 is_error") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json missing = nlohmann::json::object();
    const auto r1 = tool.execute(missing);
    CHECK(r1.is_error);

    nlohmann::json wrong_type;
    wrong_type["items"] = "不是数组";
    const auto r2 = tool.execute(wrong_type);
    CHECK(r2.is_error);
}

TEST_CASE("todo_write: content 是空字符串,报 is_error") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json input;
    input["items"] = nlohmann::json::array({{{"content", ""}, {"status", "pending"}}});
    const auto result = tool.execute(input);
    CHECK(result.is_error);
}

TEST_CASE("todo_write: 确认文案里的总数、已完成数正确") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);

    nlohmann::json input;
    input["items"] = nlohmann::json::array({
        {{"content", "一"}, {"status", "completed"}},
        {{"content", "二"}, {"status", "completed"}},
        {{"content", "三"}, {"status", "in_progress"}},
        {{"content", "四"}, {"status", "pending"}},
    });
    const auto result = tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("共 4 项") != std::string::npos);
    CHECK(result.content.find("2 项已完成") != std::string::npos);
}

TEST_CASE("todo_write: name/needs_confirm/schema 基本约定") {
    auto state = std::make_shared<TodoListState>();
    TodoWriteTool tool(state);
    CHECK(tool.name() == "todo_write");
    CHECK_FALSE(tool.needs_confirm());
    const nlohmann::json schema = tool.input_schema();
    CHECK(schema["type"] == "object");
    CHECK(schema["properties"].contains("items"));
}
