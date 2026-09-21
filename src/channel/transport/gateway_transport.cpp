// 渠道网关传输 seam:真 WsClient 的 IGatewayTransport 适配与生产工厂
//(SV-07 自 qq_gateway.cpp 原样搬移,行为零变化——含错误码折算表与
// 取消/关停的锁边界)。
#include "channel/transport/gateway_transport.hpp"

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace lubancode::channel::transport {

namespace {

// 真 WsClient 的 IGatewayTransport 适配。
class WsGatewayTransport final : public IGatewayTransport {
public:
    WsGatewayTransport(std::string ca_pem, TlsTrustMode trust_mode)
        : ca_pem_(std::move(ca_pem)), trust_mode_(trust_mode) {}

    std::expected<void, GatewayConnectError> Connect(const std::string& url) override {
        WsConnectOptions options;
        options.url = url;
        options.ca_pem = ca_pem_;
        options.trust_mode = trust_mode_;
        // 建立期取消(A08):外部 Cancel 经 cancel_state_ 打断正在建立的
        // 局部连接(DNS 后各阶段全吃句柄 shutdown)。
        auto cancel_state = std::make_shared<WsConnectCancelState>();
        options.cancel = cancel_state;
        auto client = WsClient::Connect(options);
        if (!client.has_value()) {
            // stage 用字面量 "connecting":QQ/飞书/企微的 kStageConnecting
            // 同值(各平台状态机里定义,传输层不反向依赖平台头)。
            return std::unexpected(GatewayConnectError{
                "connecting",
                client.error().error_code.empty() ? WsErrorConnectCode(client.error())
                                                  : client.error().error_code,
                "ws connect: " + client.error().detail});
        }
        // 接管与撤销在同一把锁内交接:Cancel 方要么打到登记中的句柄,要么
        // 打到已就位的 client_,无漏球窗口。
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            client_ = std::move(*client);
            connect_cancel_ = std::move(cancel_state);
            connect_cancel_->DeregisterFd();
        }
        return {};
    }

    std::expected<void, std::string> SendText(const std::string& text) override {
        const auto sent = client_.SendText(text);
        if (!sent.has_value()) {
            return std::unexpected("ws send: " + sent.error().detail);
        }
        return {};
    }

    std::expected<std::string, WsError> ReadMessage(int timeout_ms) override {
        return client_.ReadMessage(timeout_ms);
    }

    void Cancel() override {
        // 建立期:打断 Connect;运行期:shutdown 底层连接,阻塞读立即
        // 以 Closed 返回。全程持锁——与 Connect 的交接、Close 的清理
        // 互斥(A08:Close/Cancel/client_ 发布清理并发安全);阻塞读不经
        // 锁,取消延迟只受 shutdown 本身。
        std::shared_ptr<WsConnectCancelState> cancel_state;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            cancel_state = connect_cancel_;
            connect_cancel_.reset();
            client_.Cancel();
        }
        if (cancel_state != nullptr) {
            cancel_state->Cancel();  // 锁外:只碰自家的原子与登记句柄
        }
    }

    void Close(std::uint16_t code, const std::string& reason) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        (void)client_.Close(code, reason);
    }

private:
    // WsError(细码缺位时)-> 连接段稳定码。TCP 连不上/超时与 WS 升级握手
    // 失败在 WsClient::Connect 一口锅;TLS 细码已透传,其余按 kind 分。
    static std::string WsErrorConnectCode(const WsError& error) {
        switch (error.kind) {
            case WsError::Kind::Timeout:
                return "connect_timeout";
            case WsError::Kind::Protocol:
                return "ws_handshake_failed";
            case WsError::Kind::Closed:
                return error.detail == "connect cancelled" ? std::string("connect_cancelled")
                                                           : std::string("ws_handshake_closed");
            case WsError::Kind::Failed:
                return error.detail.rfind("tls: ", 0) == 0
                           ? std::string(kTlsCodeHandshakeFailed)
                           : std::string("connect_failed");
        }
        return "connect_failed";
    }

    std::string ca_pem_;
    TlsTrustMode trust_mode_ = TlsTrustMode::ExplicitCa;
    // client_ 归网关线程独占使用;Cancel 可从停止线程来,只经 Cancel() 摸
    // 连接(shutdown 句柄),不读写其余成员。connect_cancel_ 的发布/清理
    // 过 mutex_(与 Cancel 方同步)。
    std::mutex mutex_;
    std::shared_ptr<WsConnectCancelState> connect_cancel_;
    WsClient client_;
};

}  // namespace

std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(
    std::string ca_pem, TlsTrustMode trust_mode) {
    return [ca_pem = std::move(ca_pem), trust_mode]() -> std::unique_ptr<IGatewayTransport> {
        return std::make_unique<WsGatewayTransport>(ca_pem, trust_mode);
    };
}

}  // namespace lubancode::channel::transport
