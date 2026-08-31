#include "channel/router.hpp"

#include <stdexcept>

namespace lubancode::channel {

std::int64_t BridgeRouter::EnqueueOutgoingRequest(BridgeMethod method, const nlohmann::json& params) {
    const std::int64_t id = next_id_++;
    pending_.emplace(id, PendingBridgeRequest{id, method});
    outbound_.push_back(BuildRequestJson(id, method, params));
    return id;
}

void BridgeRouter::EnqueueOutgoingNotification(BridgeMethod method, const nlohmann::json& params) {
    outbound_.push_back(BuildNotificationJson(method, params));
}

nlohmann::json BridgeRouter::PopOutbound() {
    if (outbound_.empty()) {
        throw std::out_of_range("BridgeRouter::PopOutbound: outbound queue is empty");
    }
    nlohmann::json front = std::move(outbound_.front());
    outbound_.pop_front();
    return front;
}

IncomingMessage BridgeRouter::PopInbound() {
    if (inbound_.empty()) {
        throw std::out_of_range("BridgeRouter::PopInbound: inbound queue is empty");
    }
    IncomingMessage front = std::move(inbound_.front());
    inbound_.pop_front();
    return front;
}

BridgeRouter::DispatchResult BridgeRouter::Dispatch(const IncomingMessage& message) {
    DispatchResult result;
    switch (message.kind) {
        case IncomingMessageKind::Malformed: {
            result.action = DispatchResult::Action::Malformed;
            result.diagnostic = message.malformed_reason;
            diagnostics_.push_back("malformed: " + result.diagnostic);
            return result;
        }
        case IncomingMessageKind::Request: {
            if (!message.method.has_value()) {
                result.action = DispatchResult::Action::UnknownMethod;
                result.diagnostic = "unknown method in request: " + message.method_name;
                diagnostics_.push_back(result.diagnostic);
                return result;
            }
            inbound_.push_back(message);
            result.action = DispatchResult::Action::RequestQueued;
            return result;
        }
        case IncomingMessageKind::Notification: {
            if (!message.method.has_value()) {
                result.action = DispatchResult::Action::UnknownMethod;
                result.diagnostic = "unknown method in notification: " + message.method_name;
                diagnostics_.push_back(result.diagnostic);
                return result;
            }
            inbound_.push_back(message);
            result.action = DispatchResult::Action::NotificationQueued;
            return result;
        }
        case IncomingMessageKind::ResultResponse:
        case IncomingMessageKind::ErrorResponse: {
            if (!message.id.has_value()) {
                result.action = DispatchResult::Action::StaleResponse;
                result.diagnostic = "response missing id";
                diagnostics_.push_back(result.diagnostic);
                return result;
            }
            const auto it = pending_.find(*message.id);
            if (it == pending_.end()) {
                // 迟到响应、重复响应、陌生响应 id 三种都落在这一支:pending
                // 表里已经没有这个 id 了(要么从没发过,要么已经配对消费
                // 过一次)。丢弃并留诊断账,不崩宿主(bridge-protocol.md §2)。
                result.action = DispatchResult::Action::StaleResponse;
                result.diagnostic =
                    "response id " + std::to_string(*message.id) + " matches no pending request";
                diagnostics_.push_back(result.diagnostic);
                return result;
            }
            result.action = DispatchResult::Action::ResponseMatched;
            result.matched_request_id = it->second.id;
            result.matched_request_method = it->second.method;
            pending_.erase(it);
            return result;
        }
    }
    result.action = DispatchResult::Action::Malformed;
    result.diagnostic = "unreachable dispatch state";
    return result;
}

}  // namespace lubancode::channel
