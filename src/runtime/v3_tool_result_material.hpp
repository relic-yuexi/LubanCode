#pragma once

#include "api/types.hpp"
#include "trajectory/v3/result_store.hpp"

namespace lubancode::runtime {

// Store native rich blocks before request projection can discard embedded text.
// BlockToJson retains artifact references and excludes transient wire base64.
inline void PreserveNativeToolPayload(const api::ToolResultBlock& result,
                                      trajectory::v3::ResultStore::PersistRequest& persist) {
    if (result.blocks.empty()) return;
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : result.blocks) blocks.push_back(tools::BlockToJson(block));
    const auto raw = blocks.dump();
    persist.outputs.push_back(trajectory::v3::ResultStore::ChannelOutput{
        "raw_payload", "application/json", raw, result.capture_complete, result.capture_reason,
        static_cast<std::uint64_t>(raw.size()), !result.capture_complete});
}

inline trajectory::v3::PreviewRequest PreviewFromPersistedMaterials(
    const trajectory::v3::ResultStore::PersistRequest& material,
    const trajectory::v3::ResultStore::PersistedResult& persisted,
    std::uint64_t budget) {
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
                channel.display_path = ref.value("path", std::string());
                break;
            }
        }
        request.channels.push_back(std::move(channel));
    }
    return request;
}

}  // namespace lubancode::runtime
