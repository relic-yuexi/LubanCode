#include "channel/feishu/feishu_auth.hpp"

#include <utility>

#include "channel/feishu/feishu_proto.hpp"

namespace lubancode::channel::feishu {

namespace {

FeishuTokenManager::ErrorKind StatusToErrorKind(int status) {
    if (status == 429) {
        return FeishuTokenManager::ErrorKind::RateLimited;
    }
    if (status >= 500) {
        return FeishuTokenManager::ErrorKind::ServerError;
    }
    return FeishuTokenManager::ErrorKind::InvalidCredentials;
}

}  // namespace

void FeishuTokenManager::Invalidate() {
    const std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
    expires_at_ms_ = 0;
}

std::expected<std::string, FeishuTokenManager::Error> FeishuTokenManager::GetValidToken() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (token_.has_value() && options_.now_ms() < expires_at_ms_) {
        return *token_;
    }
    return RefreshLocked();
}

std::expected<std::string, FeishuTokenManager::Error> FeishuTokenManager::RefreshLocked() {
    FeishuHttpRequest request;
    request.method = "POST";
    request.url = options_.token_url;
    request.headers.emplace_back("Content-Type", "application/json");
    request.body = BuildTenantTokenRequest(options_.app_id, options_.app_secret).dump();

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
    const auto token = ParseTenantTokenResponse(parsed, &parse_error);
    if (!token.has_value()) {
        return std::unexpected(Error{ErrorKind::InvalidCredentials, parse_error});
    }
    token_ = token->tenant_access_token;
    const std::int64_t lifetime_ms =
        (token->expire_secs - options_.refresh_margin_secs) * 1000;
    expires_at_ms_ = options_.now_ms() + (lifetime_ms > 0 ? lifetime_ms : 0);
    return *token_;
}

}  // namespace lubancode::channel::feishu
