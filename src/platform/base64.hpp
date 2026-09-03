// 标准 Base64 编码的仓内唯一内核(src 收口审计 P2:五处手写——
// agent/tool_result_images、app_server/ws_frames、platform/clipboard_posix、
// tools/run_command、cli/image_input,字母表/三字节分组/padding 合同相同,
// 边角(空输入、NUL 字节、char 符号性、1/2 字节尾巴、大载荷预分配)各写
// 各的)。这里收一份无业务依赖的内核,各模块只管输入编码——PowerShell
// 先产 UTF-16LE 字节、WebSocket 的 SHA-1 摘要,公共函数只做 Base64。
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace lubancode::platform {

// 标准 Base64(RFC 4648 字母表 A-Za-z0-9+/,带 '=' padding):
//   - 空输入 -> 空串;
//   - 尾 1 字节 -> "..==",尾 2 字节 -> "...=";
//   - 内核吃字节 span,不认符号性,string_view 只是薄口。
std::string Base64Encode(std::span<const std::byte> data);

inline std::string Base64Encode(std::string_view data) {
    return Base64Encode(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()),
                                                   data.size()));
}

}  // namespace lubancode::platform
