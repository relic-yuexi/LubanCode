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

// 一行 JSON -> 信封,连带全部校验(recorder 提交口与 reader 验账口共用)。
// 解析、信封校验、payload 校验一处过齐;任一步失败给稳定 error_code。
std::optional<SchemaError> ParseAndValidateEventLine(const nlohmann::json& line,
                                                     EventEnvelope* out = nullptr);

}  // namespace lubancode::trajectory
