#pragma once

#include <filesystem>

#include "api/types.hpp"
#include "platform/paths.hpp"
#include "trajectory/v3/result_store.hpp"

namespace lubancode::runtime {

// Store native rich blocks before request projection can discard embedded text.
// BlockToJson retains artifact references and excludes transient wire base64.
inline void PreserveNativeToolPayload(const api::ToolResultBlock& result,
                                      trajectory::v3::ResultStore::PersistRequest& persist) {
    if (result.blocks.empty()) return;
    if (result.blocks.size() == 1) {
        const auto* text = std::get_if<tools::TextContent>(&result.blocks.front());
        if (text != nullptr && text->text == result.content) return;
    }

    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : result.blocks) blocks.push_back(tools::BlockToJson(block));
    const auto raw = blocks.dump();
    persist.outputs.push_back(trajectory::v3::ResultStore::ChannelOutput{
        "raw_payload", "application/json", raw, result.capture_complete, result.capture_reason,
        static_cast<std::uint64_t>(raw.size()), !result.capture_complete});
}

// session_dir:本场会话目录(result_ref.path 相对此目录)。预览给模型的
// display_path 拼成绝对路径——模型的工作目录不在会话目录下,相对路径它
// 解析不了;原文追回口 = read_file 按绝对路径分段读(T17/V3-ADD-03,#93
// 渠道附件同一纪律)。账上的 result_ref.path 仍存相对路径(可移植,消费
// 方各自拼 session_dir)。
inline trajectory::v3::PreviewRequest PreviewFromPersistedMaterials(
    const trajectory::v3::ResultStore::PersistRequest& material,
    const trajectory::v3::ResultStore::PersistedResult& persisted,
    std::uint64_t budget, const std::filesystem::path& session_dir) {
    trajectory::v3::PreviewRequest request;
    request.max_preview_bytes = budget;
    for (const auto& output : material.outputs) {
        trajectory::v3::PreviewChannel channel;
        channel.channel = output.channel;
        channel.text = output.data;
        channel.capture_complete = output.capture_complete;
        channel.capture_reason = output.capture_reason;
        channel.output_bytes = output.output_bytes;
        channel.output_bytes_lower_bound = output.output_bytes_lower_bound;
        for (const auto& ref : persisted.result_ref) {
            if (ref.value("kind", std::string()) == output.channel) {
                const std::string relative = ref.value("path", std::string());
                // lexically_normal:仓账相对路径存正斜杠,Windows 拼接后若不归一
                // 会得"..\artifacts/res-x"混血分隔符(2026-09-17 CI 实锤,追回
                // 断言两头都咬不上);归一成平台首选分隔符,read_file 两种都认。
                channel.display_path =
                    relative.empty()
                        ? std::string()
                        : platform::PathToUtf8(
                              (session_dir / platform::Utf8ToPath(relative)).lexically_normal());
                break;
            }
        }
        request.channels.push_back(std::move(channel));
    }
    return request;
}

}  // namespace lubancode::runtime
