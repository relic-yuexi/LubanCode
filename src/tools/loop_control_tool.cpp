// loop_control 工具实现:scope 校验 + 两档动作。

#include "tools/loop_control_tool.hpp"

namespace lubancode::tools {

std::string LoopControlTool::name() const { return "loop_control"; }

std::string LoopControlTool::description() const {
    return "声明当前定时循环任务的走向:目标已达成用 complete(任务正常收口,"
           "下一拍不再排);需要用户先处理用 pause(保留定义,用户回来可 "
           "resume)。只能操作本拍所属的任务,不能停别的循环。";
}

nlohmann::json LoopControlTool::input_schema() const {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = nlohmann::json::array({"action", "task_id", "reason"});
    nlohmann::json props;
    props["action"]["type"] = "string";
    props["action"]["enum"] = nlohmann::json::array({"complete", "pause"});
    props["task_id"]["type"] = "string";
    props["task_id"]["maxLength"] = 64;
    props["reason"]["type"] = "string";
    props["reason"]["maxLength"] = 600;
    schema["properties"] = props;
    return schema;
}

tools::Tool::Result LoopControlTool::execute(const nlohmann::json& input) {
    using R = tools::Tool::Result;
    const auto fail = [](std::string code, std::string message) {
        R r;
        r.is_error = true;
        r.outcome = "tool_error";
        r.error_code = std::move(code);
        r.content = std::move(message);
        return r;
    };

    // 不在 loop turn:普通 turn 看不见这只工具;真被调到(旁路)明确拒绝,
    // 不装成功。
    if (state_ == nullptr || state_->task_id.empty()) {
        return fail("loop.not_in_loop_turn",
                    "loop_control 只在定时循环的拍执行轮可用;当前会话没有进行中的 loop tick。");
    }

    if (!input.is_object()) {
        return fail("loop.schema_invalid", "input 必须是 object。");
    }
    if (!input.contains("action") || !input.contains("task_id") ||
        !input.contains("reason")) {
        return fail("loop.schema_invalid", "action/task_id/reason 三项都要给。");
    }
    const auto& action = input.at("action");
    const auto& task_id = input.at("task_id");
    const auto& reason = input.at("reason");
    if (!action.is_string() || !task_id.is_string() || !reason.is_string()) {
        return fail("loop.schema_invalid", "action/task_id/reason 都得是字符串。");
    }
    const std::string action_str = action.get<std::string>();
    const std::string task_str = task_id.get<std::string>();
    const std::string reason_str = reason.get<std::string>();
    if (reason_str.empty() || reason_str.size() > 600) {
        return fail("loop.schema_invalid", "reason 要有正文,最多 600 字。");
    }

    // scope:只能操作当前 task,不能停别人的 loop。
    if (task_str != state_->task_id) {
        return fail("loop.scope",
                    "loop_control 只能操作本拍所属的任务(" + state_->task_id +
                        ");要停别的循环请让用户敲 /loop stop。");
    }

    R ok;
    ok.is_error = false;
    ok.outcome = "success";
    if (action_str == "complete") {
        state_->complete_requested = true;
        ok.content = "已声明任务完成(" + state_->task_id + "):" + reason_str +
                     "。本拍答话收完就收口,下一拍不再排。";
        return ok;
    }
    if (action_str == "pause") {
        state_->pause_requested = true;
        ok.content = "已请求暂停(" + state_->task_id + "):" + reason_str +
                     "。任务定义保留,用户 /loop resume 可续。";
        return ok;
    }
    return fail("loop.schema_invalid", "action 只认 complete 或 pause。");
}

}  // namespace lubancode::tools
