#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------
// 四类内容的预算口径(QQBot 配对后第二轮静默失败单 P0 刀一)。
//
// 现场病:anthropic wire 第一轮带 thinking 的响应进历史后,第二轮把历史
// 回传——thinking 块防重放的 signature 字段被旧判例当成"不可估算",纯文本
// 聊天被误拦(context.unestimated_media_or_reasoning),用户端零提示。
//
// 分类与口径(各协议核对后定案):
//   1. 可计量文本:text/tool_use/tool_result/聊天 wire 的 reasoning_content
//      回传——估算器按 wire 字节计(bytes/4),无豁免。
//   2. 签名元数据:thinking.signature(anthropic)/thoughtSignature(gemini)
//      ——防重放必需,不透明但体积有界。口径:wire 字节照进估算器的账
//      (ComputeUtf8BytesDiv4Estimate 本就数它的字节),在
//      kSignatureMetadataBudgetBytes 界内豁免拒收;超界的不透明大团不是
//      签名元数据,照旧拒收——禁止把未知占用填 0,也不删保护。
//   3. 不可见推理:redacted_thinking(anthropic)/encrypted_content
//      (responses)——不透明加密思考,体积无界,wire 字节建立不了 token
//      价。保留拒收,直到按协议补明确预算策略;也不未经核对丢块。
//   4. 真媒体:image/image_url/input_image/input_audio/audio/video/file/
//      input_file/document 块与 inlineData/fileData 容器——bytes/4 定不了
//      价。保留拒收。
// ---------------------------------------------------------------------------

// 签名元数据的界(anthropic signature 实测数百字节;给足余量到 16 KiB,
// 超过它的"signature"不是协议元数据,不许混进豁免)。
inline constexpr std::size_t kSignatureMetadataBudgetBytes = 16 * 1024;

// 一次拒收的安全诊断(不记字段值/原始 wire/正文,只记结构与计数)。
struct UnestimatedFinding {
    std::string path;        // 触发位置:索引与键名(如 $.messages[2].content[1])
    std::string kind;        // media_block | media_container | encrypted_reasoning |
                             // signature_over_limit | signature_invalid_shape
    std::string block_type;  // 命中块的 type 值(协议词表;可空)
    std::string key;         // 触发键名(或 "type")
};

struct UnestimatedDiagnosis {
    std::vector<UnestimatedFinding> findings;
    std::size_t signature_metadata_fields = 0;  // 按第 2 类口径豁免的签名字段数
    std::size_t signature_metadata_bytes = 0;   // 这些字段的 wire 字节(已进估算器)

    bool refused() const { return !findings.empty(); }
    // 稳定摘要:计数 + 首枚触发的路径/类别/键/块类型。量值一个不带。
    std::string Summary() const {
        std::string out = "findings=" + std::to_string(findings.size());
        if (signature_metadata_fields > 0) {
            out += " signature_metadata_fields=" + std::to_string(signature_metadata_fields) +
                   " signature_metadata_bytes=" + std::to_string(signature_metadata_bytes);
        }
        if (!findings.empty()) {
            const UnestimatedFinding& first = findings.front();
            out += " first=" + first.path + " kind=" + first.kind + " key=" + first.key;
            if (!first.block_type.empty()) {
                out += " block_type=" + first.block_type;
            }
        }
        return out;
    }
};

namespace unestimated_detail {

inline bool IsMediaBlockType(const std::string& name) {
    for (const char* kind : {"image", "image_url", "input_image", "input_audio",
                             "audio", "video", "file", "input_file", "document"}) {
        if (name == kind) return true;
    }
    return false;
}

inline bool IsSignatureMetadataKey(const std::string& key) {
    return key == "signature" || key == "thoughtSignature" || key == "thought_signature";
}

}  // namespace unestimated_detail

// 递归诊断:返回全部触发点 + 豁免计数。结构巡检,从不看字段值;工具
// schema/arguments 里的任意键名与 type 名是普通输入文本,不递归(旧判例
// 保留)。
inline void WalkUnestimatedInput(const nlohmann::json& value, const std::string& path,
                                 UnestimatedDiagnosis* out) {
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            WalkUnestimatedInput(value[i], path + "[" + std::to_string(i) + "]", out);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }
    const auto type_it = value.find("type");
    const std::string type_name =
        type_it != value.end() && type_it->is_string() ? type_it->get<std::string>() : std::string();
    if (unestimated_detail::IsMediaBlockType(type_name)) {
        UnestimatedFinding finding;
        finding.path = path;
        finding.kind = "media_block";
        finding.block_type = type_name;
        finding.key = "type";
        out->findings.push_back(std::move(finding));
        return;  // 媒体块的 source/data 等字段不再巡检
    }
    if (type_name == "redacted_thinking") {
        UnestimatedFinding finding;
        finding.path = path;
        finding.kind = "encrypted_reasoning";
        finding.block_type = type_name;
        finding.key = "type";
        out->findings.push_back(std::move(finding));
        return;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string& key = it.key();
        // 工具 schema/arguments 可含任意键名与 type 名——普通输入文本,
        // 不是协议媒体块(旧判例保留,一字不动)。
        const bool skip = key == "tools" || key == "parameters" || key == "input_schema" ||
                          key == "arguments" || key == "args" ||
                          (key == "response" && value.contains("name")) ||
                          (key == "input" && type_name == "tool_use");
        if (skip) {
            continue;
        }
        const nlohmann::json& item = it.value();
        if (unestimated_detail::IsSignatureMetadataKey(key)) {
            if (item.is_null() || (item.is_string() && item.get_ref<const std::string&>().empty())) {
                continue;  // 空签名:兼容端回空的照实回传,旧判例本就放行
            }
            if (item.is_string()) {
                const std::size_t bytes = item.get_ref<const std::string&>().size();
                if (bytes <= kSignatureMetadataBudgetBytes) {
                    out->signature_metadata_fields += 1;
                    out->signature_metadata_bytes += bytes;
                    continue;  // 第 2 类:字节已进估算器的账
                }
                UnestimatedFinding finding;
                finding.path = path + "." + key;
                finding.kind = "signature_over_limit";
                finding.block_type = type_name;
                finding.key = key;
                out->findings.push_back(std::move(finding));
                continue;
            }
            UnestimatedFinding finding;
            finding.path = path + "." + key;
            finding.kind = "signature_invalid_shape";
            finding.block_type = type_name;
            finding.key = key;
            out->findings.push_back(std::move(finding));
            continue;
        }
        if (key == "encrypted_content" || key == "encryptedContent") {
            if (!item.is_null() && !(item.is_string() && item.get_ref<const std::string&>().empty())) {
                UnestimatedFinding finding;
                finding.path = path + "." + key;
                finding.kind = "encrypted_reasoning";
                finding.block_type = type_name;
                finding.key = key;
                out->findings.push_back(std::move(finding));
            }
            continue;
        }
        if (key == "inlineData" || key == "inline_data" || key == "fileData" || key == "file_data") {
            if (!item.is_null() && !(item.is_string() && item.get_ref<const std::string&>().empty())) {
                UnestimatedFinding finding;
                finding.path = path + "." + key;
                finding.kind = "media_container";
                finding.block_type = type_name;
                finding.key = key;
                out->findings.push_back(std::move(finding));
            }
            continue;
        }
        WalkUnestimatedInput(item, path + "." + key, out);
    }
}

inline UnestimatedDiagnosis DiagnoseUnestimatedInput(const nlohmann::json& snapshot) {
    UnestimatedDiagnosis diagnosis;
    WalkUnestimatedInput(snapshot, "$", &diagnosis);
    return diagnosis;
}

// 便捷判定(语义 = DiagnoseUnestimatedInput(value).refused())。要触发的
// 结构细节时用 DiagnoseUnestimatedInput。
inline bool HasUnestimatedInput(const nlohmann::json& value) {
    return DiagnoseUnestimatedInput(value).refused();
}

}  // namespace lubancode::api
