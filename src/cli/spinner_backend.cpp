// cli/spinner_backend.hpp 的实现:依赖真终端的 cli::Spinner,编在
// lubancode_app(可执行文件一侧)——链接 lubancode_core/engine 的单测
// 不要构造它(与从前的规矩一致,只是换了门牌)。

#include "cli/spinner_backend.hpp"

#include <utility>

#include "cli/spinner.hpp"

namespace lubancode::cli {

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

// HC-08 五口窄转发:直递 inner_,零状态零改写。这层是 UI 件,不是协议
// 件——内芯的预算映射与出站能力是什么就转什么,不掺一个转轮字节。
std::string SpinnerBackend::SerializeForDiagnostics(const lubancode::api::Request& request) const {
    return inner_.SerializeForDiagnostics(request);
}

lubancode::api::PreparedWireRequest SpinnerBackend::PrepareWireRequest(
    const lubancode::api::Request& request) const {
    return inner_.PrepareWireRequest(request);
}

std::optional<lubancode::api::WireMessageMap> SpinnerBackend::BuildWireMessageMap(
    const lubancode::api::Request& request) const {
    return inner_.BuildWireMessageMap(request);
}

lubancode::api::Backend::EffectiveOutputLimit SpinnerBackend::GetEffectiveOutputLimit(
    const lubancode::api::Request& request) const {
    return inner_.GetEffectiveOutputLimit(request);
}

void SpinnerBackend::ForceMaxOutputTokensOverride(lubancode::api::Request& request, int tokens) const {
    inner_.ForceMaxOutputTokensOverride(request, tokens);
}

}  // namespace lubancode::cli
