#include "platform/network_proxy.hpp"

#include <algorithm>
#include <cctype>

namespace lubancode::platform {
namespace {

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string Lower(std::string_view value) {
    std::string out(value);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::optional<std::string> NormalizeProxy(std::string_view value, bool socks) {
    value = Trim(value);
    if (value.empty()) return std::nullopt;
    if (value.find("://") != std::string_view::npos) return std::string(value);
    return std::string(socks ? "socks5://" : "http://") + std::string(value);
}

}  // namespace

std::optional<std::string> ParseWindowsProxyServer(std::string_view value,
                                                   std::string_view scheme) {
    value = Trim(value);
    if (value.empty()) return std::nullopt;

    // 没有按协议分栏时，这枚地址供所有协议共用。HTTPS 目标通常仍经普通
    // HTTP CONNECT 代理转发，故裸 host:port 补 http://，不补 https://。
    if (value.find('=') == std::string_view::npos) {
        return NormalizeProxy(value, false);
    }

    const std::string wanted = Lower(Trim(scheme));
    std::optional<std::string> socks_fallback;
    while (!value.empty()) {
        const std::size_t separator = value.find(';');
        const std::string_view part = Trim(value.substr(0, separator));
        const std::size_t equals = part.find('=');
        if (equals != std::string_view::npos) {
            const std::string key = Lower(Trim(part.substr(0, equals)));
            const std::string_view endpoint = part.substr(equals + 1);
            if (key == wanted) return NormalizeProxy(endpoint, false);
            if (key == "socks") socks_fallback = NormalizeProxy(endpoint, true);
        }
        if (separator == std::string_view::npos) break;
        value.remove_prefix(separator + 1);
    }
    return socks_fallback;
}

}  // namespace lubancode::platform
