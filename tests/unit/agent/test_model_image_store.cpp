// 模型输出图片的落盘口(ccmoon 真机巡检单 P0):base64 限额解码、魔数
// 验身、宽高读取、原子落盘与幂等。真字节在本册自造(1x1 PNG、2x3 GIF、
// 100x200 JPEG、手工 WebP 头),不依赖网络与真机。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/model_image_store.hpp"

using namespace lubancode;

namespace {

// 1x1 真 PNG(整只文件,魔数/IHDR/宽高齐全)。
const char* kPng1x1Base64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=";

std::filesystem::path TempDir(const char* tag) {
    static std::atomic<unsigned> counter{0};
    const auto base = std::filesystem::temp_directory_path() /
                      ("lubancode_img_" + std::string(tag) + "_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) +
                       "_" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    return base;
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

TEST_CASE("DecodeBase64Strict:合法/坏字母表/坏 padding/超限") {
    // 空串合法(空图),标准表合法。
    CHECK(agent::DecodeBase64Strict("", 1024).has_value());
    const auto png = agent::DecodeBase64Strict(kPng1x1Base64, agent::kMaxModelImageBytes);
    REQUIRE(png.has_value());
    CHECK(png->size() == 68);  // 92 字符 base64(1 个 padding)-> 68 字节

    // URL 安全变体(- _)拒收。
    CHECK_FALSE(agent::DecodeBase64Strict("ab-cd", 1024).has_value());
    // 长度不是 4 的倍数拒收。
    CHECK_FALSE(agent::DecodeBase64Strict("abc", 1024).has_value());
    // padding 藏在中间拒收。
    CHECK_FALSE(agent::DecodeBase64Strict("ab==cd==", 1024).has_value());
    // 解码后超限拒收(上限 3 字节,这串解出来 6 字节)。
    CHECK_FALSE(agent::DecodeBase64Strict("AAAAAAAAAAAA", 3).has_value());
}

TEST_CASE("SniffImageFormat:PNG/JPEG/GIF/WebP 认得出,认不得的给空") {
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png += std::string(20, '\0');
    CHECK(agent::SniffImageFormat(png).mime_type == "image/png");

    std::string jpeg("\xFF\xD8\xFF\xE0", 4);
    jpeg += std::string(20, '\0');
    CHECK(agent::SniffImageFormat(jpeg).mime_type == "image/jpeg");

    std::string gif89a = "GIF89a";
    gif89a += std::string(10, '\0');
    CHECK(agent::SniffImageFormat(gif89a).mime_type == "image/gif");

    std::string riff = "RIFF";
    riff += std::string(4, '\0');
    riff += "WEBPVP8 ";
    CHECK(agent::SniffImageFormat(riff).mime_type == "image/webp");

    CHECK(agent::SniffImageFormat("not an image at all").mime_type.empty());
    CHECK(agent::SniffImageFormat("").mime_type.empty());
}

TEST_CASE("ReadImageDimensions:PNG IHDR / GIF 逻辑屏 / JPEG SOF0 / WebP VP8L") {
    // PNG 1x1。
    const auto png = agent::DecodeBase64Strict(kPng1x1Base64, agent::kMaxModelImageBytes);
    REQUIRE(png.has_value());
    const auto png_dims = agent::ReadImageDimensions(*png, "image/png");
    CHECK(png_dims.width == 1);
    CHECK(png_dims.height == 1);

    // GIF 2x3(逻辑屏幕描述符,小端)。
    std::string gif = "GIF89a";
    gif += '\x02';
    gif += '\x00';
    gif += '\x03';
    gif += '\x00';
    gif += std::string(4, '\0');
    const auto gif_dims = agent::ReadImageDimensions(gif, "image/gif");
    CHECK(gif_dims.width == 2);
    CHECK(gif_dims.height == 3);

    // JPEG SOF0:高 0x0064、宽 0x00C8(大端)。头 12 字节里有内嵌 NUL,
    // 必须走 char 数组带长度构造——+= const char* 会按 strlen 截在 NUL 上。
    const char jpeg_head[12] = {'\xFF', '\xD8', '\xFF', '\xC0', '\x00', '\x11',
                                '\x08', '\x00', '\x64', '\x00', '\xC8', '\x03'};
    std::string jpeg(jpeg_head, sizeof(jpeg_head));
    for (int i = 0; i < 9; ++i) {
        jpeg += '\x00';
    }
    jpeg += '\xFF';
    jpeg += '\xD9';  // EOI
    REQUIRE(jpeg.size() == 23);
    const auto jpeg_dims = agent::ReadImageDimensions(jpeg, "image/jpeg");
    CHECK(jpeg_dims.width == 200);
    CHECK(jpeg_dims.height == 100);

    // WebP VP8L:签名 0x2F 后 4 字节装 14bit 宽-1 / 高-1(小端拼)。
    std::string webp = "RIFF";
    webp += std::string(4, '\0');
    webp += "WEBPVP8L";
    webp += std::string(4, '\0');  // chunk 大小
    webp += '\x2F';                // VP8L 签名
    // w-1=63(0x3F)、h-1=31(0x1F):bits = 63 | (31 << 14)
    const std::uint32_t bits = 63U | (31U << 14);
    for (int i = 0; i < 4; ++i) {
        webp += static_cast<char>((bits >> (8 * i)) & 0xFF);
    }
    const auto webp_dims = agent::ReadImageDimensions(webp, "image/webp");
    CHECK(webp_dims.width == 64);
    CHECK(webp_dims.height == 32);
}

TEST_CASE("LandModelImage:真 PNG 落盘、文件名内容寻址、幂等只写一回") {
    const auto dir = TempDir("land");
    const std::string narrow = dir.string();

    api::ImageOutput image;
    image.id = "ig_001";
    image.base64 = kPng1x1Base64;

    const auto first = agent::LandModelImage(narrow, image);
    REQUIRE(first.has_value());
    CHECK(first->block.id == "ig_001");
    CHECK(first->block.mime_type == "image/png");
    CHECK(first->block.width == 1);
    CHECK(first->block.height == 1);
    CHECK(first->block.bytes == 68);
    // P0-2:文件名即内容地址(全 hash,归拢进 artifacts/sha256/)。
    CHECK(first->block.filename.substr(0, 4) != std::string("img-"));
    CHECK(first->block.filename.size() == 64 + 1 + 3);  // <sha256>.png
    CHECK(EndsWith(first->block.filename, ".png"));
    CHECK(first->block.path == "artifacts/sha256/" + first->block.filename);
    CHECK(first->block.sha256.size() == 64);
    // 文件真落了,字节与解码后一致。
    const auto written = ReadFileBytes(dir / first->block.filename);
    REQUIRE(written.size() == 68);
    CHECK(written.substr(0, 4) == std::string("\x89PNG", 4));
    // 同图再落(重复终帧):不报错、不另起文件。
    const auto second = agent::LandModelImage(narrow, image);
    REQUIRE(second.has_value());
    CHECK(second->block.filename == first->block.filename);
    int png_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".png") {
            ++png_count;
        }
        // tmp 不许残留(原子写收尾干净)。
        CHECK(entry.path().extension() != ".tmp");
    }
    CHECK(png_count == 1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("LandModelImage:坏 base64/坏魔数/空目录,各给各的人话,不落半截文件") {
    const auto dir = TempDir("reject");
    const std::string narrow = dir.string();

    api::ImageOutput bad_b64;
    bad_b64.id = "ig_b";
    bad_b64.base64 = "not!base!64!";
    const auto b64_err = agent::LandModelImage(narrow, bad_b64);
    REQUIRE_FALSE(b64_err.has_value());
    CHECK(b64_err.error().find("base64") != std::string::npos);

    // 合法 base64、内容不是图。
    api::ImageOutput not_image;
    not_image.id = "ig_n";
    not_image.base64 = "aGVsbG8gd29ybGQ=";  // "hello world"
    const auto magic_err = agent::LandModelImage(narrow, not_image);
    REQUIRE_FALSE(magic_err.has_value());
    CHECK(magic_err.error().find("认不出格式") != std::string::npos);

    // 空目录:明说没处落。
    api::ImageOutput image;
    image.id = "ig_e";
    image.base64 = kPng1x1Base64;
    const auto dir_err = agent::LandModelImage("", image);
    REQUIRE_FALSE(dir_err.has_value());
    CHECK(dir_err.error().find("未开") != std::string::npos);

    // 目录里不许留任何半截文件。
    CHECK(std::filesystem::is_empty(dir));
    std::filesystem::remove_all(dir);
}

TEST_CASE("ModelImageReplayText:引用翻短文本,不带 base64") {
    api::ModelImageBlock block;
    block.id = "ig_1";
    block.filename = "abcd1234.png";
    block.width = 1024;
    block.height = 768;
    const std::string text = api::ModelImageReplayText(block);
    CHECK(text.find("abcd1234.png") != std::string::npos);
    CHECK(text.find("1024x768") != std::string::npos);
    CHECK(text.find("iVBOR") == std::string::npos);
}
