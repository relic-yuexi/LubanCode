// bridge_endpoint.hpp 的实现(见头注:职责边界与线程口径)。
#include "channel/bridge_endpoint.hpp"

namespace lubancode::channel {

void InProcessBridgeEndpoint::Feed(const std::byte* data, std::size_t size,
                                   const FrameHandler& on_frame) {
    decoder_.Feed(data, size);
    while (true) {
        auto next = decoder_.TryDecodeNext();
        if (!next.has_value()) {
            // 宿主来向帧坏:Fatal 通知(协议错由宿主状态机处置),停本轮
            // 分派——解码器粘性错,再 Feed 仍从这里出。
            Notify(BridgeMethod::Fatal,
                   nlohmann::json{{"reason", "invalid_frame"},
                                  {"detail", next.error().message}});
            return;
        }
        if (!next->has_value()) {
            return;  // 半帧,等更多字节
        }
        on_frame(**next);
    }
}

std::vector<std::byte> InProcessBridgeEndpoint::Drain() {
    const std::lock_guard<std::mutex> lock(out_mutex_);
    std::vector<std::byte> out = std::move(to_host_);
    to_host_.clear();
    return out;
}

void InProcessBridgeEndpoint::ReplyResult(std::int64_t id, const nlohmann::json& result) {
    AppendFrame(BuildResultResponseJson(id, result));
}

void InProcessBridgeEndpoint::ReplyDomainError(std::int64_t id, DomainErrorName name,
                                               const std::string& detail) {
    AppendFrame(BuildDomainErrorResponseJson(id, name, detail));
}

void InProcessBridgeEndpoint::Notify(BridgeMethod method, const nlohmann::json& params) {
    AppendFrame(BuildNotificationJson(method, params));
}

void InProcessBridgeEndpoint::AppendFrame(const nlohmann::json& payload) {
    const std::lock_guard<std::mutex> lock(out_mutex_);
    if (const auto encoded = EncodeFrame(payload); encoded.has_value()) {
        to_host_.insert(to_host_.end(), encoded->begin(), encoded->end());
    }
    // 编码失败(超帽/非 object):静默不入缓冲,照原三份机械(SV-08 口径:
    // 不借合并悄悄改行为)。
}

}  // namespace lubancode::channel
