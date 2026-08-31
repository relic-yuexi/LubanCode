// Telemetry 领域合同的实现(端云协同可观测架构与 Telemetry 插件设计单
// T0)。合同见 contract.hpp 文件头。
#include "telemetry/contract.hpp"

#include <algorithm>

namespace lubancode::telemetry {
namespace {

// resource attribute 键白名单(§10.1 默认 + §10.2 可选)。改表即改合同,
// kTelemetrySchemaVersion 陪同升版。
constexpr std::string_view kResourceAttributeKeys[] = {
    "service.name",
    "service.version",
    "service.instance.id",
    "deployment.environment.name",
    "os.type",
    "host.arch",
    "process.runtime.name",
    "process.runtime.version",
    "lubancode.device.instance.id",
    "lubancode.workspace.key",
    "lubancode.frontend",
    "lubancode.trajectory.schema_version",
    "lubancode.telemetry.schema_version",
    // §10.2 可选档(用户显式打开才发,键先钉进合同):
    "repository.name",
    "team.id",
    "project.id",
    "git.commit.sha",
    "model.provider",
    "model.name",
};

// span attribute 键白名单(§11.2/§11.4 + §27.3 GenAI 语义约定分层)。
constexpr std::string_view kSpanAttributeKeys[] = {
    // run/turn(§11.2)
    "lubancode.run.kind",
    "lubancode.run.start_reason",
    "lubancode.turn.trigger",
    "lubancode.turn.outcome",
    // 模型请求(§11.2/§11.3;model/provider 是否随 span 发由 cardinality
    // 预算管——它们进 span attribute 不进 metrics label 表外位)
    "gen_ai.request.model",
    "gen_ai.request.provider",
    "gen_ai.request.stop_reason",
    "gen_ai.request.attempt",
    "gen_ai.usage.input_tokens",
    "gen_ai.usage.output_tokens",
    "gen_ai.usage.cache_read_tokens",
    "gen_ai.usage.cache_creation_tokens",
    "gen_ai.usage.reasoning_tokens",
    "gen_ai.usage.coverage",
    // 工具(§11.4 全列)
    "tool.name",
    "tool.kind",
    "tool.effect_class",
    "tool.batch_id",
    "tool.sequence_in_batch",
    "tool.outcome",
    "tool.error_code",
    "tool.cancelled",
    "tool.timeout",
    "tool.input_bytes_bucket",
    "tool.output_bytes_bucket",
    // 审批/压缩/验证
    "lubancode.approval.decision",
    "lubancode.compact.trigger",
    "lubancode.compact.epoch",
    "lubancode.verification.kind",
    "lubancode.verification.passed",
    // 子代理(§11.5:终态 hash 只报在场,不报原值)
    "lubancode.subagent.child_terminal_hash_present",
    // 投影器自留(未收口明标,不冒充成功)
    "lubancode.span.terminal",
    // 失败稳定码(错误全文不进 D1)
    "error.type",
};

// metric label 键白名单(§10.3:有界枚举与受控名字才进 label)。
constexpr std::string_view kMetricLabelKeys[] = {
    "outcome",    "trigger", "provider", "model", "tool_kind",
    "effect_class", "decision", "kind",  "reason", "signal",
};

bool KeyInTable(std::string_view key, const auto& table) {
    return std::find(std::begin(table), std::end(table), key) != std::end(table);
}

// 追加一枚字符串 resource attribute(attributes 数组按固定序追加,保证
// 同输入同输出)。
void PutStr(nlohmann::json& attributes, const char* key, std::string_view value) {
    attributes.emplace(key, std::string(value));
}

}  // namespace

const char* DataClassName(DataClass value) {
    switch (value) {
        case DataClass::Off:
            return "off";
        case DataClass::Metadata:
            return "metadata";
        case DataClass::Diagnostic:
            return "diagnostic";
        case DataClass::Content:
            return "content";
    }
    return "off";
}

std::optional<DataClass> DataClassFromName(std::string_view name) {
    if (name == "off" || name == "D0") {
        return DataClass::Off;
    }
    if (name == "metadata" || name == "D1") {
        return DataClass::Metadata;
    }
    if (name == "diagnostic" || name == "D2") {
        return DataClass::Diagnostic;
    }
    if (name == "content" || name == "D3") {
        return DataClass::Content;
    }
    return std::nullopt;
}

bool DataClassWithin(DataClass value, DataClass ceiling) {
    return static_cast<int>(value) <= static_cast<int>(ceiling);
}

nlohmann::json BuildResourceAttributes(const ResourceInputs& inputs) {
    nlohmann::json attributes = nlohmann::json::object();
    PutStr(attributes, "service.name", "lubancode");
    PutStr(attributes, "service.version", inputs.service_version);
    PutStr(attributes, "service.instance.id", inputs.service_instance_id);
    // §10.1:local|ci|gateway;首版只有本地投影,固定 local。
    PutStr(attributes, "deployment.environment.name", "local");
    PutStr(attributes, "os.type", inputs.os_type);
    PutStr(attributes, "host.arch", inputs.host_arch);
    PutStr(attributes, "process.runtime.name", "native");
    PutStr(attributes, "process.runtime.version", "c++23");
    PutStr(attributes, "lubancode.device.instance.id", inputs.device_instance_id);
    PutStr(attributes, "lubancode.workspace.key", inputs.workspace_key);
    PutStr(attributes, "lubancode.frontend", inputs.frontend);
    attributes.emplace("lubancode.trajectory.schema_version",
                       inputs.trajectory_schema_version);
    attributes.emplace("lubancode.telemetry.schema_version", kTelemetrySchemaVersion);
    return attributes;
}

bool IsAllowedResourceAttributeKey(std::string_view key) {
    return KeyInTable(key, kResourceAttributeKeys);
}

bool IsAllowedSpanAttributeKey(std::string_view key) {
    return KeyInTable(key, kSpanAttributeKeys);
}

bool IsAllowedMetricLabelKey(std::string_view key) {
    return KeyInTable(key, kMetricLabelKeys);
}

nlohmann::json TraceSpan::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out.emplace("trace_id", trace_id);
    out.emplace("span_id", span_id);
    if (!parent_span_id.empty()) {
        out.emplace("parent_span_id", parent_span_id);
    }
    out.emplace("name", name);
    out.emplace("kind", static_cast<int>(kind));
    out.emplace("status", static_cast<int>(status));
    if (!status_description.empty()) {
        out.emplace("status_description", status_description);
    }
    out.emplace("start_unix_nano", start_unix_nano);
    out.emplace("end_unix_nano", end_unix_nano);
    out.emplace("attributes", attributes);
    if (!links.empty()) {
        nlohmann::json link_array = nlohmann::json::array();
        for (const SpanLink& link : links) {
            link_array.push_back(nlohmann::json{{"trace_id", link.trace_id},
                                                {"span_id", link.span_id},
                                                {"relation", link.relation}});
        }
        out.emplace("links", std::move(link_array));
    }
    out.emplace("source_event_id", source_event_id);
    if (!source_terminal_event_id.empty()) {
        out.emplace("source_terminal_event_id", source_terminal_event_id);
    }
    return out;
}

nlohmann::json MetricSample::ToJson() const {
    return nlohmann::json{{"name", name}, {"labels", labels}, {"value", value}};
}

namespace {

// 合同校验的公共小件:十六进制小写判定。
bool IsLowerHex(std::string_view value) {
    for (const char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::optional<ContractViolation> ValidateSpan(const TraceSpan& span) {
    const auto reject = [](std::string code, std::string message) {
        return ContractViolation{std::move(code), std::move(message)};
    };
    if (span.name.empty()) {
        return reject("telemetry.contract.span_name", "span name 为空");
    }
    if (span.trace_id.size() != 32 || !IsLowerHex(span.trace_id) ||
        span.trace_id.find_first_not_of('0') == std::string::npos) {
        return reject("telemetry.contract.trace_id", "trace_id 须 32 位非全零小写十六进制");
    }
    if (span.span_id.size() != 16 || !IsLowerHex(span.span_id) ||
        span.span_id.find_first_not_of('0') == std::string::npos) {
        return reject("telemetry.contract.span_id", "span_id 须 16 位非全零小写十六进制");
    }
    if (!span.parent_span_id.empty() &&
        (span.parent_span_id.size() != 16 || !IsLowerHex(span.parent_span_id))) {
        return reject("telemetry.contract.parent_span_id",
                      "parent_span_id 须 16 位小写十六进制或空");
    }
    if (span.end_unix_nano < span.start_unix_nano ||
        span.end_monotonic_ns < span.start_monotonic_ns) {
        return reject("telemetry.contract.span_time", "span 终点早于起点");
    }
    if (span.source_event_id.empty()) {
        return reject("telemetry.contract.span_source", "span 缺来源起事件 id");
    }
    if (!span.attributes.is_object()) {
        return reject("telemetry.contract.span_attributes", "attributes 须是对象");
    }
    for (auto it = span.attributes.begin(); it != span.attributes.end(); ++it) {
        if (!IsAllowedSpanAttributeKey(it.key())) {
            return reject("telemetry.contract.span_attribute_key",
                          "span attribute 键不在白名单: " + it.key());
        }
    }
    return std::nullopt;
}

std::optional<ContractViolation> ValidateMetric(const MetricSample& metric) {
    if (metric.name.empty()) {
        return ContractViolation{"telemetry.contract.metric_name", "metric 名为空"};
    }
    if (!metric.labels.is_object()) {
        return ContractViolation{"telemetry.contract.metric_labels", "labels 须是对象"};
    }
    for (auto it = metric.labels.begin(); it != metric.labels.end(); ++it) {
        if (!IsAllowedMetricLabelKey(it.key())) {
            return ContractViolation{"telemetry.contract.metric_label_key",
                                     "metric label 键不在有界枚举白名单: " + it.key()};
        }
    }
    return std::nullopt;
}

}  // namespace lubancode::telemetry
