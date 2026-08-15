// backend_stack.hpp 的实现:各请求改写层的函数体全在这只 translation
// unit 里,具体 client(anthropic/chat/responses)、prompts 拼接与 cli::Spinner
// 的依赖不再从公开头漏出去。

#include "app/backend_stack.hpp"

#include "agent/prompts.hpp"
#include "api/anthropic/client.hpp"
#include "api/chat/client.hpp"
#include "api/responses/client.hpp"
#include "cli/spinner.hpp"
#include "config/provider_catalog.hpp"

namespace lubancode::app {

std::unique_ptr<lubancode::api::Backend> BuildBackend(const lubancode::config::Config& config) {
    // M11:连接超时 / 流式空闲读超时用 Config 里实际生效的值(四级合并结果,
    // 没配就是内置默认值),不是每次都硬编码默认值。
    const auto headers = lubancode::config::ResolveProviderHeaderTemplates(config.extra_headers,
                                                                            config.auth_token);
    if (config.wire == lubancode::config::Wire::Responses) {
        return std::make_unique<lubancode::api::responses::ResponsesBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.native_web_search, config.extra_body, headers);
    }
    if (config.wire == lubancode::config::Wire::ChatCompletions) {
        // stream_usage 是 provider capability(DeepSeek 等家声明),请求体带
        // stream_options.include_usage,见 chat/request.hpp。
        lubancode::api::chat::ChatRequestOptions chat_options;
        chat_options.stream_usage = config.stream_usage;
        return std::make_unique<lubancode::api::chat::ChatCompletionsBackend>(
            config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
            config.extra_body, headers, std::move(chat_options));
    }
    return std::make_unique<lubancode::api::anthropic::AnthropicBackend>(
        config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
        config.native_web_search, config.extra_body, headers);
}

RebuildableBackend::RebuildableBackend(const lubancode::config::Config& config) { Rebuild(config); }

void RebuildableBackend::Rebuild(const lubancode::config::Config& config) { inner_ = BuildBackend(config); }

std::expected<void, lubancode::api::Error> RebuildableBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    return inner_->send_stream(request, on_event, cancel);
}

ModelOverrideBackend::ModelOverrideBackend(lubancode::api::Backend& inner,
                                           std::shared_ptr<std::string> current_model)
    : inner_(inner), current_model_(std::move(current_model)) {}

std::expected<void, lubancode::api::Error> ModelOverrideBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::api::Request patched = request;
    patched.model = *current_model_;
    return inner_.send_stream(patched, on_event, cancel);
}

ThinkOverrideBackend::ThinkOverrideBackend(lubancode::api::Backend& inner,
                                           std::shared_ptr<std::string> current_think,
                                           std::shared_ptr<std::string> current_model,
                                           const lubancode::config::ModelCatalog* catalog)
    : inner_(inner),
      current_think_(std::move(current_think)),
      current_model_(std::move(current_model)),
      catalog_(catalog) {}

std::expected<void, lubancode::api::Error> ThinkOverrideBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::api::Request patched = request;
    patched.reasoning_effort = *current_think_;
    if (catalog_ != nullptr) {
        const auto body = lubancode::config::ThinkLevelExtraBody(
            catalog_->FindBySlug(*current_model_), *current_think_);
        for (auto it = body.begin(); it != body.end(); ++it) {
            patched.extra_body[it.key()] = it.value();
        }
    }
    return inner_.send_stream(patched, on_event, cancel);
}

ModelInstructionsBackend::ModelInstructionsBackend(lubancode::api::Backend& inner,
                                                   std::shared_ptr<std::string> current_instructions)
    : inner_(inner), current_instructions_(std::move(current_instructions)) {}

std::expected<void, lubancode::api::Error> ModelInstructionsBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::api::Request patched = request;
    patched.system = lubancode::agent::WithModelInstructions(request.system, *current_instructions_);
    return inner_.send_stream(patched, on_event, cancel);
}

SoulOverlayBackend::SoulOverlayBackend(lubancode::api::Backend& inner,
                                       std::shared_ptr<std::string> current_soul)
    : inner_(inner), current_soul_(std::move(current_soul)) {}

std::expected<void, lubancode::api::Error> SoulOverlayBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::api::Request patched = request;
    patched.system = lubancode::agent::WithSoul(request.system, *current_soul_);
    return inner_.send_stream(patched, on_event, cancel);
}

DeferredIndexBackend::DeferredIndexBackend(lubancode::api::Backend& inner,
                                           std::function<std::string()> index_provider)
    : inner_(inner), index_provider_(std::move(index_provider)) {}

std::expected<void, lubancode::api::Error> DeferredIndexBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::api::Request patched = request;
    patched.system = lubancode::agent::WithDeferredToolsIndex(
        request.system, index_provider_ ? index_provider_() : std::string());
    return inner_.send_stream(patched, on_event, cancel);
}

SpinnerBackend::SpinnerBackend(lubancode::api::Backend& inner, const lubancode::cli::Theme& theme,
                               bool spinner_enabled)
    : inner_(inner), theme_(theme), spinner_enabled_(spinner_enabled) {}

std::expected<void, lubancode::api::Error> SpinnerBackend::send_stream(
    const lubancode::api::Request& request,
    const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
    const std::atomic<bool>* cancel) {
    lubancode::cli::Spinner spinner(theme_, spinner_enabled_);
    bool stopped = false;
    const auto wrapped = [&](const lubancode::api::StreamEvent& event) {
        if (!stopped) {
            spinner.Stop();
            stopped = true;
        }
        on_event(event);
    };
    return inner_.send_stream(request, wrapped, cancel);
    // spinner 在这里析构,Stop() 兜底再调一次也是安全的(空操作)——
    // 万一 send_stream 直接失败、一个事件都没吐(比如连都没连上),
    // 转轮不会一直转着。
}

}  // namespace lubancode::app
