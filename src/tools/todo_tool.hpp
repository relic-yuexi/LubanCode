// 内置 "todo_write" 工具:多步骤任务的会话级待办清单。语义是整表替换——
// 模型每次调用都传全量清单,简单可靠,不用维护"增量更新"那一套增删改的
// 边界情况(改哪一项、按下标还是按内容匹配……)。
//
// 状态存会话级:main.cpp 造一份 TodoListState(shared_ptr),塞给
// TodoWriteTool 的构造函数,同一份指针也交给 /todos 命令、on_tool_done
// 渲染回调——三处读写的是同一块内存。只挂主注册表(main.cpp 的
// registry),不挂子代理的 sub_registry:子代理是短命的一次性跑腿,不该
// 让它乱写主会话的待办清单。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

enum class TodoStatus { Pending, InProgress, Completed };

struct TodoItem {
    std::string content;
    TodoStatus status = TodoStatus::Pending;
};

// 会话级待办清单状态。田字段只有一个 vector——整表替换语义下不需要更多。
struct TodoListState {
    std::vector<TodoItem> items;
};

// "pending"/"in_progress"/"completed" 三选一,不认得的字符串返回
// std::nullopt(区分大小写,跟模型入参的 JSON schema enum 值保持一致)。
std::optional<TodoStatus> ParseTodoStatus(const std::string& text);

// 反过来,状态转回 schema 里那个字符串——测试、以后别处要用同一份映射时复用。
std::string TodoStatusToString(TodoStatus status);

class TodoWriteTool : public Tool {
public:
    explicit TodoWriteTool(std::shared_ptr<TodoListState> state) : state_(std::move(state)) {}

    std::string name() const override;
    std::string description() const override;
    nlohmann::json input_schema() const override;
    bool needs_confirm() const override { return false; }
    Result execute(const nlohmann::json& input) override;

private:
    std::shared_ptr<TodoListState> state_;
};

}  // namespace lubancode::tools
