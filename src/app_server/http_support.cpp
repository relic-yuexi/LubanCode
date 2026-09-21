// http_support.hpp 的实现。三件都是从 ws_transport 与 local_web_server 的
// 同尺复制件纯搬移而来(HC-03 第一批):返回值、次序、比较口径逐字节
// 等价,行为零变更;头部边界修正另列行为批次,不混在这份里。
#include "app_server/http_support.hpp"

#include <filesystem>
#include <fstream>

#include "app_server/ws_frames.hpp"    // IsValidArtifactName
#include "tools/path_utils.hpp"        // Utf8ToPath:UTF-8 路径进 filesystem(Windows ACP 坑)

namespace lubancode::app_server {

bool ReadUntilHeaderEnd(net::Socket& socket, std::string& header) {
    char buffer[2048];
    while (true) {
        const std::size_t head_end = header.find("\r\n\r\n");
        if (head_end != std::string::npos) {
            // 上限只算头部本体(含 \r\n\r\n 终止符);终止符之后同包挤进来
            // 的先头字节(POST body 前缀)不计——合法随包 POST 不误拒。
            return head_end + 4 <= kMaxHeaderBytes;
        }
        // 还没见终止符:已收的字节全是头部本体,哪怕终止符紧随其后也至少
        // 再进一字节——此时越界。检查放在每轮开头(原实现只在 Recv 之前
        // 查,含终止符的最后一块把缓冲拉过上限时循环条件直接退出,漏过)。
        if (header.size() >= kMaxHeaderBytes) {
            return false;
        }
        const long got = socket.Recv(buffer, sizeof(buffer));
        if (got <= 0) {
            return false;
        }
        header.append(buffer, buffer + got);
    }
}

bool ConstantTimeEqual(std::string_view given, std::string_view expected) {
    if (given.size() != expected.size()) {
        return false;
    }
    unsigned diff = 0;
    for (std::size_t i = 0; i < given.size(); ++i) {
        diff |= static_cast<unsigned char>(given[i]) ^ static_cast<unsigned char>(expected[i]);
    }
    return diff == 0;
}

ArtifactBytes LoadArtifactBytes(const std::string& artifact_dir, const std::string& name,
                                std::uintmax_t max_bytes) {
    ArtifactBytes result;
    if (artifact_dir.empty() || !ws::IsValidArtifactName(name)) {
        return result;
    }
    const std::filesystem::path path = tools::Utf8ToPath(artifact_dir) / tools::Utf8ToPath(name);
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > max_bytes) {
        return result;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return result;
    }
    result.bytes.assign(static_cast<std::size_t>(size), '\0');
    file.read(result.bytes.data(), static_cast<std::streamsize>(size));
    if (!file && file.gcount() != static_cast<std::streamsize>(size)) {
        result.bytes.clear();
        return result;
    }
    // 扩展名已由形状校验收口(png/jpeg/jpg 三选一)。
    result.mime = name.compare(name.size() - 3, 3, "png") == 0 ? "image/png" : "image/jpeg";
    result.ok = true;
    return result;
}

}  // namespace lubancode::app_server
