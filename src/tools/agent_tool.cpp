#include "tools/agent_tool.hpp"

#include <utility>
#include <variant>

#include "agent/loop.hpp"
#include "agent/prompts.hpp"

namespace lubancode::tools {

namespace {

std::string SubAgentPersona() {
    return "你是子代理,专注完成给定任务,完成后直接给出结论,不要寒暄。";
}

}  // namespace

AgentTool::AgentTool(api::Backend& backend, ToolRegistry& sub_registry, std::string cwd, std::string model,
                      int default_max_turns, std::string skills_segment)
    : backend_(backend),
      sub_registry_(sub_registry),
      cwd_(std::move(cwd)),
      model_(std::move(model)),
      default_max_turns_(default_max_turns),
      skills_segment_(std::move(skills_segment)) {}

std::string AgentTool::name() const {
    return "agent";
}

std::string AgentTool::description() const {
    return "把一个独立子任务(如搜索大目录、通读多个文件后总结)委托给子代理执行,子代理有独立上下文,"
           "只返回最终结论,不占用主对话历史。适合「需要大量翻找文件/通读多份文件,但结论只需一小段」"
           "这类任务——把翻找、试错的过程丢给子代理,主对话只留结论,省主上下文。子代理看不见当前对话"
           "历史,prompt 必须自包含,把任务目标、范围、期望的输出形式都写清楚。";
}

nlohmann::json AgentTool::input_schema() const {
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";

    nlohmann::json properties = nlohmann::json::object();

    nlohmann::json prompt_prop = nlohmann::json::object();
    prompt_prop["type"] = "string";
    prompt_prop["description"] =
        "交给子代理的任务描述,必须自包含——子代理看不见主对话历史,任务目标、范围、期望的输出形式都要"
        "写清楚。";
    properties["prompt"] = prompt_prop;

    nlohmann::json max_turns_prop = nlohmann::json::object();
    max_turns_prop["type"] = "integer";
    max_turns_prop["description"] =
        "子代理最多跑几轮(每轮一次工具调用来回算一轮),不填默认 40;传 0 = 不设上限。";
    properties["max_turns"] = max_turns_prop;

    schema["properties"] = properties;
    schema["required"] = nlohmann::json::array({"prompt"});

    return schema;
}

Tool::Result AgentTool::execute(const nlohmann::json& input) {
    if (!input.contains("prompt") || !input.at("prompt").is_string()) {
        return {"缺少必填参数 prompt(字符串)", true};
    }
    const std::string prompt = input.at("prompt").get<std::string>();
    if (prompt.empty()) {
        return {"prompt 不能是空字符串", true};
    }

    int max_turns = default_max_turns_;
    if (const auto it = input.find("max_turns"); it != input.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            return {"max_turns 得是整数", true};
        }
        max_turns = it->get<int>();
        if (max_turns < 0) {
            return {"max_turns 不能是负数(0 = 不设上限)", true};
        }
    }

    // tool_search:延迟工具索引段按"此刻的 loaded 集合"现算(provider 里
    // 闭包着 main.cpp 那份 shared_ptr),拼在子代理系统提示末尾。子代理
    // 运行中途自己 tool_search 挂载了新工具,这段索引不会跟着刷新(系统
    // 提示构造后定死)——但 tools 数组每轮现拼(见 AgentLoop 注释),挂载
    // 照样生效,索引段只是稍显陈旧,无害。
    const std::string system_prompt = agent::WithDeferredToolsIndex(
        agent::BuildSystemPrompt(cwd_, SubAgentPersona(), skills_segment_, prompts_dir_, project_instructions_),
        deferred_index_provider_ ? deferred_index_provider_() : std::string());
    // 每次 execute() 都是全新的、空历史的子代理——没有跨调用的状态,
    // 子代理内部也不做自动 compact(短命任务用不上),AgentLoop 自带的
    // 字符数硬安全网(TrimHistory)照样生效,不用额外处理。
    agent::AgentLoop sub_loop(backend_, sub_registry_, model_, system_prompt, /*max_tokens=*/4096, max_turns);
    if (tool_filter_) {
        sub_loop.SetToolFilter(tool_filter_);
    }

    agent::Callbacks sub_callbacks;
    // on_text_delta 留空:子代理的碎碎念不逐字外放,免得刷屏。
    sub_callbacks.on_tool_start = [this](const std::string& tool_name, const nlohmann::json& tool_input) {
        if (hooks_.on_sub_tool_start) {
            hooks_.on_sub_tool_start(tool_name, tool_input);
        }
    };
    sub_callbacks.on_tool_confirm = hooks_.on_tool_confirm;  // 直接转发,父级没设就是 nullptr(等价放行)
    sub_callbacks.on_usage = [this](const api::Usage& usage) {
        if (hooks_.on_usage) {
            hooks_.on_usage(usage);
        }
    };
    // M9:pre_tool/post_tool 钩子原样转发,子代理内部的工具调用同样受管。
    sub_callbacks.on_pre_tool_hook = hooks_.on_pre_tool_hook;
    sub_callbacks.on_post_tool_hook = hooks_.on_post_tool_hook;

    // 打断信号透传:hooks_.cancel 是 main.cpp 那份 cancel_flag 的地址,
    // 没设(nullptr)时 AgentLoop::Run 内部判断照旧短路成"没被打断",
    // 行为跟原来一样;设了之后子代理的工具循环才会真的看见 ESC/Ctrl+C。
    const auto result = sub_loop.Run(prompt, sub_callbacks, hooks_.cancel);
    if (!result.has_value()) {
        return {"子代理执行失败: " + result.error(), true};
    }

    const auto& history = sub_loop.History();
    if (history.empty()) {
        return {"子代理没有给出任何结论", true};
    }
    const auto& last_message = history.back();
    std::string text;
    for (const auto& block : last_message.content) {
        if (std::holds_alternative<api::TextBlock>(block)) {
            text += std::get<api::TextBlock>(block).text;
        }
    }
    if (text.empty()) {
        return {"子代理没有给出文本结论", true};
    }
    return {text, false};
}

}  // namespace lubancode::tools
