// 工具结果的富内容类型(MCP 富结果单 P0.1):provider 中立的内容块——
// 文本、图片、音频、资源链接、内嵌资源(text/blob)、未知块。MCP 的
// tools/call 结果、将来别的富工具来源,都翻成这一层;四家 wire、终端
// 显示、会话存档各自从这里投影,不许谁私藏第二份真账。
//
// 与 api::ImageBlock(用户上传图)/api::ModelImageBlock(模型生成图)分家
// (单子定案"不复用 ModelImageBlock"):这里的图来自工具返回,来源、生命
// 周期、审计口径都不同。二进制字节先落会话 artifact 目录,块里只留
// ArtifactRef 引用——base64 不许长期挂在会话 history 对象上。
//
// 本头不许 include api/ 或 agent/ 的东西:api/types.hpp 反过来要吃它
// (ToolResultBlock 的 blocks 字段),方向只能是 tools -> 平台/第三方。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::tools {

// 二进制内容落进会话 artifact 目录后的引用账。路径按内容寻址
// ("mcp-artifacts/art-<sha8>.<ext>"),文件名本地起,不信 server 给的
// 名字一个字。stored=false 表示字节没落盘(没开 artifact 目录/超帽),
// 投影必须明说"未落盘",不得冒充可取。
struct ArtifactRef {
    std::string id;        // "art-<sha8>"
    std::string filename;  // "art-<sha8>.png"
    std::string path;      // 相对会话目录("mcp-artifacts/art-xxxx.png")
    std::string mime_type;
    std::size_t bytes = 0;
    std::string sha256;  // 解码后正文的 sha256(全 hex)
    bool stored = false;

    bool operator==(const ArtifactRef&) const = default;
};

// 一段纯文本。MCP type=text。
struct TextContent {
    std::string text;
    bool operator==(const TextContent&) const = default;
};

// 工具返回的图片(MCP type=image)。字节已验身(魔数)落 artifact,这里
// 只有元数据与引用;durable history 不存 base64。
struct ImageContent {
    std::string mime_type;  // 魔数复核后的 MIME(image/png 等)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t bytes = 0;
    std::string sha256;
    ArtifactRef artifact;

    // 恳求态(transient):工具结果图片回喂单立的字段——发请求前由
    // agent::RehydrateToolResultImages 从 artifact 落盘把字节读回来、
    // 编成 base64(不带 data: 前缀)填在这里,四家 wire 据此上原生图块。
    // 只活在请求副本上:durable history、会话存档(BlockToJson)、
    // resume 重放一概不落这个字段——历史里永远是引用,字节每轮现灌。
    // 空串 = 字节没随行(未重灌/超帽/文件丢了),wire 走文本投影降级。
    std::string wire_base64;

    bool operator==(const ImageContent&) const = default;
};

// 工具返回的音频(MCP type=audio)。首版只求无损接住:字节落 artifact、
// 留引用;模型侧投影是元数据短句。
struct AudioContent {
    std::string mime_type;
    std::size_t bytes = 0;
    std::string sha256;
    ArtifactRef artifact;

    bool operator==(const AudioContent&) const = default;
};

// 资源链接(MCP type=resource_link):一根引用,不是内容本身。URI 不自动
// 抓取——file://、http://、自定义 scheme 都只是一根指针,真要读另走
// 受控的授权读取。
struct ResourceLinkContent {
    std::string uri;
    std::string name;
    std::string title;
    std::string description;
    std::string mime_type;
    std::int64_t size = -1;  // server 声明的字节数;没报 = -1

    bool operator==(const ResourceLinkContent&) const = default;
};

// 内嵌文本资源(MCP type=resource,text 变体)。text 受字符帽约束:帽内
// 原样内联;超帽落 artifact,text 只留节选,truncated=true。
struct EmbeddedTextResourceContent {
    std::string uri;
    std::string mime_type;
    std::string text;
    bool truncated = false;                    // true = text 是节选,全文在 artifact
    std::optional<ArtifactRef> artifact;       // 超帽/应卸载时才落

    bool operator==(const EmbeddedTextResourceContent&) const = default;
};

// 内嵌二进制资源(MCP type=resource,blob 变体):解码验身后落 artifact,
// 只留引用。
struct EmbeddedBlobResourceContent {
    std::string uri;
    std::string mime_type;
    std::size_t bytes = 0;
    std::string sha256;
    ArtifactRef artifact;

    bool operator==(const EmbeddedBlobResourceContent&) const = default;
};

// 认不出的内容类型:留住 server 报的原 type 与安全摘要(只有字段名与
// 长度,绝无正文),投影成明确占位;不静默吞,也不冒充解析成功。
struct UnknownContent {
    std::string original_type;
    std::string summary;

    bool operator==(const UnknownContent&) const = default;
};

using ToolContentBlock =
    std::variant<TextContent, ImageContent, AudioContent, ResourceLinkContent, EmbeddedTextResourceContent,
                 EmbeddedBlobResourceContent, UnknownContent>;

// 富结果正文(MCP 富结果单"富结果只留一份真账"):块序即真序,
// text -> image -> text 不得归并。structured_content 用 optional 保存,
// 空对象 {} 与"字段缺失"分得清(nullopt = server 没给,{} = 给了空的)。
struct ToolResultPayload {
    std::vector<ToolContentBlock> content;
    std::optional<nlohmann::json> structured_content;

    bool operator==(const ToolResultPayload&) const = default;

    bool empty() const { return content.empty() && !structured_content.has_value(); }
    // 有没有可当"兼容文本"用的 TextContent 块(structuredContent 的投影
    // 规矩:有兼容 text 就不再重复投影 JSON)。
    bool has_text_blocks() const;
};

// 纯文本 payload 的便捷构造(旧内置工具的过桥)。
ToolResultPayload MakeTextPayload(std::string text);

// 投影策略。
struct ProjectionPolicy {
    // embedded text 的内联字符帽:帽内 text 原样进投影;超帽(块本身已经
    // truncated 时不会再发生,这里兜底)截到帽并注明。
    std::size_t inline_text_chars = 4096;
};

// 唯一的文本投影口(P0.2):text 原样拼接;image/audio/blob 投成带文件
// 名、尺寸、MIME、artifact 引用的短句;resource link 投成标题 + URI;
// embedded text 帽内内联;structuredContent 只在没有兼容 text 时按稳定
// JSON 投影一次;未知块投明确占位。不许静默吞块。
std::string TextProjection(const ToolResultPayload& payload, const ProjectionPolicy& policy = {});

// 单块稳定摘要(一行,审计/trace 用,不含正文)。
std::string BlockSummary(const ToolContentBlock& block);

// 块级序列化/反序列化(会话存档用;与 session_store 的消息块序列化同路)。
nlohmann::json BlockToJson(const ToolContentBlock& block);
std::optional<ToolContentBlock> BlockFromJson(const nlohmann::json& json);

// JSON 树的 UTF-8 规范化:递归把每个字符串过 SanitizeExternalText——
// server 塞进 structuredContent 的坏串若不洗,dump() 当场 type_error,
// 整场会话每回合必挂。合法时零成本。
void SanitizeJsonTextInPlace(nlohmann::json& value);

// payload 里全部文本字段(含 structured JSON)的 UTF-8 规范化。幂等。
void SanitizePayloadTextInPlace(ToolResultPayload& payload);

}  // namespace lubancode::tools
