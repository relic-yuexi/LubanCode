#include "channel/feishu/feishu_messages.hpp"

#include <utility>

namespace lubancode::channel::feishu {

namespace {

FeishuMessageSender::Outcome::Status DeferredOrPermanent(const FeishuApiError& error) {
    switch (error.kind) {
        case FeishuApiErrorKind::RateLimited:
        case FeishuApiErrorKind::ServerError:
        case FeishuApiErrorKind::NetworkError:
            return FeishuMessageSender::Outcome::Status::DeferredRetry;
        default:
            return FeishuMessageSender::Outcome::Status::PermanentFail;
    }
}

}  // namespace

FeishuMessageSender::Outcome FeishuMessageSender::SendReply(const FeishuReplyRequest& request) {
    // Unauthorized(401)只走一次受控刷新:attempt 0 失败 → Invalidate →
    // attempt 1;再失败如实报(鉴权配置问题,不无限循环)。
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto token = options_.tokens->GetValidToken();
        if (!token.has_value()) {
            FeishuApiError error;
            error.kind = FeishuApiErrorKind::NetworkError;
            switch (token.error().kind) {
                case FeishuTokenManager::ErrorKind::RateLimited:
                    error.kind = FeishuApiErrorKind::RateLimited;
                    break;
                case FeishuTokenManager::ErrorKind::ServerError:
                    error.kind = FeishuApiErrorKind::ServerError;
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

        FeishuHttpRequest http_request;
        http_request.method = "POST";
        http_request.url = options_.open_base + ReplyPath(request.message_id);
        http_request.headers.emplace_back("Content-Type", "application/json");
        http_request.headers.emplace_back("Authorization", "Bearer " + *token);
        http_request.body = BuildReplyPayload(request.text).dump();

        const auto response = options_.http(http_request);
        if (!response.has_value()) {
            FeishuApiError error;
            error.kind = FeishuApiErrorKind::NetworkError;
            error.detail = response.error();
            Outcome outcome;
            outcome.status = Outcome::Status::DeferredRetry;
            outcome.error = std::move(error);
            return outcome;
        }
        if (response->status >= 200 && response->status < 300) {
            const auto parsed = nlohmann::json::parse(response->body, nullptr,
                                                      /*allow_exceptions=*/false);
            if (parsed.is_discarded()) {
                // 2xx 但解不出:平台状态未知,不冒充送达(同 QQ A03 纪律)。
                FeishuApiError error;
                error.kind = FeishuApiErrorKind::InvalidResponse;
                error.http_status = response->status;
                error.detail = "reply response not json";
                Outcome outcome;
                outcome.status = Outcome::Status::PermanentFail;
                outcome.error = std::move(error);
                return outcome;
            }
            std::string parse_error;
            const auto reply_result = ParseReplyResponse(parsed, &parse_error);
            if (reply_result.has_value()) {
                Outcome outcome;
                outcome.status = Outcome::Status::Sent;
                outcome.provider_message_id = reply_result->provider_message_id;
                return outcome;
            }
            FeishuApiError error = ClassifyFeishuApiFailure(response->status,
                                                            response->body);
            if (error.kind == FeishuApiErrorKind::UnknownError &&
                error.platform_code == 0) {
                // 形状错(成功合同无法核对),不是平台业务拒绝。
                error.kind = FeishuApiErrorKind::InvalidResponse;
                error.detail += " (" + parse_error + ")";
            }
            Outcome outcome;
            outcome.status = DeferredOrPermanent(error);
            outcome.error = std::move(error);
            return outcome;
        }

        FeishuApiError error = ClassifyFeishuApiFailure(response->status,
                                                        response->body);
        if (error.kind == FeishuApiErrorKind::Unauthorized && attempt == 0) {
            options_.tokens->Invalidate();
            continue;  // 刷 token 再试一次(同锚同载荷)
        }
        Outcome outcome;
        outcome.status = DeferredOrPermanent(error);
        outcome.error = std::move(error);
        return outcome;
    }
    // 两次 Unauthorized:token 换了仍被拒——鉴权配置问题。
    FeishuApiError error;
    error.kind = FeishuApiErrorKind::Unauthorized;
    error.detail = "unauthorized after token refresh";
    Outcome outcome;
    outcome.status = Outcome::Status::PermanentFail;
    outcome.error = std::move(error);
    return outcome;
}

}  // namespace lubancode::channel::feishu
