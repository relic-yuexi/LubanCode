// 中立 HTTP 传输底座的实现:cpr/libcurl 一笔完整请求 + 回调入口落帽与
// 取消 + getaddrinfo DNS seam。设计与纪律见 http_transport.hpp 文件头。
#include "net/http_transport.hpp"

#include <chrono>
#include <initializer_list>
#include <utility>

#include <cpr/cpr.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace lubancode::net {
namespace {

// Windows 下 getaddrinfo 要 Winsock 先起;全进程一次(静态局部首次调用
// 才构造)。非 Windows 是空操作。
void EnsureSocketsReady() {
#ifdef _WIN32
    struct WinsockInit {
        WinsockInit() {
            WSADATA wsa;
            ::WSAStartup(MAKEWORD(2, 2), &wsa);
        }
    };
    static WinsockInit init;
#endif
}

// ---------------------------------------------------------------------------
// IP 解析
// ---------------------------------------------------------------------------

std::optional<ParsedAddress> ParseIpv4Text(std::string_view text) {
    ParsedAddress out;
    out.is_ipv4 = true;
    std::size_t part = 0;
    std::size_t cursor = 0;
    while (part < 4) {
        if (cursor >= text.size() || text[cursor] < '0' || text[cursor] > '9') {
            return std::nullopt;
        }
        int value = 0;
        std::size_t digits = 0;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
            value = value * 10 + (text[cursor] - '0');
            ++cursor;
            ++digits;
            if (digits > 3 || value > 255) {
                return std::nullopt;
            }
        }
        // 前导零不收("01" 非法,"0" 合法)——与 inet_pton 同一严度。
        if (digits > 1 && text[cursor - digits] == '0') {
            return std::nullopt;
        }
        out.bytes[part] = static_cast<std::uint8_t>(value);
        ++part;
        if (part < 4) {
            if (cursor >= text.size() || text[cursor] != '.') {
                return std::nullopt;
            }
            ++cursor;
        }
    }
    if (cursor != text.size() || part != 4) {
        return std::nullopt;
    }
    return out;
}

std::optional<std::uint16_t> ParseHexGroup(std::string_view text) {
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    std::uint16_t value = 0;
    for (const char c : text) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return std::nullopt;
        }
        value = static_cast<std::uint16_t>((value << 4) | digit);
    }
    return value;
}

// 依 "." 切段;空段(连续点)返回 nullopt。
std::optional<std::vector<std::string_view>> SplitBy(std::string_view text, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t hit = text.find(sep, start);
        if (hit == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, hit - start));
        start = hit + 1;
    }
    for (const std::string_view part : parts) {
        if (part.empty()) {
            return std::nullopt;
        }
    }
    return parts;
}

std::optional<ParsedAddress> ParseIpv6Text(std::string_view text) {
    // "::" 至多一枚;拆成左右两串组,中间补零。
    std::size_t compress = text.find("::");
    if (text.find("::", compress + 2) != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view left = text;
    std::string_view right;
    const bool compressed = compress != std::string_view::npos;
    if (compressed) {
        left = text.substr(0, compress);
        right = text.substr(compress + 2);
        // "::" 贴边是合法的("::1"/"fe80::"),但 ":::" 一类残串到这里已是
        // 空左空右,SplitBy 会拒空段,不需要特判。
    }

    // 每半边解析出 16 位组;末段若是点分 v4,展开成两组。
    const auto parseHalf = [](std::string_view half, std::array<std::uint16_t, 8>& groups,
                              std::size_t& count) -> bool {
        if (half.empty()) {
            return true;  // "::" 贴边
        }
        const auto parts = SplitBy(half, ':');
        if (!parts.has_value()) {
            return false;
        }
        for (std::size_t i = 0; i < parts->size(); ++i) {
            if (i >= 8 || count >= 8) {
                return false;
            }
            if (parts->at(i).find('.') != std::string_view::npos) {
                // 内嵌 v4 只许出现在最后一段。
                if (i + 1 != parts->size()) {
                    return false;
                }
                const auto v4 = ParseIpv4Text(parts->at(i));
                if (!v4.has_value() || count + 2 > 8) {
                    return false;
                }
                groups[count++] = static_cast<std::uint16_t>((v4->bytes[0] << 8) | v4->bytes[1]);
                groups[count++] = static_cast<std::uint16_t>((v4->bytes[2] << 8) | v4->bytes[3]);
                continue;
            }
            const auto group = ParseHexGroup(parts->at(i));
            if (!group.has_value()) {
                return false;
            }
            groups[count++] = *group;
        }
        return true;
    };

    std::array<std::uint16_t, 8> left_groups{};
    std::size_t left_count = 0;
    std::array<std::uint16_t, 8> right_groups{};
    std::size_t right_count = 0;
    if (!parseHalf(left, left_groups, left_count) || !parseHalf(right, right_groups, right_count)) {
        return std::nullopt;
    }

    std::array<std::uint16_t, 8> groups{};
    if (!compressed) {
        if (left_count != 8) {
            return std::nullopt;
        }
        groups = left_groups;
    } else {
        if (left_count + right_count > 7) {
            return std::nullopt;  // 压缩位至少代表一组零
        }
        for (std::size_t i = 0; i < left_count; ++i) {
            groups[i] = left_groups[i];
        }
        for (std::size_t i = 0; i < right_count; ++i) {
            groups[8 - right_count + i] = right_groups[i];
        }
    }

    ParsedAddress out;
    out.is_ipv4 = false;
    for (std::size_t i = 0; i < 8; ++i) {
        out.bytes[i * 2] = static_cast<std::uint8_t>(groups[i] >> 8);
        out.bytes[i * 2 + 1] = static_cast<std::uint8_t>(groups[i] & 0xFF);
    }
    return out;
}

// 前缀匹配(字节粒度按位算):bits 为网络前缀长度。
bool PrefixMatches(const std::uint8_t* bytes, std::initializer_list<std::uint8_t> prefix, int bits) {
    std::size_t i = 0;
    for (const std::uint8_t octet : prefix) {
        const int bit_base = static_cast<int>(i) * 8;
        if (bit_base + 8 <= bits) {
            if (bytes[i] != octet) {
                return false;
            }
        } else {
            const int keep = bits - bit_base;
            const int mask = keep <= 0 ? 0 : (0xFF << (8 - keep)) & 0xFF;
            if ((bytes[i] & mask) != (octet & mask)) {
                return false;
            }
        }
        ++i;
    }
    return true;
}

}  // namespace

std::optional<ParsedAddress> ParseIpAddress(std::string_view text) {
    // "%zone" 只在 v6 里合法,先剥了再分家。
    const std::size_t zone = text.find('%');
    if (zone != std::string_view::npos) {
        text = text.substr(0, zone);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.find(':') == std::string_view::npos) {
        return ParseIpv4Text(text);
    }
    return ParseIpv6Text(text);
}

std::optional<std::string> BlockedAddressRange(std::string_view ip) {
    const auto parsed = ParseIpAddress(ip);
    if (!parsed.has_value()) {
        return std::string("不是合法 IP 地址");
    }
    const std::uint8_t* b = parsed->bytes.data();

    if (parsed->is_ipv4) {
        const std::uint32_t v4 = (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
                                 (static_cast<std::uint32_t>(b[2]) << 8) | b[3];
        const auto in = [v4](std::uint32_t net, int bits) {
            const std::uint32_t mask = bits == 0 ? 0u : ~0u << (32 - bits);
            return (v4 & mask) == (net & mask);
        };
        if (in(0x00000000u, 8)) return std::string("本网段 0.0.0.0/8");
        if (in(0x0A000000u, 8)) return std::string("RFC1918 私网 10.0.0.0/8");
        if (in(0x64400000u, 10)) return std::string("CGNAT 共享段 100.64.0.0/10(含云 metadata)");
        if (in(0x7F000000u, 8)) return std::string("loopback 127.0.0.0/8");
        if (in(0xA9FE0000u, 16)) return std::string("link-local 169.254.0.0/16(含云 metadata 169.254.169.254)");
        if (in(0xAC100000u, 12)) return std::string("RFC1918 私网 172.16.0.0/12");
        if (in(0xC0000000u, 24)) return std::string("IETF 保留 192.0.0.0/24");
        if (in(0xC0000200u, 24)) return std::string("TEST-NET-1 192.0.2.0/24");
        if (in(0xC0586300u, 24)) return std::string("保留段 192.88.99.0/24");
        if (in(0xC0A80000u, 16)) return std::string("RFC1918 私网 192.168.0.0/16");
        if (in(0xC6120000u, 15)) return std::string("基准测试段 198.18.0.0/15");
        if (in(0xC6336400u, 24)) return std::string("TEST-NET-2 198.51.100.0/24");
        if (in(0xCB007100u, 24)) return std::string("TEST-NET-3 203.0.113.0/24");
        if (in(0xE0000000u, 4)) return std::string("组播 224.0.0.0/4");
        if (in(0xF0000000u, 4)) return std::string("保留段 240.0.0.0/4");
        return std::nullopt;
    }

    // v6。
    bool all_zero = true;
    for (const std::uint8_t byte : parsed->bytes) {
        if (byte != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return std::string("未指定地址 ::");
    // ::1:前 15 字节全零、末字节为 1(v4 映射段的末字节撞不上——那些
    // 地址第 10/11 字节是 0xFF 0xFF)。
    bool is_loopback_v6 = b[15] == 0x01;
    for (std::size_t i = 0; i < 15 && is_loopback_v6; ++i) {
        is_loopback_v6 = b[i] == 0;
    }
    if (is_loopback_v6) return std::string("loopback ::1");
    // ::ffff:0:0/96 的 v4 映射:按内嵌 v4 再查一遍(那 32 位单独够成地址)。
    if (PrefixMatches(b, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}, 96)) {
        const std::string embedded = std::to_string(b[12]) + "." + std::to_string(b[13]) + "." +
                                     std::to_string(b[14]) + "." + std::to_string(b[15]);
        if (auto range = BlockedAddressRange(embedded)) {
            return std::string("v4 映射段 ::ffff:0:0/96 -> ") + *range;
        }
        return std::nullopt;
    }
    if (PrefixMatches(b, {0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0}, 96)) {
        return std::string("NAT64 段 64:ff9b::/96");
    }
    if (PrefixMatches(b, {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 64)) {
        return std::string("丢弃段 100::/64");
    }
    if (PrefixMatches(b, {0x20, 0x01, 0x0D, 0xB8}, 32)) {
        return std::string("文档段 2001:db8::/32");
    }
    if (PrefixMatches(b, {0xFE, 0x80}, 10)) {
        return std::string("link-local fe80::/10");
    }
    if (PrefixMatches(b, {0xFC}, 7)) {
        return std::string("ULA 私网 fc00::/7");
    }
    if (PrefixMatches(b, {0xFF}, 8)) {
        return std::string("组播 ff00::/8");
    }
    return std::nullopt;
}

bool IsRoutablePublicAddress(std::string_view ip) {
    return ParseIpAddress(ip).has_value() && !BlockedAddressRange(ip).has_value();
}

bool IsLoopbackAddress(std::string_view ip) {
    const auto parsed = ParseIpAddress(ip);
    if (!parsed.has_value()) {
        return false;
    }
    if (parsed->is_ipv4) {
        return parsed->bytes[0] == 127;
    }
    if (parsed->bytes[15] != 0x01) {
        return false;
    }
    for (std::size_t i = 0; i < 15; ++i) {
        if (parsed->bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SystemDnsResolver
// ---------------------------------------------------------------------------

std::expected<std::vector<std::string>, std::string> SystemDnsResolver::Resolve(const std::string& host) {
    EnsureSocketsReady();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
        std::string detail;
#ifdef _WIN32
        detail = std::to_string(rc);
#else
        detail = ::gai_strerror(rc);
#endif
        return std::unexpected("DNS 解析失败(" + host + "): " + detail);
    }

    std::vector<std::string> addresses;
    for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
        char buffer[INET6_ADDRSTRLEN] = {};
        const void* addr = nullptr;
        if (it->ai_family == AF_INET) {
            addr = &reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr;
        } else if (it->ai_family == AF_INET6) {
            addr = &reinterpret_cast<sockaddr_in6*>(it->ai_addr)->sin6_addr;
        } else {
            continue;
        }
        if (::inet_ntop(it->ai_family, const_cast<void*>(addr), buffer, sizeof(buffer)) == nullptr) {
            continue;
        }
        std::string address(buffer);
        bool duplicate = false;
        for (const std::string& seen : addresses) {
            if (seen == address) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            addresses.push_back(std::move(address));
        }
    }
    ::freeaddrinfo(result);
    if (addresses.empty()) {
        return std::unexpected("DNS 解析没给出可用地址(" + host + ")");
    }
    return addresses;
}

// ---------------------------------------------------------------------------
// PerformFullHttpRequest
// ---------------------------------------------------------------------------

std::expected<FullHttpResponse, FullHttpError> PerformFullHttpRequest(const FullHttpRequest& request,
                                                                      const FullHttpLimits& limits,
                                                                      const std::atomic<bool>* cancel,
                                                                      const PinnedDnsResolve* pinned) {
    // §8.4 第 1 段:调用前已置位——socket 都不起。
    if (cancel != nullptr && cancel->load()) {
        FullHttpError error;
        error.kind = FullHttpErrorKind::Cancelled;
        error.message = "已取消";
        return std::unexpected(std::move(error));
    }

    FullHttpResponse response;
    bool cancelled = false;
    bool hard_timeout_hit = false;
    bool header_cap_hit = false;
    bool body_cap_hit = false;
    bool received_any_bytes = false;
    std::int64_t header_bytes = 0;

    // 硬墙钟只掐"挂死",进度回调每至多 1s 醒一拍(cpr 并发挂死单的老结论),
    // 不用 cpr::Timeout——那会把正常慢响应拦腰砍断。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.hard_timeout_ms);

    cpr::HeaderCallback header_cb(
        [&](const std::string_view& header, intptr_t) -> bool {
            // 响应头帽在回调入口落锤(§8.3):达帽即中止,不多攒一块。
            header_bytes += static_cast<std::int64_t>(header.size());
            if (header_bytes > limits.response_header_bytes) {
                header_cap_hit = true;
                return false;
            }
            // 状态行与空行不进头表;只收 "Name: value"。
            if (header.rfind("HTTP/", 0) == 0 || header == "\r\n" || header == "\n" || header.empty()) {
                return true;
            }
            std::string_view line = header;
            if (!line.empty() && line.back() == '\n') {
                line.remove_suffix(1);
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) {
                return true;  // 怪行不猜,跳过
            }
            std::string_view name = line.substr(0, colon);
            std::string_view value = line.substr(colon + 1);
            // 去 OWS。
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            response.headers.emplace_back(std::string(name), std::string(value));
            return true;
        });

    cpr::WriteCallback write_cb(
        [&](const std::string_view& data, intptr_t) -> bool {
            received_any_bytes = true;
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            // 响应体帽:这一段放进去就超了 -> 不放,直接掐(§8.3"不再多攒
            // 一块")。正好到帽不超。
            if (static_cast<std::int64_t>(response.body.size() + data.size()) > limits.response_body_bytes) {
                body_cap_hit = true;
                return false;
            }
            response.body.append(data);
            return true;
        });

    // Progress 在 DNS/连接/TLS 握手、上传与等首字节阶段都会被周期性调
    // 用——取消(§8.4 的前四段)与硬墙钟都挂在这里;收体段的取消另有
    // WriteCallback 把着。
    cpr::ProgressCallback progress_cb(
        [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, intptr_t) -> bool {
            if (cancel != nullptr && cancel->load()) {
                cancelled = true;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                hard_timeout_hit = true;
                return false;
            }
            return true;
        });

    cpr::Session session;
    session.SetUrl(cpr::Url{request.url});
    // cpr::Header 是大小写不敏感 map:同名头后者盖前者。请求头表来自 Lua
    // 的 table(键天然唯一)+ 宿主注入的单枚 auth 头,不会有重复键需求;
    // 重复头的完整保真只在响应侧(那里我们自己在回调里攒)。
    cpr::Header cpr_headers;
    for (const auto& [name, value] : request.headers) {
        cpr_headers[name] = value;
    }
    session.SetHeader(cpr_headers);
    if (!request.body.empty()) {
        session.SetBody(cpr::Body{request.body});
    }
    session.SetConnectTimeout(cpr::ConnectTimeout{std::chrono::milliseconds(limits.connect_timeout_ms)});
    if (pinned != nullptr && !pinned->addr.empty()) {
        // §8.2 第 5 步:连接钉已验地址(CURLOPT_RESOLVE),libcurl 不再
        // 自行解析,把"验完才连"与"连的就是验过的"钉成同一件事。
        session.SetResolves({cpr::Resolve{pinned->host, pinned->addr,
                                          std::set<std::uint16_t>{static_cast<std::uint16_t>(pinned->port)}}});
    }
    session.SetHeaderCallback(header_cb);
    session.SetWriteCallback(write_cb);
    session.SetProgressCallback(progress_cb);
    // §8.2:重定向一概不跟。cpr 的 Session 构造器默认 SetRedirect(
    // Redirect()),而 Redirect::follow 缺省是 true——不显式关掉,cpr 会静
    // 默替我们跟 3xx(连接期还会拿真实 DNS 去解析 Location,绕过钉地址)。
    // 这里钉死 false,3xx 原样交调用方。
    session.SetRedirect(cpr::Redirect(false));

    // 重定向一概不跟(libcurl 缺省 CURLOPT_FOLLOWLOCATION=0,cpr 只有显式
    // SetRedirect 才开;这里刻意不设)。3xx 原样交调用方(§8.2)。
    const cpr::Response raw = request.method == "POST" ? session.Post() : session.Get();
    response.status = static_cast<int>(raw.status_code);

    // 收场分型,顺序有讲究:取消 > 墙钟 > 响应头帽 > 响应体帽 > curl 错误。
    // cpr 把我们主动掐的流报成 ABORTED_BY_CALLBACK/WRITE_ERROR 一类共用
    // 码,不靠旁证标志分不清是谁落的锤——与 provider SSE 路同一套章法。
    if (cancelled || (cancel != nullptr && cancel->load())) {
        FullHttpError error;
        error.kind = FullHttpErrorKind::Cancelled;
        error.message = "已取消";
        error.received_any_bytes = received_any_bytes;
        return std::unexpected(std::move(error));
    }
    if (hard_timeout_hit) {
        FullHttpError error;
        error.kind = FullHttpErrorKind::Timeout;
        error.message = "硬墙钟到点(" + std::to_string(limits.hard_timeout_ms) + " ms)";
        error.received_any_bytes = received_any_bytes;
        return std::unexpected(std::move(error));
    }
    if (header_cap_hit) {
        FullHttpError error;
        error.kind = FullHttpErrorKind::ResponseHeaderTooLarge;
        error.message = "响应头超过 " + std::to_string(limits.response_header_bytes) + " 字节,已中止";
        error.received_any_bytes = received_any_bytes;
        return std::unexpected(std::move(error));
    }
    if (body_cap_hit) {
        FullHttpError error;
        error.kind = FullHttpErrorKind::ResponseBodyTooLarge;
        error.message = "响应体超过 " + std::to_string(limits.response_body_bytes) + " 字节,已中止";
        error.received_any_bytes = received_any_bytes;
        return std::unexpected(std::move(error));
    }
    if (raw.error) {
        FullHttpError error;
        error.curl_code = static_cast<long>(raw.error.code);
        error.curl_message = raw.error.message;
        error.received_any_bytes = received_any_bytes;
        error.kind = ClassifyCurlErrorCode(error.curl_code);
        switch (error.kind) {
            case FullHttpErrorKind::DnsFailed:
                error.message = "DNS 解析失败";
                break;
            case FullHttpErrorKind::TlsFailed:
                error.message = "TLS/证书失败";
                break;
            case FullHttpErrorKind::Timeout:
                // ConnectTimeout 与墙钟共用 CURLE_OPERATION_TIMEDOUT,都是
                // timeout 分型。
                error.message = "连接或传输超时";
                break;
            default:
                error.message = "网络传输失败";
                break;
        }
        return std::unexpected(std::move(error));
    }
    return response;
}

FullHttpErrorKind ClassifyCurlErrorCode(long curl_code) {
    switch (static_cast<cpr::ErrorCode>(curl_code)) {
        case cpr::ErrorCode::COULDNT_RESOLVE_HOST:
        case cpr::ErrorCode::COULDNT_RESOLVE_PROXY:
            return FullHttpErrorKind::DnsFailed;
        case cpr::ErrorCode::SSL_CONNECT_ERROR:
        case cpr::ErrorCode::SSL_CERTPROBLEM:
        case cpr::ErrorCode::SSL_CIPHER:
        case cpr::ErrorCode::PEER_FAILED_VERIFICATION:
        case cpr::ErrorCode::USE_SSL_FAILED:
        case cpr::ErrorCode::SSL_SHUTDOWN_FAILED:
        case cpr::ErrorCode::SSL_CRL_BADFILE:
        case cpr::ErrorCode::SSL_ISSUER_ERROR:
        case cpr::ErrorCode::SSL_PINNEDPUBKEYNOTMATCH:
        case cpr::ErrorCode::SSL_INVALIDCERTSTATUS:
        case cpr::ErrorCode::SSL_CACERT_BADFILE:
        case cpr::ErrorCode::SSL_CLIENTCERT:
            return FullHttpErrorKind::TlsFailed;
        case cpr::ErrorCode::OPERATION_TIMEDOUT:
            return FullHttpErrorKind::Timeout;
        default:
            return FullHttpErrorKind::NetworkFailed;
    }
}

}  // namespace lubancode::net
