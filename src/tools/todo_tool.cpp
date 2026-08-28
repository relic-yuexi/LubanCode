#include "tools/todo_tool.hpp"

#include <algorithm>

#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

std::optional<TodoStatus> ParseTodoStatus(const std::string& text) {
    if (text == "pending") {
        return TodoStatus::Pending;
    }
    if (text == "in_progress") {
        return TodoStatus::InProgress;
    }
    if (text == "completed") {
        return TodoStatus::Completed;
    }
    return std::nullopt;
}

std::string TodoStatusToString(TodoStatus status) {
    switch (status) {
        case TodoStatus::Pending:
            return "pending";
        case TodoStatus::InProgress:
            return "in_progress";
        case TodoStatus::Completed:
            return "completed";
    }
    return "pending";
}

std::optional<std::string> BuildUnclosedTodoReminder(const TodoListState& state) {
    std::string items;
    for (const TodoItem& item : state.items) {
        if (item.status != TodoStatus::InProgress) {
            continue;
        }
        items += "  - ";
        items += item.content.empty() ? "(空条目)" : item.content;
        items += "\n";
    }
    if (items.empty()) {
        return std::nullopt;
    }
    // 与 loop.cpp 的步数将尽/预算催办同一口吻:明写"非用户输入",不伪装人话;
    // 只点名收账动作,不替模型决定各项的归宿。
    std::string text =
        "[系统提醒,非用户输入] 本回合已收口,但待办清单里还有标着 in_progress 的项没有收账:\n";
    text += items;
    text +=
        "下一次调用 todo_write 时请处理这些项:已做完的改成 completed;还在做的改回 pending,"
        "并在回复里说明为什么没做完;需要用户拍板的,说明后把条目内容标注\"待确认\"。"
        "不要不动声色地把 in_progress 留过夜。";
    return text;
}

std::string TodoWriteTool::name() const {
    return "todo_write";
}

std::string TodoWriteTool::description() const {
    // 文案在 src/prompts/tools/<语言>/todo_write.md,兜底是迁移前的原文。
    return ToolText("todo_write", "description",
                    "维护本次会话的待办清单,整表替换(每次调用都要传完整的清单,不是增量更新——"
                    "漏掉的项这次就没了)。多步骤任务开工前先列一份清单,每完成一步就把对应项的 "
                    "status 改成 completed 再整表传一次,让用户能看到进度。items 传空数组表示清空清单。");
}

nlohmann::json TodoWriteTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json item_schema = nlohmann::json::object();
    item_schema["type"] = "object";
    nlohmann::json item_properties = nlohmann::json::object();

    nlohmann::json content_prop = nlohmann::json::object();
    content_prop["type"] = "string";
    content_prop["description"] = ToolText("todo_write", "param.items.content", "这一项要做的事,一句话说清楚");
    item_properties["content"] = content_prop;

    nlohmann::json status_prop = nlohmann::json::object();
    status_prop["type"] = "string";
    status_prop["enum"] = nlohmann::json::array({"pending", "in_progress", "completed"});
    status_prop["description"] = ToolText("todo_write", "param.items.status", "这一项当前的状态");
    item_properties["status"] = status_prop;

    item_schema["properties"] = item_properties;
    item_schema["required"] = nlohmann::json::array({"content", "status"});

    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json items_prop = nlohmann::json::object();
    items_prop["type"] = "array";
    items_prop["description"] =
        ToolText("todo_write", "param.items", "完整的待办清单,整表替换(不是增量更新,每次都传全量列表)");
    items_prop["items"] = item_schema;
    properties["items"] = items_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"items"});

    return schema;
}

Tool::Result TodoWriteTool::execute(const nlohmann::json& input) {
    if (!input.contains("items") || !input.at("items").is_array()) {
        return {"缺少必填参数 items(数组)", true};
    }
    const auto& items_json = input.at("items");

    std::vector<TodoItem> parsed;
    parsed.reserve(items_json.size());
    for (std::size_t i = 0; i < items_json.size(); ++i) {
        const auto& item = items_json[i];
        if (!item.is_object() || !item.contains("content") || !item.at("content").is_string() ||
            !item.contains("status") || !item.at("status").is_string()) {
            return {"第 " + std::to_string(i + 1) + " 项格式不对,要有 content(字符串)和 status(字符串)", true};
        }
        const std::string content = item.at("content").get<std::string>();
        if (content.empty()) {
            return {"第 " + std::to_string(i + 1) + " 项 content 不能是空字符串", true};
        }
        const std::string status_str = item.at("status").get<std::string>();
        const std::optional<TodoStatus> status = ParseTodoStatus(status_str);
        if (!status.has_value()) {
            return {"第 " + std::to_string(i + 1) + " 项 status 不认得: " + status_str +
                         "(只认 pending/in_progress/completed)",
                    true};
        }
        parsed.push_back(TodoItem{content, *status});
    }

    // 校验全过了才真的替换——半路校验失败绝不能留一份"改了一半"的清单。
    // 顺手记下哪些位置真变了。工具协议仍是全量覆盖；这点元数据只让
    // 终端能把首次创建、后续更新和无变化三种结果分开画。
    const bool had_previous_write = state_->revision > 0;
    std::vector<std::size_t> changed;
    const std::size_t compared = (std::max)(state_->items.size(), parsed.size());
    changed.reserve(compared);
    for (std::size_t i = 0; i < compared; ++i) {
        if (i >= state_->items.size() || i >= parsed.size() || state_->items[i].content != parsed[i].content ||
            state_->items[i].status != parsed[i].status) {
            changed.push_back(i);
        }
    }
    state_->items = std::move(parsed);
    state_->last_changed_indices = std::move(changed);
    if (!had_previous_write) {
        state_->last_write_kind = TodoWriteKind::Created;
    } else if (state_->items.empty()) {
        state_->last_write_kind = TodoWriteKind::Cleared;
    } else if (state_->last_changed_indices.empty()) {
        state_->last_write_kind = TodoWriteKind::Unchanged;
    } else {
        state_->last_write_kind = TodoWriteKind::Updated;
    }
    ++state_->revision;

    std::size_t completed = 0;
    for (const auto& it : state_->items) {
        if (it.status == TodoStatus::Completed) {
            ++completed;
        }
    }
    std::string action;
    switch (state_->last_write_kind) {
        case TodoWriteKind::Created:
            action = "已创建";
            break;
        case TodoWriteKind::Updated:
            action = "已更新 " + std::to_string(state_->last_changed_indices.size()) + " 项";
            break;
        case TodoWriteKind::Unchanged:
            action = "清单没有变化";
            break;
        case TodoWriteKind::Cleared:
            action = "已清空";
            break;
    }
    return {action + ",共 " + std::to_string(state_->items.size()) + " 项," + std::to_string(completed) +
                " 项已完成",
            false};
}

}  // namespace lubancode::tools
