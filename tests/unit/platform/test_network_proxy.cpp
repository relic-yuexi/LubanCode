#include <doctest/doctest.h>

#include "platform/network_proxy.hpp"

using lubancode::platform::ParseWindowsProxyServer;

TEST_CASE("Windows 系统代理: 通用地址供 HTTP 和 HTTPS 共用") {
    CHECK(ParseWindowsProxyServer("127.0.0.1:10808", "https") ==
          "http://127.0.0.1:10808");
    CHECK(ParseWindowsProxyServer(" http://proxy.example:8080 ", "http") ==
          "http://proxy.example:8080");
}

TEST_CASE("Windows 系统代理: 按协议取值并以 SOCKS 兜底") {
    const std::string split = "http=proxy.example:80; HTTPS = secure.example:443";
    CHECK(ParseWindowsProxyServer(split, "http") == "http://proxy.example:80");
    CHECK(ParseWindowsProxyServer(split, "https") == "http://secure.example:443");
    CHECK_FALSE(ParseWindowsProxyServer(split, "ftp").has_value());
    CHECK(ParseWindowsProxyServer("socks=127.0.0.1:1080", "https") ==
          "socks5://127.0.0.1:1080");
}

TEST_CASE("Windows 系统代理: 空串和空分栏不生地址") {
    CHECK_FALSE(ParseWindowsProxyServer("   ", "https").has_value());
    CHECK_FALSE(ParseWindowsProxyServer("http=proxy:80;https= ", "https").has_value());
}
