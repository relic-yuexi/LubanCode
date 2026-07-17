#include "tools/todo_tool.hpp"

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

std::string TodoWriteTool::name() const {
    return "todo_write";
}

std::string TodoWriteTool::description() const {
    return "维护本次会话的待办清单,整表替换(每次调用都要传完整的清单,不是增量更新——"
           "漏掉的项这次就没了)。多步骤任务开工前先列一份清单,每完成一步就把对应项的 "
           "status 改成 completed 再整表传一次,让用户能看到进度。items 传空数组表示清空清单。";
}

nlohmann::json TodoWriteTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json item_schema = nlohmann::json::object();
    item_schema["type"] = "object";
    nlohmann::json item_properties = nlohmann::json::object();

    nlohmann::json content_prop = nlohmann::json::object();
    content_prop["type"] = "string";
    content_prop["description"] = "这一项要做的事,一句话说清楚";
    item_properties["content"] = content_prop;

    nlohmann::json status_prop = nlohmann::json::object();
    status_prop["type"] = "string";
    status_prop["enum"] = nlohmann::json::array({"pending", "in_progress", "completed"});
    status_prop["description"] = "这一项当前的状态";
    item_properties["status"] = status_prop;

    item_schema["properties"] = item_properties;
    item_schema["required"] = nlohmann::json::array({"content", "status"});

    nlohmann::json properties = nlohmann::json::object();
    nlohmann::json items_prop = nlohmann::json::object();
    items_prop["type"] = "array";
    items_prop["description"] = "完整的待办清单,整表替换(不是增量更新,每次都传全量列表)";
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
    state_->items = std::move(parsed);

    std::size_t completed = 0;
    for (const auto& it : state_->items) {
        if (it.status == TodoStatus::Completed) {
            ++completed;
        }
    }
    return {"已更新,共 " + std::to_string(state_->items.size()) + " 项," + std::to_string(completed) + " 项已完成",
            false};
}

}  // namespace lubancode::tools
