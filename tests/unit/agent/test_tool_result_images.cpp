// 工具结果图片回喂单:artifact 重灌(agent::RehydrateToolResultImages)的
// 纯函数单测——好图灌上、伪 MIME 拒、字节帽拒、脏文件名拒、文件丢了不炸、
// 幂等与请求合计帽;外加预检图片 token 的像素口径尺
// (EstimateImageTokensForPreflight)。不碰网络;artifact 目录用临时目录。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "agent/model_image_store.hpp"  // DecodeBase64Strict(解码夹具 base64)
#include "agent/tool_result_images.hpp"

using namespace lubancode;
namespace agent = lubancode::agent;

namespace {

// 1x1 PNG(与 fixtures/mcp_test_server.py 同一颗)。
const char* kTinyPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg==";

struct TempArtifactDir {
    std::filesystem::path path;
    TempArtifactDir() {
        std::error_code ec;
        path = std::filesystem::temp_directory_path() /
              ("lubancode_tool_image_test_" + std::to_string(++counter_));
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~TempArtifactDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string utf8() const { return path.generic_string(); }
    void Write(const std::string& name, const std::string& bytes) const {
        std::ofstream out(path / name, std::ios::binary);
        out << bytes;
    }

  private:
    static int counter_;
};
int TempArtifactDir::counter_ = 0;

tools::ImageContent MakeImage(const std::string& filename, std::size_t bytes = 70) {
    tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 1;
    image.height = 1;
    image.bytes = bytes;
    image.artifact.filename = filename;
    image.artifact.path = "mcp-artifacts/" + filename;
    image.artifact.stored = true;
    return image;
}

api::Message ResultWith(tools::ImageContent image) {
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "c1";
    rich.content = "已截图";
    rich.blocks.push_back(std::move(image));
    message.content.push_back(std::move(rich));
    return message;
}

std::string TinyPngBytes() {
    const auto decoded = agent::DecodeBase64Strict(kTinyPngB64, 1024);
    REQUIRE(decoded.has_value());
    return *decoded;
}

// ---- 像素口径尺的夹具:合成带指定宽高头的 PNG/JPEG 字节,再编 base64。----

std::string TestBase64Encode(const std::string& bytes) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
    if (bytes.size() - i == 1) {
        const std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += "==";
    } else if (bytes.size() - i == 2) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

void AppendBe32(std::string& out, std::uint32_t value) {
    out += static_cast<char>((value >> 24) & 0xFF);
    out += static_cast<char>((value >> 16) & 0xFF);
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>(value & 0xFF);
}

void AppendBe16(std::string& out, std::uint32_t value) {
    out += static_cast<char>((value >> 8) & 0xFF);
    out += static_cast<char>(value & 0xFF);
}

// 只带签名 + IHDR 头的 PNG 段(ReadImageDimensions 只认签名与 IHDR 标签,
// CRC 不进门)。宽高按大端写进 16..24。
std::string MakePngHeaderBytes(std::uint32_t width, std::uint32_t height) {
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    AppendBe32(bytes, 13);  // IHDR 长度
    bytes += "IHDR";
    AppendBe32(bytes, width);
    AppendBe32(bytes, height);
    bytes += std::string(5, '\x08');  // bit depth/color type/压缩/滤波/隔行
    return bytes;
}

// SOF0 段的 JPEG 头(同 test_model_image_store 的形状):宽高大端住段里。
std::string MakeJpegHeaderBytes(std::uint32_t width, std::uint32_t height) {
    std::string bytes("\xFF\xD8", 2);  // SOI
    bytes += '\xFF';
    bytes += '\xC0';  // SOF0
    AppendBe16(bytes, 17);  // 段长
    bytes += '\x08';        // 精度
    AppendBe16(bytes, height);
    AppendBe16(bytes, width);
    bytes += '\x01';  // 分量数
    bytes += std::string(9, '\x00');
    bytes += '\xFF';
    bytes += '\xD9';  // EOI
    return bytes;
}

}  // namespace

TEST_CASE("重灌: 好图从 artifact 落盘读回 base64,与原编码逐字节一致") {
    TempArtifactDir dir;
    const std::string png = TinyPngBytes();
    dir.Write("art-00112233.png", png);

    api::Request request;
    request.model = "m";
    request.messages.push_back(ResultWith(MakeImage("art-00112233.png", png.size())));
    CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 1);
    const auto& rich = std::get<api::ToolResultBlock>(request.messages[0].content[0]);
    const auto& image = std::get<tools::ImageContent>(rich.blocks[0]);
    CHECK(image.wire_base64 == kTinyPngB64);
}

TEST_CASE("重灌: 伪 MIME(文件内容与声明对不上)拒灌") {
    TempArtifactDir dir;
    dir.Write("art-00112233.png", "this is definitely not a png file");
    api::Request request;
    request.model = "m";
    request.messages.push_back(ResultWith(MakeImage("art-00112233.png", 35)));
    CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    const auto& rich = std::get<api::ToolResultBlock>(request.messages[0].content[0]);
    CHECK(std::get<tools::ImageContent>(rich.blocks[0]).wire_base64.empty());
}

TEST_CASE("重灌: 账面字节超帽/文件丢失/脏文件名/未落盘标记各按跳过收口") {
    TempArtifactDir dir;
    const std::string png = TinyPngBytes();
    dir.Write("art-good.png", png);

    // 账面超帽:不读文件直接跳。
    {
        api::Request request;
        request.messages.push_back(ResultWith(MakeImage("art-good.png", agent::kMaxToolResultWireImageBytes + 1)));
        CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    }
    // 文件不在:跳,不炸。
    {
        api::Request request;
        request.messages.push_back(ResultWith(MakeImage("art-missing.png", png.size())));
        CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    }
    // 脏文件名(路径串了进来):拒读。
    for (const char* dirty : {"../escape.png", "sub/dir.png", "back\\slash.png", ".."}) {
        api::Request request;
        request.messages.push_back(ResultWith(MakeImage(dirty, png.size())));
        CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    }
    // stored=false(字节没落盘):投影已明说"未落盘",这里不冒充可取。
    {
        api::Request request;
        auto image = MakeImage("art-good.png", png.size());
        image.artifact.stored = false;
        request.messages.push_back(ResultWith(std::move(image)));
        CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    }
}

TEST_CASE("重灌: 空 artifact 目录零操作;幂等——已灌过的不再灌") {
    api::Request request;
    request.messages.push_back(ResultWith(MakeImage("art-any.png")));
    CHECK(agent::RehydrateToolResultImages(request, "") == 0);

    TempArtifactDir dir;
    const std::string png = TinyPngBytes();
    dir.Write("art-good.png", png);
    request.messages.clear();
    request.messages.push_back(ResultWith(MakeImage("art-good.png", png.size())));
    CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 1);
    // 第二轮(同请求对象):已灌过,不再动。
    CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 0);
    CHECK(std::get<tools::ImageContent>(std::get<api::ToolResultBlock>(request.messages[0].content[0]).blocks[0])
              .wire_base64 == kTinyPngB64);
}

TEST_CASE("重灌: 多张图逐张判——好的都灌,坏的各按各的规矩跳过") {
    TempArtifactDir dir;
    const std::string png = TinyPngBytes();
    dir.Write("a.png", png);
    dir.Write("b.png", png);
    dir.Write("c.png", "not a png at all");  // 伪 MIME
    dir.Write("d.png", png);                 // 账面超帽(帽内文件配帽外账)

    api::Request request;
    request.model = "m";
    api::Message message;
    message.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "c1";
    rich.content = "四张图";
    rich.blocks.push_back(MakeImage("a.png", png.size()));
    rich.blocks.push_back(MakeImage("b.png", png.size()));
    rich.blocks.push_back(MakeImage("c.png", 16));
    rich.blocks.push_back(MakeImage("d.png", agent::kMaxToolResultWireImageBytes + 1));
    message.content.push_back(std::move(rich));
    request.messages.push_back(message);

    // a、b 灌上;c 伪 MIME 拒、d 账面超帽拒——一坏不连坐。
    CHECK(agent::RehydrateToolResultImages(request, dir.utf8()) == 2);
    const auto& blocks = std::get<api::ToolResultBlock>(request.messages[0].content[0]).blocks;
    CHECK(std::get<tools::ImageContent>(blocks[0]).wire_base64 == kTinyPngB64);
    CHECK(std::get<tools::ImageContent>(blocks[1]).wire_base64 == kTinyPngB64);
    CHECK(std::get<tools::ImageContent>(blocks[2]).wire_base64.empty());
    CHECK(std::get<tools::ImageContent>(blocks[3]).wire_base64.empty());
}

// ---------------------------------------------------------------------------
// 预检图片 token 的像素口径尺:宽×高/750 优先,base64 读头次之,字节兜底。
// ---------------------------------------------------------------------------

TEST_CASE("图片 token 预检尺: 宽高已知按像素折,大图不再记成 base64 字节账") {
    // 用户炸单的那副形状:3072x1918 的整窗截图。像素口径 5892096/750=7856;
    // 老字节口径会把约 2.6MB base64 记成 65 万 token,误报越窗。
    CHECK(agent::EstimateImageTokensForPreflight(3072, 1918, std::string(2632812, 'A')) == 7856);
    // 1568 长边内的小图:1568*982/750=2053。
    CHECK(agent::EstimateImageTokensForPreflight(1568, 982, "") == 2053);
    // 1x1 极小图:像素口径下不足一枚 token,记 0。
    CHECK(agent::EstimateImageTokensForPreflight(1, 1, kTinyPngB64) == 0);
    // base64 都没有(未重灌的引用块):图不上 wire,零账。
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, "") == 0);
}

TEST_CASE("图片 token 预检尺: 宽高未知从 base64 读头——PNG IHDR / JPEG SOFn") {
    const std::string png_b64 = TestBase64Encode(MakePngHeaderBytes(1568, 982));
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, png_b64) == 2053);
    const std::string jpeg_b64 = TestBase64Encode(MakeJpegHeaderBytes(200, 100));
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, jpeg_b64) == 26);  // 20000/750
    // 宽高只报了一半:不算已知,走解码读头(夹具真尺寸 1568x982)。
    CHECK(agent::EstimateImageTokensForPreflight(0, 982, png_b64) == 2053);
}

TEST_CASE("图片 token 预检尺: 读不出宽高退字节口径——截断头/认不得格式/坏 base64") {
    // PNG 签名认得但 IHDR 被截(不足 24 字节):格式对、宽高读不出。
    std::string truncated("\x89PNG\r\n\x1a\n", 8);
    truncated += std::string(2, '\0');
    const std::string truncated_b64 = TestBase64Encode(truncated);
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, truncated_b64) == truncated_b64.size() / 4);
    // 合法 base64 但字节认不出格式(解码全零):老尺 base64/4 兜底。
    const std::string garbage_b64(4000, 'A');
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, garbage_b64) == 1000);
    // 坏 base64(长度不是 4 的倍数):同样兜底,不炸。
    const std::string bad_b64(4001, 'A');
    CHECK(agent::EstimateImageTokensForPreflight(0, 0, bad_b64) == 4001 / 4);
}
