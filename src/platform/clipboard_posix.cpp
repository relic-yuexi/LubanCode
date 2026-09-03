// clipboard.hpp 的 POSIX 实现:OSC 52 转义(base64 载荷)。终端收下后
// 转交本机剪贴板;支不支持、允不允许(有的终端默认关 OSC 52 写剪贴板)
// 都由终端说了算——失败路径不能探知,只能把"已把序列写进终端、剪贴板
// 是否更新取决于终端"如实告诉调用方。

#include "platform/clipboard.hpp"

#ifndef _WIN32

#include <cstdio>
#include <optional>
#include <vector>

#include "platform/base64.hpp"  // Base64Encode:OSC 52 载荷的公共内核(审计 P2)
#include "platform/console.hpp"

namespace lubancode::platform {

namespace {

// 薄名:剪贴板载荷过公共 base64 内核(标准字母表 + 填充)。
std::string Base64Encode(const std::string& bytes) {
    return platform::Base64Encode(std::string_view(bytes));
}

}  // namespace

bool ClipboardLikelyAvailable() {
    // OSC 52 只对真终端有意义;管道/重定向写出去就是污染。
    return StdinIsInteractive();
}

std::optional<std::vector<unsigned char>> ReadClipboardImagePng(std::size_t max_bytes, std::string& error) {
    // OSC 52 只有"写"方向(终端转交本机剪贴板),没有可靠的"读"回协议;
    // 明确不支持,不装作能读。
    (void)max_bytes;
    error = "此平台读不了剪贴板位图(OSC 52 只写不读)";
    return std::nullopt;
}

bool ClipboardHasImage() {
    // 读方向没有协议,自然也探不出图。
    return false;
}

std::optional<std::string> ReadClipboardTextUtf8(std::string& error) {
    error = "此平台读不了剪贴板文本(OSC 52 只写不读)";
    return std::nullopt;
}

ClipboardResult CopyTextToClipboard(const std::string& utf8_text, std::string& error_detail) {
    if (!ClipboardLikelyAvailable()) {
        error_detail = "stdin 不是终端";
        return ClipboardResult::Unsupported;
    }
    // ESC ] 52 ; <剪贴板名 c=系统剪贴板> ; <base64> BEL。超长载荷(>100k)
    // 有终端会拒收,如实降报。
    if (utf8_text.size() > 100 * 1024) {
        error_detail = "文本超长(OSC 52 大载荷多数终端拒收)";
        return ClipboardResult::Failure;
    }
    const std::string sequence = "\x1b]52;c;" + Base64Encode(utf8_text) + "\x07";
    if (std::fwrite(sequence.data(), 1, sequence.size(), stdout) != sequence.size()) {
        error_detail = "写终端失败";
        return ClipboardResult::Failure;
    }
    std::fflush(stdout);
    // OSC 52 是"写完即走":终端是否真更新剪贴板探不出来,不装作已知。
    error_detail = "OSC 52 序列已写入,剪贴板是否更新取决于终端设置";
    return ClipboardResult::Ok;
}

}  // namespace lubancode::platform

#endif  // !_WIN32
