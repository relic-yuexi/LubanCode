// 内置 "todo_write" 工具:多步骤任务的会话级待办清单。语义是整表替换——
// 模型每次调用都传全量清单,简单可靠,不用维护"增量更新"那一套增删改的
// 边界情况(改哪一项、按下标还是按内容匹配……)。
//
// 状态存会话级:main.cpp 造一份 TodoListState(shared_ptr),塞给
// TodoWriteTool 的构造函数,同一份指针也交给 /todos 命令、on_tool_done
// 渲染回调——三处读写的是同一块内存。主表与子表各挂各的实例;子代理的
// todo 由 AgentTool::RunTask 给每只任务换独占实例(私有 todo),不写
// main 的待办清单。
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tools/tool.hpp"

namespace lubancode::tools {

enum class TodoStatus { Pending, InProgress, Completed };

// 最近一次成功写入相对旧清单做了什么。工具 API 仍是整表替换；这份
// 元数据只供 TUI 把“新建计划”和“更新计划”讲清楚。
enum class TodoWriteKind { Created, Updated, Unchanged, Cleared };

struct TodoItem {
    std::string content;
    TodoStatus status = TodoStatus::Pending;
};

// 会话级待办清单状态。items 存当前全表，其余字段只记最近一次改动，
// 供终端区分 create/update 并点亮变化项。
struct TodoListState {
    std::vector<TodoItem> items;
    std::size_t revision = 0;
    TodoWriteKind last_write_kind = TodoWriteKind::Created;
    std::vector<std::size_t> last_changed_indices;
};

// "pending"/"in_progress"/"completed" 三选一,不认得的字符串返回
// std::nullopt(区分大小写,跟模型入参的 JSON schema enum 值保持一致)。
std::optional<TodoStatus> ParseTodoStatus(const std::string& text);

// 反过来,状态转回 schema 里那个字符串——测试、以后别处要用同一份映射时复用。
std::string TodoStatusToString(TodoStatus status);

// 回合收口时若清单里仍有 in_progress 项(P3-4),宿主给模型的提醒行:逐条
// 列出未收口项,提示更新状态、说明缘由或标待确认。全表没有 in_progress
// 返回 nullopt(不打扰)。纯函数,注入时机与通道归调用方(turn_runner 在
// 正常收口的回合把文本经 InjectIncoming 送进双账,模型下一请求看得见)。
std::optional<std::string> BuildUnclosedTodoReminder(const TodoListState& state);

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
