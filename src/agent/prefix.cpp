#include "agent/prefix.hpp"

#include <type_traits>
#include <variant>

namespace lubancode::agent {

namespace {

// 两枚内容块逐字段深等。TextBlock/ImageBlock/ToolUseBlock/ToolResultBlock/
// ThinkingBlock 全字段都算——缓存只认请求字节,一个字段都不能放过。
bool SameBlock(const api::ContentBlock& left, const api::ContentBlock& right) {
    if (left.index() != right.index()) {
        return false;
    }
    return std::visit(
        [&right](const auto& l) -> bool {
            using T = std::decay_t<decltype(l)>;
            const T& r = std::get<T>(right);
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                return l.text == r.text;
            } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                return l.media_type == r.media_type && l.data == r.data && l.filename == r.filename &&
                       l.width == r.width && l.height == r.height;
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                return l.id == r.id && l.name == r.name && l.input == r.input && l.caller == r.caller;
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                // MCP 富结果单 P0.3:富块在身时块序与 structuredContent 都算
                // ——图片引用的 MIME/sha/尺寸在块里逐字段比,structured 用
                // 稳定序列化比,一个字段都不放过(content 是投影,富块时由
                // blocks 派生,重复比不断错案)。
                if (l.blocks.empty() && r.blocks.empty() && !l.structured_content.has_value() &&
                    !r.structured_content.has_value()) {
                    return l.tool_use_id == r.tool_use_id && l.content == r.content && l.is_error == r.is_error;
                }
                return l.tool_use_id == r.tool_use_id && l.is_error == r.is_error && l.blocks == r.blocks &&
                       l.structured_content == r.structured_content;
            } else if constexpr (std::is_same_v<T, api::ModelImageBlock>) {
                // 引用块全字段比(路径/尺寸/sha 都是请求可见面,一个不放过)。
                return l.id == r.id && l.filename == r.filename && l.path == r.path && l.mime_type == r.mime_type &&
                       l.width == r.width && l.height == r.height && l.bytes == r.bytes && l.sha256 == r.sha256;
            } else if constexpr (std::is_same_v<T, api::ServerToolUseBlock>) {
                // 服务端工具搜索(动态工具 P3):id/name/入参都是回传面,逐项比。
                return l.id == r.id && l.name == r.name && l.input == r.input;
            } else if constexpr (std::is_same_v<T, api::ServerToolResultBlock>) {
                return l.tool_use_id == r.tool_use_id && l.content == r.content;
            } else {
                return l.text == r.text && l.signature == r.signature;
            }
        },
        left);
}

bool SameMessage(const api::Message& left, const api::Message& right) {
    if (left.role != right.role || left.content.size() != right.content.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.content.size(); ++i) {
        if (!SameBlock(left.content[i], right.content[i])) {
            return false;
        }
    }
    return true;
}

bool SameTools(const std::vector<api::ToolDefinition>& left, const std::vector<api::ToolDefinition>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        // load_mode(动态工具 P3)也算:同一枚定义 eager/deferred 的翻动就是
        // 请求字节翻动(defer_loading 字段进出),指纹必须看得见。
        if (left[i].name != right[i].name || left[i].description != right[i].description ||
            left[i].input_schema != right[i].input_schema || left[i].load_mode != right[i].load_mode) {
            return false;
        }
    }
    return true;
}

// FNV-1a 64(context_events.cpp 同款):指纹只判"完全相同",不做安全用途。
void HashMix(std::uint64_t& hash, const std::string& text) {
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
}

void HashMixBlock(std::uint64_t& hash, const api::ContentBlock& block) {
    std::visit(
        [&hash](const auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, api::TextBlock>) {
                HashMix(hash, "t:");
                HashMix(hash, b.text);
            } else if constexpr (std::is_same_v<T, api::ImageBlock>) {
                HashMix(hash, "i:");
                HashMix(hash, b.media_type);
                HashMix(hash, b.data);
                HashMix(hash, b.filename);
                hash ^= static_cast<std::uint64_t>(b.width) << 3;
                hash ^= static_cast<std::uint64_t>(b.height) << 5;
                hash *= 1099511628211ULL;
            } else if constexpr (std::is_same_v<T, api::ToolUseBlock>) {
                HashMix(hash, "u:");
                HashMix(hash, b.id);
                HashMix(hash, b.name);
                HashMix(hash, b.input.dump());
                HashMix(hash, b.caller);
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                HashMix(hash, "r:");
                HashMix(hash, b.tool_use_id);
                HashMix(hash, b.content);
                // MCP 富结果单:图片引用的 MIME/SHA/尺寸、structured 的稳定
                // JSON 都混进指纹——缓存只认请求字节,base64 不进这层,但
                // 引用字段换了,请求语义就换了,指纹必须跟着换。
                for (const auto& rich : b.blocks) {
                    HashMix(hash, tools::BlockToJson(rich).dump());
                }
                if (b.structured_content.has_value()) {
                    HashMix(hash, "s:");
                    HashMix(hash, b.structured_content->dump());
                }
                hash ^= b.is_error ? 0x9e3779b97f4a7c15ULL : 0;
                hash *= 1099511628211ULL;
            } else if constexpr (std::is_same_v<T, api::ModelImageBlock>) {
                HashMix(hash, "m:");
                HashMix(hash, b.id);
                HashMix(hash, b.filename);
                HashMix(hash, b.path);
                HashMix(hash, b.mime_type);
                HashMix(hash, b.sha256);
                hash ^= static_cast<std::uint64_t>(b.width) << 3;
                hash ^= static_cast<std::uint64_t>(b.height) << 5;
                hash ^= static_cast<std::uint64_t>(b.bytes) << 7;
                hash *= 1099511628211ULL;
            } else if constexpr (std::is_same_v<T, api::ServerToolUseBlock>) {
                // 服务端工具搜索(动态工具 P3):这对块原样回传,进指纹——
                // 块变了请求字节就变,追加律判定必须看得见。
                HashMix(hash, "su:");
                HashMix(hash, b.id);
                HashMix(hash, b.name);
                HashMix(hash, b.input.dump());
            } else if constexpr (std::is_same_v<T, api::ServerToolResultBlock>) {
                HashMix(hash, "sr:");
                HashMix(hash, b.tool_use_id);
                HashMix(hash, b.content.dump());
            } else {
                HashMix(hash, "h:");
                HashMix(hash, b.text);
                HashMix(hash, b.signature);
            }
        },
        block);
}

std::string HashFinish(std::uint64_t hash) {
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

}  // namespace

PrefixDiff DiffRequests(const api::Request& prev, const api::Request& next) {
    PrefixDiff diff;
    diff.model_changed = prev.model != next.model;
    diff.system_changed = prev.system != next.system;
    diff.tools_changed = !SameTools(prev.tools, next.tools);

    const std::size_t shared = prev.messages.size() <= next.messages.size() ? prev.messages.size()
                                                                            : next.messages.size();
    bool shared_diverged = false;
    for (std::size_t i = 0; i < shared; ++i) {
        if (!SameMessage(prev.messages[i], next.messages[i])) {
            diff.messages_append_only = false;
            shared_diverged = true;
            diff.old_message_changed_at = i;
            break;
        }
    }
    if (next.messages.size() < prev.messages.size()) {
        // 旧消息被裁掉了:共用段也许逐条相等,但前缀整体缩短,同样是追改。
        diff.messages_append_only = false;
        if (!shared_diverged) {
            diff.old_message_changed_at = shared;  // 第一条被裁掉的位置
        }
    }
    diff.old_message_changed = !diff.messages_append_only;
    diff.appended_messages =
        next.messages.size() > prev.messages.size() ? next.messages.size() - prev.messages.size() : 0;
    return diff;
}

PrefixFingerprint FingerprintRequest(const api::Request& request) {
    PrefixFingerprint out;
    out.model = request.model;
    std::uint64_t system_hash = 14695981039346656037ULL;
    HashMix(system_hash, request.system);
    out.system_hash = HashFinish(system_hash);
    std::uint64_t tools_hash = 14695981039346656037ULL;
    for (const auto& tool : request.tools) {
        HashMix(tools_hash, tool.name);
        HashMix(tools_hash, tool.description);
        HashMix(tools_hash, tool.input_schema.dump());
        // 动态工具 P3:load_mode 决定 wire 上有没有 defer_loading 字段,同一
        // 枚定义翻档就是请求字节翻动,指纹一并混入。
        HashMix(tools_hash, tool.load_mode == api::ToolLoadMode::Deferred ? "deferred" : "eager");
    }
    out.tools_hash = HashFinish(tools_hash);
    out.message_hashes.reserve(request.messages.size());
    for (const auto& message : request.messages) {
        std::uint64_t hash = 14695981039346656037ULL;
        HashMix(hash, message.role == api::Role::User ? "U" : "A");
        for (const auto& block : message.content) {
            HashMixBlock(hash, block);
        }
        out.message_hashes.push_back(HashFinish(hash));
    }
    return out;
}

PrefixDiff DiffFingerprints(const PrefixFingerprint& prev, const PrefixFingerprint& next) {
    PrefixDiff diff;
    diff.model_changed = prev.model != next.model;
    diff.system_changed = prev.system_hash != next.system_hash;
    diff.tools_changed = prev.tools_hash != next.tools_hash;

    const std::size_t shared = prev.message_hashes.size() <= next.message_hashes.size()
                                   ? prev.message_hashes.size()
                                   : next.message_hashes.size();
    bool shared_diverged = false;
    for (std::size_t i = 0; i < shared; ++i) {
        if (prev.message_hashes[i] != next.message_hashes[i]) {
            diff.messages_append_only = false;
            shared_diverged = true;
            diff.old_message_changed_at = i;
            break;
        }
    }
    if (next.message_hashes.size() < prev.message_hashes.size()) {
        diff.messages_append_only = false;
        if (!shared_diverged) {
            diff.old_message_changed_at = shared;
        }
    }
    diff.old_message_changed = !diff.messages_append_only;
    diff.appended_messages = next.message_hashes.size() > prev.message_hashes.size()
                                 ? next.message_hashes.size() - prev.message_hashes.size()
                                 : 0;
    return diff;
}

StablePrefixView StablePrefixOf(const PrefixFingerprint& prev, const PrefixFingerprint& next) {
    StablePrefixView out;
    const std::size_t shared =
        prev.message_hashes.size() <= next.message_hashes.size() ? prev.message_hashes.size()
                                                                 : next.message_hashes.size();
    std::size_t stable = 0;
    while (stable < shared && prev.message_hashes[stable] == next.message_hashes[stable]) {
        ++stable;
    }
    out.messages = stable;
    if (stable == 0) {
        return out;  // 一条都不共享:无稳定前缀,hash 留空
    }
    // 合成指纹:把这段前缀的逐条 hash 再折一层 FNV——诊断账只留这一枚
    // 短 hash,不落任何消息正文。
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t i = 0; i < stable; ++i) {
        HashMix(hash, prev.message_hashes[i]);
        hash ^= static_cast<std::uint64_t>(i);
        hash *= 1099511628211ULL;
    }
    out.hash = HashFinish(hash);
    return out;
}

}  // namespace lubancode::agent
