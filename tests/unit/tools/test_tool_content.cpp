// MCP 富结果单 P0.1/P0.2:中立内容块(tools::ToolContentBlock)与
// ToolResultPayload 的单测——投影规矩(块序、图片短句、structured 只在无
// 兼容文本时投影一次)、序列化往返、相等比较、JSON 文本规范化。
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "platform/text_encoding.hpp"
#include "tools/tool_content.hpp"

using namespace lubancode::tools;

namespace {

ArtifactRef MakeArtifact(std::string mime = "image/png") {
    ArtifactRef artifact;
    artifact.id = "art-00112233";
    artifact.filename = "art-00112233.png";
    artifact.path = "mcp-artifacts/art-00112233.png";
    artifact.mime_type = std::move(mime);
    artifact.bytes = 123;
    artifact.sha256 = "00112233445566778899aabbccddeeff";
    artifact.stored = true;
    return artifact;
}

}  // namespace

TEST_CASE("payload: 纯文本投影原样拼接,块序保持,不加修饰") {
    ToolResultPayload payload;
    payload.content.push_back(TextContent{"第一段 "});
    payload.content.push_back(TextContent{"第二段"});
    CHECK(TextProjection(payload) == "第一段 第二段");
    CHECK(payload.has_text_blocks());
}

TEST_CASE("payload: text->image->text 不归并,图片短句带文件名/尺寸/MIME/artifact") {
    ToolResultPayload payload;
    payload.content.push_back(TextContent{"看这张图"});
    ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 2048;
    image.sha256 = "00112233";
    image.artifact = MakeArtifact();
    payload.content.push_back(std::move(image));
    payload.content.push_back(TextContent{"看完了"});

    const std::string projection = TextProjection(payload);
    const auto head = projection.find("看这张图");
    const auto image_line = projection.find("[图片 art-00112233.png image/png 640x480 2048字节 "
                                            "artifact=mcp-artifacts/art-00112233.png]");
    const auto tail = projection.find("看完了");
    REQUIRE(head != std::string::npos);
    REQUIRE(image_line != std::string::npos);
    REQUIRE(tail != std::string::npos);
    CHECK(head < image_line);
    CHECK(image_line < tail);
}

TEST_CASE("payload: 未落盘的 artifact 投影明说'未落盘',不冒充可取") {
    ToolResultPayload payload;
    ImageContent image;
    image.mime_type = "image/png";
    image.bytes = 10;
    image.artifact = MakeArtifact();
    image.artifact.stored = false;
    payload.content.push_back(std::move(image));
    const std::string projection = TextProjection(payload);
    CHECK(projection.find("未落盘") != std::string::npos);
    CHECK(projection.find("artifact=") == std::string::npos);
}

TEST_CASE("payload: resource_link 投成标题+URI;没标题用 name") {
    ToolResultPayload payload;
    ResourceLinkContent link;
    link.uri = "file:///tmp/report.pdf";
    link.name = "report.pdf";
    link.title = "季度报告";
    payload.content.push_back(std::move(link));
    CHECK(TextProjection(payload).find("[资源链接 季度报告: file:///tmp/report.pdf]") != std::string::npos);

    ToolResultPayload unnamed;
    ResourceLinkContent bare;
    bare.uri = "https://example.com/x";
    bare.name = "x.html";
    unnamed.content.push_back(std::move(bare));
    CHECK(TextProjection(unnamed).find("[资源链接 x.html: https://example.com/x]") != std::string::npos);
}

TEST_CASE("payload: structuredContent 只在没有兼容 text 时投影一次") {
    ToolResultPayload text_and_json;
    text_and_json.content.push_back(TextContent{"已有文本"});
    text_and_json.structured_content = nlohmann::json{{"z", 1}, {"a", 2}};
    const std::string mixed = TextProjection(text_and_json);
    CHECK(mixed.find("已有文本") != std::string::npos);
    CHECK(mixed.find("\"a\":2") == std::string::npos);  // 有文本就不重复投影 JSON

    ToolResultPayload json_only;
    json_only.structured_content = nlohmann::json{{"b", 2}, {"a", 1}};
    CHECK(TextProjection(json_only).find("\"a\":1") != std::string::npos);  // 稳定序列化(key 有序)
}

TEST_CASE("payload: 空对象 {} 与字段缺失分得清") {
    ToolResultPayload empty_object;
    empty_object.structured_content = nlohmann::json::object();
    CHECK(empty_object.structured_content.has_value());
    CHECK(empty_object.structured_content->empty());
    CHECK(!empty_object.empty());

    ToolResultPayload missing;
    CHECK(!missing.structured_content.has_value());
    CHECK(missing.empty());
}

TEST_CASE("payload: 未知块投影明确占位,不静默吞") {
    ToolResultPayload payload;
    payload.content.push_back(UnknownContent{"video", "字段: data(4096B)"});
    CHECK(TextProjection(payload).find("[不支持的内容类型: video]") != std::string::npos);
}

TEST_CASE("payload: 内嵌文本超帽时投影节选并注明 artifact;帽内原样") {
    ToolResultPayload small;
    EmbeddedTextResourceContent resource;
    resource.uri = "file:///notes.txt";
    resource.text = "短正文";
    small.content.push_back(resource);
    CHECK(TextProjection(small).find("短正文") != std::string::npos);

    ToolResultPayload big;
    EmbeddedTextResourceContent long_resource;
    long_resource.uri = "file:///big.txt";
    long_resource.text = std::string(200 * 1024, 'x');
    long_resource.truncated = true;
    long_resource.artifact = MakeArtifact("text/plain");
    big.content.push_back(std::move(long_resource));
    ProjectionPolicy policy;
    policy.inline_text_chars = 100;
    const std::string projection = TextProjection(big, policy);
    CHECK(projection.find("正文过长已卸载") != std::string::npos);
    CHECK(projection.find("artifact=mcp-artifacts/art-00112233.png") != std::string::npos);
    CHECK(projection.size() < 400);  // 节选生效,不是整篇灌进来
}

TEST_CASE("块序列化往返:全类型逐块对称") {
    ToolResultPayload payload;
    payload.content.push_back(TextContent{"文本"});
    ImageContent image;
    image.mime_type = "image/png";
    image.width = 10;
    image.height = 20;
    image.bytes = 30;
    image.sha256 = "sha";
    image.artifact = MakeArtifact();
    payload.content.push_back(image);
    AudioContent audio;
    audio.mime_type = "audio/wav";
    audio.bytes = 99;
    audio.sha256 = "sha9";
    audio.artifact = MakeArtifact("audio/wav");
    payload.content.push_back(audio);
    ResourceLinkContent link;
    link.uri = "https://e.com/a";
    link.title = "T";
    payload.content.push_back(link);
    EmbeddedTextResourceContent text_resource;
    text_resource.uri = "file:///a.txt";
    text_resource.text = "R";
    text_resource.truncated = true;
    text_resource.artifact = MakeArtifact("text/plain");
    payload.content.push_back(text_resource);
    EmbeddedBlobResourceContent blob;
    blob.uri = "file:///b.bin";
    blob.mime_type = "application/zip";
    blob.bytes = 8;
    blob.sha256 = "shab";
    blob.artifact = MakeArtifact("application/zip");
    payload.content.push_back(blob);
    payload.content.push_back(UnknownContent{"weird", "s"});
    payload.structured_content = nlohmann::json{{"k", "v"}};

    // 往返:块数、块序、逐块相等。
    std::vector<ToolContentBlock> round_trip;
    for (const auto& block : payload.content) {
        const auto restored = BlockFromJson(BlockToJson(block));
        REQUIRE(restored.has_value());
        round_trip.push_back(*restored);
    }
    REQUIRE(round_trip.size() == payload.content.size());
    for (std::size_t i = 0; i < round_trip.size(); ++i) {
        CHECK(round_trip[i] == payload.content[i]);
    }
}

TEST_CASE("块序列化:坏块/认不出的类型给 nullopt,不抛不崩") {
    CHECK_FALSE(BlockFromJson(nlohmann::json::object()).has_value());
    CHECK_FALSE(BlockFromJson(nlohmann::json{{"type", "不存在"}}).has_value());
    // image 没了 artifact 落点 = 坏块。
    CHECK_FALSE(BlockFromJson(nlohmann::json{{"type", "image"}, {"mime_type", "image/png"}}).has_value());
}

TEST_CASE("SanitizeJsonTextInPlace: 坏 UTF-8 字符串被替换,dump 不再抛") {
    nlohmann::json value = nlohmann::json::array();
    value.push_back("好" + std::string("\xC4\xE3", 2) + "尾");
    value.push_back(nlohmann::json{{"nested", std::string("\xFF\xFE", 2)}});
    SanitizeJsonTextInPlace(value);
    const std::string dumped = value.dump();  // 不抛 type_error 即过
    CHECK(dumped.find("好") != std::string::npos);
    CHECK(dumped.find('\xFF') == std::string::npos);
}

TEST_CASE("SanitizePayloadTextInPlace: 文本块/资源文本/structured 全洗,幂等") {
    ToolResultPayload payload;
    payload.content.push_back(TextContent{"前" + std::string("\xC4", 1) + "后"});
    ResourceLinkContent link;
    link.uri = std::string("\xE3\x80", 2) + "bad";
    payload.content.push_back(link);
    payload.structured_content = nlohmann::json{{"k", std::string("\xC0\x80", 2)}};
    SanitizePayloadTextInPlace(payload);
    SanitizePayloadTextInPlace(payload);  // 幂等
    const std::string projection = TextProjection(payload);
    CHECK(projection.find('\xC4') == std::string::npos);
    CHECK(payload.structured_content.has_value());
    CHECK(payload.structured_content->dump().find('\xC0') == std::string::npos);
}
