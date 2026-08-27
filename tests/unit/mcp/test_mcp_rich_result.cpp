// MCP 富结果单 P0.7:tools/call 富结果解析(mcp::ParseCallToolResult)的
// 纯函数单测——六类块逐块解析、块序保持、坏 base64、伪 MIME、大小帽、
// structuredContent 的 outputSchema 校验、artifact 落盘的内容寻址与幂等。
// 不起进程;真进程链路(含 Python 夹具)见 test_mcp_client.cpp。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "mcp/rich_result.hpp"

using namespace lubancode;
using namespace lubancode::mcp;

namespace {

// 1x1 PNG(与 fixtures/mcp_test_server.py 同一颗)。
const char* kTinyPngB64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg==";

std::string TempArtifactDir() {
    const auto dir = std::filesystem::temp_directory_path() / "lubancode_mcp_rich_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.generic_string();
}

CallToolParseContext MakeContext(std::string artifact_dir = TempArtifactDir()) {
    CallToolParseContext context;
    context.server_name = "test";
    context.artifact_dir = std::move(artifact_dir);
    return context;
}

}  // namespace

TEST_CASE("解析: text 块按序拼接,structuredContent 原样保留") {
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", "一"}},
                                           {{"type", "text"}, {"text", "二"}}})},
        {"structuredContent", nlohmann::json{{"ok", true}}},
        {"isError", false}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE_FALSE(parsed.protocol_error);
    REQUIRE(parsed.payload.content.size() == 2);
    CHECK(std::get<tools::TextContent>(parsed.payload.content[0]).text == "一");
    CHECK(std::get<tools::TextContent>(parsed.payload.content[1]).text == "二");
    REQUIRE(parsed.payload.structured_content.has_value());
    CHECK((*parsed.payload.structured_content)["ok"] == true);
    CHECK(tools::TextProjection(parsed.payload) == "一二");  // 有文本就不重复投影 JSON
}

TEST_CASE("解析: image 块解码落盘,块里只剩引用(MIME/尺寸/SHA 齐全)") {
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", "图"}},
                                           {{"type", "image"}, {"data", kTinyPngB64}, {"mimeType", "image/png"}}})}};
    const std::string dir = TempArtifactDir();
    const auto parsed = ParseCallToolResult(result, MakeContext(dir));
    REQUIRE_FALSE(parsed.protocol_error);
    REQUIRE(parsed.payload.content.size() == 2);
    const auto& image = std::get<tools::ImageContent>(parsed.payload.content[1]);
    CHECK(image.mime_type == "image/png");
    CHECK(image.width == 1);
    CHECK(image.height == 1);
    CHECK(image.artifact.stored);
    CHECK(image.artifact.path.rfind(dir + "/art-", 0) == 0);
    CHECK(image.artifact.path.find(".png") != std::string::npos);
    CHECK(parsed.landed_bytes == image.bytes);

    // 文件真在盘上,内容寻址幂等:同字节再来一次还是同一文件名。
    const auto again = ParseCallToolResult(result, MakeContext(dir));
    REQUIRE_FALSE(again.protocol_error);
    CHECK(std::get<tools::ImageContent>(again.payload.content[1]).artifact.path == image.artifact.path);
    CHECK(std::filesystem::file_size(std::filesystem::path(image.artifact.path)) == image.bytes);
}

TEST_CASE("解析: audio 块魔数复核过关;WAV 落盘留引用") {
    const char* kTinyWavB64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA=";
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "audio"}, {"data", kTinyWavB64}, {"mimeType", "audio/wav"}}})}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE_FALSE(parsed.protocol_error);
    const auto& audio = std::get<tools::AudioContent>(parsed.payload.content[0]);
    CHECK(audio.mime_type == "audio/wav");
    CHECK(audio.artifact.stored);
    CHECK(audio.artifact.filename.find(".wav") != std::string::npos);
    CHECK(tools::TextProjection(parsed.payload).find("[音频") != std::string::npos);
}

TEST_CASE("解析: resource_link 六字段接住;resource 的 text/blob 两变体都对") {
    const nlohmann::json result = {
        {"content",
         nlohmann::json::array(
             {{{"type", "resource_link"},
               {"uri", "file:///r/q3.md"},
               {"name", "q3.md"},
               {"title", "三季度报告"},
               {"description", "报告正文"},
               {"mimeType", "text/markdown"},
               {"size", 4096}},
              {{"type", "resource"},
               {"resource", {{"uri", "file:///n/a.txt"}, {"mimeType", "text/plain"}, {"text", "内嵌正文"}}}},
              {{"type", "resource"},
               {"resource",
                {{"uri", "file:///b/x.bin"}, {"mimeType", "application/zip"}, {"blob", "AAECAwQ="}}}}})}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE_FALSE(parsed.protocol_error);
    REQUIRE(parsed.payload.content.size() == 3);
    const auto& link = std::get<tools::ResourceLinkContent>(parsed.payload.content[0]);
    CHECK(link.uri == "file:///r/q3.md");
    CHECK(link.title == "三季度报告");
    CHECK(link.size == 4096);
    const auto& text_resource = std::get<tools::EmbeddedTextResourceContent>(parsed.payload.content[1]);
    CHECK(text_resource.text == "内嵌正文");
    CHECK_FALSE(text_resource.truncated);
    const auto& blob = std::get<tools::EmbeddedBlobResourceContent>(parsed.payload.content[2]);
    CHECK(blob.bytes == 5);  // "AAECAwQ=" 解码 5 字节
    CHECK(blob.artifact.stored);
    CHECK(blob.artifact.filename.find(".zip") != std::string::npos);
}

TEST_CASE("解析: 内嵌文本超帽先落 artifact,块里只留节选") {
    // 造一段超帽文本(默认帽 200k 字符)。
    const std::string big_text(300 * 1024, 'z');
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "resource"},
                                            {"resource", {{"uri", "file:///big.txt"},
                                                          {"mimeType", "text/plain"},
                                                          {"text", big_text}}}}})}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE_FALSE(parsed.protocol_error);
    const auto& text_resource = std::get<tools::EmbeddedTextResourceContent>(parsed.payload.content[0]);
    CHECK(text_resource.truncated);
    REQUIRE(text_resource.artifact.has_value());
    CHECK(text_resource.artifact->stored);
    CHECK(text_resource.text.size() < big_text.size());
    CHECK(parsed.landed_bytes == big_text.size());
    const std::string projection = tools::TextProjection(parsed.payload);
    CHECK(projection.find("正文过长已卸载") != std::string::npos);
}

TEST_CASE("解析: 伪 MIME(image/png 装 EXE 字节)必须拒绝") {
    const std::string fake = "this is definitely not a png file";
    // 手工 base64(标准字母表,padding 齐)。
    const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (std::size_t i = 0; i < fake.size(); i += 3) {
        const int b0 = static_cast<unsigned char>(fake[i]);
        const int b1 = i + 1 < fake.size() ? static_cast<unsigned char>(fake[i + 1]) : 0;
        const int b2 = i + 2 < fake.size() ? static_cast<unsigned char>(fake[i + 2]) : 0;
        b64 += table[b0 >> 2];
        b64 += table[((b0 & 3) << 4) | (b1 >> 4)];
        b64 += i + 1 < fake.size() ? table[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        b64 += i + 2 < fake.size() ? table[b2 & 63] : '=';
    }
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "image"}, {"data", b64}, {"mimeType", "image/png"}}})}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.mime_mismatch");
    // 盘上不许留下半截文件:该 sha 的 art 文件不该存在。
    CHECK(parsed.landed_bytes == 0);
}

TEST_CASE("解析: 坏 base64/缺字段/类型错整次按协议错收口") {
    const nlohmann::json bad_base64 = {
        {"content", nlohmann::json::array({{{"type", "image"}, {"data", "not!base!64!!"}, {"mimeType", "image/png"}}})}};
    auto parsed = ParseCallToolResult(bad_base64, MakeContext());
    CHECK(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.bad_base64");

    const nlohmann::json missing_text = {{"content", nlohmann::json::array({{{"type", "text"}}})}};
    parsed = ParseCallToolResult(missing_text, MakeContext());
    CHECK(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.malformed_content");

    const nlohmann::json wrong_is_error = {{"content", nlohmann::json::array()}, {"isError", "yes"}};
    parsed = ParseCallToolResult(wrong_is_error, MakeContext());
    CHECK(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.malformed_content");

    const nlohmann::json content_not_array = {{"content", 42}};
    parsed = ParseCallToolResult(content_not_array, MakeContext());
    CHECK(parsed.protocol_error);
}

TEST_CASE("解析: 字节帽——单块超帽与单次调用合计超帽都拦") {
    // 预算收窄成 8 字节:4 字节 blob 过,再来 8 字节就超。
    CallToolParseContext context = MakeContext();
    context.binary_budget = 8;
    const nlohmann::json ok = {
        {"content", nlohmann::json::array({{{"type", "resource"},
                                           {"resource", {{"uri", "file:///a"}, {"blob", "AAECAwQ="}}}}})}};
    auto parsed = ParseCallToolResult(ok, context);
    REQUIRE_FALSE(parsed.protocol_error);
    CHECK(parsed.landed_bytes == 5);  // 同上,5 字节

    const nlohmann::json over = {
        {"content", nlohmann::json::array({{{"type", "resource"},
                                           {"resource", {{"uri", "file:///b"}, {"blob", "AAECAwQFBgcICQ=="}}}}})}};
    parsed = ParseCallToolResult(over, context);
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.size_cap_exceeded");
}

TEST_CASE("解析: 没有落盘地时二进制块按稳定错收口,文本不受影响") {
    CallToolParseContext context;
    context.server_name = "test";
    context.artifact_dir = "";
    const nlohmann::json with_image = {
        {"content",
         nlohmann::json::array({{{"type", "image"}, {"data", kTinyPngB64}, {"mimeType", "image/png"}}})}};
    auto parsed = ParseCallToolResult(with_image, context);
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.artifact_unavailable");

    const nlohmann::json text_only = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", "纯文本不需要落盘地"}}})}};
    parsed = ParseCallToolResult(text_only, context);
    CHECK_FALSE(parsed.protocol_error);
    CHECK(std::get<tools::TextContent>(parsed.payload.content[0]).text == "纯文本不需要落盘地");
}

TEST_CASE("解析: structuredContent 对 outputSchema 校验,不合给稳定码") {
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"answer", {{"type", "integer"}}}}},
                                   {"required", nlohmann::json::array({"answer"})}};
    CallToolParseContext context = MakeContext();
    context.output_schema = schema;

    const nlohmann::json good = {{"structuredContent", nlohmann::json{{"answer", 42}}}};
    auto parsed = ParseCallToolResult(good, context);
    CHECK_FALSE(parsed.protocol_error);

    const nlohmann::json bad_type = {{"structuredContent", nlohmann::json{{"answer", "字符串"}}}};
    parsed = ParseCallToolResult(bad_type, context);
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.output_schema_mismatch");

    const nlohmann::json missing_required = {{"structuredContent", nlohmann::json{{"other", 1}}}};
    parsed = ParseCallToolResult(missing_required, context);
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.output_schema_mismatch");
}

TEST_CASE("解析: structuredContent 深度与字节帽") {
    // 深度 40 的嵌套数组。
    nlohmann::json deep = nlohmann::json::array();
    nlohmann::json nested = 1;
    for (int i = 0; i < 40; ++i) {
        nlohmann::json wrapper = nlohmann::json::array();
        wrapper.push_back(nested);
        nested = std::move(wrapper);
    }
    const nlohmann::json result = {{"structuredContent", nlohmann::json{{"deep", nested}}}};
    auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE(parsed.protocol_error);
    CHECK(parsed.error_code == "mcp.size_cap_exceeded");
}

TEST_CASE("解析: isError=true 也允许富内容(错误截图不丢)") {
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", "出错了,截图为证"}},
                                           {{"type", "image"}, {"data", kTinyPngB64}, {"mimeType", "image/png"}}})},
        {"isError", true}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    REQUIRE_FALSE(parsed.protocol_error);
    CHECK(parsed.server_is_error);
    CHECK(parsed.payload.content.size() == 2);  // 错误状态的富内容原样保留
}

TEST_CASE("解析: 未知 content type 留占位与码,不吞不崩") {
    const nlohmann::json result = {
        {"content", nlohmann::json::array({{{"type", "hologram"}, {"data", "xyz"}},
                                           {{"type", "text"}, {"text", "正常文本"}}})}};
    const auto parsed = ParseCallToolResult(result, MakeContext());
    CHECK_FALSE(parsed.protocol_error);
    REQUIRE(parsed.payload.content.size() == 2);
    const auto& unknown = std::get<tools::UnknownContent>(parsed.payload.content[0]);
    CHECK(unknown.original_type == "hologram");
    CHECK(unknown.summary.find("data") != std::string::npos);  // 安全摘要:字段名在,正文不在
    CHECK(parsed.details.value("mcp_notice_code", std::string()) == "mcp.unsupported_content_type");
    CHECK(tools::TextProjection(parsed.payload).find("不支持的内容类型: hologram") != std::string::npos);
}

TEST_CASE("LandToolArtifact: 内容寻址、原子落盘、空入参给空串") {
    const std::string dir = TempArtifactDir();
    CHECK(LandToolArtifact("", "bytes", "bin").empty());
    CHECK(LandToolArtifact(dir, "", "bin").empty());
    const std::string path = LandToolArtifact(dir, "artifact-body", "txt");
    REQUIRE_FALSE(path.empty());
    CHECK(path.rfind(dir + "/art-", 0) == 0);
    CHECK(path.find(".txt") != std::string::npos);
    CHECK(LandToolArtifact(dir, "artifact-body", "txt") == path);  // 同字节同名
    CHECK(LandToolArtifact(dir, "artifact-body-2", "txt") != path);
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
}
