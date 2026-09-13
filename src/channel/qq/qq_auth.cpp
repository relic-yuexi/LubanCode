#include "channel/qq/qq_auth.hpp"

#include <utility>

#include "channel/qq/qq_proto.hpp"

namespace lubancode::channel::qq {

namespace {

QqTokenManager::ErrorKind StatusToErrorKind(int status) {
    if (status == 429) {
        return QqTokenManager::ErrorKind::RateLimited;
    }
    if (status >= 500) {
        return QqTokenManager::ErrorKind::ServerError;
    }
    return QqTokenManager::ErrorKind::InvalidCredentials;
}

}  // namespace

void QqTokenManager::Invalidate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
    expires_at_ms_ = 0;
}

std::expected<std::string, QqTokenManager::Error> QqTokenManager::GetValidToken() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (token_.has_value() && options_.now_ms() < expires_at_ms_) {
        return *token_;
    }
    return RefreshLocked();
}

std::expected<std::string, QqTokenManager::Error> QqTokenManager::RefreshLocked() {
    QqHttpRequest request;
    request.method = "POST";
    request.url = options_.token_url;
    request.headers.emplace_back("Content-Type", "application/json");
    request.body = BuildAccessTokenRequest(options_.app_id, options_.client_secret).dump();

    const auto response = options_.http(request);
    if (!response.has_value()) {
        // 传输失败(detail 由 http seam 保证脱敏)。
        return std::unexpected(Error{ErrorKind::NetworkError, response.error()});
    }
    if (response->status < 200 || response->status >= 300) {
        return std::unexpected(Error{
            StatusToErrorKind(response->status),
            "token endpoint status " + std::to_string(response->status)});
    }
    const auto parsed = nlohmann::json::parse(response->body, nullptr,
                                              /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        return std::unexpected(Error{ErrorKind::InvalidCredentials,
                                     "token response not json"});
    }
    std::string parse_error;
    const auto token = ParseAccessTokenResponse(parsed, &parse_error);
    if (!token.has_value()) {
        return std::unexpected(Error{ErrorKind::InvalidCredentials, parse_error});
    }
    token_ = token->access_token;
    const std::int64_t lifetime_ms =
        (token->expires_in_secs - options_.refresh_margin_secs) * 1000;
    expires_at_ms_ = options_.now_ms() + (lifetime_ms > 0 ? lifetime_ms : 0);
    return *token_;
}

}  // namespace lubancode::channel::qq
