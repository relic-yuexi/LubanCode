#include "tools/tool_content.hpp"

#include <algorithm>

#include "platform/text_encoding.hpp"

namespace lubancode::tools {

namespace {

// 图片/音频/资源块的投影短句。stored=false 时明说未落盘——模型与人都
// 看得出这份引用取不回正文,不冒充可取。
std::string ArtifactNote(const ArtifactRef& artifact) {
    if (!artifact.stored) {
        return "未落盘";
    }
    return "artifact=" + artifact.path;
}

std::string SizeNote(std::size_t bytes) {
    return std::to_string(bytes) + "字节";
}

}  // namespace

bool ToolResultPayload::has_text_blocks() const {
    for (const auto& block : content) {
        if (std::holds_alternative<TextContent>(block)) {
            return true;
        }
    }
    return false;
}

ToolResultPayload MakeTextPayload(std::string text) {
    ToolResultPayload payload;
    payload.content.push_back(TextContent{std::move(text)});
    return payload;
}

std::string TextProjection(const ToolResultPayload& payload, const ProjectionPolicy& policy) {
    std::string out;
    for (const auto& block : payload.content) {
        std::visit(
            [&](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextContent>) {
                    out += b.text;
                } else if constexpr (std::is_same_v<T, ImageContent>) {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    out += "[图片 " + b.artifact.filename + " " + b.artifact.mime_type;
                    if (b.width > 0 || b.height > 0) {
                        out += " " + std::to_string(b.width) + "x" + std::to_string(b.height);
                    }
                    out += " " + SizeNote(b.bytes) + " " + ArtifactNote(b.artifact) + "]\n";
                } else if constexpr (std::is_same_v<T, AudioContent>) {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    out += "[音频 " + b.artifact.filename + " " + b.artifact.mime_type + " " + SizeNote(b.bytes) +
                           " " + ArtifactNote(b.artifact) + "]\n";
                } else if constexpr (std::is_same_v<T, ResourceLinkContent>) {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    std::string label = !b.title.empty() ? b.title : b.name;
                    out += "[资源链接 " + label + ": " + b.uri + "]\n";
                } else if constexpr (std::is_same_v<T, EmbeddedTextResourceContent>) {
                    std::size_t chars = 0;
                    for (const char c : b.text) {
                        (void)c;
                        ++chars;
                    }
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    if (chars <= policy.inline_text_chars) {
                        out += b.text;
                    } else {
                        out += platform::Utf8PrefixBoundary(b.text, policy.inline_text_chars) <
                                       std::string::npos
                                   ? b.text.substr(0, platform::Utf8PrefixBoundary(b.text,
                                                                                 policy.inline_text_chars))
                                   : b.text.substr(0, policy.inline_text_chars);
                        out += "\n[资源 " + b.uri + " 正文超投影帽,以上为节选";
                        if (b.artifact.has_value() && b.artifact->stored) {
                            out += ",全文 artifact=" + b.artifact->path;
                        }
                        out += "]";
                    }
                    if (b.truncated) {
                        out += "\n[资源 " + b.uri + " 正文过长已卸载,以上为节选";
                        if (b.artifact.has_value() && b.artifact->stored) {
                            out += ",全文 artifact=" + b.artifact->path;
                        }
                        out += "]";
                    }
                    out += "\n";
                } else if constexpr (std::is_same_v<T, EmbeddedBlobResourceContent>) {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    out += "[内嵌资源 " + b.uri + " " + b.mime_type + " " + SizeNote(b.bytes) + " " +
                           ArtifactNote(b.artifact) + "]\n";
                } else {
                    if (!out.empty() && out.back() != '\n') {
                        out += '\n';
                    }
                    const std::string type =
                        b.original_type.empty() ? std::string("未知") : b.original_type;
                    out += "[不支持的内容类型: " + type + "]\n";
                }
            },
            block);
    }
    // structuredContent:只在没有兼容 text 时投影一次;稳定序列化由
    // nlohmann 默认对象(key 有序)保证。
    if (payload.structured_content.has_value() && !payload.has_text_blocks()) {
        if (!out.empty() && out.back() != '\n') {
            out += '\n';
        }
        out += payload.structured_content->dump() + "\n";
    }
    if (!out.empty() && out.back() == '\n') {
        // 纯文本 payload 不许被投影加上莫名的尾换行;非文本块收尾的换行
        // 留着(块与块之间的分隔),只有最末一个换行收掉。
        const bool only_text_blocks = std::all_of(payload.content.begin(), payload.content.end(),
                                                  [](const ToolContentBlock& b) {
                                                      return std::holds_alternative<TextContent>(b);
                                                  });
        const bool structured_tail =
            payload.structured_content.has_value() && payload.content.empty();
        if (only_text_blocks && !structured_tail) {
            out.pop_back();
        }
    }
    return out;
}

std::string BlockSummary(const ToolContentBlock& block) {
    return std::visit(
        [](const auto& b) -> std::string {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextContent>) {
                return "text(" + std::to_string(b.text.size()) + "B)";
            } else if constexpr (std::is_same_v<T, ImageContent>) {
                return "image(" + b.mime_type + "," + std::to_string(b.width) + "x" +
                       std::to_string(b.height) + "," + std::to_string(b.bytes) + "B,sha=" +
                       b.sha256.substr(0, std::min<std::size_t>(b.sha256.size(), 8)) + ")";
            } else if constexpr (std::is_same_v<T, AudioContent>) {
                return "audio(" + b.mime_type + "," + std::to_string(b.bytes) + "B)";
            } else if constexpr (std::is_same_v<T, ResourceLinkContent>) {
                return "resource_link(" + b.uri + ")";
            } else if constexpr (std::is_same_v<T, EmbeddedTextResourceContent>) {
                return "resource.text(" + b.uri + "," + std::to_string(b.text.size()) + "B" +
                       (b.truncated ? ",节选" : "") + ")";
            } else if constexpr (std::is_same_v<T, EmbeddedBlobResourceContent>) {
                return "resource.blob(" + b.uri + "," + b.mime_type + "," + std::to_string(b.bytes) + "B)";
            } else {
                return "unknown(" + b.original_type + ")";
            }
        },
        block);
}

namespace {

nlohmann::json ArtifactToJson(const ArtifactRef& artifact) {
    return nlohmann::json{{"id", artifact.id},
                          {"filename", artifact.filename},
                          {"path", artifact.path},
                          {"mime_type", artifact.mime_type},
                          {"bytes", artifact.bytes},
                          {"sha256", artifact.sha256},
                          {"stored", artifact.stored}};
}

std::optional<ArtifactRef> ArtifactFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    ArtifactRef artifact;
    artifact.id = json.value("id", std::string());
    artifact.filename = json.value("filename", std::string());
    artifact.path = json.value("path", std::string());
    artifact.mime_type = json.value("mime_type", std::string());
    if (json.contains("bytes") && json["bytes"].is_number_unsigned()) {
        artifact.bytes = json["bytes"].get<std::size_t>();
    }
    artifact.sha256 = json.value("sha256", std::string());
    artifact.stored = json.value("stored", false);
    // 引用没了落点就是坏引用:filename/path 双空且没 stored 标记的按坏块弃。
    if (artifact.filename.empty() || artifact.path.empty()) {
        return std::nullopt;
    }
    return artifact;
}

}  // namespace

nlohmann::json BlockToJson(const ToolContentBlock& block) {
    return std::visit(
        [](const auto& b) -> nlohmann::json {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextContent>) {
                return nlohmann::json{{"type", "text"}, {"text", b.text}};
            } else if constexpr (std::is_same_v<T, ImageContent>) {
                nlohmann::json j = {{"type", "image"},
                                    {"mime_type", b.mime_type},
                                    {"width", b.width},
                                    {"height", b.height},
                                    {"bytes", b.bytes},
                                    {"sha256", b.sha256}};
                j["artifact"] = ArtifactToJson(b.artifact);
                return j;
            } else if constexpr (std::is_same_v<T, AudioContent>) {
                nlohmann::json j = {{"type", "audio"},
                                    {"mime_type", b.mime_type},
                                    {"bytes", b.bytes},
                                    {"sha256", b.sha256}};
                j["artifact"] = ArtifactToJson(b.artifact);
                return j;
            } else if constexpr (std::is_same_v<T, ResourceLinkContent>) {
                return nlohmann::json{{"type", "resource_link"},
                                      {"uri", b.uri},
                                      {"name", b.name},
                                      {"title", b.title},
                                      {"description", b.description},
                                      {"mime_type", b.mime_type},
                                      {"size", b.size}};
            } else if constexpr (std::is_same_v<T, EmbeddedTextResourceContent>) {
                nlohmann::json j = {{"type", "resource_text"},
                                    {"uri", b.uri},
                                    {"mime_type", b.mime_type},
                                    {"text", b.text},
                                    {"truncated", b.truncated}};
                if (b.artifact.has_value()) {
                    j["artifact"] = ArtifactToJson(*b.artifact);
                }
                return j;
            } else if constexpr (std::is_same_v<T, EmbeddedBlobResourceContent>) {
                nlohmann::json j = {{"type", "resource_blob"},
                                    {"uri", b.uri},
                                    {"mime_type", b.mime_type},
                                    {"bytes", b.bytes},
                                    {"sha256", b.sha256}};
                j["artifact"] = ArtifactToJson(b.artifact);
                return j;
            } else {
                return nlohmann::json{{"type", "unknown"}, {"original_type", b.original_type},
                                      {"summary", b.summary}};
            }
        },
        block);
}

std::optional<ToolContentBlock> BlockFromJson(const nlohmann::json& json) {
    if (!json.is_object()) {
        return std::nullopt;
    }
    const std::string type = json.value("type", std::string());
    if (type == "text") {
        TextContent b;
        b.text = platform::SanitizeExternalText(json.value("text", std::string()));
        return ToolContentBlock{std::move(b)};
    }
    if (type == "image") {
        ImageContent b;
        b.mime_type = json.value("mime_type", std::string());
        b.width = json.value("width", 0U);
        b.height = json.value("height", 0U);
        if (json.contains("bytes") && json["bytes"].is_number_unsigned()) {
            b.bytes = json["bytes"].get<std::size_t>();
        }
        b.sha256 = json.value("sha256", std::string());
        if (auto artifact = ArtifactFromJson(json.value("artifact", nlohmann::json::object()))) {
            b.artifact = std::move(*artifact);
        } else {
            return std::nullopt;  // 引用块没了落点就是坏块
        }
        return ToolContentBlock{std::move(b)};
    }
    if (type == "audio") {
        AudioContent b;
        b.mime_type = json.value("mime_type", std::string());
        if (json.contains("bytes") && json["bytes"].is_number_unsigned()) {
            b.bytes = json["bytes"].get<std::size_t>();
        }
        b.sha256 = json.value("sha256", std::string());
        if (auto artifact = ArtifactFromJson(json.value("artifact", nlohmann::json::object()))) {
            b.artifact = std::move(*artifact);
        } else {
            return std::nullopt;
        }
        return ToolContentBlock{std::move(b)};
    }
    if (type == "resource_link") {
        ResourceLinkContent b;
        b.uri = platform::SanitizeExternalText(json.value("uri", std::string()));
        b.name = platform::SanitizeExternalText(json.value("name", std::string()));
        b.title = platform::SanitizeExternalText(json.value("title", std::string()));
        b.description = platform::SanitizeExternalText(json.value("description", std::string()));
        b.mime_type = json.value("mime_type", std::string());
        if (json.contains("size") && json["size"].is_number_integer()) {
            b.size = json["size"].get<std::int64_t>();
        }
        return ToolContentBlock{std::move(b)};
    }
    if (type == "resource_text") {
        EmbeddedTextResourceContent b;
        b.uri = platform::SanitizeExternalText(json.value("uri", std::string()));
        b.mime_type = json.value("mime_type", std::string());
        b.text = platform::SanitizeExternalText(json.value("text", std::string()));
        b.truncated = json.value("truncated", false);
        if (json.contains("artifact")) {
            if (auto artifact = ArtifactFromJson(json["artifact"])) {
                b.artifact = std::move(*artifact);
            }
        }
        return ToolContentBlock{std::move(b)};
    }
    if (type == "resource_blob") {
        EmbeddedBlobResourceContent b;
        b.uri = platform::SanitizeExternalText(json.value("uri", std::string()));
        b.mime_type = json.value("mime_type", std::string());
        if (json.contains("bytes") && json["bytes"].is_number_unsigned()) {
            b.bytes = json["bytes"].get<std::size_t>();
        }
        b.sha256 = json.value("sha256", std::string());
        if (auto artifact = ArtifactFromJson(json.value("artifact", nlohmann::json::object()))) {
            b.artifact = std::move(*artifact);
        } else {
            return std::nullopt;
        }
        return ToolContentBlock{std::move(b)};
    }
    if (type == "unknown") {
        UnknownContent b;
        b.original_type = platform::SanitizeExternalText(json.value("original_type", std::string()));
        b.summary = platform::SanitizeExternalText(json.value("summary", std::string()));
        return ToolContentBlock{std::move(b)};
    }
    return std::nullopt;
}

void SanitizeJsonTextInPlace(nlohmann::json& value) {
    if (value.is_string()) {
        value = platform::SanitizeExternalText(value.get<std::string>());
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            SanitizeJsonTextInPlace(item);
        }
        return;
    }
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            SanitizeJsonTextInPlace(it.value());
        }
    }
}

void SanitizePayloadTextInPlace(ToolResultPayload& payload) {
    for (auto& block : payload.content) {
        std::visit(
            [](auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextContent>) {
                    b.text = platform::SanitizeExternalText(b.text);
                } else if constexpr (std::is_same_v<T, EmbeddedTextResourceContent>) {
                    b.uri = platform::SanitizeExternalText(b.uri);
                    b.text = platform::SanitizeExternalText(b.text);
                } else if constexpr (std::is_same_v<T, ResourceLinkContent>) {
                    b.uri = platform::SanitizeExternalText(b.uri);
                    b.name = platform::SanitizeExternalText(b.name);
                    b.title = platform::SanitizeExternalText(b.title);
                    b.description = platform::SanitizeExternalText(b.description);
                } else if constexpr (std::is_same_v<T, UnknownContent>) {
                    b.original_type = platform::SanitizeExternalText(b.original_type);
                    b.summary = platform::SanitizeExternalText(b.summary);
                }
            },
            block);
    }
    if (payload.structured_content.has_value()) {
        SanitizeJsonTextInPlace(*payload.structured_content);
    }
}

}  // namespace lubancode::tools
