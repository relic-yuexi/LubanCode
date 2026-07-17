#include "agent/loop.hpp"

#include <type_traits>
#include <utility>
#include <variant>

#include "api/assembler.hpp"

namespace lubancode::agent {

namespace {

// 执行一个工具调用:先通知上层要开始了,needs_confirm 的话先问一句,
// 拒绝/找不到工具/正常执行,最后都会走 on_tool_done 通知一遍,保证上层
// 能看到完整的生命周期。
tools::Tool::Result RunOneTool(tools::ToolRegistry& registry, const api::ToolUseBlock& call, const Callbacks& callbacks) {
    if (callbacks.on_tool_start) {
        callbacks.on_tool_start(call.name, call.input);
    }

    tools::Tool* tool = registry.Find(call.name);
    if (tool == nullptr) {
        tools::Tool::Result result{"未知工具: " + call.name, true};
        if (callbacks.on_tool_done) {
            callbacks.on_tool_done(call.name, result);
        }
        return result;
    }

    if (tool->needs_confirm()) {
        const bool allowed = callbacks.on_tool_confirm ? callbacks.on_tool_confirm(call.name, call.input) : true;
        if (!allowed) {
            tools::Tool::Result result{"用户拒绝执行该工具", true};
            if (callbacks.on_tool_done) {
                callbacks.on_tool_done(call.name, result);
            }
            return result;
        }
    }

    tools::Tool::Result result = tool->execute(call.input);
    if (callbacks.on_tool_done) {
        callbacks.on_tool_done(call.name, result);
    }
    return result;
}

}  // namespace

AgentLoop::AgentLoop(api::Backend& backend, tools::ToolRegistry& registry, std::string model,
                      std::string system_prompt, int max_tokens, int max_turns)
    : backend_(backend),
      registry_(registry),
      model_(std::move(model)),
      system_prompt_(std::move(system_prompt)),
      max_tokens_(max_tokens),
      max_turns_(max_turns) {}

std::vector<api::ToolDefinition> AgentLoop::BuildToolDefinitions() const {
    std::vector<api::ToolDefinition> defs;
    defs.reserve(registry_.All().size());
    for (const auto& tool : registry_.All()) {
        defs.push_back(api::ToolDefinition{tool->name(), tool->description(), tool->input_schema()});
    }
    return defs;
}

std::expected<void, std::string> AgentLoop::Run(const std::string& user_input, const Callbacks& callbacks) {
    api::Message user_message;
    user_message.role = api::Role::User;
    user_message.content.push_back(api::TextBlock{user_input});
    history_.push_back(std::move(user_message));

    const std::vector<api::ToolDefinition> tool_defs = BuildToolDefinitions();

    for (int turn = 0; turn < max_turns_; ++turn) {
        api::Request request;
        request.model = model_;
        request.system = system_prompt_;
        request.messages = history_;
        request.max_tokens = max_tokens_;
        request.tools = tool_defs;

        api::MessageAssembler assembler;
        bool stream_error = false;
        std::string stream_error_message;

        const auto send_result = backend_.send_stream(request, [&](const api::StreamEvent& event) {
            assembler.Feed(event);
            std::visit(
                [&](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, api::TextDelta>) {
                        if (callbacks.on_text_delta) {
                            callbacks.on_text_delta(e.text);
                        }
                    } else if constexpr (std::is_same_v<T, api::StreamError>) {
                        stream_error = true;
                        stream_error_message = e.message;
                    }
                },
                event);
        });

        if (!send_result.has_value()) {
            return std::unexpected("请求失败: " + send_result.error().message);
        }
        if (stream_error) {
            return std::unexpected("模型返回错误: " + stream_error_message);
        }

        api::Message assistant_message = assembler.BuildMessage();
        const std::string stop_reason = assembler.stop_reason();
        history_.push_back(assistant_message);

        if (stop_reason != "tool_use") {
            return {};
        }

        std::vector<api::ContentBlock> tool_results;
        for (const auto& block : assistant_message.content) {
            if (!std::holds_alternative<api::ToolUseBlock>(block)) {
                continue;
            }
            const auto& call = std::get<api::ToolUseBlock>(block);
            const tools::Tool::Result result = RunOneTool(registry_, call, callbacks);
            tool_results.push_back(api::ToolResultBlock{call.id, result.content, result.is_error});
        }

        api::Message tool_result_message;
        tool_result_message.role = api::Role::User;
        tool_result_message.content = std::move(tool_results);
        history_.push_back(std::move(tool_result_message));
    }

    return std::unexpected("超过最大轮数(" + std::to_string(max_turns_) + "),已停止,避免死循环。");
}

}  // namespace lubancode::agent
