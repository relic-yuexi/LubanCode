// Windows 实现:环境变量 + 主目录 + 宽窄转换。转换三件套从
// tools/process_exec.cpp 原样搬来(跨平台单搬家),GetEnvVar 从
// config/config.cpp 原样搬来,逻辑一字未改。
#include "platform/paths.hpp"

#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace lubancode::platform {

std::optional<std::string> GetEnvVar(const char* name) {
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, name);
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> HomeDir() {
    return GetEnvVar("USERPROFILE");
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    if (len > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    }
    return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(len), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
    }
    return utf8;
}

std::string AcpBytesToUtf8(const std::string& acp_bytes) {
    if (acp_bytes.empty()) {
        return std::string();
    }
    const int wlen = MultiByteToWideChar(CP_ACP, 0, acp_bytes.data(), static_cast<int>(acp_bytes.size()), nullptr, 0);
    if (wlen <= 0) {
        return acp_bytes;  // 解不出来就原样返回,好歹不丢数据
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, acp_bytes.data(), static_cast<int>(acp_bytes.size()), wide.data(), wlen);
    return WideToUtf8(wide);
}

}  // namespace lubancode::platform
