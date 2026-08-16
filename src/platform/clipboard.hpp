// 剪贴板薄壳(交互抛光总账第二批 /copy、第三批贴图共用):
//   - Windows:Win32 Unicode clipboard(CF_UNICODETEXT),开箱即用;
//   - POSIX:OSC 52 转义(base64 载荷)写进终端,由终端转交本机剪贴板——
//     支不支持取决于终端,调用方先问 ClipboardLikelyAvailable。
// 纯管道/重定向场景:IsClipboardAvailable 恒 No——不往管道里写转义。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lubancode::platform {

enum class ClipboardResult {
    Ok,
    Unsupported,  // 平台/环境没有这条路(如 POSIX 下非真终端)
    Failure,      // 有路但没走通(剪贴板被占、写失败……)
};

// 把一段 UTF-8 文本送上剪贴板。error_detail 在非 Ok 时写给人看的短错
// (已本地化由调用方拼 i18n,这里给原因词)。
ClipboardResult CopyTextToClipboard(const std::string& utf8_text, std::string& error_detail);

// 剪贴板这条路像不像走得通(能力探测,不许真写):Windows 恒真;POSIX
// 看 stdout 是不是真终端(OSC 52 只往终端写)。tmux/SSH 下一律真——
// 终端会自己决定转不转发,写失败由 CopyTextToClipboard 如实报。
bool ClipboardLikelyAvailable();

// 读剪贴板里的一张位图,返回 PNG 字节(0.30.x Alt+V 直贴)。Windows:
// 先试剪贴板里现成的 "PNG" 注册格式(浏览器/编辑器复制图片常带),没有
// 再把 CF_DIB 经 WIC 转码成 PNG。POSIX:OSC 52 只写不读,明确不支持。
// max_bytes:超限直接拒(不暗降糊图),error 写明原因。失败给空。
std::optional<std::vector<unsigned char>> ReadClipboardImagePng(std::size_t max_bytes, std::string& error);

}  // namespace lubancode::platform
