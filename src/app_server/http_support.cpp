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
    while (header.find("\r\n\r\n") == std::string::npos) {
        if (header.size() > kMaxHeaderBytes) {
            return false;
        }
        const long got = socket.Recv(buffer, sizeof(buffer));
        if (got <= 0) {
            return false;
        }
        header.append(buffer, buffer + got);
    }
    return true;
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
