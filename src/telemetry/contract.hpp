// Telemetry 领域合同(端云协同可观测架构与 Telemetry 插件设计单 §十/§15.1,
// 实施分期 T0"冻结 trace/span/resource/data class 合同")。
//
// 本件只钉类型、枚举、固定合同值与校验;不读文件、不联网、不看时钟。
// 合同值改动即改 schema(lubancode.telemetry.schema_version 与
// kProjectorVersion 一并升版),不原地兼容。
//
// 依赖铁律(设计单 §25.2):telemetry 只读 trajectory 合同与 platform 抽象,
// 不 include app/cli/exporter/云端 SDK;trajectory 不反过来认 telemetry。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::telemetry {

// ---------------------------------------------------------------------------
// 固定合同值(§27.1:合同版本与产品版本分开)
// ---------------------------------------------------------------------------

// Telemetry 投影 schema 名与版本:批内一切派生物以此对账。
inline constexpr std::string_view kTelemetrySchema = "lubancode.telemetry";
inline constexpr int kTelemetrySchemaVersion = 1;
// 投影器版本(§14.2 cursor 的 projector_version;映射不兼容须升版另开
// generation,不原地改旧 spool payload)。
inline constexpr std::string_view kProjectorVersion = "telemetry-projector-v1";
// Redaction 规则版本(§15.3 RedactionManifest.policy_version)。
inline constexpr std::string_view kRedactionPolicyVersion = "redact-v1";
// OTLP 编码名(§3.2:第一版 OTLP/HTTP JSON,不手搓私有协议)。
inline constexpr std::string_view kOtlpEncoding = "otlp-http-json";

// ---------------------------------------------------------------------------
// 数据分级(§15.1 四档)
// ---------------------------------------------------------------------------

enum class DataClass {
    Off,         // D0:不采、不存、不发(默认关闭时的唯一档)
    Metadata,    // D1:id/枚举/耗时/大小桶/错误码/版本;开启后默认档
    Diagnostic,  // D2:脱敏后短错误、选定插件 stderr、有限 URL host
    Content,     // D3:prompt/tool/file 片段;首版不实现,合同先钉住
};
const char* DataClassName(DataClass value);
std::optional<DataClass> DataClassFromName(std::string_view name);
// 档位高低:Off < Metadata < Diagnostic < Content。远端策略只许收窄或在
// 本地许可范围内放宽,越档判非法(§21.4)。
bool DataClassWithin(DataClass value, DataClass ceiling);

// ---------------------------------------------------------------------------
// OTel 侧枚举(值即 OTLP wire 值,不另造)
// ---------------------------------------------------------------------------

// OTLP SpanKind 的 proto 枚举值。
enum class SpanKind {
    Internal = 1,
    Server = 2,
    Client = 3,
    Producer = 4,
    Consumer = 5,
};

// OTLP Status code 的 proto 枚举值。
enum class StatusCode {
    Unset = 0,
    Ok = 1,
    Error = 2,
};

// ---------------------------------------------------------------------------
// TraceSpan:投影出的最小 span 合同(§11)
// ---------------------------------------------------------------------------

// 一枚关联边(§9.3 retry、§9.2 resume、§11.5 子代理跨进程)。
struct SpanLink {
    std::string trace_id;
    std::string span_id;
    std::string relation;  // retry_of | resume_source | child_run | causation
};

struct TraceSpan {
    std::string trace_id;   // 32 位十六进制小写(identity 派生)
    std::string span_id;    // 16 位十六进制小写(identity 派生)
    std::string parent_span_id;  // 空 = 根(run span)
    std::string name;            // lubancode.agent.run / gen_ai.request / …
    SpanKind kind = SpanKind::Internal;
    StatusCode status = StatusCode::Unset;
    std::string status_description;  // 失败原因的稳定码(不是错误全文)
    std::int64_t start_unix_nano = 0;  // wall_time_ms 换算(§9.5)
    std::int64_t end_unix_nano = 0;    // 同上;未收口 = 起点(不猜)
    std::int64_t start_monotonic_ns = 0;  // 同进程耗时优先单调钟
    std::int64_t end_monotonic_ns = 0;
    nlohmann::json attributes = nlohmann::json::object();  // D1 已脱敏
    std::vector<SpanLink> links;
    std::string source_event_id;        // 起事件(§15 不变量 9:可追源)
    std::string source_terminal_event_id;  // 终事件;空 = 未收口

    nlohmann::json ToJson() const;  // 诊断/fixture 用(已脱敏形状)
};

// ---------------------------------------------------------------------------
// Metric 合同(§12.1 首批 + §10.3 cardinality 预算)
// ---------------------------------------------------------------------------

// 端上首批只有单调累加计数(§16.4:高频计数先在端上按周期聚合;直方图
// 首版不进 T0)。value 是投影窗口内的累计值。
struct MetricSample {
    std::string name;  // lubancode.turn.completed_total 一类
    nlohmann::json labels = nlohmann::json::object();  // 只许有界枚举 label
    std::uint64_t value = 0;

    nlohmann::json ToJson() const;
};

// metric label 名白名单(§10.3:高基数字段不进 metrics label,只进
// span/log attribute)。表是封闭的:投影器只发这些 label 名。
bool IsAllowedMetricLabelKey(std::string_view key);

// ---------------------------------------------------------------------------
// Resource 合同(§10.1 默认 Resource Attributes)
// ---------------------------------------------------------------------------

// 组默认 resource 的输入(装配层给;不含主机名/用户名/绝对路径)。
struct ResourceInputs {
    std::string service_version;            // 产品版本
    std::string service_instance_id;        // 进程实例一次性 id
    std::string os_type;                    // windows|linux|darwin
    std::string host_arch;                  // amd64|arm64|…
    std::string device_instance_id;         // 匿名设备实例(§9.1)
    std::string workspace_key;              // 既有假名 workspace key
    std::string frontend = "terminal";      // terminal|app_server|channel|embedded
    int trajectory_schema_version = 1;      // Journal 的 envelope schema major
};

// 组默认 resource attributes(键集封闭,§10.1 全表)。deployment.environment
// 首版固定 local。
nlohmann::json BuildResourceAttributes(const ResourceInputs& inputs);

// resource attribute 键白名单(§10.1 默认 + §10.2 可选)。出厂自带键全部
// 在表内;表外键一律不许过(redactor 兜底也用它)。
bool IsAllowedResourceAttributeKey(std::string_view key);

// ---------------------------------------------------------------------------
// span attribute 合同(§11.4 工具字段 + GenAI 语义约定 §27.3)
// ---------------------------------------------------------------------------

// 投影器允许产出的 span attribute 键(封闭表;redactor 的 allowlist 与它
// 同源)。外部标准字段 gen_ai.* 与 Luban 自有字段 lubancode.* 分开(§27.3)。
bool IsAllowedSpanAttributeKey(std::string_view key);

// ---------------------------------------------------------------------------
// 合同校验(T0 验收线:ids/时长/键集全钉死)
// ---------------------------------------------------------------------------

struct ContractViolation {
    std::string code;     // telemetry.contract.* 稳定码
    std::string message;
};

// span 合同校验:trace/span/parent id 形状、名字非空、end>=start、
// attribute 键全在封闭白名单、来源事件可追。返回 nullopt = 过。
std::optional<ContractViolation> ValidateSpan(const TraceSpan& span);

// metric 合同校验:名字非空、label 键全在有界枚举白名单。
std::optional<ContractViolation> ValidateMetric(const MetricSample& metric);

}  // namespace lubancode::telemetry
