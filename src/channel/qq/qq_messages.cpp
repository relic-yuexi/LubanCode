#include "channel/qq/qq_messages.hpp"

#include <utility>

namespace lubancode::channel::qq {

namespace {

QqMessageSender::Outcome::Status DeferredOrPermanent(const QqApiError& error) {
    switch (error.kind) {
        case QqApiErrorKind::RateLimited:
        case QqApiErrorKind::ServerError:
        case QqApiErrorKind::NetworkError:
        case QqApiErrorKind::FriendCheckFailed:  // 40054006:官方建议重试(A03)
        case QqApiErrorKind::BotOffline:  // 40054016:下线状态可恢复,有限重试
            return QqMessageSender::Outcome::Status::DeferredRetry;
        default:
            return QqMessageSender::Outcome::Status::PermanentFail;
    }
}

}  // namespace

std::uint32_t QqMessageSender::AssignSeq(const C2cSendRequest& request) {
    // 同 delivery 重试:复用冻结值。
    const auto frozen = frozen_seq_by_delivery_.find(request.outbound_delivery_id);
    if (frozen != frozen_seq_by_delivery_.end()) {
        return frozen->second;
    }
    // 主动消息(无 msg_id)不带 seq。
    if (request.msg_id.empty()) {
        return 0;
    }
    std::uint32_t& next = next_seq_by_msg_id_[request.msg_id];
    const std::uint32_t seq = next + 1;  // 官方口径:不填默认 1,从 1 起
    next = seq;
    frozen_seq_by_delivery_[request.outbound_delivery_id] = seq;
    return seq;
}

std::size_t QqMessageSender::frozen_delivery_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frozen_seq_by_delivery_.size();
}

QqMessageSender::Outcome QqMessageSender::SendC2c(const C2cSendRequest& request) {
    C2cSendRequest effective = request;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        effective.msg_seq = AssignSeq(request);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // attempt 0:正常路;attempt 1 仅在 Unauthorized(token 失效)后重走。
        const auto token = options_.tokens->GetValidToken();
        if (!token.has_value()) {
            QqApiError error;
            error.kind = QqApiErrorKind::NetworkError;
            switch (token.error().kind) {
                case QqTokenManager::ErrorKind::RateLimited:
                    error.kind = QqApiErrorKind::RateLimited;
                    break;
                case QqTokenManager::ErrorKind::ServerError:
                    error.kind = QqApiErrorKind::ServerError;
                    break;
                default:
                    break;
            }
            error.detail = "token: " + token.error().detail;
            Outcome outcome;
            outcome.status = DeferredOrPermanent(error);
            outcome.error = std::move(error);
            return outcome;
        }

        QqHttpRequest http_request;
        http_request.method = "POST";
        http_request.url = options_.api_base + C2cSendPath(request.openid);
        http_request.headers.emplace_back("Content-Type", "application/json");
        http_request.headers.emplace_back("Authorization", "QQBot " + *token);
        http_request.body = BuildC2cSendPayload(effective).dump();

        const auto response = options_.http(http_request);
        if (!response.has_value()) {
            QqApiError error;
            error.kind = QqApiErrorKind::NetworkError;
            error.detail = response.error();
            Outcome outcome;
            outcome.status = Outcome::Status::DeferredRetry;
            outcome.error = std::move(error);
            return outcome;
        }
        if (response->status >= 200 && response->status < 300) {
            const auto parsed = nlohmann::json::parse(response->body, nullptr,
                                                      /*allow_exceptions=*/false);
            // 2xx 不等于成功:平台错误体两代形状都可能走 HTTP 200(官方
            // API 调用指南 {"err_code":..} / 发送页 {"code":..})。共用解析
            // 器裁决;码字段非法或冲突不折算成 0——成功合同无法核对即按
            // InvalidResponse 收,不冒充送达(A03)。
            const QqErrorBodyShape shape = ParseQqErrorBody(response->body);
            const auto effective = QqErrorEffectiveCode(shape);
            const bool has_code_field = shape.has_code || shape.has_err_code;
            if (has_code_field && (shape.conflict || !effective.has_value())) {
                QqApiError error = ClassifyQqSendFailure(response->status, response->body);
                Outcome outcome;  // 2xx + 码不可判定:不可重试,走人工账
                outcome.status = DeferredOrPermanent(error);
                outcome.error = std::move(error);
                return outcome;
            }
            if (effective.has_value() && *effective != 0) {
                QqApiError error = ClassifyQqSendFailure(response->status, response->body);
                if (error.kind == QqApiErrorKind::Deduped) {
                    Outcome outcome;
                    outcome.status = Outcome::Status::Deduped;
                    outcome.error = std::move(error);
                    return outcome;
                }
                if (error.kind == QqApiErrorKind::Unauthorized && attempt == 0) {
                    options_.tokens->Invalidate();
                    continue;
                }
                Outcome outcome;
                outcome.status = DeferredOrPermanent(error);
                outcome.error = std::move(error);
                return outcome;
            }
            if (parsed.is_discarded()) {
                QqApiError error;
                error.kind = QqApiErrorKind::InvalidResponse;
                error.detail = "send response not json";
                Outcome outcome;  // 2xx 但解不出:不可重试(平台状态未知),
                outcome.status = Outcome::Status::PermanentFail;  // 走人工账
                outcome.error = std::move(error);
                return outcome;
            }
            std::string parse_error;
            const auto send_result = ParseC2cSendResponse(parsed, &parse_error);
            if (!send_result.has_value()) {
                QqApiError error;
                error.kind = QqApiErrorKind::InvalidResponse;
                error.detail = parse_error;
                Outcome outcome;
                outcome.status = Outcome::Status::PermanentFail;
                outcome.error = std::move(error);
                return outcome;
            }
            Outcome outcome;
            outcome.status = Outcome::Status::Sent;
            outcome.provider_message_id = send_result->provider_message_id;
            {
                // 送达终结:清冻结账(记账规模有界)。
                const std::lock_guard<std::mutex> lock(mutex_);
                frozen_seq_by_delivery_.erase(request.outbound_delivery_id);
                ++frozen_done_;
            }
            return outcome;
        }

        QqApiError error = ClassifyQqSendFailure(response->status, response->body);
        if (error.kind == QqApiErrorKind::Deduped) {
            Outcome outcome;
            outcome.status = Outcome::Status::Deduped;
            outcome.error = std::move(error);
            return outcome;
        }
        if (error.kind == QqApiErrorKind::Unauthorized && attempt == 0) {
            options_.tokens->Invalidate();
            continue;  // 刷 token 再试一次(载荷不变——同 msg_seq)
        }
        Outcome outcome;
        outcome.status = DeferredOrPermanent(error);
        outcome.error = std::move(error);
        return outcome;
    }
    // 两次 Unauthorized:token 换了仍被拒——鉴权配置问题,不无限循环。
    QqApiError error;
    error.kind = QqApiErrorKind::Unauthorized;
    error.detail = "unauthorized after token refresh";
    Outcome outcome;
    outcome.status = Outcome::Status::PermanentFail;
    outcome.error = std::move(error);
    return outcome;
}

QqMessageSender::AckOutcome QqMessageSender::AckInteraction(const std::string& interaction_id,
                                                             int code) {
    // PUT /interactions/{id}(官方回应接口页):body {"code": N};2xx 空
    // 对象即成功。同一 id 只能回应一次——失败不重试(重试既可能撞"只能
    // 一次"的墙,客户端等待也已超时),失败分型如实回给调用方记账。
    AckOutcome outcome;
    const auto token = options_.tokens->GetValidToken();
    if (!token.has_value()) {
        outcome.error.kind = QqApiErrorKind::NetworkError;
        outcome.error.detail = "token: " + token.error().detail;
        return outcome;
    }
    QqHttpRequest request;
    request.method = "PUT";
    request.url = options_.api_base + InteractionAckPath(interaction_id);
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Authorization", "QQBot " + *token);
    request.body = BuildInteractionAckPayload(code).dump();
    const auto response = options_.http(request);
    if (!response.has_value()) {
        outcome.error.kind = QqApiErrorKind::NetworkError;
        outcome.error.detail = response.error();
        return outcome;
    }
    if (response->status >= 200 && response->status < 300) {
        // 本端点的成功合同(A03):官方 API 指南列 204 为"成功且无正文"——
        // PUT 回应允许空成功;空体(含纯空白)放行。非空响应必须对得上账:
        // 非 JSON / 非 object = 损坏,不冒充 Acked;JSON object 查
        // code/err_code(缺或 0 = 成功,非 0 = 平台拒绝,非法/冲突 = 无法
        // 核对)——一律不入 Acked。
        bool blank = true;
        for (const char c : response->body) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                blank = false;
                break;
            }
        }
        if (blank) {
            outcome.status = AckStatus::Acked;
            return outcome;
        }
        const auto parsed =
            nlohmann::json::parse(response->body, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            outcome.error.kind = QqApiErrorKind::InvalidResponse;
            outcome.error.http_status = response->status;
            outcome.error.detail = "interaction ack 2xx body not a json object";
            return outcome;
        }
        const QqErrorBodyShape shape = ParseQqErrorBody(response->body);
        const auto effective = QqErrorEffectiveCode(shape);
        const bool has_code_field = shape.has_code || shape.has_err_code;
        if (has_code_field && (shape.conflict || !effective.has_value())) {
            outcome.error = ClassifyQqSendFailure(response->status, response->body);
            return outcome;  // 码不可判定:不冒充 Acked
        }
        if (effective.has_value() && *effective != 0) {
            outcome.error = ClassifyQqSendFailure(response->status, response->body);
            return outcome;
        }
        outcome.status = AckStatus::Acked;
        return outcome;
    }
    outcome.error = ClassifyQqSendFailure(response->status, response->body);
    return outcome;
}

}  // namespace lubancode::channel::qq
