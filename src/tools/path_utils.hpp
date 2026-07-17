// 路径相关的小工具函数,给 tools 层各文件共用,免得每个工具都各写一遍
// UTF-8 <-> std::filesystem::path 的转换代码(read_file.cpp/run_command.cpp
// 里原本各有一份,这里抽出来一份共用的,新工具直接 #include 就好)。

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace lubancode::tools {

// path 是 UTF-8 编码的字符串;std::filesystem::path 的 char8_t 构造函数
// 明确把输入当 UTF-8 解码,内部再转成 native 编码(Windows 上是 UTF-16)。
// reinterpret_cast<const char8_t*> 在 char/char8_t 之间做字节重解读,
// C++20 里是合法用法(两者都是 1 字节的字符类型)。
// 正斜杠、反斜杠 std::filesystem 在 Windows 下都认得,不用额外转换。
inline std::filesystem::path Utf8ToPath(const std::string& utf8) {
    const std::u8string_view view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(view);
}

// std::filesystem::path -> UTF-8 字符串,给要回传给模型/打印到终端的场合用。
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace lubancode::tools
