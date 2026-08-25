#include "api/types.hpp"

#include <variant>

#include "platform/json_safe.hpp"  // SanitizeJsonStrings:工具入参/结果这类 JSON 树字段的递归清洗

namespace lubancode::api {

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
            } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                b.text = platform::SanitizeExternalText(b.text);
                b.signature = platform::SanitizeExternalText(b.signature);
            }
        },
        block);
}

void SanitizeMessage(Message& message) {
    for (auto& block : message.content) {
        SanitizeContentBlock(block);
    }
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
