#include "cli/image_input.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>

#include "platform/base64.hpp"  // Base64Encode:标准 base64 公共内核(审计 P2)

namespace lubancode::cli {

namespace {

namespace fs = std::filesystem;

fs::path Utf8Path(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string PathToUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

bool IsSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && IsSpace(text[begin])) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && IsSpace(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string ToLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::vector<std::string> ParsePathList(std::string_view input) {
    std::vector<std::string> paths;
    std::size_t pos = 0;
    while (pos < input.size()) {
        while (pos < input.size() && IsSpace(input[pos])) {
            ++pos;
        }
        if (pos == input.size()) {
            break;
        }
        const char quote = input[pos] == '"' || input[pos] == '\'' ? input[pos++] : '\0';
        const std::size_t begin = pos;
        if (quote != '\0') {
            while (pos < input.size() && input[pos] != quote) {
                ++pos;
            }
            paths.emplace_back(input.substr(begin, pos - begin));
            if (pos < input.size()) {
                ++pos;
            }
        } else {
            while (pos < input.size() && !IsSpace(input[pos])) {
                ++pos;
            }
            paths.emplace_back(input.substr(begin, pos - begin));
        }
    }
    return paths;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> ImageSize(const std::vector<std::uint8_t>& bytes,
                                                                   const std::string& media_type) {
    const auto be16 = [&](std::size_t at) -> std::uint32_t {
        return (static_cast<std::uint32_t>(bytes[at]) << 8U) | bytes[at + 1];
    };
    const auto be32 = [&](std::size_t at) -> std::uint32_t {
        return (static_cast<std::uint32_t>(bytes[at]) << 24U) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) | bytes[at + 3];
    };
    const auto valid = [](std::uint32_t width, std::uint32_t height)
        -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
        if (width == 0 || height == 0) {
            return std::nullopt;
        }
        return std::pair{width, height};
    };

    if (media_type == "image/png") {
        static constexpr std::uint8_t kPng[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        if (bytes.size() < 24 || !std::equal(std::begin(kPng), std::end(kPng), bytes.begin()) ||
            bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R') {
            return std::nullopt;
        }
        return valid(be32(16), be32(20));
    }
    if (media_type == "image/gif") {
        if (bytes.size() < 10 || bytes[0] != 'G' || bytes[1] != 'I' || bytes[2] != 'F') {
            return std::nullopt;
        }
        const std::uint32_t width = bytes[6] | (static_cast<std::uint32_t>(bytes[7]) << 8U);
        const std::uint32_t height = bytes[8] | (static_cast<std::uint32_t>(bytes[9]) << 8U);
        return valid(width, height);
    }
    if (media_type == "image/jpeg") {
        if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) {
            return std::nullopt;
        }
        std::size_t pos = 2;
        while (pos + 1 < bytes.size()) {
            if (bytes[pos++] != 0xff) {
                continue;
            }
            while (pos < bytes.size() && bytes[pos] == 0xff) {
                ++pos;
            }
            if (pos == bytes.size()) {
                break;
            }
            const std::uint8_t marker = bytes[pos++];
            if (marker == 0xd8 || marker == 0xd9) {
                continue;
            }
            if (marker == 0xda || pos + 1 >= bytes.size()) {
                break;
            }
            const std::size_t length = be16(pos);
            if (length < 2 || pos + length > bytes.size()) {
                break;
            }
            const bool sof = (marker >= 0xc0 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xc7) ||
                             (marker >= 0xc9 && marker <= 0xcb) || (marker >= 0xcd && marker <= 0xcf);
            if (sof && length >= 7) {
                // SOF 段布局: precision(1) + height/Y(2, 在 pos+3) + width/X(2, 在 pos+5)。
                return valid(be16(pos + 5), be16(pos + 3));
            }
            pos += length;
        }
        return std::nullopt;
    }
    if (media_type == "image/webp") {
        if (bytes.size() < 30 || bytes[0] != 'R' || bytes[1] != 'I' || bytes[2] != 'F' || bytes[3] != 'F' ||
            bytes[8] != 'W' || bytes[9] != 'E' || bytes[10] != 'B' || bytes[11] != 'P') {
            return std::nullopt;
        }
        std::size_t pos = 12;
        while (pos + 8 <= bytes.size()) {
            const std::uint32_t chunk_size = static_cast<std::uint32_t>(bytes[pos + 4]) |
                                             (static_cast<std::uint32_t>(bytes[pos + 5]) << 8U) |
                                             (static_cast<std::uint32_t>(bytes[pos + 6]) << 16U) |
                                             (static_cast<std::uint32_t>(bytes[pos + 7]) << 24U);
            const std::size_t data = pos + 8;
            if (data + chunk_size > bytes.size()) {
                return std::nullopt;
            }
            if (bytes[pos] == 'V' && bytes[pos + 1] == 'P' && bytes[pos + 2] == '8' && bytes[pos + 3] == 'X' &&
                chunk_size >= 10) {
                const std::uint32_t width = 1U + bytes[data + 4] + (static_cast<std::uint32_t>(bytes[data + 5]) << 8U) +
                                            (static_cast<std::uint32_t>(bytes[data + 6]) << 16U);
                const std::uint32_t height = 1U + bytes[data + 7] + (static_cast<std::uint32_t>(bytes[data + 8]) << 8U) +
                                             (static_cast<std::uint32_t>(bytes[data + 9]) << 16U);
                return valid(width, height);
            }
            if (bytes[pos] == 'V' && bytes[pos + 1] == 'P' && bytes[pos + 2] == '8' && bytes[pos + 3] == 'L' &&
                chunk_size >= 5 && bytes[data] == 0x2f) {
                const std::uint32_t width = 1U + bytes[data + 1] + ((bytes[data + 2] & 0x3fU) << 8U);
                const std::uint32_t height =
                    1U + ((bytes[data + 2] >> 6U) | (static_cast<std::uint32_t>(bytes[data + 3]) << 2U) |
                          ((bytes[data + 4] & 0x0fU) << 10U));
                return valid(width, height);
            }
            if (bytes[pos] == 'V' && bytes[pos + 1] == 'P' && bytes[pos + 2] == '8' && bytes[pos + 3] == ' ' &&
                chunk_size >= 10 && bytes[data + 3] == 0x9d && bytes[data + 4] == 0x01 && bytes[data + 5] == 0x2a) {
                const std::uint32_t width = bytes[data + 6] | ((bytes[data + 7] & 0x3fU) << 8U);
                const std::uint32_t height = bytes[data + 8] | ((bytes[data + 9] & 0x3fU) << 8U);
                return valid(width, height);
            }
            pos = data + chunk_size + (chunk_size % 2U);
        }
    }
    return std::nullopt;
}

std::expected<api::ImageBlock, ImageInputError> LoadImage(const std::string& input_path) {
    const std::optional<std::string> media_type = MediaTypeForPath(input_path);
    if (!media_type.has_value()) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::UnsupportedType, input_path});
    }

    const fs::path path = Utf8Path(input_path);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::NotFound, input_path});
    }
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::NotRegularFile, input_path});
    }
    const std::uintmax_t file_size = fs::file_size(path, ec);
    if (ec) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::ReadFailed, input_path});
    }
    if (file_size > kMaxImageBytes) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::TooLarge, input_path});
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::ReadFailed, input_path});
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file && !bytes.empty()) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::ReadFailed, input_path});
    }
    const auto size = ImageSize(bytes, *media_type);
    if (!size.has_value()) {
        return std::unexpected(ImageInputError{ImageInputErrorKind::InvalidImage, input_path});
    }

    api::ImageBlock image;
    image.media_type = *media_type;
    image.data = Base64Encode(bytes);
    image.filename = PathToUtf8(path.filename());
    image.width = size->first;
    image.height = size->second;
    return image;
}

}  // namespace

// 内联 @词的判定规矩:扩展名属于 png/jpg/jpeg/gif/webp 才算图片候选;
// @cache/@todo/@某人、邮箱、代码注解一类普通提及原样留在正文,不进图片
// 入口(旧毛病:内联 @ 太宽、图片加载又太严,普通提及整条消息被格式错误
// 截走)。明确图片模样但文件不存在/损坏的,仍照旧报错,不悄悄降成正文;
// /image 是明确命令,继续走严格校验,不受这条收窄影响。
namespace {

bool LooksLikeImagePath(std::string_view path) {
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string_view extension = path.substr(dot);
    auto equals = [&](std::string_view wanted) {
        if (extension.size() != wanted.size()) {
            return false;
        }
        for (std::size_t i = 0; i < extension.size(); ++i) {
            const char left = static_cast<char>(std::tolower(static_cast<unsigned char>(extension[i])));
            if (left != wanted[i]) {
                return false;
            }
        }
        return true;
    };
    return equals(".png") || equals(".jpg") || equals(".jpeg") || equals(".gif") || equals(".webp");
}

}  // namespace

std::vector<std::string> ParseInlineImagePaths(std::string_view input) {
    std::vector<std::string> paths;
    for (std::size_t pos = 0; pos < input.size(); ++pos) {
        if (input[pos] != '@' || (pos > 0 && !IsSpace(input[pos - 1]))) {
            continue;
        }
        std::size_t begin = pos + 1;
        if (begin == input.size()) {
            continue;
        }
        // 0.30.x 起角括号也是引式(@<带 空格 的路径>,Alt+V 贴图与 @ 提及
        // 菜单插的就是这种):开 '<' 闭 '>'。
        char quote = '\0';
        char closer = '\0';
        if (input[begin] == '"' || input[begin] == '\'') {
            quote = input[begin];
            closer = quote;
            ++begin;
        } else if (input[begin] == '<') {
            quote = '<';
            closer = '>';
            ++begin;
        }
        std::size_t end = begin;
        if (quote != '\0') {
            while (end < input.size() && input[end] != closer) {
                ++end;
            }
            if (end == input.size() || !LooksLikeImagePath(input.substr(begin, end - begin))) {
                continue;  // 引号没闭合,或不是图片模样的提及:留在正文
            }
            if (end == input.size()) {
                continue;
            }
            paths.emplace_back(input.substr(begin, end - begin));
            pos = end;
        } else {
            while (end < input.size() && !IsSpace(input[end])) {
                ++end;
            }
            if (end > begin && LooksLikeImagePath(input.substr(begin, end - begin))) {
                paths.emplace_back(input.substr(begin, end - begin));
            }
            pos = end == 0 ? 0 : end - 1;
        }
    }
    return paths;
}

std::optional<std::string> MediaTypeForPath(std::string_view path) {
    const std::string extension = ToLower(PathToUtf8(Utf8Path(path).extension()));
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    return std::nullopt;
}

std::string Base64Encode(const std::vector<std::uint8_t>& bytes) {
    // 薄口:标准 base64 内核统一住 platform/base64(审计 P2:五处手写收
    // 一)。签名保留 vector 入参——历史导出面,测试与调用方在用。
    return lubancode::platform::Base64Encode(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

std::expected<PreparedImageInput, ImageInputError> PrepareImageInput(std::string_view input) {
    PreparedImageInput prepared;
    prepared.message.role = api::Role::User;

    const std::string trimmed = Trim(input);
    const std::size_t word_end = trimmed.find_first_of(" \t\r\n");
    const std::string command = ToLower(trimmed.substr(0, word_end));
    const bool image_command = command == "/image";
    std::vector<std::string> paths;
    if (image_command) {
        paths = ParsePathList(word_end == std::string::npos ? std::string_view() : std::string_view(trimmed).substr(word_end));
        if (paths.empty()) {
            return std::unexpected(ImageInputError{ImageInputErrorKind::MissingPath, {}});
        }
    } else {
        prepared.message.content.push_back(api::TextBlock{std::string(input)});
        paths = ParseInlineImagePaths(input);
    }

    for (const std::string& path : paths) {
        if (path.empty()) {
            return std::unexpected(ImageInputError{ImageInputErrorKind::MissingPath, path});
        }
        auto image = LoadImage(path);
        if (!image.has_value()) {
            return std::unexpected(image.error());
        }
        prepared.attachments.push_back(ImageAttachmentInfo{image->filename, image->width, image->height});
        prepared.message.content.push_back(std::move(*image));
    }
    return prepared;
}

}  // namespace lubancode::cli
