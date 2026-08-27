// 工具结果图片回喂单:artifact 重灌(agent::RehydrateToolResultImages)的
// 纯函数单测——好图灌上、伪 MIME 拒、字节帽拒、脏文件名拒、文件丢了不炸、
// 幂等与请求合计帽。不碰网络;artifact 目录用临时目录。
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
