// 离线 OTLP/HTTP JSON encoder(端云协同可观测架构与 Telemetry 插件设计单
// §3.2/§19.1,实施分期 T0"产本地 OTLP JSON fixture,不联网")。
//
// 严格按 OTLP/HTTP JSON 的 proto JSON 映射发(§3.2:要么严格按 OTLP
// schema,要么明叫私有协议,不许混名):
//   - attribute value 带 类型键:stringValue/boolValue/intValue/doubleValue
//   - int64(sint64/uint64/fixed64)在 proto JSON 里是十进制字符串
//   - startTimeUnixNano/endTimeUnixNano 是十进制字符串纳秒
//   - span kind 与 status code 是 proto 枚举数值
//   - id 是小写十六进制字符串(traceId 32 位,spanId 16 位)
//
// 本件只做纯编码:吃已脱敏的合同对象,吐 JSON 与 POST body 文本。不碰
// cpr/libcurl、不认 endpoint/凭证/重试——那是 T2 exporter 的活。
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"

namespace lubancode::telemetry {

// resource attributes(已脱敏)+ spans -> ExportTraceServiceRequest。
// 属性键序按 nlohmann 默认字典序,同输入同输出(确定性验收线)。
nlohmann::json EncodeTracesRequest(const nlohmann::json& resource_attributes,
                                   const std::vector<TraceSpan>& spans);

// resource attributes + 首批计数 metric -> ExportMetricsServiceRequest
// (CUMULATIVE 单调累加,aggregationTemporality=2)。
nlohmann::json EncodeMetricsRequest(const nlohmann::json& resource_attributes,
                                    const std::vector<MetricSample>& metrics);

// POST body 文本(OTLP/HTTP JSON 的 application/json 面)。
std::string EncodeOtlpBody(const nlohmann::json& request);

// 编码后自校验:traceId/spanId 形状、时间字符串非负十进制、attribute
// value 都带类型键。nullopt = 过。(测试与 exporter 前的最后一道闸。)
std::optional<ContractViolation> ValidateOtlpTracesJson(const nlohmann::json& request);

}  // namespace lubancode::telemetry
