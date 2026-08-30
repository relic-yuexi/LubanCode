// Trajectory schema 强校验(P0 新轨迹记录单 §四/§五)。
//
// 两层:
//   1. 信封语义校验(结构解析在 event.hpp 的 FromJsonStrict):枚举组合、
//      kind 固定 plane、event_id 形状、hash 字段形状、id 三档要求。
//   2. payload 按 kind 强校验:必填缺了拒绝、未知字段拒绝、类型不合拒绝。
//
// fail-closed 是合同:未知/缺字段一律拒,不猜着读(§2.7 第 8 条、§8.4)。
// 同一 major 内加字段须同步登记这张表;删字段、改语义升 major。
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "trajectory/event.hpp"

namespace lubancode::trajectory {

// 校验失败:稳定 error_code(schema.* / 前缀固定)+ 人话。
struct SchemaError {
    std::string error_code;
    std::string message;
};

// 信封语义校验。返回 nullopt = 过。
std::optional<SchemaError> ValidateEnvelope(const EventEnvelope& envelope);

// payload 按 kind 强校验。返回 nullopt = 过。
std::optional<SchemaError> ValidatePayload(EventKind kind, const nlohmann::json& payload);

// 按信封 schema 版本叠 v1/v2 差异(Token 账本单 §6.1.1):
//   v1:拒收 model.usage.recorded(那是 v2 事件);
//   v2:model.output.completed 不许带 usage 键;model.usage.recorded 的
//       reported_by_provider 与 token 五项做条件硬约束(报了必带、没报禁现、
//       非负、reasoning ⊆ output)。返回 nullopt = 过。
std::optional<SchemaError> ValidatePayloadWithVersion(int schema_version, EventKind kind,
                                                      const nlohmann::json& payload);

// 一行 JSON -> 信封,连带全部校验(recorder 提交口与 reader 验账口共用)。
// 解析、信封校验、payload 校验一处过齐;任一步失败给稳定 error_code。
std::optional<SchemaError> ParseAndValidateEventLine(const nlohmann::json& line,
                                                     EventEnvelope* out = nullptr);

}  // namespace lubancode::trajectory
