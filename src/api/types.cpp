#include "api/types.hpp"

#include <algorithm>
#include <cctype>

#include <variant>

#include "platform/json_safe.hpp"  // SanitizeJsonStrings:工具入参/结果这类 JSON 树字段的递归清洗

namespace lubancode::api {

std::string LowerReasoningEffort(std::string effort) {
    for (char& c : effort) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return effort;
}

std::string ToolResultImageDegradedNote(const ToolResultBlock& result) {
    std::size_t count = 0;
    std::string names;
    for (const auto& block : result.blocks) {
        if (const auto* image = std::get_if<tools::ImageContent>(&block); image != nullptr) {
            ++count;
            if (!names.empty()) {
                names += "、";
            }
            names += image->artifact.filename;
        }
    }
    if (count == 0) {
        return std::string();
    }
    return "[wire 降级] 该 wire 不支持工具结果图片," + std::to_string(count) +
           " 张未随行,字节已存盘: " + names;
}

bool ReasoningEffortIsOff(const std::string& effort) {
    const std::string lower = LowerReasoningEffort(effort);
    return lower == "none" || lower == "minimal";
}

namespace {

// 疑似密钥的串打码:sk- 起、Bearer 后面的长 token——各留前 6 位加 "...",
// 够对账不够复用。
std::string MaskSecrets(std::string text) {
    static const std::string_view triggers[] = {"sk-", "Bearer "};
    for (std::size_t from = 0;;) {
        std::size_t best = std::string::npos;
        std::string_view picked;
        for (const std::string_view trigger : triggers) {
            const std::size_t at = text.find(trigger, from);
            if (at != std::string::npos && (best == std::string::npos || at < best)) {
                best = at;
                picked = trigger;
            }
        }
        if (best == std::string::npos) {
            break;
        }
        const std::size_t tail = best + picked.size();
        const std::size_t run_end = text.find_first_of(" \"'\n\r,}", tail);
        const std::size_t len = (run_end == std::string::npos ? text.size() : run_end) - tail;
        if (len > 12) {
            text.replace(tail, len, text.substr(tail, 6) + "...");
        }
        from = tail + 9;
    }
    return text;
}

// 截短到约 240 字节(退到 UTF-8 码点边界),尾巴标 "...(截短)"。
std::string TruncateForUser(std::string text) {
    constexpr std::size_t kCap = 240;
    if (text.size() <= kCap) {
        return text;
    }
    std::size_t cut = kCap;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;  // 退到码点起点
    }
    text.resize(cut);
    text += "...(截短)";
    return text;
}

std::string HumanizeKnownProviderError(std::string text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.find("maximum context length") != std::string::npos ||
        lower.find("context length exceeded") != std::string::npos ||
        (lower.find("context window") != std::string::npos && lower.find("exceed") != std::string::npos)) {
        return "上下文超出模型窗口；请缩短输入、开新会话，或调低输出上限。服务端原话: " + text;
    }
    if (lower.find("not a multimodal model") != std::string::npos ||
        (lower.find("image input") != std::string::npos && lower.find("not support") != std::string::npos)) {
        return "当前模型不支持图片输入；请换多模态模型。服务端原话: " + text;
    }
    return text;
}

}  // namespace

std::string SummarizeErrorBodyForUser(const std::string& body) {
    std::string flat = body;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(body);
        const nlohmann::json* error = nullptr;
        if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
            error = &parsed["error"];
        } else if (parsed.is_object()) {
            error = &parsed;
        }
        if (error != nullptr) {
            const std::string message = error->value("message", std::string());
            const std::string type = error->value("type", std::string());
            const std::string code = error->value("code", std::string());
            std::string out = message.empty() ? std::string("服务端回了错误体,没有 message 字段") : message;
            if (!type.empty()) {
                out += " (type=" + type;
                if (!code.empty()) {
                    out += ", code=" + code;
                }
                out += ")";
            } else if (!code.empty()) {
                out += " (code=" + code + ")";
            }
            flat = out;
        }
    } catch (const nlohmann::json::exception&) {
        // 不是 JSON:原文走同一道打码截短。
    }
    return TruncateForUser(MaskSecrets(HumanizeKnownProviderError(std::move(flat))));
}

namespace {

// 目录档案里有没有把某枚档位名声明成 supported_efforts 之一。
bool DeclaresEffortLevel(const ReasoningConfig& config, const std::string& lowered) {
    for (const auto& declared : config.supported_efforts) {
        if (LowerReasoningEffort(declared) == lowered) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool ReasoningEffortIsOff(const std::string& effort, const ReasoningConfig& config) {
    const std::string lower = LowerReasoningEffort(effort);
    if (lower == "none") {
        return true;  // 目录里 none 永远是关,没有第二种语义
    }
    if (lower != "minimal") {
        return false;
    }
    // minimal:目录声明了它(与 none 并列)就是真档;没声明才沿用旧口径当关。
    return !DeclaresEffortLevel(config, "minimal");
}

ReasoningHistorySupport ReasoningHistorySupportFor(const ReasoningConfig& config) {
    // 方言声明了请求控制就以它为准(哪怕是 replay=always 的模型——声明
    // 了 thinking_keep 说明这家的保留要客户端显式请求,与固定开启分家)。
    if (config.dialect.history_control == "thinking_keep") {
        return ReasoningHistorySupport::RequestControl;
    }
    if (!config.dialect.empty() && config.dialect.replay == "always") {
        return ReasoningHistorySupport::ServerFixed;
    }
    return ReasoningHistorySupport::None;
}

int ReasoningBudgetForEffort(const ReasoningConfig& config, const std::string& effort,
                             int max_tokens) {
    const std::string lower = LowerReasoningEffort(effort);
    if (!config.budget_min.has_value() && !config.budget_max.has_value()) {
        int legacy = 16384;
        if (lower == "low" || lower == "minimal") legacy = 1024;
        else if (lower == "medium" || lower == "auto") legacy = 4096;
        else if (lower == "xhigh" || lower == "extra") legacy = 32768;
        else if (lower == "max") legacy = 49152;
        if (max_tokens > 0 && legacy >= max_tokens) {
            legacy = max_tokens > 256 ? max_tokens - 256 : max_tokens / 2;
        }
        return std::max(1, legacy);
    }
    const int configured_min = config.budget_min.value_or(1024);
    const int configured_max = config.budget_max.value_or(49152);
    const int minimum = std::max(1, configured_min);
    const int maximum = std::max(minimum, configured_max);
    int rank = 2;
    if (lower == "low" || lower == "minimal") rank = 0;
    else if (lower == "medium") rank = 1;
    else if (lower == "high") rank = 2;
    else if (lower == "xhigh" || lower == "extra") rank = 3;
    else if (lower == "max") rank = 4;
    const long long span = static_cast<long long>(maximum) - minimum;
    int budget = minimum + static_cast<int>((span * rank) / 4);
    if (max_tokens > 0 && budget >= max_tokens) {
        budget = max_tokens > 256 ? max_tokens - 256 : max_tokens / 2;
    }
    return std::max(1, budget);
}

namespace {

// JSON 树里所有字符串字段过一遍 SanitizeExternalText(合法时零成本)。
// 工具入参(ToolUseBlock.input)、BuiltinTool 的 input 这类字段是任意
// JSON,这里递归洗整棵树——洗法跟 platform/json_safe.cpp 的
// SanitizeJsonStrings 同一套,只是它藏在匿名命名空间里,不能直接用。
void SanitizeJsonTree(nlohmann::json& value) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        if (!platform::IsValidUtf8(text)) {
            value = platform::SanitizeExternalText(text);
        }
        return;
    }
    if (value.is_array()) {
        for (auto& item : value) {
            SanitizeJsonTree(item);
        }
        return;
    }
    if (value.is_object()) {
        for (auto& item : value) {
            SanitizeJsonTree(item);
        }
    }
}

}  // namespace

void SanitizeContentBlock(ContentBlock& block) {
    std::visit(
        [](auto& b) {
            using T = std::decay_t<decltype(b)>;
            if constexpr (std::is_same_v<T, TextBlock>) {
                b.text = platform::SanitizeExternalText(b.text);
            } else if constexpr (std::is_same_v<T, ImageBlock>) {
                b.media_type = platform::SanitizeExternalText(b.media_type);
                b.data = platform::SanitizeExternalText(b.data);
                b.filename = platform::SanitizeExternalText(b.filename);
            } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                b.id = platform::SanitizeExternalText(b.id);
                b.name = platform::SanitizeExternalText(b.name);
                SanitizeJsonTree(b.input);
            } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                b.tool_use_id = platform::SanitizeExternalText(b.tool_use_id);
                b.content = platform::SanitizeExternalText(b.content);
                // 富结果(MCP 富结果单 P0.3):块文本与 structuredContent
                // 同过编码关——server 塞进来的坏串不洗,dump() 当场 316。
                if (!b.blocks.empty() || b.structured_content.has_value()) {
                    tools::ToolResultPayload payload;
                    payload.content = std::move(b.blocks);
                    payload.structured_content = std::move(b.structured_content);
                    tools::SanitizePayloadTextInPlace(payload);
                    // 投影先算再搬走:payload.content move 走之后投影就只剩
                    // 空壳,缓存会被清成空串。
                    const std::string projection = tools::TextProjection(payload);
                    b.blocks = std::move(payload.content);
                    b.structured_content = std::move(payload.structured_content);
                    b.content = std::move(projection);
                }
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                b.text = platform::SanitizeExternalText(b.text);
                b.signature = platform::SanitizeExternalText(b.signature);
            } else if constexpr (std::is_same_v<T, ModelImageBlock>) {
                // 引用块:字段全是本地起的(id 是 wire 串,文件名是 ASCII),
                // 照样过一遍编码关,坏串洗掉,不另开例外。
                b.id = platform::SanitizeExternalText(b.id);
                b.filename = platform::SanitizeExternalText(b.filename);
                b.path = platform::SanitizeExternalText(b.path);
                b.mime_type = platform::SanitizeExternalText(b.mime_type);
                b.sha256 = platform::SanitizeExternalText(b.sha256);
            }
        },
        block);
}

void SanitizeMessage(Message& message) {
    for (auto& block : message.content) {
        SanitizeContentBlock(block);
    }
}

void ApplyRequestProfile(Request& request, const RequestProfile& profile) {
    if (!profile.model.empty()) {
        request.model = profile.model;
    }
    request.reasoning_effort = profile.reasoning_effort;
    request.reasoning = profile.reasoning;
    request.reasoning_history = profile.reasoning_history;
}

void SanitizeRequest(Request& request) {
    request.model = platform::SanitizeExternalText(request.model);
    request.system = platform::SanitizeExternalText(request.system);
    request.reasoning_effort = platform::SanitizeExternalText(request.reasoning_effort);
    for (auto& message : request.messages) {
        SanitizeMessage(message);
    }
    for (auto& tool : request.tools) {
        tool.name = platform::SanitizeExternalText(tool.name);
        tool.description = platform::SanitizeExternalText(tool.description);
        SanitizeJsonTree(tool.input_schema);
    }
    SanitizeJsonTree(request.extra_body);
}

}  // namespace lubancode::api
