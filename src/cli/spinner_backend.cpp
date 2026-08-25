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

}  // namespace lubancode::cli
