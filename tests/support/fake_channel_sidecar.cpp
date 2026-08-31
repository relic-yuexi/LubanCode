#include "fake_channel_sidecar.hpp"

#include "channel/bridge_protocol.hpp"

namespace lubancode::test_support {

using lubancode::channel::BridgeMethod;
using lubancode::channel::BuildDomainErrorResponseJson;
using lubancode::channel::BuildNotificationJson;
using lubancode::channel::BuildResultResponseJson;
using lubancode::channel::BuildStandardErrorResponseJson;
using lubancode::channel::DomainErrorName;
using lubancode::channel::EncodeFrame;
using lubancode::channel::IncomingMessage;
using lubancode::channel::IncomingMessageKind;
using lubancode::channel::JsonRpcStandardErrorCode;
using lubancode::channel::kBridgeHandshakeProtocolVersion;
using lubancode::channel::ParseIncomingMessage;

void FakeChannelSidecar::FeedFromHost(const std::byte* data, std::size_t size) {
    decoder_.Feed(data, size);
    while (true) {
        auto next = decoder_.TryDecodeNext();
        if (!next.has_value()) {
            diagnostics_.push_back(std::string("frame error: ") + next.error().message);
            break;
        }
        if (!next->has_value()) break;  // 数据不够,等下次喂
        HandleIncomingJson(**next);
    }
}

void FakeChannelSidecar::FeedFromHost(std::string_view bytes) {
    FeedFromHost(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
}

std::vector<std::byte> FakeChannelSidecar::DrainToHost() {
    std::vector<std::byte> out = std::move(to_host_bytes_);
    to_host_bytes_.clear();
    return out;
}

void FakeChannelSidecar::SendBack(const nlohmann::json& message_json) {
    auto encoded = EncodeFrame(message_json);
    if (!encoded.has_value()) {
        diagnostics_.push_back(std::string("encode error: ") + encoded.error().message);
        return;
    }
    to_host_bytes_.insert(to_host_bytes_.end(), encoded->begin(), encoded->end());
}

void FakeChannelSidecar::EmitInboundEvent(const lubancode::channel::ChannelInboundEvent& event) {
    SendBack(BuildNotificationJson(BridgeMethod::Inbound, event.ToJson()));
}

void FakeChannelSidecar::HandleIncomingJson(const nlohmann::json& frame_json) {
    const IncomingMessage message = ParseIncomingMessage(frame_json);

    if (message.kind == IncomingMessageKind::Malformed) {
        diagnostics_.push_back("malformed message: " + message.malformed_reason);
        return;
    }
    if (message.kind == IncomingMessageKind::ResultResponse ||
        message.kind == IncomingMessageKind::ErrorResponse) {
        // v1 协议里 sidecar 不主动发 request,故不该收到 host 的 response;
        // 收到也不崩,记诊断即可(镜像方向的宽容处理,呼应 router 的口径)。
        diagnostics_.push_back("unexpected response from host (fake sidecar never requests)");
        return;
    }
    if (!message.method.has_value()) {
        // 陌生 method:request 须回 -32601,notification 无 response 可回,
        // 记诊断(bridge-protocol.md §2"陌生 method 回 -32601,不可静默吞")。
        diagnostics_.push_back("unknown method: " + message.method_name);
        if (message.kind == IncomingMessageKind::Request && message.id.has_value()) {
            SendBack(BuildStandardErrorResponseJson(*message.id, JsonRpcStandardErrorCode::MethodNotFound,
                                                     "method not found: " + message.method_name));
        }
        return;
    }

    const BridgeMethod method = *message.method;
    if (message.kind == IncomingMessageKind::Notification) {
        // channel.typing 是唯一的 host->sidecar notification;丢了也无妨
        //(bridge-protocol.md §4),假 sidecar 不需要另外应答。
        return;
    }

    // 剩下都是 Request,须有 id。
    if (!message.id.has_value()) {
        diagnostics_.push_back("request without id: " + message.method_name);
        return;
    }
    const std::int64_t id = *message.id;

    if (auto shape_error = lubancode::channel::ValidateMethodParamsShape(method, message.params);
        shape_error.has_value()) {
        SendBack(BuildStandardErrorResponseJson(id, JsonRpcStandardErrorCode::InvalidParams, *shape_error));
        return;
    }

    switch (method) {
        case BridgeMethod::Initialize: {
            const std::string protocol_version = message.params.value("protocol_version", "");
            if (force_protocol_mismatch_ || protocol_version != kBridgeHandshakeProtocolVersion) {
                force_protocol_mismatch_ = false;
                SendBack(BuildDomainErrorResponseJson(
                    id, DomainErrorName::ProtocolIncompatible,
                    "sidecar only speaks " + std::string(kBridgeHandshakeProtocolVersion) + ", host asked " +
                        protocol_version));
                return;
            }
            channel_id_ = message.params.value("channel_id", "");
            account_id_ = message.params.value("account_id", "");
            handshake_completed_ = true;
            nlohmann::json result = nlohmann::json::object();
            result["protocol_version"] = std::string(kBridgeHandshakeProtocolVersion);
            result["adapter"] = {{"name", "fake-channel-sidecar"}, {"version", "1.0.0"}};
            result["capabilities"] =
                nlohmann::json{{"transports", nlohmann::json::array({"websocket"})},
                              {"delivery", nlohmann::json::array({"send", "typing"})}};
            result["account_state_version"] = 1;
            SendBack(BuildResultResponseJson(id, result));
            return;
        }
        case BridgeMethod::Start: {
            started_ = true;
            stopped_ = false;
            nlohmann::json result = nlohmann::json::object();
            result["started"] = true;
            result["transport"] = message.params.value("transport", "websocket");
            SendBack(BuildResultResponseJson(id, result));
            return;
        }
        case BridgeMethod::Stop: {
            started_ = false;
            stopped_ = true;
            SendBack(BuildResultResponseJson(id, {{"stopped", true}, {"flushed", true}}));
            return;
        }
        case BridgeMethod::Health: {
            nlohmann::json result = nlohmann::json::object();
            result["state"] = started_ ? "running" : "stopped";
            result["connected"] = started_;
            result["cursor"] = nullptr;
            result["backlog"] = 0;
            result["last_error"] = nullptr;
            SendBack(BuildResultResponseJson(id, result));
            return;
        }
        case BridgeMethod::Send: {
            RecordedSend record;
            record.client_id = message.params.value("client_id", "");
            record.params = message.params;
            record.provider_message_id = "om_" + std::to_string(next_provider_message_seq_++);
            SendBack(BuildResultResponseJson(
                id, {{"provider_message_id", record.provider_message_id}, {"accepted", true}}));
            sent_messages_.push_back(std::move(record));
            return;
        }
        case BridgeMethod::Edit: {
            SendBack(BuildResultResponseJson(id, {{"edited", true}}));
            return;
        }
        case BridgeMethod::React: {
            SendBack(BuildResultResponseJson(id, {{"applied", true}}));
            return;
        }
        case BridgeMethod::LoginBegin: {
            SendBack(BuildResultResponseJson(id, {{"begun", true}}));
            return;
        }
        case BridgeMethod::LoginCancel: {
            SendBack(BuildResultResponseJson(id, {{"cancelled", true}}));
            return;
        }
        case BridgeMethod::Logout: {
            SendBack(BuildResultResponseJson(id, {{"logged_out", true}}));
            return;
        }
        case BridgeMethod::InboundAck: {
            acked_delivery_ids_.push_back(message.params.value("delivery_id", ""));
            SendBack(BuildResultResponseJson(id, {{"acked", true}}));
            return;
        }
        case BridgeMethod::InboundNack: {
            SendBack(BuildResultResponseJson(id, {{"nacked", true}}));
            return;
        }
        default:
            // Inbound/Status/DeliveryReceipt/LoginChallenge/LoginCompleted/
            // CapabilitiesChanged/Fatal 都是 sidecar -> host 方向,host 不会
            // 拿这些当 request 发过来;真出现按陌生 method 处理。
            SendBack(BuildStandardErrorResponseJson(id, JsonRpcStandardErrorCode::MethodNotFound,
                                                     std::string("method not host-callable: ") +
                                                         lubancode::channel::BridgeMethodName(method)));
            return;
    }
}

}  // namespace lubancode::test_support
