// backend_stack.hpp 的实现(骨架拆解批四:五层请求改写后端退役,这里
// 只剩按 wire 造 client 与稳定壳 RebuildableBackend;请求策略的现拼挪去
// Agent 拼请求那一步,见 agent/agent.hpp)。

#include "app/backend_stack.hpp"

#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/gemini/client.hpp"
#include "api/responses/client.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::app {

std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    // M11:连接超时 / 流式空闲读超时用 Config 里实际生效的值(四级合并结果,
    // 没配就是内置默认值),不是每次都硬编码默认值。request_hard_timeout_secs
    // (cpr 并发挂死单)同样从配置来:每枚流式请求的硬墙钟,挂死兜底。
    const auto headers = lubancode::config::ResolveProviderHeaderTemplates(config.extra_headers,
                                                                            config.auth_token);
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.native_web_search, config.extra_body, headers, config.request_hard_timeout_secs);
    }
    if (config.wire == lubancode::config::Wire::GoogleGenerateContent) {
        // Gemini 原生 wire:鉴权走 x-goog-api-key(client 里自理),stream_usage/
        // reasoning_replay 这类 Chat 私有的 capability 都不沾;思考开关经
        // reasoning_effort 与模型推理档案一同翻成 thinkingConfig。
        return std::make_unique<lubancode::api::gemini::GeminiBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.extra_body, headers, config.request_hard_timeout_secs);
    }
    if (config.wire == lubancode::config::Wire::ChatCompletions) {
        // stream_usage/reasoning_replay 都是 provider capability(目录声明),
        // 语义见 chat/request.hpp。
        lubancode::api::chat::ChatRequestOptions chat_options;
        chat_options.stream_usage = config.stream_usage;
        chat_options.reasoning_param = config.think_param;  // 空 = 默认 reasoning_effort
        chat_options.reasoning_delta_field = config.reasoning_delta_field;  // 空 = 两别名自动兼容
        chat_options.reasoning_replay_field = config.reasoning_replay_field;  // 空 = reasoning_content
        chat_options.reasoning_replay =
            config.reasoning_replay == "tool_episode"
                ? lubancode::api::chat::ReasoningReplayPolicy::ToolEpisode
                : lubancode::api::chat::ReasoningReplayPolicy::Never;
        return std::make_unique<lubancode::api::chat::ChatCompletionsBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.extra_body, headers, std::move(chat_options), config.request_hard_timeout_secs);
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(
        config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
        config.native_web_search, config.extra_body, headers, config.request_hard_timeout_secs);
}

RebuildableBackend::RebuildableBackend(const lubancode::config::Config& config) { Rebuild(config); }

void RebuildableBackend::Rebuild(const lubancode::config::Config& config) { inner_ = BuildBackend(config); }

std::expected<void, lubancode::api::Error> RebuildableBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    return inner_->send_stream(request, on_event, cancel);
}

}  // namespace lubancode::app
