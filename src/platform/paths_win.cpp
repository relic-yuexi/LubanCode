// Windows 实现:环境变量 + 主目录 + 宽窄转换。转换三件套从
// tools/process_exec.cpp 原样搬来(跨平台单搬家),GetEnvVar 从
// config/config.cpp 原样搬来;宽窄转换异常单起 Utf8ToWide/WideToUtf8
// 加了"不许抛"的合同(坏字符替换 U+FFFD,见函数注释)。
#include "platform/paths.hpp"

#include <cstdlib>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincon.h>  // GetConsoleOutputCP(LEAN_AND_MEAN 会裁掉它)

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

std::optional<std::filesystem::path> ExecutablePath() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(length) < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> OfficialSkillsDir() {
    const auto executable = ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path candidate = executable->parent_path() / "skills";
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        return std::nullopt;
    }
    const std::u8string value = candidate.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::optional<std::string> OfficialPackagesDir() {
    const auto executable = ExecutablePath();
    if (!executable.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path candidate = executable->parent_path() / "packages";
    std::error_code ec;
    if (!std::filesystem::is_directory(candidate, ec) || ec) {
        return std::nullopt;
    }
    const std::u8string value = candidate.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::expected<void, std::string> ReplaceFileAtomically(const std::filesystem::path& source,
                                                        const std::filesystem::path& destination) {
    if (MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return {};
    }
    return std::unexpected("原子替换文件失败，Windows 错误码 " + std::to_string(GetLastError()));
}

// 宽窄转换三件套的合同(宽窄转换异常单):任何输入都不抛、不把
// GetLastError 的文案变成异常往上送。目标本就是 UTF-8/UTF-16,可解的都
// 尽力解,解不动的字符替换 U+FFFD——坏一个字,不丢一整段、不掀会话。
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    // CP_UTF8 + flags=0 对坏序列(劈半的/孤立的续字节)按 U+FFFD 尽力替换,
    // 不报失败(实测 Win11;报失败要显式 MB_ERR_INVALID_CHARS)。长度探测
    // 返回 0 属极端兜底:给空串,不抛。
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string();
    }
    // 现行 Windows 上孤立代理对也会被替换成 U+FFFD(Win11 实测);返回 0
    // 的老机器兜一手:手动把孤立代理换成 U+FFFD 再转一遍——目标是 UTF-8,
    // 任何 Unicode 标量都编得出来,替换之后必然成功。绝不把 1113 的文案
    // 从这条路穿透出去。
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                        nullptr);
    if (len > 0) {
        std::string utf8(static_cast<std::size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr,
                            nullptr);
        return utf8;
    }
    std::wstring sanitized;
    sanitized.reserve(wide.size());
    bool pending_high = false;
    for (const wchar_t wc : wide) {
        if (wc >= 0xD800 && wc <= 0xDBFF) {
            if (pending_high) {
                sanitized.push_back(0xFFFD);  // 连着两个高代理:前一个孤立
            }
            pending_high = true;
            continue;
        }
        if (wc >= 0xDC00 && wc <= 0xDFFF) {
            if (!pending_high) {
                sanitized.push_back(0xFFFD);  // 没有高代理领着的低代理
                continue;
            }
            sanitized.push_back(wc);  // 成对代理:原样保留,转换时自然拼回
            pending_high = false;
            continue;
        }
        if (pending_high) {
            sanitized.push_back(0xFFFD);  // 高代理后面跟的不是低代理
            pending_high = false;
        }
        sanitized.push_back(wc);
    }
    if (pending_high) {
        sanitized.push_back(0xFFFD);  // 收尾还挂着孤立高代理
    }
    const int retry = WideCharToMultiByte(CP_UTF8, 0, sanitized.data(), static_cast<int>(sanitized.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (retry <= 0) {
        return std::string();  // 理论上到不了:不再抛,也不再猜
    }
    std::string utf8(static_cast<std::size_t>(retry), '\0');
    WideCharToMultiByte(CP_UTF8, 0, sanitized.data(), static_cast<int>(sanitized.size()), utf8.data(), retry, nullptr,
                        nullptr);
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

std::optional<std::string> CodePageBytesToUtf8(unsigned int code_page, const std::string& bytes) {
    if (bytes.empty()) {
        return std::string();
    }
    // MB_ERR_INVALID_CHARS:字节序列在该代码页里非法就报失败,不做"尽量解、
    // 坏字节顶个问号"的静默替换——调用的 hooks 解码层要拿成败当判定。
    const int wlen = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
    if (wlen <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                        wlen);
    return WideToUtf8(wide);
}

std::vector<unsigned int> ChildStreamCodePageCandidates() {
    // 控制台输出页在前:PowerShell 5.1 往重定向管道写 stderr/stdout 用的就
    // 是它(中文机器 936/GBK)。系统 ANSI 页在后:cmd.exe 一路。两页相同只
    // 列一次。
    const unsigned int console_cp = GetConsoleOutputCP();
    std::vector<unsigned int> out;
    out.push_back(console_cp);
    if (GetACP() != console_cp) {
        out.push_back(GetACP());
    }
    return out;
}

}  // namespace lubancode::platform
