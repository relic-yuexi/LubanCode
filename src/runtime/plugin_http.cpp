// plugin_http 合同的实现:错误码表、URL 规范化、网络账对账与 fake
// transport。不碰 cpr/libcurl(真传输是阶段 2)、不碰 Lua state(阶段 3)。
#include "runtime/plugin_http.hpp"

#include <utility>

namespace lubancode::runtime {

std::string_view LuaHostErrorCodeName(LuaHostErrorCode code) {
    switch (code) {
        case LuaHostErrorCode::NoActiveToolCall:
            return "no_active_tool_call";
        case LuaHostErrorCode::NetworkNotDeclared:
            return "network_not_declared";
        case LuaHostErrorCode::NetworkTargetDenied:
            return "network_target_denied";
        case LuaHostErrorCode::NetworkAddressDenied:
            return "network_address_denied";
        case LuaHostErrorCode::SecretNotDeclared:
            return "secret_not_declared";
        case LuaHostErrorCode::SecretMissing:
            return "secret_missing";
        case LuaHostErrorCode::InvalidRequest:
            return "invalid_request";
        case LuaHostErrorCode::RequestTooLarge:
            return "request_too_large";
        case LuaHostErrorCode::ResponseTooLarge:
            return "response_too_large";
        case LuaHostErrorCode::Timeout:
            return "timeout";
        case LuaHostErrorCode::Cancelled:
            return "cancelled";
        case LuaHostErrorCode::DnsFailed:
            return "dns_failed";
        case LuaHostErrorCode::TlsFailed:
            return "tls_failed";
        case LuaHostErrorCode::NetworkFailed:
            return "network_failed";
        case LuaHostErrorCode::HttpStatus:
            return "http_status";
        case LuaHostErrorCode::InvalidJson:
            return "invalid_json";
        case LuaHostErrorCode::ConcurrencyLimit:
            return "concurrency_limit";
    }
    return "network_failed";
}

bool LuaHostErrorCodeDefaultRetryable(LuaHostErrorCode code) {
    switch (code) {
        case LuaHostErrorCode::Timeout:
        case LuaHostErrorCode::DnsFailed:
        case LuaHostErrorCode::NetworkFailed:
        case LuaHostErrorCode::ConcurrencyLimit:
            return true;
        default:
            // tls_failed 的缺省 false(证书类不可重试),连接类由 transport
            // 层按分型覆盖;http_status 由 Lua 按 408/429/5xx 自判。
            return false;
    }
}

std::string LuaHostErrorCodeDefaultMessage(LuaHostErrorCode code) {
    switch (code) {
        case LuaHostErrorCode::NoActiveToolCall:
            return "Host API 只在工具调用的动态作用域里可用(顶层加载期零网络、零 Secret 解析)";
        case LuaHostErrorCode::NetworkNotDeclared:
            return "manifest 未声明网络权限";
        case LuaHostErrorCode::NetworkTargetDenied:
            return "请求的 scheme/host/port/method 不在 manifest 声明里";
        case LuaHostErrorCode::NetworkAddressDenied:
            return "目标地址落在私网/保留段,或 host 不是可信 DNS 名";
        case LuaHostErrorCode::SecretNotDeclared:
            return "引用了 manifest 未声明的 Secret";
        case LuaHostErrorCode::SecretMissing:
            return "必需的 Secret 没找到";
        case LuaHostErrorCode::InvalidRequest:
            return "请求形状不合法(URL/header/body/auth)";
        case LuaHostErrorCode::RequestTooLarge:
            return "请求超字节帽,未发送";
        case LuaHostErrorCode::ResponseTooLarge:
            return "响应超字节帽,已中止";
        case LuaHostErrorCode::Timeout:
            return "宿主墙钟到点";
        case LuaHostErrorCode::Cancelled:
            return "已取消";
        case LuaHostErrorCode::DnsFailed:
            return "DNS 解析失败";
        case LuaHostErrorCode::TlsFailed:
            return "TLS/证书失败";
        case LuaHostErrorCode::NetworkFailed:
            return "网络传输失败";
        case LuaHostErrorCode::HttpStatus:
            return "HTTP 非 2xx 响应(status 原样带回)";
        case LuaHostErrorCode::InvalidJson:
            return "JSON 序列化或解析失败";
        case LuaHostErrorCode::ConcurrencyLimit:
            return "在途请求超过宿主并发上限";
    }
    return "网络传输失败";
}

// ---------------------------------------------------------------------------
// URL 规范化(纯函数)
// ---------------------------------------------------------------------------

std::expected<NormalizedUrl, LuaHostErrorCode> NormalizeHttpUrl(std::string_view raw_url) {
    // scheme:// 后续三段:authority、path、query。fragment 禁。
    const std::size_t scheme_end = raw_url.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return std::unexpected(LuaHostErrorCode::InvalidRequest);
    }
    std::string scheme;
    for (const char c : raw_url.substr(0, scheme_end)) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.')) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        scheme += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (scheme != "https" && scheme != "http") {
        return std::unexpected(LuaHostErrorCode::InvalidRequest);
    }
    if (raw_url.find('#') != std::string_view::npos) {
        return std::unexpected(LuaHostErrorCode::InvalidRequest);  // 禁 fragment
    }
    std::string_view rest = raw_url.substr(scheme_end + 3);
    const std::size_t path_start = rest.find('/');
    const std::size_t query_start = rest.find('?');
    // authority 段:到 path 或 query 先到者为止。
    std::size_t authority_end = path_start;
    if (query_start != std::string_view::npos && (path_start == std::string_view::npos || query_start < path_start)) {
        authority_end = query_start;
    }
    if (authority_end == std::string_view::npos) {
        authority_end = rest.size();
    }
    std::string_view authority = rest.substr(0, authority_end);
    // 禁 userinfo。
    if (authority.find('@') != std::string_view::npos) {
        return std::unexpected(LuaHostErrorCode::InvalidRequest);
    }
    // host[:port]。IPv6 方括号形态剥了壳交给 NormalizeDnsHost(IP 一律拒)。
    std::string_view host_part = authority;
    std::string_view port_part;
    if (!host_part.empty() && host_part.front() == '[') {
        const std::size_t bracket_end = host_part.find(']');
        if (bracket_end == std::string_view::npos) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        port_part = bracket_end + 1 < host_part.size() ? host_part.substr(bracket_end + 1) : std::string_view{};
        host_part = host_part.substr(0, bracket_end + 1);
    } else {
        const std::size_t colon = host_part.rfind(':');
        if (colon != std::string_view::npos) {
            port_part = host_part.substr(colon + 1);
            host_part = host_part.substr(0, colon);
        }
    }
    int explicit_port = 0;  // 0 = 没写端口
    if (!port_part.empty()) {
        if (port_part.find_first_not_of("0123456789") != std::string_view::npos) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        if (port_part.size() > 5) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        for (const char c : port_part) {
            explicit_port = explicit_port * 10 + (c - '0');
            if (explicit_port > 65535) {
                return std::unexpected(LuaHostErrorCode::InvalidRequest);
            }
        }
        if (explicit_port == 0) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
    }
    auto host = NormalizeDnsHost(host_part);
    if (!host.has_value()) {
        // IP 字面量/localhost/.local 一类落地址否决;形状坏落请求否决。
        if (host.error().find("IP 字面量") != std::string::npos ||
            host.error().find("单标签") != std::string::npos || host.error().find(".local") != std::string::npos) {
            return std::unexpected(LuaHostErrorCode::NetworkAddressDenied);
        }
        return std::unexpected(LuaHostErrorCode::InvalidRequest);
    }
    NormalizedUrl out;
    out.scheme = scheme;
    out.host = std::move(*host);
    out.port = explicit_port != 0 ? explicit_port : (scheme == "https" ? 443 : 80);
    out.has_explicit_port = explicit_port != 0;
    if (path_start != std::string_view::npos) {
        out.path = std::string(rest.substr(path_start, query_start == std::string_view::npos
                                                    ? std::string_view::npos
                                                    : query_start - path_start));
        // query 段剥 '?'。
        if (query_start != std::string_view::npos) {
            out.query = std::string(rest.substr(query_start + 1));
        }
    } else {
        out.path = "/";
        if (query_start != std::string_view::npos) {
            out.query = std::string(rest.substr(query_start + 1));
        }
    }
    out.text = out.scheme + "://" + out.host;
    if (out.has_explicit_port) {
        out.text += ":" + std::to_string(out.port);
    }
    out.text += out.path;
    if (!out.query.empty()) {
        out.text += "?" + out.query;
    }
    return out;
}

bool NetworkPermissionAllows(const std::vector<NetworkPermission>& permissions, const NormalizedUrl& url) {
    for (const NetworkPermission& permission : permissions) {
        if (permission.scheme != url.scheme || permission.host != url.host || permission.port != url.port) {
            continue;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// FakeHttpTransport
// ---------------------------------------------------------------------------

void FakeHttpTransport::EnqueueResponse(HttpExchangeResponse response) {
    Scripted scripted;
    scripted.is_error = false;
    scripted.response = std::move(response);
    script_.push_back(std::move(scripted));
}

void FakeHttpTransport::EnqueueError(HttpTransportError error) {
    Scripted scripted;
    scripted.is_error = true;
    scripted.error = std::move(error);
    script_.push_back(std::move(scripted));
}

void FakeHttpTransport::ClearScript() { script_.clear(); }

std::expected<HttpExchangeResponse, HttpTransportError> FakeHttpTransport::Execute(
    const HttpExchangeRequest& request, const EffectiveHttpLimits& limits, const std::atomic<bool>* cancel) {
    Call call;
    call.request = request;
    call.limits = limits;
    call.cancel_observed = cancel != nullptr;
    calls_.push_back(std::move(call));
    if (script_.empty()) {
        HttpTransportError error;
        error.code = LuaHostErrorCode::NetworkFailed;
        error.message = "fake transport 没有编排结果";
        return std::unexpected(error);
    }
    Scripted scripted = std::move(script_.front());
    script_.erase(script_.begin());
    if (scripted.is_error) {
        return std::unexpected(std::move(scripted.error));
    }
    return std::move(scripted.response);
}

}  // namespace lubancode::runtime
