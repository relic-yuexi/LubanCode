// 渠道网关传输 seam(架构审查 SV-07,自 qq_gateway.hpp 迁中立位):平台
// 无关的 WS 网关传输合同与生产工厂——QQ/企微网关状态机、装配层
//(wiring/registry)与测试注假共用。合同只定 Connect/SendText/
// ReadMessage/Cancel/Close 五口;平台帧协议(hello/identify/subscribe 等)
// 不在这里,各归各的网关状态机。工厂底下本就用共享层 WsClient
//(R0),不另造网络栈。
//
// 迁移兼容:qq 命名空间留 GatewayConnectError/IGatewayTransport 别名与
// MakeWsTransportFactory 转发(qq_gateway.hpp),旧调用方照旧编译;调用方
// 全数改用 transport:: 命名后即可退役。
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "channel/transport/ws_client.hpp"  // WsError/TlsTrustMode(ReadMessage 与工厂)

namespace lubancode::channel::transport {

// 连接失败的稳定账:失败阶段 + 稳定错误码 + 脱敏说明。provider/transport
// 的失败都折成它,网关/适配器按它记"最近失败"(退避事件不改写)。
struct GatewayConnectError {
    std::string stage;       // 各平台 kStage* 之一
    std::string error_code;  // 稳定码(QQ 网关的码表注释见 qq_gateway.cpp)
    std::string detail;      // 脱敏人话(不带 token/secret/响应体)
    // 服务端 Retry-After 的建议退避下限(毫秒;0 = 无)。网关退避取
    // max(阶梯值, retry_after_ms) 再封 max_backoff_ms 帽——429 服从有效
    // Retry-After 且有上限(QQ §四;企微同款口径)。
    std::int64_t retry_after_ms = 0;
};

// 网关传输 seam(测试注入假流;生产 MakeWsTransportFactory)。一只连接
// 归一只线程(ws_client.hpp 头注);建立期取消与运行期 shutdown 都走
// Cancel,在途传输持共享所有权(A08)。
class IGatewayTransport {
public:
    virtual ~IGatewayTransport() = default;
    virtual std::expected<void, GatewayConnectError> Connect(const std::string& url) = 0;
    virtual std::expected<void, std::string> SendText(const std::string& text) = 0;
    virtual std::expected<std::string, WsError> ReadMessage(int timeout_ms) = 0;
    virtual void Cancel() = 0;
    virtual void Close(std::uint16_t code, const std::string& reason) = 0;
};

// 生产传输工厂:真 WsClient(明文 ws:// 与 wss:// 同一路,ca_pem 供 wss;
// trust_mode 透传 TLS 层,见 transport/tls.hpp)。
std::function<std::unique_ptr<IGatewayTransport>()> MakeWsTransportFactory(
    std::string ca_pem, TlsTrustMode trust_mode = TlsTrustMode::ExplicitCa);

}  // namespace lubancode::channel::transport
