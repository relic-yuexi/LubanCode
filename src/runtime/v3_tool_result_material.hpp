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
    if (!tools::HasNativePayloadBeyondProjection(result.content, result.blocks)) return;

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
using trajectory::v3::PreviewFromPersistedMaterials;

}  // namespace lubancode::runtime
