#include "platform/network_proxy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "platform/paths.hpp"

namespace lubancode::platform {
namespace {

bool ProxyEnvironmentIsSet(std::string_view scheme) {
    const std::string lower(scheme);
    std::string upper(lower);
    for (char& ch : upper) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    const std::array<std::string, 4> names = {
        lower + "_proxy", upper + "_PROXY", "all_proxy", "ALL_PROXY"};
    return std::ranges::any_of(names, [](const std::string& name) {
        return GetEnvVar(name.c_str()).has_value();
    });
}

std::optional<std::wstring> ReadInternetSettingString(const wchar_t* name) {
    constexpr wchar_t kInternetSettings[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD bytes = 0;
    const LSTATUS size_status = RegGetValueW(HKEY_CURRENT_USER, kInternetSettings, name,
                                             RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (size_status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return std::nullopt;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    const LSTATUS read_status = RegGetValueW(HKEY_CURRENT_USER, kInternetSettings, name,
                                             RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes);
    if (read_status != ERROR_SUCCESS || buffer.front() == L'\0') return std::nullopt;
    return std::wstring(buffer.data());
}

}  // namespace

std::optional<std::string> SystemProxyForScheme(std::string_view scheme) {
    if (ProxyEnvironmentIsSet(scheme)) return std::nullopt;

    constexpr wchar_t kInternetSettings[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD enabled = 0;
    DWORD bytes = sizeof(enabled);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kInternetSettings, L"ProxyEnable",
                                        RRF_RT_REG_DWORD, nullptr, &enabled, &bytes);
    if (status != ERROR_SUCCESS || enabled == 0) return std::nullopt;

    const auto proxy_server = ReadInternetSettingString(L"ProxyServer");
    if (!proxy_server.has_value()) return std::nullopt;
    return ParseWindowsProxyServer(WideToUtf8(*proxy_server), scheme);
}

}  // namespace lubancode::platform
