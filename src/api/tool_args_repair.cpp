#include "api/tool_args_repair.hpp"

#include <cctype>
#include <optional>
#include <utility>

#include "platform/text_encoding.hpp"  // TruncateUtf8Prefix:片段按 UTF-8 对齐截

namespace lubancode::api {

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

std::string Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

// 修补档只收 object:工具入参本该是对象,修出个数组/标量不算救回。
std::optional<nlohmann::json> ParseObject(const std::string& text) {
    nlohmann::json value = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded() || !value.is_object()) {
        return std::nullopt;
    }
    return value;
}

// 剥 markdown 围栏:开头 ``` 带可选语言名,结尾 ``` 可有可无(截断时没有)。
std::optional<std::string> StripFence(const std::string& text) {
    if (text.rfind("```", 0) != 0) {
        return std::nullopt;
    }
    std::size_t pos = 3;
    while (pos < text.size() && (std::isalnum(static_cast<unsigned char>(text[pos])) != 0 ||
                                 text[pos] == '_' || text[pos] == '-')) {
        ++pos;
    }
    std::string body = Trim(std::string_view(text).substr(pos));
    if (body.size() >= 3 && body.compare(body.size() - 3, 3, "```") == 0) {
        body = Trim(std::string_view(body).substr(0, body.size() - 3));
    }
    return body;
}

// 去字符串外、紧贴 } / ] 的多余逗号。返回 nullopt = 没有可去的。
std::optional<std::string> DropTrailingCommas(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_string = false;
    bool escaped = false;
    bool changed = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out.push_back(c);
            continue;
        }
        if (c == ',') {
            std::size_t next = i + 1;
            while (next < text.size() && IsSpace(text[next])) {
                ++next;
            }
            if (next < text.size() && (text[next] == '}' || text[next] == ']')) {
                changed = true;
                continue;  // 吞掉这枚逗号
            }
        }
        out.push_back(c);
    }
    if (!changed) {
        return std::nullopt;
    }
    return out;
}

// 通篇单引号换双引号:调用方已确认整段没有双引号,串里不必再转义;
// 单引号串里的 \' 还原成裸 '。
std::optional<std::string> SingleToDoubleQuotes(const std::string& text) {
    if (text.find('"') != std::string::npos || text.find('\'') == std::string::npos) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size());
    bool in_string = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string && c == '\\' && i + 1 < text.size()) {
            if (text[i + 1] == '\'') {
                out.push_back('\'');
            } else {
                out.push_back(c);
                out.push_back(text[i + 1]);
            }
            ++i;
            continue;
        }
        if (c == '\'') {
            out.push_back('"');
            in_string = !in_string;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// 末尾补齐:没闭合的字符串补引号,悬着的逗号去掉,括号按栈逆序补。
// 括号错配或悬在冒号上(值整个没了)不补。返回 nullopt = 无可补/补不了。
std::optional<std::string> CloseTruncated(const std::string& text) {
    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            stack.push_back('}');
        } else if (c == '[') {
            stack.push_back(']');
        } else if (c == '}' || c == ']') {
            if (stack.empty() || stack.back() != c) {
                return std::nullopt;  // 错配:不是截断,是写乱了
            }
            stack.pop_back();
        }
    }
    if (stack.empty() && !in_string) {
        return std::nullopt;
    }
    std::string out = text;
    if (in_string) {
        if (escaped) {
            out.pop_back();  // 悬着半枚转义,丢掉反斜杠
        }
        out.push_back('"');
    } else {
        while (!out.empty() && IsSpace(out.back())) {
            out.pop_back();
        }
        if (!out.empty() && out.back() == ',') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == ':') {
            return std::nullopt;
        }
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        out.push_back(*it);
    }
    return out;
}

}  // namespace

ToolArgsRepairResult RepairToolArgs(std::string_view raw) {
    ToolArgsRepairResult result;
    const std::string trimmed = Trim(raw);
    if (trimmed.empty()) {
        result.ok = true;
        result.value = nlohmann::json::object();
        return result;
    }
    try {
        result.value = nlohmann::json::parse(trimmed);
        result.ok = true;
        // 双重编码:整段是一枚字符串,里头才是对象。拆一层,标修补。
        if (result.value.is_string()) {
            if (auto inner = ParseObject(result.value.get<std::string>()); inner.has_value()) {
                result.value = std::move(*inner);
                result.repaired = true;
                result.repairs.push_back("拆掉一层字符串套壳(入参被双重 JSON 编码)");
            }
        }
        return result;
    } catch (const nlohmann::json::exception& e) {
        result.error = e.what();
    }

    std::string candidate = trimmed;
    std::vector<std::string> repairs;
    const auto accept = [&result, &repairs](nlohmann::json value, bool closed) {
        result.ok = true;
        result.value = std::move(value);
        result.repaired = true;
        result.repairs = repairs;
        result.closed_truncation = closed;
        result.error.clear();
    };

    if (auto stripped = StripFence(candidate); stripped.has_value()) {
        candidate = std::move(*stripped);
        repairs.push_back("剥掉 markdown 代码围栏");
        if (auto value = ParseObject(candidate); value.has_value()) {
            accept(std::move(*value), false);
            return result;
        }
    }
    if (auto quoted = SingleToDoubleQuotes(candidate); quoted.has_value()) {
        candidate = std::move(*quoted);
        repairs.push_back("单引号改双引号");
        if (auto value = ParseObject(candidate); value.has_value()) {
            accept(std::move(*value), false);
            return result;
        }
    }
    if (auto dropped = DropTrailingCommas(candidate); dropped.has_value()) {
        candidate = std::move(*dropped);
        repairs.push_back("去掉末尾多余逗号");
        if (auto value = ParseObject(candidate); value.has_value()) {
            accept(std::move(*value), false);
            return result;
        }
    }
    if (auto closed = CloseTruncated(candidate); closed.has_value()) {
        std::string patched = std::move(*closed);
        if (auto dropped = DropTrailingCommas(patched); dropped.has_value()) {
            patched = std::move(*dropped);
        }
        if (auto value = ParseObject(patched); value.has_value()) {
            repairs.push_back("补齐末尾未闭合的字符串/括号(疑似输出被截断)");
            accept(std::move(*value), true);
            return result;
        }
    }
    return result;  // 修不成:ok=false,error 留严格档原话
}

std::string ToolArgsSnippet(std::string_view raw, std::size_t max_bytes) {
    if (raw.size() <= max_bytes) {
        return std::string(raw);
    }
    return platform::TruncateUtf8Prefix(raw, max_bytes) + "...(共 " + std::to_string(raw.size()) + " 字节)";
}

std::string BuildToolArgsParseErrorText(const std::string& tool_name, const std::string& parse_error,
                                        std::string_view raw) {
    return "工具 " + tool_name + " 的入参不是合法 JSON,本次未执行。\n解析错误: " + parse_error +
           "\n原文片段(前 200 字节): " + ToolArgsSnippet(raw) +
           "\n请重新调用该工具,arguments 必须是一个完整合法的 JSON 对象:键和字符串用双引号,"
           "不留末尾逗号,不包 markdown 代码围栏,字符串里的换行与引号要转义。";
}

std::string BuildToolArgsTruncatedText(const std::string& tool_name, std::string_view raw) {
    return "工具 " + tool_name + " 的入参不完整:模型输出撞上长度上限(max_tokens)被截断,"
           "本次未执行(半截内容不会落盘)。\n原文片段(前 200 字节): " + ToolArgsSnippet(raw) +
           "\n输出被截断,请把内容拆小分多次写:先写一段,再用编辑/追加补齐后续,"
           "每次调用的 arguments 都要是完整合法的 JSON 对象。";
}

bool IsLengthStopReason(std::string_view stop_reason) {
    return stop_reason == "max_tokens" || stop_reason == "length";
}

}  // namespace lubancode::api
