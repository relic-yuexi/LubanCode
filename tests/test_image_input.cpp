#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "cli/image_input.hpp"

using namespace lubancode;

namespace {

std::vector<std::uint8_t> Png(std::uint32_t width, std::uint32_t height) {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 'I',  'H',  'D',  'R',
        static_cast<std::uint8_t>(width >> 24U), static_cast<std::uint8_t>(width >> 16U),
        static_cast<std::uint8_t>(width >> 8U), static_cast<std::uint8_t>(width),
        static_cast<std::uint8_t>(height >> 24U), static_cast<std::uint8_t>(height >> 16U),
        static_cast<std::uint8_t>(height >> 8U), static_cast<std::uint8_t>(height),
    };
}

std::filesystem::path FixtureDir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-image-input-tests";
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

}  // namespace

TEST_CASE("图片路径识别: @路径和带空格的引号路径") {
    const auto paths = cli::ParseInlineImagePaths("看看 @screens/err.png 再看 @\"shots/last one.JPEG\"");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "screens/err.png");
    CHECK(paths[1] == "shots/last one.JPEG");
}

TEST_CASE("普通 @词不是图片:留在正文,不进图片入口") {
    // 内联候选只认图片模样的扩展名;@cache/@todo/邮箱/代码注解原样留正文。
    CHECK(cli::ParseInlineImagePaths("@cache").empty());
    CHECK(cli::ParseInlineImagePaths("提到 @todo 再往下说").empty());
    CHECK(cli::ParseInlineImagePaths("邮箱 foo@bar.com").empty());
    CHECK(cli::ParseInlineImagePaths("代码 @decorator").empty());
    CHECK(cli::ParseInlineImagePaths("@\"我的笔记\"").empty());
    // 混合:普通提及留下,真图片只认那一枚。
    const auto mixed = cli::ParseInlineImagePaths("先提 @cache,再看 @a.png");
    REQUIRE(mixed.size() == 1);
    CHECK(mixed[0] == "a.png");
    // 大写后缀仍算图片候选(交给加载层判存在与格式)。
    const auto upper = cli::ParseInlineImagePaths("看图 @shot.PNG");
    REQUIRE(upper.size() == 1);
    CHECK(upper[0] == "shot.PNG");

    // 整条消息层面:正文逐字保留,零附件,不报错。
    const auto plain = cli::PrepareImageInput("看到没有,@cache 里那句漂亮话,没交代答案怎么长出来的。");
    REQUIRE(plain.has_value());
    REQUIRE(plain->attachments.empty());
    REQUIRE(plain->message.content.size() == 1);
    CHECK(std::get<api::TextBlock>(plain->message.content[0]).text ==
          "看到没有,@cache 里那句漂亮话,没交代答案怎么长出来的。");

    // 明确图片模样但不存在:仍报文件不存在,不悄悄降成正文。
    const auto absent = cli::PrepareImageInput("看图 @missing.png");
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error().kind == cli::ImageInputErrorKind::NotFound);
}

TEST_CASE("图片 MIME 和 base64") {
    CHECK(cli::MediaTypeForPath("a.PNG") == "image/png");
    CHECK(cli::MediaTypeForPath("a.jpeg") == "image/jpeg");
    CHECK(cli::MediaTypeForPath("a.gif") == "image/gif");
    CHECK(cli::MediaTypeForPath("a.webp") == "image/webp");
    CHECK_FALSE(cli::MediaTypeForPath("a.bmp").has_value());
    CHECK(cli::Base64Encode(std::vector<std::uint8_t>{'M'}) == "TQ==");
    CHECK(cli::Base64Encode(std::vector<std::uint8_t>{'M', 'a'}) == "TWE=");
    CHECK(cli::Base64Encode(std::vector<std::uint8_t>{'M', 'a', 'n'}) == "TWFu");
}

TEST_CASE("普通输入的图片读入 base64 并保留文本") {
    const std::filesystem::path path = FixtureDir() / "error.png";
    const auto bytes = Png(640, 480);
    WriteBytes(path, bytes);

    const auto prepared = cli::PrepareImageInput("看看这个报错 @" + path.string());
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->attachments.size() == 1);
    CHECK(prepared->attachments[0].filename == "error.png");
    CHECK(prepared->attachments[0].width == 640);
    CHECK(prepared->attachments[0].height == 480);
    REQUIRE(prepared->message.content.size() == 2);
    CHECK(std::get<api::TextBlock>(prepared->message.content[0]).text.find("看看") != std::string::npos);
    const auto* image = std::get_if<api::ImageBlock>(&prepared->message.content[1]);
    REQUIRE(image != nullptr);
    CHECK(image->media_type == "image/png");
    CHECK(image->data == cli::Base64Encode(bytes));
}

TEST_CASE("gif、jpeg、webp 从文件头读出尺寸") {
    const std::filesystem::path gif = FixtureDir() / "size.gif";
    WriteBytes(gif, {'G', 'I', 'F', '8', '9', 'a', 7, 0, 9, 0});
    const auto gif_input = cli::PrepareImageInput("@" + gif.string());
    REQUIRE(gif_input.has_value());
    CHECK(gif_input->attachments[0].width == 7);
    CHECK(gif_input->attachments[0].height == 9);

    const std::filesystem::path jpeg = FixtureDir() / "size.jpg";
    WriteBytes(jpeg, {0xff, 0xd8, 0xff, 0xc0, 0x00, 0x11, 0x08, 0x00, 0x0a, 0x00, 0x14,
                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto jpeg_input = cli::PrepareImageInput("@" + jpeg.string());
    REQUIRE(jpeg_input.has_value());
    CHECK(jpeg_input->attachments[0].width == 20);
    CHECK(jpeg_input->attachments[0].height == 10);

    const std::filesystem::path webp = FixtureDir() / "size.webp";
    WriteBytes(webp, {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X',
                      10, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 19, 0, 0});
    const auto webp_input = cli::PrepareImageInput("@" + webp.string());
    REQUIRE(webp_input.has_value());
    CHECK(webp_input->attachments[0].width == 10);
    CHECK(webp_input->attachments[0].height == 20);
}
TEST_CASE("/image 一轮可带多张图片") {
    const std::filesystem::path first = FixtureDir() / "first.png";
    const std::filesystem::path second = FixtureDir() / "second.png";
    WriteBytes(first, Png(12, 34));
    WriteBytes(second, Png(56, 78));

    const auto prepared = cli::PrepareImageInput("/image \"" + first.string() + "\" \"" + second.string() + "\"");
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->message.content.size() == 2);
    CHECK(std::get<api::ImageBlock>(prepared->message.content[0]).width == 12);
    CHECK(std::get<api::ImageBlock>(prepared->message.content[1]).height == 78);
}

TEST_CASE("图片入口报清楚的路径、类型、大小错误") {
    const auto empty = cli::PrepareImageInput("/image");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == cli::ImageInputErrorKind::MissingPath);

    const auto absent = cli::PrepareImageInput("@does-not-exist.png");
    REQUIRE_FALSE(absent.has_value());
    CHECK(absent.error().kind == cli::ImageInputErrorKind::NotFound);

    const auto unsupported = cli::PrepareImageInput("/image notes.txt");
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error().kind == cli::ImageInputErrorKind::UnsupportedType);

    const std::filesystem::path too_large = FixtureDir() / "too-large.png";
    std::ofstream file(too_large, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.seekp(static_cast<std::streamoff>(cli::kMaxImageBytes));
    file.put('\0');
    file.close();
    const auto oversized = cli::PrepareImageInput("@" + too_large.string());
    REQUIRE_FALSE(oversized.has_value());
    CHECK(oversized.error().kind == cli::ImageInputErrorKind::TooLarge);
}
