// 工具结果图片回喂单的实现:artifact 重灌 + 魔数/字节/边长帽。见头注释。
#include "agent/tool_result_images.hpp"

#include <fstream>

#include "agent/model_image_store.hpp"  // SniffImageFormat/ReadImageDimensions
#include "platform/log_sink.hpp"        // 退字节口径时的留痕日志
#include "platform/paths.hpp"           // Utf8ToPath:目录路径不走 ACP 窄口
#include "platform/text_encoding.hpp"

namespace lubancode::agent {

namespace {

// 标准 base64(带 padding)。仓库里已有三份局部实现(cli/platform/tools),
// 那些都在各自的静态命名空间里够不着;这里再落一份局部——四处调用点
// 各自独立,等哪天真抽公共件再并。
std::string Base64Encode(const std::string& bytes) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6) & 0x3F];
        out += kTable[n & 0x3F];
        i += 3;
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

// artifact 文件名干净检查:宿主自己起的内容寻址名(art-<sha8>.<ext>)只
// 该是平名。带分隔符/..的按脏拒——那是引用被外来的路径串污染了,不读。
bool CleanArtifactFilename(const std::string& filename) {
    if (filename.empty() || filename == "." || filename == "..") {
        return false;
    }
    for (const char c : filename) {
        if (c == '/' || c == '\\') {
            return false;
        }
    }
    return filename.find("..") == std::string::npos;
}

bool ReadFileBytes(const std::string& path_utf8, std::string& out) {
    std::ifstream in(platform::Utf8ToPath(path_utf8), std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaxToolResultWireImageBytes) {
        return false;  // 读不到尺寸/超帽:按不灌收口
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(out.data(), size);
    }
    return in.good() || in.eof();
}

}  // namespace

std::size_t EstimateImageTokensForPreflight(std::uint32_t width, std::uint32_t height,
                                            const std::string& base64) {
    // 像素优先:块上的宽高是解析期读好的账(贴图 LoadImage/工具图验身
    // 时读的),不用再解一遍 base64。
    if (width > 0 && height > 0) {
        return static_cast<std::size_t>(static_cast<std::uint64_t>(width) * height / 750);
    }
    // base64 也没有(未重灌的引用块/空块):图不上 wire,零账。
    if (base64.empty()) {
        return 0;
    }
    // 现解码读头。cap 与模型图上限同档(kMaxModelImageBytes,20MiB):
    // 超帽/坏串按读不出走字节兜底——兜底只会多算,不会漏算。
    const auto decoded = DecodeBase64Strict(base64, kMaxModelImageBytes);
    if (decoded.has_value()) {
        const ImageFormat format = SniffImageFormat(*decoded);
        if (!format.mime_type.empty()) {
            const ImageDimensions dims = ReadImageDimensions(*decoded, format.mime_type);
            if (dims.width > 0 && dims.height > 0) {
                return static_cast<std::size_t>(static_cast<std::uint64_t>(dims.width) * dims.height / 750);
            }
            platform::LogSink::Instance().Debug(
                "tool-image", "预检图片 token:格式认得(" + format.mime_type + ")但宽高读不出,base64 " +
                                  std::to_string(base64.size()) + " 字符,退回字节口径估算");
        } else {
            platform::LogSink::Instance().Debug(
                "tool-image", "预检图片 token:认不出图片格式,base64 " + std::to_string(base64.size()) +
                                  " 字符,退回字节口径估算");
        }
    } else {
        platform::LogSink::Instance().Debug("tool-image",
                                            "预检图片 token:base64 解不开(" + decoded.error() +
                                                "),退回字节口径估算");
    }
    return base64.size() / 4;  // 老尺兜底:base64 字符 4 折 1,宁多勿漏
}

std::size_t RehydrateToolResultImages(api::Request& request, const std::string& artifact_root) {
    if (artifact_root.empty()) {
        return 0;  // 没开 artifact 目录的调用路(单测/单发)无图可灌
    }
    std::size_t rehydrated = 0;
    std::size_t total_base64 = 0;
    for (auto& message : request.messages) {
        for (auto& block : message.content) {
            auto* result = std::get_if<api::ToolResultBlock>(&block);
            if (result == nullptr) {
                continue;
            }
            for (auto& rich : result->blocks) {
                auto* image = std::get_if<tools::ImageContent>(&rich);
                if (image == nullptr || !image->wire_base64.empty()) {
                    continue;  // 非图片块/已灌过(幂等)
                }
                if (!image->artifact.stored || !CleanArtifactFilename(image->artifact.filename)) {
                    continue;
                }
                if (image->bytes > kMaxToolResultWireImageBytes) {
                    continue;  // 解码字节超帽:账面就超了,不用读
                }
                std::string bytes;
                if (!ReadFileBytes(artifact_root + "/" + image->artifact.filename, bytes)) {
                    continue;
                }
                if (bytes.empty() || bytes.size() > kMaxToolResultWireImageBytes) {
                    continue;
                }
                // 魔数复核:文件内容与块声明对不上(文件被换过/引用串了
                // 行)就拒灌——伪 MIME 不出门,与 MCP 侧同一条规矩。
                const ImageFormat format = SniffImageFormat(bytes);
                if (format.mime_type != image->mime_type) {
                    continue;
                }
                // 边长帽:块上的尺寸是解析期读的;为 0(个别变体流读不出)
                // 现读一遍,读不出就按超帽拒——anthropic 对 computer-use
                // 截图超限是硬拒,别把锅递给服务端。
                std::uint32_t width = image->width;
                std::uint32_t height = image->height;
                if (width == 0 || height == 0) {
                    const ImageDimensions dims = ReadImageDimensions(bytes, format.mime_type);
                    width = dims.width;
                    height = dims.height;
                }
                if (width == 0 || height == 0 || width > kMaxToolResultWireImageSide ||
                    height > kMaxToolResultWireImageSide) {
                    continue;
                }
                std::string encoded = Base64Encode(bytes);
                if (total_base64 + encoded.size() > kMaxToolResultWireImageTotalBytes) {
                    continue;  // 请求合计帽:后面的图全部留给降级路
                }
                total_base64 += encoded.size();
                image->wire_base64 = std::move(encoded);
                ++rehydrated;
            }
        }
    }
    return rehydrated;
}

}  // namespace lubancode::agent
