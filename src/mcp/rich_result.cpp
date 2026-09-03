#include "mcp/rich_result.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "agent/model_image_store.hpp"  // DecodeBase64Strict/SniffImageFormat/ReadImageDimensions
#include "hooks/hash.hpp"               // Sha256Hex:内容寻址与审计
#include "platform/atomic_write.hpp"    // 统一原子写(审计 P1:替掉先删后换)
#include "platform/paths.hpp"           // Utf8ToPath:目录路径不走 ACP 窄口
#include "platform/text_encoding.hpp"   // Utf8PrefixBoundary:节选取头的字节边界
#include "tools/schema_check.hpp"       // structuredContent 对 outputSchema 的子集校验

namespace lubancode::mcp {

namespace {

bool StartsWith(const std::string& bytes, const char* prefix) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= bytes.size() || bytes[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

// 音频魔数:认得出才收,伪 MIME 在这道拦。
bool AudioMagicMatches(const std::string& bytes, const std::string& mime) {
    if (mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/wave") {
        return bytes.size() >= 12 && StartsWith(bytes, "RIFF") && bytes.substr(8, 4) == "WAVE";
    }
    if (mime == "audio/mpeg" || mime == "audio/mp3") {
        if (StartsWith(bytes, "ID3")) {
            return true;
        }
        return bytes.size() >= 2 && (static_cast<unsigned char>(bytes[0]) & 0xFF) == 0xFF &&
               (static_cast<unsigned char>(bytes[1]) & 0xE0) == 0xE0;
    }
    if (mime == "audio/ogg" || mime == "application/ogg") {
        return StartsWith(bytes, "OggS");
    }
    if (mime == "audio/flac" || mime == "audio/x-flac") {
        return StartsWith(bytes, "fLaC");
    }
    if (mime == "audio/webm") {
        return bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0x1A &&
               static_cast<unsigned char>(bytes[1]) == 0x45 && static_cast<unsigned char>(bytes[2]) == 0xDF &&
               static_cast<unsigned char>(bytes[3]) == 0xA3;
    }
    return false;  // 不在音频 allowlist 里
}

// 图片 allowlist:四类,魔数复核交给 agent::SniffImageFormat。
bool IsAllowedImageMime(const std::string& mime) {
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp";
}

std::string ExtensionForMime(const std::string& mime) {
    static const std::pair<const char*, const char*> kTable[] = {
        {"image/png", "png"},  {"image/jpeg", "jpg"},        {"image/gif", "gif"},
        {"image/webp", "webp"}, {"audio/wav", "wav"},        {"audio/x-wav", "wav"},
        {"audio/wave", "wav"}, {"audio/mpeg", "mp3"},       {"audio/mp3", "mp3"},
        {"audio/ogg", "ogg"},  {"audio/flac", "flac"},      {"audio/x-flac", "flac"},
        {"audio/webm", "webm"}, {"text/plain", "txt"},      {"text/html", "html"},
        {"text/markdown", "md"}, {"application/json", "json"}, {"application/zip", "zip"},
        {"application/pdf", "pdf"}, {"application/octet-stream", "bin"},
    };
    for (const auto& [mime_name, ext] : kTable) {
        if (mime == mime_name) {
            return ext;
        }
    }
    const auto slash = mime.find('/');
    if (slash != std::string::npos) {
        std::string subtype = mime.substr(slash + 1);
        // subtype 只许字母数字与减号,别的字符一律落 bin——文件名不收
        // server 的怪字符(路径注入面)。
        const bool clean = !subtype.empty() && std::all_of(subtype.begin(), subtype.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        });
        if (clean && subtype.size() <= 8) {
            return subtype;
        }
    }
    return "bin";
}

// 原子写,统一走 platform::AtomicWriteFile(旧写法与 model_image_store
// 同款"先删正式件再 rename";平台件原子替换,失败不动正式件)。
bool AtomicWriteBytes(const std::filesystem::path& path, const std::string& content) {
    return platform::AtomicWriteFile(path, content).has_value();
}

// JSON 深度(structuredContent 的帽)。
int JsonDepth(const nlohmann::json& value, int depth = 1) {
    if (!value.is_array() && !value.is_object()) {
        return depth;
    }
    int max_depth = depth;
    for (const auto& item : value) {
        max_depth = std::max(max_depth, JsonDepth(item, depth + 1));
    }
    return max_depth;
}

// 字段取值的小兜底:存在但类型不对时给 nullopt(调用方按坏块收口),
// 不让 .value() 的 type_error 穿透。
std::optional<std::string> GetStringField(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::string BlockKeysSummary(const nlohmann::json& block) {
    std::string keys;
    if (block.is_object()) {
        for (auto it = block.begin(); it != block.end(); ++it) {
            if (!keys.empty()) {
                keys += ",";
            }
            keys += it.key();
        }
    }
    return "keys=" + keys + ";~" + std::to_string(block.dump().size()) + "B";
}

// 协议错收口的便捷构造。
CallToolParseResult ProtocolError(std::string code, std::string message) {
    CallToolParseResult out;
    out.protocol_error = true;
    out.error_code = std::move(code);
    out.error_message = std::move(message);
    return out;
}

// 前 N 个码点的字节边界(节选取头用):先数码点定字节位,再对齐边界
// 防止切进多字节序列的腰。
std::size_t CharPrefixBytes(const std::string& text, std::size_t chars) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (seen >= chars) {
            return platform::Utf8PrefixBoundary(text, i);
        }
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            ++seen;  // 非后续字节 = 新码点
        }
    }
    return text.size();
}

}  // namespace

std::string LandToolArtifact(const std::string& artifact_dir, const std::string& bytes,
                             const std::string& extension) {
    if (artifact_dir.empty() || bytes.empty()) {
        return std::string();
    }
    const std::string sha = hooks::Sha256Hex(bytes);
    // P0-2:文件名即内容地址(全 hash;目录是 session artifacts/sha256/,
    // hash 即地址,同字节天然去重)。id 保留 art-<sha8> 短形(会话内引用)。
    const std::string filename = sha + "." + extension;
    const std::filesystem::path dir = platform::Utf8ToPath(artifact_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return std::string();
    }
    const std::filesystem::path file = dir / platform::Utf8ToPath(filename);
    // 内容寻址:同字节已落过不再写(幂等,重复块的天然去重)。
    if (std::filesystem::exists(file) || AtomicWriteBytes(file, bytes)) {
        return artifact_dir + "/" + filename;
    }
    return std::string();
}

CallToolParseResult ParseCallToolResult(const nlohmann::json& result, const CallToolParseContext& context) {
    if (!result.is_object()) {
        return ProtocolError("mcp.malformed_content", "MCP 服务器 " + context.server_name +
                                                           " 的 tools/call result 不是对象");
    }

    CallToolParseResult out;
    // isError 该是 bool;给了别的类型按坏块收口。
    if (result.contains("isError")) {
        if (!result["isError"].is_boolean()) {
            return ProtocolError("mcp.malformed_content",
                                 "MCP 服务器 " + context.server_name + " 的 tools/call isError 不是布尔值");
        }
        out.server_is_error = result["isError"].get<bool>();
    }

    std::size_t call_binary_total = 0;  // 单次调用的二进制合计
    const bool has_content = result.contains("content");
    if (has_content && !result["content"].is_array()) {
        return ProtocolError("mcp.malformed_content",
                             "MCP 服务器 " + context.server_name + " 的 tools/call content 不是数组");
    }
    if (has_content) {
        for (const auto& block : result["content"]) {
            if (!block.is_object()) {
                return ProtocolError("mcp.malformed_content",
                                     "MCP 服务器 " + context.server_name + " 的 content 块不是对象");
            }
            std::optional<std::string> type = GetStringField(block, "type");
            if (!type.has_value()) {
                return ProtocolError("mcp.malformed_content",
                                     "MCP 服务器 " + context.server_name + " 的 content 块缺 type 字段");
            }
            if (*type == "text") {
                auto text = GetStringField(block, "text");
                if (!text.has_value()) {
                    return ProtocolError("mcp.malformed_content",
                                         "MCP 服务器 " + context.server_name + " 的 text 块缺 text 字段");
                }
                out.payload.content.push_back(tools::TextContent{std::move(*text)});
                continue;
            }
            if (*type == "image" || *type == "audio") {
                const bool is_image = *type == "image";
                auto data = GetStringField(block, "data");
                auto mime = GetStringField(block, "mimeType");
                if (!data.has_value() || !mime.has_value()) {
                    return ProtocolError("mcp.malformed_content",
                                         "MCP 服务器 " + context.server_name + " 的 " + *type +
                                             " 块缺 data/mimeType 字段");
                }
                if (is_image && !IsAllowedImageMime(*mime)) {
                    return ProtocolError("mcp.unsupported_mime_type",
                                         "MCP 服务器 " + context.server_name + " 的图片 MIME 不在允许表内: " +
                                             *mime);
                }
                const std::size_t cap = is_image ? kMaxImageBlockBytes : kMaxAudioBlockBytes;
                const auto decoded = agent::DecodeBase64Strict(*data, cap);
                if (!decoded.has_value()) {
                    return ProtocolError("mcp.bad_base64",
                                         "MCP 服务器 " + context.server_name + " 的 " + *type +
                                             " 块 base64 解码失败: " + decoded.error());
                }
                if (call_binary_total + decoded->size() > context.binary_budget) {
                    return ProtocolError("mcp.size_cap_exceeded",
                                         "MCP 服务器 " + context.server_name + " 的 " + *type +
                                             " 块超出本次调用的二进制字节帽");
                }
                // 魔数复核:声明与实字节对不上 = 伪 MIME,拒绝。
                if (is_image) {
                    const agent::ImageFormat format = agent::SniffImageFormat(*decoded);
                    if (format.mime_type != *mime) {
                        return ProtocolError("mcp.mime_mismatch",
                                             "MCP 服务器 " + context.server_name + " 的图片声明 " + *mime +
                                                 ",字节魔数认成 " +
                                                 (format.mime_type.empty() ? std::string("认不出") : format.mime_type));
                    }
                } else if (!AudioMagicMatches(*decoded, *mime)) {
                    return ProtocolError("mcp.mime_mismatch",
                                         "MCP 服务器 " + context.server_name + " 的音频声明 " + *mime +
                                             ",字节魔数对不上");
                }
                if (context.artifact_dir.empty()) {
                    return ProtocolError("mcp.artifact_unavailable",
                                         "MCP 服务器 " + context.server_name + " 返回了 " + *type +
                                             " 内容,但本次会话没有 artifact 落盘地(未开会话/单发路)");
                }
                const std::string relative =
                    LandToolArtifact(context.artifact_dir, *decoded, ExtensionForMime(*mime));
                if (relative.empty()) {
                    return ProtocolError("mcp.artifact_write_failed",
                                         "MCP 服务器 " + context.server_name + " 的 " + *type +
                                             " 内容落盘失败: " + context.artifact_dir);
                }
                const std::string sha = hooks::Sha256Hex(*decoded);
                out.landed_bytes += decoded->size();
                tools::ArtifactRef artifact;
                artifact.id = "art-" + sha.substr(0, 8);
                artifact.filename = sha + "." + ExtensionForMime(*mime);
                artifact.path = relative;
                artifact.mime_type = *mime;
                artifact.bytes = decoded->size();
                artifact.sha256 = sha;
                artifact.stored = true;
                call_binary_total += decoded->size();
                if (is_image) {
                    tools::ImageContent image;
                    const agent::ImageDimensions dims = agent::ReadImageDimensions(*decoded, *mime);
                    image.mime_type = std::move(*mime);
                    image.width = dims.width;
                    image.height = dims.height;
                    image.bytes = artifact.bytes;
                    image.sha256 = sha;
                    image.artifact = artifact;
                    out.payload.content.push_back(std::move(image));
                } else {
                    tools::AudioContent audio;
                    audio.mime_type = std::move(*mime);
                    audio.bytes = artifact.bytes;
                    audio.sha256 = sha;
                    audio.artifact = artifact;
                    out.payload.content.push_back(std::move(audio));
                }
                continue;
            }
            if (*type == "resource_link") {
                auto uri = GetStringField(block, "uri");
                auto name = GetStringField(block, "name");
                if (!uri.has_value() || !name.has_value()) {
                    return ProtocolError("mcp.malformed_content",
                                         "MCP 服务器 " + context.server_name + " 的 resource_link 块缺 uri/name");
                }
                tools::ResourceLinkContent link;
                link.uri = std::move(*uri);
                link.name = std::move(*name);
                if (auto title = GetStringField(block, "title")) {
                    link.title = std::move(*title);
                }
                if (auto description = GetStringField(block, "description")) {
                    link.description = std::move(*description);
                }
                if (auto mime = GetStringField(block, "mimeType")) {
                    link.mime_type = std::move(*mime);
                }
                if (block.contains("size") && block["size"].is_number_integer()) {
                    link.size = block["size"].get<std::int64_t>();
                }
                out.payload.content.push_back(std::move(link));
                continue;
            }
            if (*type == "resource") {
                if (!block.contains("resource") || !block["resource"].is_object()) {
                    return ProtocolError("mcp.malformed_content",
                                         "MCP 服务器 " + context.server_name + " 的 resource 块缺 resource 对象");
                }
                const nlohmann::json& resource = block["resource"];
                auto uri = GetStringField(resource, "uri");
                if (!uri.has_value()) {
                    return ProtocolError("mcp.malformed_content",
                                         "MCP 服务器 " + context.server_name + " 的 resource 缺 uri");
                }
                const std::string mime = GetStringField(resource, "mimeType").value_or(std::string());
                if (resource.contains("text")) {
                    if (!resource["text"].is_string()) {
                        return ProtocolError("mcp.malformed_content",
                                             "MCP 服务器 " + context.server_name + " 的资源 text 不是字符串");
                    }
                    tools::EmbeddedTextResourceContent embedded;
                    embedded.uri = std::move(*uri);
                    embedded.mime_type = mime;
                    embedded.text = resource["text"].get<std::string>();
                    // 内联字符帽:超帽先落 artifact,块里只留节选。
                    std::size_t chars = 0;
                    for (const char c : embedded.text) {
                        (void)c;
                        ++chars;
                    }
                    if (chars > kMaxEmbeddedTextInlineChars) {
                        if (context.artifact_dir.empty()) {
                            return ProtocolError("mcp.artifact_unavailable",
                                                 "MCP 服务器 " + context.server_name +
                                                     " 的内嵌文本资源超帽,但本次没有 artifact 落盘地");
                        }
                        const std::string relative =
                            LandToolArtifact(context.artifact_dir, embedded.text, ExtensionForMime(mime));
                        if (relative.empty()) {
                            return ProtocolError("mcp.artifact_write_failed",
                                                 "MCP 服务器 " + context.server_name + " 的内嵌文本资源落盘失败");
                        }
                        tools::ArtifactRef artifact;
                        out.landed_bytes += embedded.text.size();
                        const std::string sha = hooks::Sha256Hex(embedded.text);
                        artifact.id = "art-" + sha.substr(0, 8);
                        artifact.filename = "art-" + sha.substr(0, 8) + "." + ExtensionForMime(mime);
                        artifact.path = relative;
                        artifact.mime_type = mime;
                        artifact.bytes = embedded.text.size();
                        artifact.sha256 = sha;
                        artifact.stored = true;
                        embedded.text =
                            embedded.text.substr(0, CharPrefixBytes(embedded.text, kMaxEmbeddedTextInlineChars / 2));
                        embedded.truncated = true;
                        embedded.artifact = std::move(artifact);
                    }
                    out.payload.content.push_back(std::move(embedded));
                    continue;
                }
                if (resource.contains("blob")) {
                    if (!resource["blob"].is_string()) {
                        return ProtocolError("mcp.malformed_content",
                                             "MCP 服务器 " + context.server_name + " 的资源 blob 不是字符串");
                    }
                    const auto decoded =
                        agent::DecodeBase64Strict(resource["blob"].get<std::string>(), kMaxBlobBlockBytes);
                    if (!decoded.has_value()) {
                        return ProtocolError("mcp.bad_base64",
                                             "MCP 服务器 " + context.server_name + " 的 blob 解码失败: " +
                                                 decoded.error());
                    }
                    if (call_binary_total + decoded->size() > context.binary_budget) {
                        return ProtocolError("mcp.size_cap_exceeded",
                                             "MCP 服务器 " + context.server_name +
                                                 " 的 blob 超出本次调用的二进制字节帽");
                    }
                    if (context.artifact_dir.empty()) {
                        return ProtocolError("mcp.artifact_unavailable",
                                             "MCP 服务器 " + context.server_name +
                                                 " 返回了 blob 资源,但本次会话没有 artifact 落盘地");
                    }
                    const std::string relative =
                        LandToolArtifact(context.artifact_dir, *decoded, ExtensionForMime(mime));
                    if (relative.empty()) {
                        return ProtocolError("mcp.artifact_write_failed",
                                             "MCP 服务器 " + context.server_name + " 的 blob 落盘失败");
                    }
                    tools::EmbeddedBlobResourceContent blob_block;
                    out.landed_bytes += decoded->size();
                    const std::string sha = hooks::Sha256Hex(*decoded);
                    blob_block.uri = std::move(*uri);
                    blob_block.mime_type = mime;
                    blob_block.bytes = decoded->size();
                    blob_block.sha256 = sha;
                    blob_block.artifact.id = "art-" + sha.substr(0, 8);
                    blob_block.artifact.filename = "art-" + sha.substr(0, 8) + "." + ExtensionForMime(mime);
                    blob_block.artifact.path = relative;
                    blob_block.artifact.mime_type = mime;
                    blob_block.artifact.bytes = decoded->size();
                    blob_block.artifact.sha256 = sha;
                    blob_block.artifact.stored = true;
                    call_binary_total += decoded->size();
                    out.payload.content.push_back(std::move(blob_block));
                    continue;
                }
                return ProtocolError("mcp.malformed_content",
                                     "MCP 服务器 " + context.server_name + " 的 resource 缺 text/blob");
            }
            // 未知 content type:占位块 + 留码,不吞不崩,整次结果仍可用。
            tools::UnknownContent unknown;
            unknown.original_type = *type;
            unknown.summary = BlockKeysSummary(block);
            out.payload.content.push_back(std::move(unknown));
            nlohmann::json unsupported = out.details.value("unsupported_content_types", nlohmann::json::array());
            unsupported.push_back(*type);
            out.details["unsupported_content_types"] = unsupported;
            out.details["mcp_notice_code"] = "mcp.unsupported_content_type";
            continue;
        }
    }

    // structuredContent:必须是对象,过深度/字节帽,洗 UTF-8。
    if (result.contains("structuredContent")) {
        const nlohmann::json& structured = result["structuredContent"];
        if (!structured.is_object()) {
            return ProtocolError("mcp.malformed_content",
                                 "MCP 服务器 " + context.server_name + " 的 structuredContent 不是对象");
        }
        if (JsonDepth(structured) > kMaxStructuredDepth) {
            return ProtocolError("mcp.size_cap_exceeded",
                                 "MCP 服务器 " + context.server_name + " 的 structuredContent 深度超帽");
        }
        if (structured.dump().size() > kMaxStructuredBytes) {
            return ProtocolError("mcp.size_cap_exceeded",
                                 "MCP 服务器 " + context.server_name + " 的 structuredContent 字节数超帽");
        }
        out.payload.structured_content = structured;
        tools::SanitizeJsonTextInPlace(*out.payload.structured_content);
        // outputSchema 声明过就必须校验(子集:顶层 type/required/properties/
        // enum,与 PreToolUse 改写校验同一颗;深层形状交给 server 自律)。
        if (context.output_schema.has_value()) {
            const auto schema_error =
                tools::ValidateInputAgainstSchema(*out.payload.structured_content, *context.output_schema);
            if (schema_error.has_value()) {
                return ProtocolError("mcp.output_schema_mismatch",
                                     "MCP 服务器 " + context.server_name +
                                         " 的 structuredContent 不合自己声明的 outputSchema: " + *schema_error);
            }
        }
    }

    tools::SanitizePayloadTextInPlace(out.payload);
    return out;
}

}  // namespace lubancode::mcp
