#pragma once

#include <expected>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::api {

// Select input fields after adapter serialization/extra_body. Output limits,
// transport flags and sampling options are not input tokens. Keep JSON string
// arguments as strings; never reparse or reformat a tool's arguments.
inline std::expected<nlohmann::json, std::string> ModelInputSnapshotFromWire(const std::string& wire) {
    if (wire.empty()) return std::unexpected("tool_batch.adapter_snapshot_unavailable");
    const auto body = nlohmann::json::parse(wire, nullptr, false);
    if (!body.is_object()) return std::unexpected("tool_batch.adapter_snapshot_invalid");
    for (const char* hidden : {"previous_response_id", "conversation", "cachedContent", "cached_content"}) {
        if (body.contains(hidden) && !body.at(hidden).is_null()) {
            return std::unexpected("tool_batch.server_context_unestimated");
        }
    }
    nlohmann::json input = nlohmann::json::object();
    for (const char* field : {"system", "messages", "tools", "input", "instructions",
                              "contents", "systemInstruction", "system_instruction", "toolConfig"}) {
        if (body.contains(field)) input[field] = body[field];
    }
    if (input.empty()) return std::unexpected("tool_batch.adapter_input_fields_missing");
    return input;
}

// bytes/4 does not price media or encrypted reasoning. Refuse to treat their
// absence from a text estimate as zero. This deliberately inspects structure,
// never arbitrary user text. Unknown component policies can be added explicitly.
inline bool HasUnestimatedInput(const nlohmann::json& value) {
    if (value.is_array()) {
        for (const auto& item : value) if (HasUnestimatedInput(item)) return true;
    } else if (value.is_object()) {
        const auto type = value.find("type");
        if (type != value.end() && type->is_string()) {
            const auto name = type->get<std::string>();
            for (const char* kind : {"image", "image_url", "input_image", "input_audio",
                                     "audio", "video", "file", "input_file", "document",
                                     "redacted_thinking"}) {
                if (name == kind) return true;
            }
        }
        for (const char* key : {"inlineData", "inline_data", "fileData", "file_data",
                                "encrypted_content", "thoughtSignature", "thought_signature", "signature"}) {
            const auto it = value.find(key);
            if (it != value.end() && !it->is_null() && *it != "") return true;
        }
        for (const auto& [key, item] : value.items()) {
            // A tool schema/arguments may contain arbitrary keys and type names;
            // those are ordinary input text, not protocol media blocks.
            if (key == "tools" || key == "parameters" || key == "input_schema" || key == "arguments" ||
                key == "args" || (key == "response" && value.contains("name")) ||
                (key == "input" && type != value.end() && *type == "tool_use")) continue;
            if (HasUnestimatedInput(item)) return true;
        }
    }
    return false;
}

}  // namespace lubancode::api
