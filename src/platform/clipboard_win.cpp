// clipboard.hpp 的 Windows 实现:Win32 Unicode clipboard。

#include "platform/clipboard.hpp"

#ifdef _WIN32

#include <windows.h>

#include <mutex>

#include "platform/paths.hpp"  // Utf8ToWide

namespace lubancode::platform {

namespace {

// 全局剪贴板一把锁:会话里可能多处并发想写(/copy、贴图、测试),Win32
// 的 OpenClipboard 本身互斥,但排队重试逻辑不想要,直接锁掉。
std::mutex& ClipboardMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

bool ClipboardLikelyAvailable() {
    return true;  // Win32 剪贴板开箱即用(远程桌面裁剪另说,失败路径如实报)
}

ClipboardResult CopyTextToClipboard(const std::string& utf8_text, std::string& error_detail) {
    std::lock_guard<std::mutex> lock(ClipboardMutex());
    if (!OpenClipboard(nullptr)) {
        error_detail = "OpenClipboard 失败(剪贴板被别的程序占着)";
        return ClipboardResult::Failure;
    }
    // RAII 收尾:无论哪条路退出都关板,不让后来者一直吃拒绝。
    struct CloseGuard {
        ~CloseGuard() { CloseClipboard(); }
    } guard;

    if (!EmptyClipboard()) {
        error_detail = "EmptyClipboard 失败";
        return ClipboardResult::Failure;
    }
    const std::wstring wide = Utf8ToWide(utf8_text);
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        error_detail = "GlobalAlloc 失败";
        return ClipboardResult::Failure;
    }
    wchar_t* target = static_cast<wchar_t*>(GlobalLock(memory));
    if (target == nullptr) {
        GlobalFree(memory);
        error_detail = "GlobalLock 失败";
        return ClipboardResult::Failure;
    }
    std::copy(wide.begin(), wide.end(), target);
    target[wide.size()] = L'\0';
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        error_detail = "SetClipboardData 失败";
        return ClipboardResult::Failure;
    }
    // 成功后内存归剪贴板所有,不许再 free。
    return ClipboardResult::Ok;
}

}  // namespace lubancode::platform

#endif  // _WIN32
