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

ExpiringTokenCache<FeishuTokenManager::Error> FeishuTokenManager::MakeCache() {
    ExpiringTokenCache<Error>::Options cache_options;
    cache_options.now_ms = options_.now_ms;
    cache_options.refresh_margin_secs = options_.refresh_margin_secs;
    cache_options.refresh = [this]() { return Refresh(); };
    return ExpiringTokenCache<Error>(std::move(cache_options));
}

void FeishuTokenManager::Invalidate() {
    cache_.Invalidate();
}

std::expected<std::string, FeishuTokenManager::Error> FeishuTokenManager::GetValidToken() {
    return cache_.GetValid();
}

std::expected<ExpiringTokenCache<FeishuTokenManager::Error>::Refreshed,
              FeishuTokenManager::Error>
FeishuTokenManager::Refresh() {
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
    return ExpiringTokenCache<Error>::Refreshed{token->tenant_access_token,
                                                token->expire_secs};
}

}  // namespace lubancode::channel::feishu
