// agent 核心循环:user 消息入历史 -> 带工具发请求 -> 流式转发给上层(打字机
// 输出)同时喂给 assembler 攒消息 -> stop_reason 是 tool_use 就把模型要的
// 工具都执行一遍、结果攒成一条 user 消息喂回去 -> 再发请求 -> 如此往复,
// 直到 end_turn,或者达到轮数上限。

#pragma once

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

namespace lubancode::agent {

struct Callbacks {
    // 流式文本增量,打字机效果打印用。
    std::function<void(const std::string& text)> on_text_delta;

    // 模型发起了一次工具调用,还没执行,给上层显示用(比如打印
    // `[工具] read_file {"path":...}`)。
    std::function<void(const std::string& name, const nlohmann::json& input)> on_tool_start;

    // 工具 needs_confirm() 为真时才会调用;返回 true 表示允许执行。
    // 没设这个回调、或者工具本来就不需要确认,都视为允许。
    std::function<bool(const std::string& name, const nlohmann::json& input)> on_tool_confirm;

    // 工具跑完了(不管成功、失败、被拒绝、还是压根没找到这个工具),都会调用一次。
    std::function<void(const std::string& name, const tools::Tool::Result& result)> on_tool_done;
};

class AgentLoop {
public:
    // max_turns:一次 Run() 里最多跟模型来回几趟(每趟一次工具调用算一趟),
    // 超过这个数还没到 end_turn 就报错退出,防止死循环。默认 25。
    AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
              std::string system_prompt, int max_tokens = 4096, int max_turns = 25);

    // 发一轮用户输入。内部可能会跑好几个来回(工具调用),直到模型给出
    // end_turn(或者别的非 tool_use 的 stop_reason)才返回。历史跨多次
    // Run() 调用保留,下一句问话会带着之前的上下文。
    std::expected<void, std::string> Run(const std::string& user_input, const Callbacks& callbacks);

    const std::vector<api::Message>& history() const { return history_; }

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    std::string model_;
    std::string system_prompt_;
    int max_tokens_;
    int max_turns_;
    std::vector<api::Message> history_;

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
