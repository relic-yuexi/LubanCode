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

#include "agent/context.hpp"
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

    // 每一次到模型的独立请求结束时(MessageDone 到达那一刻)都会调用一次,
    // 把这一次的 usage 报出来。一次 Run() 内部可能因为工具调用来回好几趟,
    // 也就是好几次独立请求——这个回调按请求粒度触发,不是按 Run() 粒度,
    // 上层(main.cpp)自己决定要不要跨请求累计。可选;不设就跳过,不影响
    // 其余行为。
    std::function<void(const api::Usage& usage)> on_usage;
};

class AgentLoop {
public:
    // max_turns:一次 Run() 里最多跟模型来回几趟(每趟一次工具调用算一趟),
    // 超过这个数还没到 end_turn 就报错退出,防止死循环。默认 25。
    // max_context_chars:发给模型前 history 裁剪的阈值(字符数),默认读
    // 环境变量 LUBANCODE_MAX_CONTEXT(没设置就是 kDefaultMaxContextChars)。
    AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
              std::string system_prompt, int max_tokens = 4096, int max_turns = 25,
              std::size_t max_context_chars = MaxContextCharsFromEnv());

    // 发一轮用户输入。内部可能会跑好几个来回(工具调用),直到模型给出
    // end_turn(或者别的非 tool_use 的 stop_reason)才返回。历史跨多次
    // Run() 调用保留,下一句问话会带着之前的上下文。
    std::expected<void, std::string> Run(const std::string& user_input, const Callbacks& callbacks);

    const std::vector<api::Message>& history() const { return history_; }

    // M6.6:/compact 用。跟 history() 是同一份数据,单独起个大写名字是为了
    // 跟任务规矩"只许新增两个方法,不许改现有的"对齐——不改名、不改签名、
    // 不复用 history(),原样再加一份。
    const std::vector<api::Message>& History() const { return history_; }

    // M6.6:/compact 压缩完之后,把 AgentLoop 内部存的完整历史换成压缩后的
    // 那份(archive 消息 + 最近一轮完整对话)。是本次任务里唯一允许写
    // history_ 的新入口,agent/compact.cpp 里的 Compact() 本身不碰
    // AgentLoop,只管算出新历史,真正替换由调用方(main.cpp)拿到新历史后
    // 调这个方法完成。
    void ReplaceHistory(std::vector<api::Message> new_history) { history_ = std::move(new_history); }

private:
    api::Backend& backend_;
    tools::ToolRegistry& registry_;
    std::string model_;
    std::string system_prompt_;
    int max_tokens_;
    int max_turns_;
    std::size_t max_context_chars_;
    std::vector<api::Message> history_;

    std::vector<api::ToolDefinition> BuildToolDefinitions() const;
};

}  // namespace lubancode::agent
