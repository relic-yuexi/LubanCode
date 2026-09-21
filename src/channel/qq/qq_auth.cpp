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

ExpiringTokenCache<QqTokenManager::Error> QqTokenManager::MakeCache() {
    ExpiringTokenCache<Error>::Options cache_options;
    cache_options.now_ms = options_.now_ms;
    cache_options.refresh_margin_secs = options_.refresh_margin_secs;
    cache_options.refresh = [this]() { return Refresh(); };
    return ExpiringTokenCache<Error>(std::move(cache_options));
}

void QqTokenManager::Invalidate() {
    cache_.Invalidate();
}

std::expected<std::string, QqTokenManager::Error> QqTokenManager::GetValidToken() {
    return cache_.GetValid();
}

std::expected<ExpiringTokenCache<QqTokenManager::Error>::Refreshed, QqTokenManager::Error>
QqTokenManager::Refresh() {
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
    return ExpiringTokenCache<Error>::Refreshed{token->access_token,
                                                token->expires_in_secs};
}

}  // namespace lubancode::channel::qq
