// 确定性 trace/span id(端云协同可观测架构与 Telemetry 插件设计单 §9.2/
// §9.3/§9.4,实施分期 T0)。
//
//   trace_id = HMAC-SHA256(projection_key, "trace|<session_id>|<run_id>")
//              前 16 字节,十六进制小写 32 位
//   span_id  = HMAC-SHA256(projection_key, "span|<start_event_id>|<span_role>")
//              前 8 字节,十六进制小写 16 位
//
// 规矩(§9.2/§9.3):
//   - 不从 workspace_key/session_id 直接裸 hash——派生一律过本地
//     projection_key 的 HMAC,云端拿 id 反推不了本地 hash 链关系。
//   - 不截 event_hash:span id 不暴露 Journal 链。
//   - 同一 projector version 对同一 Journal 重建必得同一组 id(T0 验收线)。
//   - projection_key 是端上本地秘密;测试用固定假钥匙,真机由 T1 的
//     TelemetryService 持有,不进日志与 spool。
//
// 依赖:只吃 hooks 的自含 SHA-256(不引第三方加密库)。
#pragma once

#include <string>
#include <string_view>

namespace lubancode::telemetry {

// 16 字节 trace id(§9.2:每场 run 一枚;resume-as-new/clear 后新 trace)。
std::string DeriveTraceId(std::string_view projection_key, std::string_view session_id,
                          std::string_view run_id);

// 8 字节 span id(§9.3:event_id + span_role 过 HMAC 截 8 字节)。
std::string DeriveSpanId(std::string_view projection_key, std::string_view start_event_id,
                         std::string_view span_role);

// W3C Trace Context 形状判定(§9.5 引用的 W3C 规:32/16 位小写十六进制、
// 非全零)。
bool IsValidTraceId(std::string_view trace_id);
bool IsValidSpanId(std::string_view span_id);

// W3C traceparent 头(§9.4):00-<trace_id>-<span_id>-<flags>。flags 首位
// 是 sampled。只在策略许可的边界注入,本件只管格式。
std::string FormatTraceParent(std::string_view trace_id, std::string_view span_id,
                              bool sampled);

}  // namespace lubancode::telemetry
