// 离线 OTLP/HTTP JSON encoder 的实现。合同见 otlp_json.hpp 文件头。
#include "telemetry/otlp_json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "telemetry/identity.hpp"

namespace lubancode::telemetry {
namespace {

// 标量 -> OTLP AnyValue 的 typed 形状。整型走 intValue 字符串(proto JSON
// 对 64 位整数的规矩);浮点/数组/对象不在 T0 合同里,判 null 占位。
nlohmann::json EncodeAttributeValue(const nlohmann::json& value) {
    if (value.is_string()) {
        return nlohmann::json{{"stringValue", value.get<std::string>()}};
    }
    if (value.is_boolean()) {
        return nlohmann::json{{"boolValue", value.get<bool>()}};
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return nlohmann::json{{"intValue", std::to_string(value.get<std::int64_t>())}};
    }
    return nlohmann::json{{"stringValue", ""}};
}

// {k: v} -> [{key: k, value: typed}, …](键序 = 字典序,nlohmann 默认)。
nlohmann::json EncodeAttributes(const nlohmann::json& attributes) {
    nlohmann::json out = nlohmann::json::array();
    if (!attributes.is_object()) {
        return out;
    }
    for (auto it = attributes.begin(); it != attributes.end(); ++it) {
        out.push_back(nlohmann::json{{"key", it.key()},
                                     {"value", EncodeAttributeValue(it.value())}});
    }
    return out;
}

nlohmann::json EncodeResource(const nlohmann::json& resource_attributes) {
    return nlohmann::json{{"attributes", EncodeAttributes(resource_attributes)}};
}

// scope 名与版本:instrumentation scope 固定报本投影器。
nlohmann::json EncodeScope() {
    return nlohmann::json{{"name", std::string(kTelemetrySchema)},
                          {"version", std::string(kProjectorVersion)}};
}

nlohmann::json EncodeLink(const SpanLink& link) {
    nlohmann::json out = nlohmann::json{{"traceId", link.trace_id},
                                        {"spanId", link.span_id}};
    out.emplace("attributes", EncodeAttributes(
                                  nlohmann::json{{"lubancode.link.relation", link.relation}}));
    return out;
}

nlohmann::json EncodeSpan(const TraceSpan& span) {
    nlohmann::json out;
    out.emplace("traceId", span.trace_id);
    out.emplace("spanId", span.span_id);
    if (!span.parent_span_id.empty()) {
        out.emplace("parentSpanId", span.parent_span_id);
    }
    out.emplace("name", span.name);
    out.emplace("kind", static_cast<int>(span.kind));
    out.emplace("startTimeUnixNano", std::to_string(span.start_unix_nano));
    out.emplace("endTimeUnixNano", std::to_string(span.end_unix_nano));
    out.emplace("attributes", EncodeAttributes(span.attributes));
    if (!span.links.empty()) {
        nlohmann::json links = nlohmann::json::array();
        for (const SpanLink& link : span.links) {
            links.push_back(EncodeLink(link));
        }
        out.emplace("links", std::move(links));
    }
    nlohmann::json status = nlohmann::json{{"code", static_cast<int>(span.status)}};
    if (!span.status_description.empty()) {
        status.emplace("description", span.status_description);
    }
    out.emplace("status", std::move(status));
    return out;
}

// 十进制字符串判型(时间/intValue 用)。
bool IsDecimalString(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace

nlohmann::json EncodeTracesRequest(const nlohmann::json& resource_attributes,
                                   const std::vector<TraceSpan>& spans) {
    nlohmann::json span_array = nlohmann::json::array();
    for (const TraceSpan& span : spans) {
        span_array.push_back(EncodeSpan(span));
    }
    return nlohmann::json{
        {"resourceSpans",
         nlohmann::json::array({nlohmann::json{
             {"resource", EncodeResource(resource_attributes)},
             {"scopeSpans", nlohmann::json::array({nlohmann::json{
                                {"scope", EncodeScope()},
                                {"spans", std::move(span_array)},
                            }})},
         }})}};
}

nlohmann::json EncodeMetricsRequest(const nlohmann::json& resource_attributes,
                                    const std::vector<MetricSample>& metrics) {
    nlohmann::json metric_array = nlohmann::json::array();
    for (const MetricSample& metric : metrics) {
        metric_array.push_back(nlohmann::json{
            {"name", metric.name},
            {"sum",
             nlohmann::json{
                 {"dataPoints",
                  nlohmann::json::array({nlohmann::json{
                      {"attributes", EncodeAttributes(metric.labels)},
                      {"asInt", std::to_string(metric.value)},
                  }})},
                 {"aggregationTemporality", 2},  // CUMULATIVE
                 {"isMonotonic", true},
             }},
        });
    }
    return nlohmann::json{
        {"resourceMetrics",
         nlohmann::json::array({nlohmann::json{
             {"resource", EncodeResource(resource_attributes)},
             {"scopeMetrics", nlohmann::json::array({nlohmann::json{
                                  {"scope", EncodeScope()},
                                  {"metrics", std::move(metric_array)},
                              }})},
         }})}};
}

std::string EncodeOtlpBody(const nlohmann::json& request) { return request.dump(); }

std::optional<ContractViolation> ValidateOtlpTracesJson(const nlohmann::json& request) {
    const auto reject = [](std::string code, std::string message) {
        return ContractViolation{std::move(code), std::move(message)};
    };
    if (!request.is_object() || !request.contains("resourceSpans") ||
        !request.at("resourceSpans").is_array() || request.at("resourceSpans").empty()) {
        return reject("telemetry.otlp.shape", "resourceSpans 缺失或为空");
    }
    const nlohmann::json& resource_spans = request.at("resourceSpans").at(0);
    if (!resource_spans.contains("scopeSpans") || !resource_spans.at("scopeSpans").is_array()) {
        return reject("telemetry.otlp.shape", "scopeSpans 缺失");
    }
    for (const nlohmann::json& scope_spans : resource_spans.at("scopeSpans")) {
        if (!scope_spans.contains("spans") || !scope_spans.at("spans").is_array()) {
            return reject("telemetry.otlp.shape", "spans 缺失");
        }
        for (const nlohmann::json& span : scope_spans.at("spans")) {
            if (!span.contains("traceId") || !IsValidTraceId(span.at("traceId").get<std::string>())) {
                return reject("telemetry.otlp.trace_id", "span traceId 形状不合");
            }
            if (!span.contains("spanId") || !IsValidSpanId(span.at("spanId").get<std::string>())) {
                return reject("telemetry.otlp.span_id", "span spanId 形状不合");
            }
            if (span.contains("parentSpanId")) {
                const std::string parent = span.at("parentSpanId").get<std::string>();
                if (!parent.empty() && !IsValidSpanId(parent)) {
                    return reject("telemetry.otlp.parent_span_id", "parentSpanId 形状不合");
                }
            }
            for (const char* field : {"startTimeUnixNano", "endTimeUnixNano"}) {
                if (!span.contains(field) || !span.at(field).is_string() ||
                    !IsDecimalString(span.at(field).get<std::string>())) {
                    return reject("telemetry.otlp.span_time",
                                  std::string(field) + " 须是十进制字符串纳秒");
                }
            }
            if (span.contains("attributes")) {
                for (const nlohmann::json& attribute : span.at("attributes")) {
                    // value 须是恰好一键的对象({stringValue:…} 一类);裸标量
                    // 不是合法 AnyValue 形状(nlohmann 对字符串 size() 也回 1,
                    // 故先判 is_object 再数键)。
                    const nlohmann::json& value =
                        attribute.contains("value") ? attribute.at("value") : nlohmann::json();
                    if (!attribute.contains("key") || !value.is_object() || value.size() != 1) {
                        return reject("telemetry.otlp.attribute",
                                      "attribute 须带 key 与单键 typed value");
                    }
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace lubancode::telemetry
