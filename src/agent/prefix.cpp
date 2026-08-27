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
                return l.id == r.id && l.name == r.name && l.input == r.input;
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                return l.tool_use_id == r.tool_use_id && l.content == r.content && l.is_error == r.is_error;
            } else if constexpr (std::is_same_v<T, api::ModelImageBlock>) {
                // 引用块全字段比(路径/尺寸/sha 都是请求可见面,一个不放过)。
                return l.id == r.id && l.filename == r.filename && l.path == r.path && l.mime_type == r.mime_type &&
                       l.width == r.width && l.height == r.height && l.bytes == r.bytes && l.sha256 == r.sha256;
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
        if (left[i].name != right[i].name || left[i].description != right[i].description ||
            left[i].input_schema != right[i].input_schema) {
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
            } else if constexpr (std::is_same_v<T, api::ToolResultBlock>) {
                HashMix(hash, "r:");
                HashMix(hash, b.tool_use_id);
                HashMix(hash, b.content);
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

}  // namespace lubancode::agent
