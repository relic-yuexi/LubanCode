#include "hooks/protocol.hpp"

#include <algorithm>

#include "platform/text_encoding.hpp"

#ifdef _WIN32
#include "platform/paths.hpp"  // ChildStreamCodePageCandidates / CodePageBytesToUtf8
#endif

namespace lubancode::hooks {

namespace {

bool IsBlank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; });
}

std::string FirstLines(const std::string& text, int max_lines) {
    std::string out;
    int lines = 0;
    std::size_t pos = 0;
    while (pos <= text.size() && lines < max_lines) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!out.empty()) {
            out += "\n";
        }
        out += line;
        ++lines;
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

bool ParsePermissionString(const std::string& value, HookEventResult::Permission& out) {
    if (value == "allow") {
        out = HookEventResult::Permission::Allow;
        return true;
    }
    if (value == "deny") {
        out = HookEventResult::Permission::Deny;
        return true;
    }
    if (value == "ask") {
        out = HookEventResult::Permission::Ask;
        return true;
    }
    return false;
}

}  // namespace

nlohmann::json BuildStdinPayload(const HookPayload& payload, const HookContext& context,
                                 const std::string& hook_run_id) {
    nlohmann::json out;
    out["schema_version"] = 2;
    out["hook_event_name"] = std::string(ToString(payload.event));
    out["hook_run_id"] = hook_run_id;
    out["session_id"] = context.session_id;
    out["turn_id"] = context.turn_id;
    out["cwd"] = context.cwd;
    out["transcript_path"] = context.transcript_path;
    out["permission_mode"] = context.permission_mode;
    out["agent_id"] = context.agent_id.has_value() ? nlohmann::json(*context.agent_id) : nlohmann::json();
    out["agent_type"] = context.agent_type.has_value() ? nlohmann::json(*context.agent_type) : nlohmann::json();
    out["parent_agent_id"] =
        context.parent_agent_id.has_value() ? nlohmann::json(*context.parent_agent_id) : nlohmann::json();
    // 事件字段原样并进来(字段名由发射方按事件 schema 给,公共字段若被事件
    // 字段撞名,事件字段胜——只有公共字段没填全的边角才会发生)。
    if (payload.fields.is_object()) {
        for (auto it = payload.fields.begin(); it != payload.fields.end(); ++it) {
            out[it.key()] = it.value();
        }
    }
    return out;
}

HookOutput ParseStdoutJson(HookEvent event, const std::string& stdout_text) {
    HookOutput parsed;
    if (IsBlank(stdout_text)) {
        parsed.ok = true;  // 没有结构化输出,全部字段缺省
        return parsed;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(stdout_text);
    } catch (const std::exception& e) {
        parsed.error = std::string("stdout 不是合法 JSON: ") + e.what();
        return parsed;
    }
    if (!root.is_object()) {
        parsed.error = "stdout JSON 必须是 object";
        return parsed;
    }

    const EventOutputCapabilities caps = OutputCapabilities(event);

    if (root.contains("continue")) {
        if (!root["continue"].is_boolean()) {
            parsed.error = "continue 字段必须是 boolean";
            return parsed;
        }
        parsed.has_continue = true;
        parsed.continue_flag = root["continue"].get<bool>();
        if (!parsed.continue_flag && !caps.can_block) {
            // PostToolUse/SessionEnd/SessionStart 这类拦不住任何事的事件上写
            // continue=false:字段用错,报 schema_error。副作用都发生了,不许
            // 冒充"撤销了"。
            parsed.error = std::string(ToString(event)) + " 事件不支持 continue=false 阻断";
            return parsed;
        }
    }
    if (root.contains("stopReason")) {
        if (!root["stopReason"].is_string()) {
            parsed.error = "stopReason 字段必须是 string";
            return parsed;
        }
        parsed.stop_reason = root["stopReason"].get<std::string>();
    }
    if (root.contains("systemMessage")) {
        if (!root["systemMessage"].is_string()) {
            parsed.error = "systemMessage 字段必须是 string";
            return parsed;
        }
        parsed.system_message = root["systemMessage"].get<std::string>();
    }

    if (root.contains("hookSpecificOutput")) {
        const auto& specific = root["hookSpecificOutput"];
        if (!specific.is_object()) {
            parsed.error = "hookSpecificOutput 必须是 object";
            return parsed;
        }
        if (specific.contains("hookEventName")) {
            if (!specific["hookEventName"].is_string()) {
                parsed.error = "hookSpecificOutput.hookEventName 必须是 string";
                return parsed;
            }
            if (specific["hookEventName"].get<std::string>() != std::string(ToString(event))) {
                parsed.error = "hookSpecificOutput.hookEventName 与触发事件不符";
                return parsed;
            }
        }

        if (specific.contains("permissionDecision")) {
            if (!caps.permission_decision) {
                parsed.error = std::string(ToString(event)) + " 事件不支持 permissionDecision";
                return parsed;
            }
            if (!specific["permissionDecision"].is_string() ||
                !ParsePermissionString(specific["permissionDecision"].get<std::string>(), parsed.permission)) {
                parsed.error = "permissionDecision 必须是 allow/deny/ask";
                return parsed;
            }
            parsed.has_permission_decision = true;
        }
        if (specific.contains("permissionDecisionReason")) {
            if (!specific["permissionDecisionReason"].is_string()) {
                parsed.error = "permissionDecisionReason 必须是 string";
                return parsed;
            }
            parsed.permission_reason = specific["permissionDecisionReason"].get<std::string>();
        }
        if (specific.contains("updatedInput")) {
            if (!caps.updated_input) {
                parsed.error = std::string(ToString(event)) + " 事件不支持 updatedInput(改写参数只走 PreToolUse)";
                return parsed;
            }
            if (!specific["updatedInput"].is_object()) {
                parsed.error = "updatedInput 必须是 object";
                return parsed;
            }
            // updatedInput 只能与 allow 同返(规格决策矩阵)。deny/ask 带着它来
            // = 字段用错。
            if (parsed.permission != HookEventResult::Permission::Allow) {
                parsed.error = "updatedInput 只能与 permissionDecision=allow 同返";
                return parsed;
            }
            parsed.has_updated_input = true;
            parsed.updated_input = specific["updatedInput"];
        }
        if (specific.contains("additionalContext")) {
            if (!caps.additional_context) {
                parsed.error = std::string(ToString(event)) + " 事件不支持 additionalContext";
                return parsed;
            }
            if (!specific["additionalContext"].is_string()) {
                parsed.error = "additionalContext 必须是 string";
                return parsed;
            }
            parsed.has_additional_context = true;
            parsed.additional_context = specific["additionalContext"].get<std::string>();
        }
    } else {
        // 没写 hookSpecificOutput 却写了权限字段?规格只有嵌套一种形状,顶层
        // 出现这些键按用错处理,不悄悄认。
        for (const char* key : {"permissionDecision", "permissionDecisionReason", "updatedInput", "additionalContext"}) {
            if (root.contains(key)) {
                parsed.error = std::string("字段 ") + key + " 须写在 hookSpecificOutput 里";
                return parsed;
            }
        }
    }

    parsed.ok = true;
    return parsed;
}

SingleOutcome JudgeSingleRun(HookEvent event, unsigned long exit_code, bool timed_out, bool spawn_failed,
                             const HookOutput& parsed, const std::string& stderr_text) {
    SingleOutcome out;
    if (spawn_failed) {
        out.outcome = "spawn_failed";
        out.decision = "none";
        out.detail = "钩子进程起不来";
        return out;
    }
    if (timed_out) {
        out.outcome = "timeout";
        out.decision = "none";
        out.detail = "钩子超时被杀";
        return out;
    }
    if (exit_code == 2) {
        out.outcome = "blocked";
        out.decision = "deny";
        out.detail = IsBlank(stderr_text) ? "钩子以退出码 2 阻断(未给理由)" : FirstLines(stderr_text, 5);
        return out;
    }
    if (exit_code != 0) {
        out.outcome = "failure";
        out.decision = "none";
        out.detail = "钩子退出码 " + std::to_string(exit_code) +
                     (IsBlank(stderr_text) ? "" : ":" + FirstLines(stderr_text, 5));
        return out;
    }
    // exit 0:stdout 有 JSON 就按解析结果算;解析失败在这里不定罪——调用方
    // 拿 parsed.ok 自己判(JudgeSingleRun 不知道该事件该不该有 JSON)。
    if (!parsed.ok) {
        out.outcome = "schema_error";
        out.decision = "none";
        out.detail = parsed.error;
        return out;
    }
    out.outcome = "ok";
    switch (parsed.permission) {
        case HookEventResult::Permission::Allow:
            out.decision = "allow";
            break;
        case HookEventResult::Permission::Deny:
            out.decision = "deny";
            break;
        case HookEventResult::Permission::Ask:
            out.decision = "ask";
            break;
        case HookEventResult::Permission::None:
            out.decision = "none";
            break;
    }
    (void)event;
    return out;
}

// 原始字节摘要:前 max_bytes 的十六进制,拿不准编码时的兜底——不替换成
// U+FFFD,留人能对出来的原始证据。
namespace {

std::string RawByteDigest(const std::string& bytes, std::size_t max_bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    const std::size_t shown = std::min(bytes.size(), max_bytes);
    out.reserve(shown * 3);
    for (std::size_t i = 0; i < shown; ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (i > 0) {
            out += ' ';
        }
        out += kHex[byte >> 4];
        out += kHex[byte & 0x0F];
    }
    if (bytes.size() > shown) {
        out += " …";
    }
    return out;
}

}  // namespace

DecodedHookText DecodeHookStreamBytes(const std::string& bytes, const std::vector<unsigned int>& code_pages) {
    DecodedHookText out;
    if (bytes.empty()) {
        out.encoding = "utf-8";  // 空流无编码可言,按契约口径报
        return out;
    }
    if (platform::IsValidUtf8(bytes)) {
        out.text = bytes;
        out.encoding = "utf-8";
        return out;
    }
#ifdef _WIN32
    for (const unsigned int cp : code_pages) {
        if (const std::optional<std::string> converted = platform::CodePageBytesToUtf8(cp, bytes)) {
            out.text = *converted;
            out.encoding = "cp" + std::to_string(cp);
            return out;
        }
    }
#else
    (void)code_pages;  // POSIX 不做代码页次选:非 UTF-8 即摘要,不猜
#endif
    out.text = RawByteDigest(bytes, 64);
    out.encoding = "unknown";
    out.from_raw_digest = true;
    return out;
}

DecodedHookText DecodeHookStreamBytes(const std::string& bytes) {
#ifdef _WIN32
    return DecodeHookStreamBytes(bytes, platform::ChildStreamCodePageCandidates());
#else
    return DecodeHookStreamBytes(bytes, {});
#endif
}

std::vector<std::pair<std::string, std::string>> BuildLegacyToolEnv(const std::string& tool_name,
                                                                    const nlohmann::json& tool_input,
                                                                    const std::optional<std::string>& tool_result,
                                                                    bool tool_is_error) {
    std::vector<std::pair<std::string, std::string>> env;
    env.emplace_back("LUBAN_TOOL_NAME", tool_name);
    env.emplace_back("LUBAN_TOOL_INPUT", tool_input.is_null() ? nlohmann::json::object().dump() : tool_input.dump());
    if (tool_result.has_value()) {
        std::string snippet = *tool_result;
        if (snippet.size() > 8192) {
            snippet.resize(8192);  // legacy 语义照旧:结果只给前 8192 字节
        }
        env.emplace_back("LUBAN_TOOL_RESULT", std::move(snippet));
        env.emplace_back("LUBAN_TOOL_IS_ERROR", tool_is_error ? "true" : "false");
    }
    return env;
}

}  // namespace lubancode::hooks
