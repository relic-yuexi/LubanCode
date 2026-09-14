// hidden_input.hpp 的实现。Windows:标准输入句柄摘 ENABLE_ECHO_INPUT;
// Ctrl+C 的回显恢复靠一次性安装的转发处理器(见文件尾注释)。POSIX:
// tcgetattr/tcsetattr ~ECHO,行编辑(ICANON)保留,退格/粘贴照常工作。
#include "platform/hidden_input.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>

#include "platform/paths.hpp"  // WideToUtf8

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lubancode::platform {

#ifdef _WIN32

namespace {

// 回显关闭期间的登记:Ctrl+C 到来时先复原模式再交默认处理。
// echo_handle 非空 = 当前处于"回显已关"窗口(同进程同时只有一路隐藏输入,
// 向导是串行问答,不另加锁)。
std::atomic<HANDLE> g_echo_off_console{nullptr};
std::atomic<unsigned long> g_echo_off_original_mode{0};

BOOL WINAPI EchoOffCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        const HANDLE handle = g_echo_off_console.load(std::memory_order_acquire);
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            SetConsoleMode(handle, g_echo_off_original_mode.load(std::memory_order_relaxed));
        }
        g_echo_off_console.store(nullptr, std::memory_order_release);
    }
    return FALSE;  // 继续交默认处理(默认:进程退出),恢复动作已做完
}

void MarkEchoOffForCtrlHandler(HANDLE handle, unsigned long original_mode) {
    static std::atomic<bool> handler_installed{false};
    if (!handler_installed.load(std::memory_order_acquire)) {
        SetConsoleCtrlHandler(EchoOffCtrlHandler, TRUE);
        handler_installed.store(true, std::memory_order_release);
    }
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        g_echo_off_original_mode.store(original_mode, std::memory_order_relaxed);
        g_echo_off_console.store(handle, std::memory_order_release);
    } else {
        g_echo_off_console.store(nullptr, std::memory_order_release);
    }
}

}  // namespace

#endif

bool StdinIsTerminal() {
#ifdef _WIN32
    DWORD mode = 0;
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetConsoleMode(handle, &mode) != FALSE;
#else
    return ::isatty(STDIN_FILENO) == 1;
#endif
}

EchoOffScope::EchoOffScope() {
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
        return;  // 不是真控制台(管道/重定向):进不去,调用方明报
    }
    original_mode_ = mode;
    const DWORD masked = mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT);
    if (masked == mode) {
        ok_ = true;  // 回显本来就关着:无事可做,也不背恢复账
        return;
    }
    if (!SetConsoleMode(handle, masked)) {
        return;
    }
    restore_ = true;
    ok_ = true;
    MarkEchoOffForCtrlHandler(handle, original_mode_);
#else
    struct termios current {};
    if (::tcgetattr(STDIN_FILENO, &current) != 0) {
        return;
    }
    original_termios_ = current;
    struct termios masked = current;
    masked.c_lflag &= ~static_cast<unsigned int>(ECHO);
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &masked) != 0) {
        return;
    }
    restore_ = true;
    ok_ = true;
#endif
}

EchoOffScope::~EchoOffScope() {
    if (!restore_) {
        return;
    }
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        SetConsoleMode(handle, original_mode_);
    }
    MarkEchoOffForCtrlHandler(nullptr, 0);
#else
    ::tcsetattr(STDIN_FILENO, TCSANOW, &original_termios_);
#endif
}

std::expected<std::string, HiddenInputError> ReadHiddenLine() {
    if (!StdinIsTerminal()) {
        HiddenInputError error;
        error.reason = "hidden_input_no_tty";
        error.detail = "stdin 不是交互终端,无法隐藏输入;自动化配置请用 secret_file/secret_env";
        return std::unexpected(error);
    }
    EchoOffScope echo_off;  // 析构(含异常展开)复原回显
    if (!echo_off.ok()) {
        HiddenInputError error;
        error.reason = "hidden_input_no_tty";
        error.detail = "关不掉终端回显(非交互终端或权限不足)";
        return std::unexpected(error);
    }
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        HiddenInputError error;
        error.reason = "hidden_input_read_failed";
        error.detail = "拿不到控制台输入句柄";
        return std::unexpected(error);
    }
    std::wstring wide;
    wchar_t buffer[512];
    DWORD read = 0;
    while (true) {
        if (!ReadConsoleW(handle, buffer, 511, &read, nullptr)) {
            HiddenInputError error;
            error.reason = "hidden_input_read_failed";
            error.detail = "读控制台输入失败";
            return std::unexpected(error);
        }
        if (read == 0) {
            if (wide.empty()) {
                HiddenInputError error;
                error.reason = "hidden_input_cancelled";
                error.detail = "输入流已结束";
                return std::unexpected(error);
            }
            break;
        }
        wide.append(buffer, buffer + read);
        if (!wide.empty() && wide.back() == L'\n') {
            break;  // 行到头
        }
    }
    // 剥行尾 \r\n。Ctrl+C 不走这条路(PROCESSED_INPUT 开着,控制台处理器
    // 先复原回显再交默认终止)。
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) {
        wide.pop_back();
    }
    return WideToUtf8(wide);
#else
    std::string line;
    bool saw_any = false;
    while (true) {
        char ch = '\0';
        const ssize_t got = ::read(STDIN_FILENO, &ch, 1);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (!saw_any) {
                HiddenInputError error;
                error.reason = "hidden_input_cancelled";
                error.detail = "输入流已结束";
                return std::unexpected(error);
            }
            break;
        }
        saw_any = true;
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line.push_back(ch);
        }
    }
    return line;
#endif
}

}  // namespace lubancode::platform
