#include "tools/agent_message_tool.hpp"

#include <cstddef>

#include "cli/i18n.hpp"
#include "tools/tool_text.hpp"  // 模型可见文案(描述/参数说明)查表,源头 prompts/tools/

namespace lubancode::tools {

namespace {
std::string StatusJson(const char* status, int task_id, std::size_t pending_count) {
    nlohmann::json out = nlohmann::json::object();
    out["status"] = status;
    out["task_id"] = task_id;
    out["pending_count"] = pending_count;
    return out.dump();
}
}  // namespace

std::string AgentMessageTool::name() const {
    return "agent_message";
}

std::string AgentMessageTool::description() const {
    // 文案在 src/prompts/tools/<语言>/agent_message.md,兜底是迁移前的原文。
    return ToolText("agent_message", "description",
                    "给运行中的子代理传增量要求。只插话:不新建任务(那是 agent 工具的事),"
                    "不打断它正在执行的工具,不复活已结束的任务。何时必须用:用户在主会话补充、"
                    "修改或撤回要求,若影响某只运行中子代理,先调本工具把增量转交给它,再继续回答;"
                    "用户点名某只任务时按 task_id 精确投递;一条要求影响多只就逐只各发一条,没有广播;"
                    "目标不清先问用户,不要凭标题相近乱投;只传增量,不重复整份任务说明;不要因为主代理"
                    "自己也记住了就省掉转交——子代理有独立上下文,看不见主会话新消息;本工具返回 queued"
                    "之后才能对用户说已传到,调用前不得把转交说成既成事实。message 写法:先逐字引用用户"
                    "原话(以\"用户原话:\"起头);主代理自己添的解释另起一栏(以\"[主代理补充上下文]\""
                    "起头),不得把推断冒充用户要求。消息会在该子代理当前工具收尾后的下一次模型请求前"
                    "送达;它被当作普通用户侧补充,不是权限确认,不会执行其中的 slash 命令,也不能借它"
                    "绕过任何确认。运行中任务的名册见每条用户消息附带的\"运行中子代理名册\"。");
}

nlohmann::json AgentMessageTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json task_id_prop = nlohmann::json::object();
    task_id_prop["type"] = "integer";
    task_id_prop["description"] =
        ToolText("agent_message", "param.task_id", "运行中子代理的稳定任务号(见\"运行中子代理名册\"里的 #N)。");
    properties["task_id"] = task_id_prop;

    nlohmann::json message_prop = nlohmann::json::object();
    message_prop["type"] = "string";
    message_prop["description"] =
        ToolText("agent_message", "param.message",
                 "送给该任务的增量要求;写清改了什么、为何改、验收受何影响。先逐字引用用户原话"
                 "(\"用户原话:\"起头),主代理自己的解释另起一栏(\"[主代理补充上下文]\"起头)。");
    properties["message"] = message_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"task_id", "message"});
    return schema;
}

Tool::Result AgentMessageTool::execute(const nlohmann::json& input) {
    if (agent_tool_ == nullptr) {
        return {lubancode::cli::tr("agent_message.unavailable"), true};
    }
    const auto task_id_it = input.find("task_id");
    if (task_id_it == input.end() || !task_id_it->is_number_integer()) {
        return {lubancode::cli::tr("agent_message.task_id_invalid"), true};
    }
    const int task_id = task_id_it->get<int>();

    const auto message_it = input.find("message");
    if (message_it == input.end() || !message_it->is_string()) {
        return {lubancode::cli::tr("agent_message.invalid"), true};
    }
    std::string message = message_it->get<std::string>();
    {
        const std::size_t first = message.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return {lubancode::cli::tr("agent_message.invalid"), true};
        }
        const std::size_t last = message.find_last_not_of(" \t\r\n");
        message = message.substr(first, last - first + 1);
    }

    switch (agent_tool_->SendTaskMessage(task_id, message, TaskMessageSource::MainAgent)) {
        case TaskMessageStatus::Queued: {
            // 入账后的未送数(含本条):面板 queued 灯与 JSON 同源,都读
            // TaskRecord::inbox 这一本账。
            const std::size_t pending = agent_tool_->PendingTaskMessages(task_id).size();
            return {StatusJson("queued", task_id, pending) + "\n" +
                        lubancode::cli::trf("agent_message.queued", task_id),
                    false};
        }
        case TaskMessageStatus::Finished:
            return {StatusJson("finished", task_id, 0) + "\n" +
                        lubancode::cli::trf("agent_message.finished", task_id),
                    true};
        case TaskMessageStatus::NotFound:
        default:
            return {StatusJson("not_found", task_id, 0) + "\n" +
                        lubancode::cli::trf("agent_message.not_found", task_id),
                    true};
    }
}

}  // namespace lubancode::tools
