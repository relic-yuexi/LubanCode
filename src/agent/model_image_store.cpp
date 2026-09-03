#include "agent/model_image_store.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "hooks/hash.hpp"      // Sha256Hex:内容寻址文件名
#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1:替掉先删后换)
#include "platform/paths.hpp"  // Utf8ToPath:目录路径不走 ACP 窄口

namespace lubancode::agent {

namespace {

bool StartsWith(const std::string& bytes, const char* prefix, std::size_t at = 0) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (at + i >= bytes.size() || bytes[at + i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

std::uint32_t ReadBe16(const std::string& bytes, std::size_t at) {
    if (at + 2 > bytes.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1]));
}

std::uint32_t ReadBe32(const std::string& bytes, std::size_t at) {
    if (at + 4 > bytes.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

std::uint32_t ReadLe16(const std::string& bytes, std::size_t at) {
    if (at + 2 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

std::uint32_t ReadLe32(const std::string& bytes, std::size_t at) {
    if (at + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
}

// 原子写,统一走 platform::AtomicWriteFile(旧写法先删正式件再 rename,
// 换名窗口里图不在;平台件原子替换,失败不动正式件)。
bool AtomicWriteBytes(const std::filesystem::path& path, const std::string& content) {
    return platform::AtomicWriteFile(path, content).has_value();
}

}  // namespace

std::expected<std::string, std::string> DecodeBase64Strict(const std::string& text, std::size_t max_bytes) {
    if (text.size() % 4 != 0) {
        return std::unexpected("base64 长度(" + std::to_string(text.size()) + ")不是 4 的倍数");
    }
    if (text.size() / 4 * 3 > max_bytes + 2) {  // 每组 3 字节,padding 最多折 2
        return std::unexpected("图片解码后超过上限 " + std::to_string(max_bytes) + " 字节,拒绝解码");
    }
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int padding = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                // padding 只许出现在末组末位(1 或 2 个)。
                if (i + 4 != text.size() || k < 2) {
                    return std::unexpected("base64 含非法 padding 位置");
                }
                ++padding;
                v[k] = 0;
                continue;
            }
            if (padding > 0) {
                return std::unexpected("base64 padding 后还有数据");
            }
            v[k] = value_of(c);
            if (v[k] < 0) {
                return std::unexpected(std::string("base64 含非法字符: '") + c + "'");
            }
        }
        const std::uint32_t triple = (static_cast<std::uint32_t>(v[0]) << 18) |
                                     (static_cast<std::uint32_t>(v[1]) << 12) |
                                     (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        out += static_cast<char>((triple >> 16) & 0xFF);
        if (padding < 2) {
            out += static_cast<char>((triple >> 8) & 0xFF);
        }
        if (padding < 1) {
            out += static_cast<char>(triple & 0xFF);
        }
    }
    if (out.size() > max_bytes) {
        return std::unexpected("图片解码后 " + std::to_string(out.size()) + " 字节,超过上限 " +
                               std::to_string(max_bytes));
    }
    return out;
}

ImageFormat SniffImageFormat(const std::string& bytes) {
    if (bytes.size() >= 8 && StartsWith(bytes, "\x89PNG\r\n\x1a\n")) {
        return {"image/png", "png"};
    }
    if (bytes.size() >= 3 && StartsWith(bytes, "\xFF\xD8\xFF")) {
        return {"image/jpeg", "jpg"};
    }
    if (bytes.size() >= 6 && StartsWith(bytes, "GIF87a")) {
        return {"image/gif", "gif"};
    }
    if (bytes.size() >= 6 && StartsWith(bytes, "GIF89a")) {
        return {"image/gif", "gif"};
    }
    if (bytes.size() >= 12 && StartsWith(bytes, "RIFF") && StartsWith(bytes, "WEBP", 8)) {
        return {"image/webp", "webp"};
    }
    return {};
}

namespace {

// JPEG 的宽高住在 SOFn 段里(表内列全了:C0-CF 里除 C4/C8/CC)。逐段走,
// 遇到 SOS(正文开始)还没见着 SOF 就放弃(渐进/奇怪排列按 0x0 处理)。
ImageDimensions JpegDimensions(const std::string& bytes) {
    std::size_t at = 2;  // 跳过 SOI + 首 marker 起步
    while (at + 4 <= bytes.size()) {
        if (bytes[at] != '\xFF') {
            ++at;  // 段间填充字节,跳一个再找
            continue;
        }
        const unsigned char marker = static_cast<unsigned char>(bytes[at + 1]);
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            at += 2;  // 无长度段的独立 marker
            continue;
        }
        if (marker == 0xDA) {
            return {};  // SOS:熵编码正文开始,前面没见着 SOF 就读不出
        }
        const std::uint32_t length = ReadBe16(bytes, at + 2);
        if (length < 2 || at + 2 + length > bytes.size()) {
            return {};
        }
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (at + 9 <= bytes.size()) {
                return {ReadBe16(bytes, at + 7), ReadBe16(bytes, at + 5)};
            }
            return {};
        }
        at += 2 + length;
    }
    return {};
}

ImageDimensions WebpDimensions(const std::string& bytes) {
    // chunk tag 是 ASCII:ReadLe32 读出来 "VP8 "/"VP8L"/"VP8X" 各是一个常量。
    const std::uint32_t tag = ReadLe32(bytes, 12);
    if (tag == 0x20385056) {  // "VP8 "(有损):帧头里 14bit 宽高
        if (bytes.size() >= 30) {
            const std::uint32_t w = (ReadLe16(bytes, 26) & 0x3FFF);
            const std::uint32_t h = (ReadLe16(bytes, 28) & 0x3FFF);
            return {w == 0 ? 0 : w, h == 0 ? 0 : h};
        }
        return {};
    }
    if (tag == 0x4C385056) {  // "VP8L"(无损):位流里 14bit 宽高(减一存储)
        if (bytes.size() >= 25) {
            const std::uint32_t bits = ReadLe32(bytes, 21);
            return {(bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1};
        }
        return {};
    }
    if (tag == 0x58385056) {  // "VP8X"(扩展):画布 24bit 宽高(减一存储)
        if (bytes.size() >= 30) {
            const std::uint32_t w = 1 + static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[24])) +
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[25])) << 8) +
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[26])) << 16);
            const std::uint32_t h = 1 + static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[27])) +
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[28])) << 8) +
                                    (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[29])) << 16);
            return {w, h};
        }
        return {};
    }
    return {};
}

}  // namespace

ImageDimensions ReadImageDimensions(const std::string& bytes, const std::string& mime_type) {
    if (mime_type == "image/png") {
        // IHDR 是首块(at 8 起块头,数据 16 起宽高各 4 字节大端)。
        if (bytes.size() >= 24 && ReadBe32(bytes, 12) == 0x49484452) {
            return {ReadBe32(bytes, 16), ReadBe32(bytes, 20)};
        }
        return {};
    }
    if (mime_type == "image/gif") {
        if (bytes.size() >= 10) {
            return {ReadLe16(bytes, 6), ReadLe16(bytes, 8)};
        }
        return {};
    }
    if (mime_type == "image/jpeg") {
        return JpegDimensions(bytes);
    }
    if (mime_type == "image/webp") {
        return WebpDimensions(bytes);
    }
    return {};
}

std::expected<ModelImageLanding, std::string> LandModelImage(const std::string& images_dir,
                                                             const api::ImageOutput& image) {
    if (images_dir.empty()) {
        return std::unexpected("会话图片目录未开,图片无处可落");
    }
    if (image.base64.empty()) {
        return std::unexpected("服务端回了空图片正文");
    }
    const auto decoded = DecodeBase64Strict(image.base64, kMaxModelImageBytes);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    const ImageFormat format = SniffImageFormat(*decoded);
    if (format.mime_type.empty()) {
        return std::unexpected("图片字节认不出格式(PNG/JPEG/GIF/WebP 以外),不落盘");
    }
    const ImageDimensions dims = ReadImageDimensions(*decoded, format.mime_type);
    const std::string sha = hooks::Sha256Hex(*decoded);

    const std::filesystem::path dir = platform::Utf8ToPath(images_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::unexpected("图片目录建不成(" + images_dir + "): " + ec.message());
    }

    ModelImageLanding landing;
    api::ModelImageBlock& block = landing.block;
    block.id = image.id;
    block.mime_type = format.mime_type;
    block.width = dims.width;
    block.height = dims.height;
    block.bytes = decoded->size();
    block.sha256 = sha;
    // P0-2:归拢进 session artifacts/sha256/(与 MCP rich、上下文仓 blob
    // 同层);文件名即内容地址(全 hash),同图天然只落一份。
    block.filename = sha + "." + format.extension;
    block.path = "artifacts/sha256/" + block.filename;

    const std::filesystem::path file = dir / platform::Utf8ToPath(block.filename);
    // 内容寻址:同图已落过就不再写(幂等,重复终帧/断点重来的自然去重)。
    if (!std::filesystem::exists(file) && !AtomicWriteBytes(file, *decoded)) {
        return std::unexpected("图片落盘失败: " + images_dir + "/" + block.filename);
    }
    const std::u8string u8 = file.u8string();
    landing.display_path = std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    return landing;
}

}  // namespace lubancode::agent
