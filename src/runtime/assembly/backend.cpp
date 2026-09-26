// 共用后端装配:按配置造 client,稳定引用壳只负责替换与能力转发。

#include "runtime/assembly/backend.hpp"

#include <utility>

#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/gemini/client.hpp"
#include "api/responses/client.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::runtime::assembly {

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

RebuildableBackend::RebuildableBackend(std::shared_ptr<lubancode::api::Backend> inner)
    : inner_(std::move(inner)) {}

void RebuildableBackend::Rebuild(const lubancode::config::Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    inner_ = BuildBackend(config);
}

std::shared_ptr<lubancode::api::Backend> RebuildableBackend::SnapshotInner() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inner_;
}

std::expected<void, lubancode::api::Error> RebuildableBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    // HC-08:快照收进 SnapshotInner,与五口 const 查询同一条锁规矩——锁内
    // 拷指针,放锁后调,在飞请求持旧内芯跑到完,不为发送持锁跨流式网络。
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api,
                                                     "backend 尚未装配"});
    }
    return inner->send_stream(request, on_event, cancel);
}

// HC-08 五口窄转发。未装配(快照空)按接口合同返回错误/不可得:落回
// 基类默认形态——空串/不可得/回退 Request::max_tokens/no-op,与
// "trace/桩后端不提供"同一张脸,不冒充实数。
std::string RebuildableBackend::SerializeForDiagnostics(const lubancode::api::Request& request) const {
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        return Backend::SerializeForDiagnostics(request);
    }
    return inner->SerializeForDiagnostics(request);
}

lubancode::api::PreparedWireRequest RebuildableBackend::PrepareWireRequest(
    const lubancode::api::Request& request) const {
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        return Backend::PrepareWireRequest(request);
    }
    return inner->PrepareWireRequest(request);
}

std::optional<lubancode::api::WireMessageMap> RebuildableBackend::BuildWireMessageMap(
    const lubancode::api::Request& request) const {
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        return Backend::BuildWireMessageMap(request);
    }
    return inner->BuildWireMessageMap(request);
}

lubancode::api::Backend::EffectiveOutputLimit RebuildableBackend::GetEffectiveOutputLimit(
    const lubancode::api::Request& request) const {
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        return Backend::GetEffectiveOutputLimit(request);
    }
    return inner->GetEffectiveOutputLimit(request);
}

void RebuildableBackend::ForceMaxOutputTokensOverride(lubancode::api::Request& request, int tokens) const {
    const std::shared_ptr<lubancode::api::Backend> inner = SnapshotInner();
    if (inner == nullptr) {
        Backend::ForceMaxOutputTokensOverride(request, tokens);
        return;
    }
    inner->ForceMaxOutputTokensOverride(request, tokens);
}

}  // namespace lubancode::runtime::assembly
