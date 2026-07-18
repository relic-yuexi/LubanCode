#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "api/types.hpp"

namespace lubancode::cli {

constexpr std::size_t kMaxImageBytes = 5U * 1024U * 1024U;

enum class ImageInputErrorKind {
    MissingPath,
    NotFound,
    NotRegularFile,
    UnsupportedType,
    TooLarge,
    ReadFailed,
    InvalidImage,
};

struct ImageInputError {
    ImageInputErrorKind kind = ImageInputErrorKind::ReadFailed;
    std::string path;
};

struct ImageAttachmentInfo {
    std::string filename;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct PreparedImageInput {
    api::Message message;
    std::vector<ImageAttachmentInfo> attachments;
};

// 抓普通输入里以空白开头的 @路径；带空格的路径可写成 @"a b.png"。
std::vector<std::string> ParseInlineImagePaths(std::string_view input);

// png/jpg/jpeg/gif/webp -> MIME；其他扩展名不给过。
std::optional<std::string> MediaTypeForPath(std::string_view path);

// 标准 base64，无换行。单列出来，方便小文件的纯函数测试。
std::string Base64Encode(const std::vector<std::uint8_t>& bytes);

// 普通输入里的 @路径，或单独的 /image <路径...>，都在这里读成 ImageBlock。
std::expected<PreparedImageInput, ImageInputError> PrepareImageInput(std::string_view input);

}  // namespace lubancode::cli
