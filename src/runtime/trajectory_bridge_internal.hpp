// 轨迹桥间共用件(AR-12 机械拆分的内部头,不对外):主桥与旁路桥共用的
// 事实构造函数——purpose/system_ref/request_snapshot_ref 的事实口径只此
// 一处,两只桥不许各写各的(原 trajectory_session.cpp 匿名 namespace 提升
// 为具名声明,实现仍在主桥件);journal emergency reserve 常量为轮桥存储门
// 与账本容量门共用。仅供 src/runtime 内部 include。
#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "agent/loop.hpp"              // RequestPreparedContext
#include "agent/prompt_manifest.hpp"   // RequestSnapshotMetadata
#include "api/types.hpp"
#include "api/usage_json.hpp"
#include "runtime/trajectory_turn_bridge.hpp"  // TrajectoryTurnBridge::Identity

namespace lubancode::runtime {

// §12.2/§13 journal emergency reserve 首版起始值(16 MiB)。配置面
//(trajectory.journal_emergency_reserve_bytes)尚未落:reserve 目前是
// 起始常量,/doctor trajectory 如实展示;配置档与 settings 批次合流时
// 再开键(留白注记见 todos/P0新轨迹记录 P0-6 段)。
constexpr std::uint64_t kJournalEmergencyReserveBytes = 16ULL * 1024 * 1024;

// api::Usage -> v3 usage json(§五键名;只在 provider 明报时调用,缺报
// 走 null 不补 0)。非文本/内部口径另立账,这里只翻实报五件。
nlohmann::json UsageToJson(const api::Usage& usage);

// request_snapshot_ref 的 metadata_only 底账(Token 账本单 §6.4)。ctx 的
// prefix 账缺席(has_prefix_account=false)时 toolset_hash 留空——不拿
// 假 hash 冒充,消费侧按空串识别"这份没有前缀账可对"。
agent::RequestSnapshotMetadata BuildRequestSnapshot(const api::Request& request,
                                                    const agent::RequestPreparedContext& ctx);

// 消息 content -> 规范 blocks 数组(主桥与旁路桥共用;模型中立,大正文
// 交 blob,由 recorder 的 offload 上限管)。
nlohmann::json MessageToBlocksJson(const api::Message& message);

// model.request.prepared 的 payload(Token 账本单 A1):主桥与旁路桥共用
// 一份构造——purpose/system_ref/request_snapshot_ref 的事实口径只此一处,
// 两只桥不许各写各的。
nlohmann::json BuildPreparedPayload(const api::Request& request, const agent::RequestPreparedContext& ctx,
                                    const TrajectoryTurnBridge::Identity& identity,
                                    const std::string& last_input_event_id);

// The callback owns no heap storage and only rebuilds the original returned facts.
// No model call or observer callback runs inside the writer's preparation boundary.
inline std::optional<trajectory::v3::WriteReceipt> AppendUsageObservation(
    trajectory::v3::V3Writer& writer, const std::string& request_id,
    const std::string& turn_id, const std::string& step_id, const api::Usage& usage,
    const ::lubancore::usage::v1::Observation* observation, bool reported,
    std::string_view response_id, bool incomplete) {
    struct Facts {
        const std::string& request_id;
        const std::string& turn_id;
        const std::string& step_id;
        const api::Usage& usage;
        const ::lubancore::usage::v1::Observation* observation;
        bool reported;
        std::string_view response_id;
        bool incomplete;
    } facts{request_id, turn_id, step_id, usage, observation, reported, response_id, incomplete};
    return writer.AppendPreparedEvent(+[](void* context) -> std::optional<trajectory::v3::EventDraft> {
        const auto& facts = *static_cast<const Facts*>(context);
        auto payload = api::usage_json::ObservationPayload(facts.usage, facts.observation,
            facts.reported, facts.response_id, facts.incomplete);
        if (!payload) return std::nullopt;
        trajectory::v3::EventDraft draft;
        draft.kind = trajectory::v3::EventKindV3::ModelUsageObserved;
        draft.request_id = facts.request_id;
        draft.turn_id = facts.turn_id;
        draft.step_id = facts.step_id;
        draft.payload = std::move(*payload);
        return draft;
    }, &facts, trajectory::Durability::ProcessCrash);
}

}  // namespace lubancode::runtime
