// Telemetry 领域合同测试(端云协同可观测单 §10.1 resource/§10.3
// cardinality/§15.1 数据分级/§29.1 contract,T0"冻结合同"):
//   - DataClass 四档命名与越档判定;
//   - 默认 resource attributes 全键在白名单,表外键(主机名/用户名类)拒;
//   - span 合同校验:ids/时长/来源/attribute 键;
//   - metric label 有界枚举白名单。
#include <doctest/doctest.h>

#include <string>

#include "telemetry/contract.hpp"
#include "telemetry/identity.hpp"

using namespace lubancode::telemetry;

namespace {

TraceSpan ValidSpan() {
    TraceSpan span;
    span.trace_id = DeriveTraceId("key", "sess-1", "run-1");
    span.span_id = DeriveSpanId("key", "run-1:evt-00000001", "run");
    span.name = "lubancode.agent.run";
    span.start_unix_nano = 1000;
    span.end_unix_nano = 2000;
    span.source_event_id = "run-1:evt-00000001";
    span.attributes = nlohmann::json{{"lubancode.run.kind", "main_session"}};
    return span;
}

}  // namespace

TEST_CASE("DataClass: 四档命名与越档判定") {
    CHECK(std::string(DataClassName(DataClass::Off)) == "off");
    CHECK(std::string(DataClassName(DataClass::Metadata)) == "metadata");
    CHECK(std::string(DataClassName(DataClass::Diagnostic)) == "diagnostic");
    CHECK(std::string(DataClassName(DataClass::Content)) == "content");
    CHECK(DataClassFromName("metadata") == DataClass::Metadata);
    CHECK(DataClassFromName("D3") == DataClass::Content);
    CHECK_FALSE(DataClassFromName("secret").has_value());
    // 远端策略只能收窄或本地许可内放宽(§21.4)。
    CHECK(DataClassWithin(DataClass::Metadata, DataClass::Metadata));
    CHECK(DataClassWithin(DataClass::Metadata, DataClass::Diagnostic));
    CHECK_FALSE(DataClassWithin(DataClass::Content, DataClass::Metadata));
    CHECK_FALSE(DataClassWithin(DataClass::Diagnostic, DataClass::Off));
}

TEST_CASE("resource attributes: §10.1 全键在表,表外键拒") {
    ResourceInputs inputs;
    inputs.service_version = "0.26.0";
    inputs.service_instance_id = "proc-0001";
    inputs.os_type = "windows";
    inputs.host_arch = "amd64";
    inputs.device_instance_id = "device-0001";
    inputs.workspace_key = "ws-0001";
    inputs.frontend = "terminal";
    inputs.trajectory_schema_version = 1;
    const nlohmann::json attributes = BuildResourceAttributes(inputs);

    // 默认键全在(§10.1 逐键)。
    for (const char* key : {"service.name", "service.version", "service.instance.id",
                            "deployment.environment.name", "os.type", "host.arch",
                            "process.runtime.name", "process.runtime.version",
                            "lubancode.device.instance.id", "lubancode.workspace.key",
                            "lubancode.frontend", "lubancode.trajectory.schema_version",
                            "lubancode.telemetry.schema_version"}) {
        CHECK_MESSAGE(attributes.contains(key), key);
        CHECK_MESSAGE(IsAllowedResourceAttributeKey(key), key);
    }
    CHECK(attributes.at("service.name") == "lubancode");
    CHECK(attributes.at("deployment.environment.name") == "local");
    CHECK(attributes.at("process.runtime.name") == "native");

    // §10.2 可选档的键在表(值由装配层显式给,这里只验合同)。
    CHECK(IsAllowedResourceAttributeKey("repository.name"));
    CHECK(IsAllowedResourceAttributeKey("git.commit.sha"));

    // 默认不发的身份键不在表:主机名/用户名/HOME/绝对路径。
    CHECK_FALSE(IsAllowedResourceAttributeKey("host.name"));
    CHECK_FALSE(IsAllowedResourceAttributeKey("user.name"));
    CHECK_FALSE(IsAllowedResourceAttributeKey("os.home"));
    CHECK_FALSE(IsAllowedResourceAttributeKey("lubancode.home_path"));
}

TEST_CASE("span 合同校验: 合法 span 过,各违例给稳定码") {
    CHECK_FALSE(ValidateSpan(ValidSpan()).has_value());

    SUBCASE("全零 trace_id") {
        TraceSpan span = ValidSpan();
        span.trace_id = std::string(32, '0');
        CHECK(ValidateSpan(span)->code == "telemetry.contract.trace_id");
    }
    SUBCASE("span_id 长度不合") {
        TraceSpan span = ValidSpan();
        span.span_id = "abc";
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_id");
    }
    SUBCASE("名字为空") {
        TraceSpan span = ValidSpan();
        span.name.clear();
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_name");
    }
    SUBCASE("终点早于起点") {
        TraceSpan span = ValidSpan();
        span.end_unix_nano = span.start_unix_nano - 1;
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_time");
    }
    SUBCASE("来源事件缺失") {
        TraceSpan span = ValidSpan();
        span.source_event_id.clear();
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_source");
    }
    SUBCASE("attribute 键不在白名单") {
        TraceSpan span = ValidSpan();
        span.attributes.emplace("prompt.text", "读一下 README");
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_attribute_key");
    }
    SUBCASE("session/run id 高基数字段不许进 span attribute") {
        TraceSpan span = ValidSpan();
        span.attributes.emplace("lubancode.session.id", "20260830-031522-7K4M2P");
        CHECK(ValidateSpan(span)->code == "telemetry.contract.span_attribute_key");
    }
}

TEST_CASE("metric label: 有界枚举白名单") {
    MetricSample metric;
    metric.name = "lubancode.model.request_total";
    metric.labels = nlohmann::json{{"provider", "demo"}, {"outcome", "completed"}};
    CHECK_FALSE(ValidateMetric(metric).has_value());

    metric.labels.emplace("model", "demo-model");  // §10.3:model 默认有限白名单内
    CHECK(IsAllowedMetricLabelKey("model"));

    SUBCASE("高基数字段拒") {
        metric.labels = nlohmann::json{{"session_id", "20260830-031522"}};
        CHECK(ValidateMetric(metric)->code == "telemetry.contract.metric_label_key");
        CHECK_FALSE(IsAllowedMetricLabelKey("session_id"));
        CHECK_FALSE(IsAllowedMetricLabelKey("trace_id"));
        CHECK_FALSE(IsAllowedMetricLabelKey("file_path"));
    }
    SUBCASE("空名拒") {
        metric.name.clear();
        CHECK(ValidateMetric(metric)->code == "telemetry.contract.metric_name");
    }
}
