// plugin_http 合同的实现:错误码表、URL 规范化、网络账对账、fake
// transport(以上阶段 0),加上真传输 CprBoundedHttpTransport、Secret 头
// 注入、并发帽、JSON 件与单笔编排(阶段 2)。不碰 Lua state(阶段 3)。
#include "runtime/plugin_http.hpp"

#include <algorithm>
#include <utility>

#include <cpr/cpr.h>

namespace lubancode::runtime {
namespace {

// ASCII 小写化/大小写不敏感比较(避开 locale,头名只认 ASCII)。
char AsciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool AsciiIEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (AsciiLower(a[i]) != AsciiLower(b[i])) {
            return false;
        }
    }
    return true;
}

std::string AsciiLowered(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out += AsciiLower(c);
    }
    return out;
}

// RFC 7230 token 字符(头名合法集)。
bool IsHeaderTokenChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '!' || c == '#' ||
           c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' ||
           c == '_' || c == '`' || c == '|' || c == '~';
}

// 头值禁 CTL 与 DEL(防 smuggle);可打印 ASCII 与 UTF-8 字节放行。
bool HasHeaderValueForbiddenChar(std::string_view value) {
    for (const char c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            return true;
        }
    }
    return false;
}

HttpTransportError MakeTransportError(LuaHostErrorCode code, std::string message) {
    HttpTransportError error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

// 响应头过滤表(§6.2):带凭据/代理协商的字段不进 Lua 表。
bool IsFilteredResponseHeaderName(std::string_view lower_name) {
    return lower_name == "set-cookie" || lower_name == "set-cookie2" || lower_name == "proxy-authenticate" ||
           lower_name == "proxy-authorization";
}

}  // namespace


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

// ---------------------------------------------------------------------------
// 禁写头表与 Secret 头注入(§5.4/§6.2)
// ---------------------------------------------------------------------------

bool IsForbiddenLuaHeaderName(std::string_view name) {
    return AsciiIEquals(name, "authorization") || AsciiIEquals(name, "proxy-authorization") ||
           AsciiIEquals(name, "cookie") || AsciiIEquals(name, "host") || AsciiIEquals(name, "content-length");
}

std::expected<std::vector<std::pair<std::string, std::string>>, LuaHostErrorCode> BuildOutgoingHeaders(
    const std::vector<std::pair<std::string, std::string>>& lua_headers, const HttpAuthSpec* auth,
    std::string_view secret_value) {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(lua_headers.size() + 1);
    for (const auto& [name, value] : lua_headers) {
        if (name.empty()) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        for (const char c : name) {
            if (!IsHeaderTokenChar(c)) {
                return std::unexpected(LuaHostErrorCode::InvalidRequest);
            }
        }
        if (IsForbiddenLuaHeaderName(name)) {
            // 禁写表:Authorization 一族由宿主代填,Lua 自写即拒(§5.4)。
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        if (HasHeaderValueForbiddenChar(value)) {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
        out.emplace_back(name, value);
    }

    if (auth != nullptr && !secret_value.empty()) {
        if (auth->type == "bearer") {
            // bearer 的形状是死的:Authorization: Bearer <secret>。
            out.emplace_back("Authorization", "Bearer " + std::string(secret_value));
        } else if (auth->type == "header") {
            // 只认三类规范名,免得拿 Secret 拼任意业务字段(§6.2)。
            static constexpr std::string_view kCanonicalNames[] = {"Authorization", "X-Api-Key", "Api-Key"};
            const std::string_view* canonical = nullptr;
            for (const std::string_view& candidate : kCanonicalNames) {
                if (AsciiIEquals(auth->name, candidate)) {
                    canonical = &candidate;
                    break;
                }
            }
            if (canonical == nullptr) {
                return std::unexpected(LuaHostErrorCode::InvalidRequest);
            }
            out.emplace_back(std::string(*canonical), auth->prefix.empty() ? std::string(secret_value)
                                                                           : auth->prefix + " " + std::string(secret_value));
        } else {
            return std::unexpected(LuaHostErrorCode::InvalidRequest);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 并发帽(§8.5)
// ---------------------------------------------------------------------------

std::shared_ptr<InFlightGate> GlobalHttpInFlightGate() {
    static const std::shared_ptr<InFlightGate> gate = std::make_shared<InFlightGate>(kHttpMaxInFlightGlobal);
    return gate;
}

// ---------------------------------------------------------------------------
// CprBoundedHttpTransport
// ---------------------------------------------------------------------------

CprBoundedHttpTransport::CprBoundedHttpTransport(Options options)
    : options_(std::move(options)),
      global_gate_(options_.global_gate != nullptr ? options_.global_gate : GlobalHttpInFlightGate()),
      plugin_gate_(options_.per_plugin_max_in_flight > 0 ? options_.per_plugin_max_in_flight
                                                         : kHttpMaxInFlightPerPlugin) {
    if (options_.dns != nullptr) {
        dns_ = options_.dns;
    } else {
        owned_dns_ = std::make_unique<net::SystemDnsResolver>();
        dns_ = owned_dns_.get();
    }
}

std::expected<HttpExchangeResponse, HttpTransportError> CprBoundedHttpTransport::Execute(
    const HttpExchangeRequest& request, const EffectiveHttpLimits& limits, const std::atomic<bool>* cancel) {
    // 并发帽(§8.5):每插件与全局两道闸,超限即回,不排无限队。
    if (!plugin_gate_.TryAcquire()) {
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::ConcurrencyLimit,
            "本插件在途 HTTP 请求已达 " + std::to_string(plugin_gate_.max_in_flight()) + " 笔上限,未排队"));
    }
    const InFlightGate::Releaser plugin_releaser(&plugin_gate_);
    if (!global_gate_->TryAcquire()) {
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::ConcurrencyLimit,
            "全局在途 HTTP 请求已达 " + std::to_string(global_gate_->max_in_flight()) + " 笔上限,未排队"));
    }
    const InFlightGate::Releaser global_releaser(global_gate_.get());

    // §8.4 第 1 段:调用前已置位。
    if (cancel != nullptr && cancel->load()) {
        return std::unexpected(
            MakeTransportError(LuaHostErrorCode::Cancelled, LuaHostErrorCodeDefaultMessage(LuaHostErrorCode::Cancelled)));
    }

    // §8.3:URL 帽在解析前落锤。
    if (static_cast<std::int64_t>(request.url.size()) > limits.url_bytes) {
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::RequestTooLarge,
            "URL 长 " + std::to_string(request.url.size()) + " 字节,超过帽 " + std::to_string(limits.url_bytes)));
    }

    // §8.2 第 1/3 步:完整解析;禁 userinfo/fragment;host 不是 IP 字面量/
    // localhost/.local(复用阶段 0 纯函数,防御纵深——接口写明收已规范化
    // URL,这里不再信调用方)。
    const auto normalized = NormalizeHttpUrl(request.url);
    if (!normalized.has_value()) {
        return std::unexpected(MakeTransportError(normalized.error(),
                                                  LuaHostErrorCodeDefaultMessage(normalized.error())));
    }

    // 方法形状:只认大写 GET/POST;GET 不带体。
    if (request.method != "GET" && request.method != "POST") {
        return std::unexpected(
            MakeTransportError(LuaHostErrorCode::InvalidRequest, "method 只认 GET/POST: " + request.method));
    }
    if (request.method == "GET" && !request.body.empty()) {
        return std::unexpected(MakeTransportError(LuaHostErrorCode::InvalidRequest, "GET 不带请求体"));
    }

    // 头表防御(禁写表的 Lua 侧拦在 BuildOutgoingHeaders;这里再拦四枚
    // 宿主代填名与 CTL,Authorization 例外——宿主自己注入的 bearer/header
    // auth 头合法在表)。
    for (const auto& [name, value] : request.headers) {
        if (AsciiIEquals(name, "proxy-authorization") || AsciiIEquals(name, "cookie") || AsciiIEquals(name, "host") ||
            AsciiIEquals(name, "content-length")) {
            return std::unexpected(MakeTransportError(LuaHostErrorCode::InvalidRequest, "请求头由宿主代填: " + name));
        }
        if (HasHeaderValueForbiddenChar(value) || HasHeaderValueForbiddenChar(name) || name.empty()) {
            return std::unexpected(MakeTransportError(LuaHostErrorCode::InvalidRequest, "请求头含非法字符"));
        }
    }

    // §8.3:请求头帽按最终头表(Secret 注入后)计,发包头前落锤。
    // 口径:每枚头按 name + ": " + value + CRLF 记 4 字节开销。
    std::int64_t header_bytes = 0;
    for (const auto& [name, value] : request.headers) {
        header_bytes += static_cast<std::int64_t>(name.size() + value.size()) + 4;
    }
    if (header_bytes > limits.request_header_bytes) {
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::RequestTooLarge,
            "请求头合计 " + std::to_string(header_bytes) + " 字节,超过帽 " + std::to_string(limits.request_header_bytes)));
    }

    // §8.3:请求体帽(JSON dump/body 接收后、cpr 前)。
    if (static_cast<std::int64_t>(request.body.size()) > limits.request_body_bytes) {
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::RequestTooLarge,
            "请求体 " + std::to_string(request.body.size()) + " 字节,超过帽 " +
                std::to_string(limits.request_body_bytes)));
    }

    // §8.2 第 2 步:scheme/host/port 命中精确声明,method 命中该声明的表。
    const NetworkPermission* matched = nullptr;
    for (const NetworkPermission& permission : options_.permissions) {
        if (permission.scheme == normalized->scheme && permission.host == normalized->host &&
            permission.port == normalized->port) {
            matched = &permission;
            break;
        }
    }
    if (matched == nullptr) {
        const LuaHostErrorCode code = options_.permissions.empty()
                                          ? LuaHostErrorCode::NetworkNotDeclared
                                          : LuaHostErrorCode::NetworkTargetDenied;
        return std::unexpected(MakeTransportError(code, LuaHostErrorCodeDefaultMessage(code)));
    }
    if (std::find(matched->methods.begin(), matched->methods.end(), request.method) == matched->methods.end()) {
        return std::unexpected(MakeTransportError(LuaHostErrorCode::NetworkTargetDenied,
                                                  "method " + request.method + " 不在声明表里"));
    }

    // §8.2 第 4 步:DNS 解析 + 候选地址安全分类(每个候选都不落禁连段,
    // 有一枚落了即整体否决——最严口径,不给 rebinding 留缝)。
    const auto resolved = dns_->Resolve(normalized->host);
    if (!resolved.has_value()) {
        return std::unexpected(MakeTransportError(LuaHostErrorCode::DnsFailed, resolved.error()));
    }
    std::string pinned_addr;
    for (const std::string& candidate : *resolved) {
        if (net::IsRoutablePublicAddress(candidate)) {
            if (pinned_addr.empty()) {
                pinned_addr = candidate;
            }
            continue;
        }
        // loopback 例外只开给测试口(本机假服务);私网/保留/metadata 与
        // loopback 混排时按最严口径整体否决。
        if (options_.allow_loopback_targets && net::IsLoopbackAddress(candidate)) {
            if (pinned_addr.empty()) {
                pinned_addr = candidate;
            }
            continue;
        }
        const auto blocked = net::BlockedAddressRange(candidate);
        return std::unexpected(MakeTransportError(
            LuaHostErrorCode::NetworkAddressDenied,
            "DNS 候选地址 " + candidate + " 落禁连段: " + blocked.value_or("不可解析")));
    }
    if (pinned_addr.empty()) {
        return std::unexpected(MakeTransportError(LuaHostErrorCode::NetworkAddressDenied,
                                                  "DNS 候选地址全部不可放行"));
    }

    // §8.2 第 5 步 + §8.3/§8.4:连接钉已验地址,交给中立底座。timeout
    // 一枚 knob 同担连接帽与硬墙钟(§11 只有一枚 timeout)。
    net::FullHttpRequest net_request;
    net_request.method = request.method;
    net_request.url = normalized->text;
    net_request.headers = request.headers;
    net_request.body = request.body;
    net::FullHttpLimits net_limits;
    const std::int64_t timeout_ms = limits.timeout_ms > 0 ? limits.timeout_ms : kHttpTimeoutDefaultMs;
    net_limits.connect_timeout_ms = timeout_ms;
    net_limits.hard_timeout_ms = timeout_ms;
    net_limits.response_header_bytes = limits.response_header_bytes;
    net_limits.response_body_bytes = limits.response_body_bytes;
    net::PinnedDnsResolve pin;
    pin.host = normalized->host;
    pin.port = normalized->port;
    pin.addr = pinned_addr;

    auto performed = net::PerformFullHttpRequest(net_request, net_limits, cancel, &pin);
    if (!performed.has_value()) {
        const net::FullHttpError& raw = performed.error();
        LuaHostErrorCode code = LuaHostErrorCode::NetworkFailed;
        switch (raw.kind) {
            case net::FullHttpErrorKind::Cancelled:
                code = LuaHostErrorCode::Cancelled;
                break;
            case net::FullHttpErrorKind::Timeout:
                code = LuaHostErrorCode::Timeout;
                break;
            case net::FullHttpErrorKind::ResponseHeaderTooLarge:
            case net::FullHttpErrorKind::ResponseBodyTooLarge:
                code = LuaHostErrorCode::ResponseTooLarge;
                break;
            default:
                // DnsFailed/TlsFailed/NetworkFailed:按 curl 码走 §11 映射。
                code = ClassifyCurlErrorCode(raw.curl_code);
                break;
        }
        // curl 原话拼在后面留排查线索;curl 文案不含我们的头与体。
        std::string message = raw.message;
        if (!raw.curl_message.empty()) {
            message += " (" + raw.curl_message + ")";
        }
        return std::unexpected(MakeTransportError(code, std::move(message)));
    }

    // 响应整形:过滤敏感头、小写化;3xx/4xx/5xx 原样交(§11:HTTP 非 2xx
    // 不混作网络错,status 原样带回,由 Lua 翻厂商语义)。不跟重定向,
    // final_url 即请求 URL。
    HttpExchangeResponse out;
    out.status = performed->status;
    out.headers.reserve(performed->headers.size());
    for (auto& [name, value] : performed->headers) {
        std::string lower_name = AsciiLowered(name);
        if (IsFilteredResponseHeaderName(lower_name)) {
            continue;
        }
        out.headers.emplace_back(std::move(lower_name), std::move(value));
    }
    out.body = std::move(performed->body);
    out.final_url = normalized->text;
    return out;
}

// ---------------------------------------------------------------------------
// §11 的 cpr/libcurl 错误映射(纯函数)。分型真源在 net 层
// (net::ClassifyCurlErrorCode);这里把传输分型叠到 §11 的稳定码上。
// ---------------------------------------------------------------------------

LuaHostErrorCode ClassifyCurlErrorCode(long curl_code) {
    switch (net::ClassifyCurlErrorCode(curl_code)) {
        case net::FullHttpErrorKind::DnsFailed:
            return LuaHostErrorCode::DnsFailed;
        case net::FullHttpErrorKind::TlsFailed:
            return LuaHostErrorCode::TlsFailed;
        case net::FullHttpErrorKind::Timeout:
            // ConnectTimeout 与硬墙钟共用 CURLE_OPERATION_TIMEDOUT,统一归
            // timeout;cancelled 是我们自己掐的流,不走这条映射,不串码。
            return LuaHostErrorCode::Timeout;
        default:
            return LuaHostErrorCode::NetworkFailed;
    }
}

// ---------------------------------------------------------------------------
// JSON 件(§6.2/§11 invalid_json)
// ---------------------------------------------------------------------------

std::expected<std::string, LuaHostErrorCode> SerializeJsonBody(const nlohmann::json& value) {
    try {
        return value.dump();
    } catch (const nlohmann::json::exception&) {
        // 插件作者是外人:坏树响亮报,不清洗(与 provider 路"清洗保会话"
        // 的取舍不同——那边是历史脏数据,这边是当下就能修的合同错)。
        return std::unexpected(LuaHostErrorCode::InvalidJson);
    }
}

std::expected<std::optional<nlohmann::json>, LuaHostErrorCode> ParseJsonResponseBody(std::string_view content_type,
                                                                                      std::string_view body) {
    if (AsciiLowered(content_type).find("json") == std::string::npos || body.empty()) {
        return std::optional<nlohmann::json>{};
    }
    try {
        return std::optional<nlohmann::json>(nlohmann::json::parse(body));
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(LuaHostErrorCode::InvalidJson);
    }
}

// ---------------------------------------------------------------------------
// 单笔编排(阶段 3 的 luban.http.request 落点;阶段 2 的端到端测试面)
// ---------------------------------------------------------------------------

std::expected<PluginHttpApiResponse, PluginHttpCallError> ExecutePluginHttp(const PluginHttpApiRequest& request,
                                                                           const PluginHttpCallSpec& spec) {
    const auto fail = [](LuaHostErrorCode code, std::string message, int status = 0) {
        PluginHttpCallError error;
        error.code = code;
        error.message = std::move(message);
        error.status = status;
        error.retryable = LuaHostErrorCodeDefaultRetryable(code);
        return std::unexpected(error);
    };

    if (spec.transport == nullptr) {
        return fail(LuaHostErrorCode::NetworkFailed, "HTTP transport 未接线(宿主装配缺口)");
    }

    // 方法形状(§6.2 入参表)。
    if (request.method != "GET" && request.method != "POST") {
        return fail(LuaHostErrorCode::InvalidRequest, "method 只认 GET/POST: " + request.method);
    }
    if (request.has_json && request.has_body) {
        return fail(LuaHostErrorCode::InvalidRequest, "json 与 body 只能二选一");
    }
    std::string body;
    if (request.has_json) {
        auto serialized = SerializeJsonBody(request.json);
        if (!serialized.has_value()) {
            return fail(serialized.error(), "请求 json 序列化失败");
        }
        body = std::move(*serialized);
    } else if (request.has_body) {
        body = request.body;
    }
    if (request.method == "GET" && !body.empty()) {
        return fail(LuaHostErrorCode::InvalidRequest, "GET 不带请求体");
    }

    // Secret 解析(§7.1)与注入(§5.4:只在最终发包头一刻拼)。明文寿命
    // = 本函数栈:SecretValue RAII 持有,函数返回即覆写。
    std::optional<SecretValue> held_secret;
    std::string_view secret_view;
    if (request.has_auth) {
        if (request.auth.type != "bearer" && request.auth.type != "header") {
            return fail(LuaHostErrorCode::InvalidRequest, "auth.type 只认 bearer/header");
        }
        if (request.auth.type == "bearer" && (!request.auth.name.empty() || !request.auth.prefix.empty())) {
            return fail(LuaHostErrorCode::InvalidRequest, "auth.type=bearer 不收 name/prefix");
        }
        const SecretDeclaration* declaration = nullptr;
        for (const SecretDeclaration& candidate : spec.secrets) {
            if (candidate.id == request.auth.secret_id) {
                declaration = &candidate;
                break;
            }
        }
        if (declaration == nullptr) {
            return fail(LuaHostErrorCode::SecretNotDeclared, "Secret 未声明: " + request.auth.secret_id);
        }
        if (spec.secret_resolver == nullptr) {
            return fail(LuaHostErrorCode::SecretMissing, "SecretResolver 未接线(宿主装配缺口)");
        }
        auto resolved = spec.secret_resolver->Resolve(*declaration);
        if (!resolved.has_value()) {
            // required 缺失:SecretMissing;optional 缺失:匿名降级(§6.2)。
            if (!request.auth.optional) {
                const LuaHostErrorCode code = resolved.error().issue == SecretResolveIssue::NotDeclared
                                                  ? LuaHostErrorCode::SecretNotDeclared
                                                  : LuaHostErrorCode::SecretMissing;
                return fail(code, resolved.error().message);
            }
        } else if (resolved->HasValue()) {
            held_secret = std::move(*resolved);
            secret_view = held_secret->View();
        } else if (!request.auth.optional) {
            return fail(LuaHostErrorCode::SecretMissing, "必需的 Secret 没找到: " + request.auth.secret_id);
        }
    }

    // Lua 头表 + json 体自动 Content-Type(Lua 没写才补)。
    std::vector<std::pair<std::string, std::string>> lua_headers = request.headers;
    if (request.has_json) {
        bool has_content_type = false;
        for (const auto& [name, value] : lua_headers) {
            if (AsciiIEquals(name, "content-type")) {
                has_content_type = true;
                break;
            }
        }
        if (!has_content_type) {
            lua_headers.emplace_back("Content-Type", "application/json");
        }
    }

    // 拼最终头表(禁写表/三类规范名在这里拦;表不落日志——§5.4)。
    auto outgoing = BuildOutgoingHeaders(lua_headers, request.has_auth ? &request.auth : nullptr, secret_view);
    if (!outgoing.has_value()) {
        return fail(outgoing.error(), "请求头形状不合法(URL/header/auth)");
    }

    // 生效帽:timeout 只降不升(§6.2)。加括号防 windows.h 的 min 宏
    //(cpr 在 Windows 侧捎带 windows.h)。
    EffectiveHttpLimits limits = spec.limits;
    if (request.timeout_ms > 0) {
        limits.timeout_ms = (std::min)(request.timeout_ms, spec.limits.timeout_ms);
    }

    HttpExchangeRequest exchange;
    exchange.method = request.method;
    exchange.url = request.url;
    exchange.headers = std::move(*outgoing);
    exchange.body = std::move(body);
    auto result = spec.transport->Execute(exchange, limits, spec.cancel);
    if (!result.has_value()) {
        PluginHttpCallError error;
        error.code = result.error().code;
        error.message = result.error().message;
        error.status = result.error().status;
        error.retryable = LuaHostErrorCodeDefaultRetryable(result.error().code);
        return std::unexpected(std::move(error));
    }

    PluginHttpApiResponse out;
    out.status = result->status;
    out.headers = std::move(result->headers);
    out.body = std::move(result->body);
    out.url = result->final_url;
    out.bytes = static_cast<std::int64_t>(out.body.size());

    // json 字段:Content-Type 标了 json 才解析(§6.2);坏 JSON 落
    // invalid_json(§11),status 随错误带回,不静默吞响应。
    std::string content_type;
    for (const auto& [name, value] : out.headers) {
        if (name == "content-type") {
            content_type = value;
            break;
        }
    }
    auto parsed = ParseJsonResponseBody(content_type, out.body);
    if (!parsed.has_value()) {
        return fail(parsed.error(), "响应 json 解析失败", out.status);
    }
    if (parsed->has_value()) {
        out.json = std::move(**parsed);
        out.json_parsed = true;
    }
    return out;
}

}  // namespace lubancode::runtime
