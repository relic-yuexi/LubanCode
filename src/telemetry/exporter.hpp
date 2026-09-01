// OTLP/HTTP exporter(端云协同可观测架构与 Telemetry 插件设计单 §19,
// 实施分期 T2"cpr/libcurl exporter、retry/partial success/ACK 语义、
// consent、SecretRef、TLS 与回环/公网分界")。
//
// 本件分两层:
//   1) 纯函数层——URL 分界判定(回环/HTTPS/展示脱敏)、Retry-After 解析、
//      指数退避+jitter、HTTP 状态分型。不碰网络,单测直接钉。
//   2) OtlpHttpExporter——一次 OTLP/HTTP JSON POST(§19.1:traces/metrics
//      走 <endpoint>/v1/<signal>)。只做单次尝试;重试/退避/双限/ACK 归
//      TelemetryService 的出口线程(§19.2),这里只回一纸分型清楚的
//      ExportAttempt。payload 用 T0 编码器已产好的 OTLP JSON,不重编码。
//
// 凭证(§15.4/§19.4):token 不进 URL query、不进日志/状态/spool;来源走
// 装配层适配的 runtime::SecretResolver(telemetry 在 engine 层,不 include
// runtime——依赖方向 §25.2,adapter 在 app 装配层)。token_source 每次
// 出口现解析(轮换 Key 无需重启);返回 nullopt = 匿名。
//
// 回环/公网分界(§8.4/§19.4):非回环 endpoint 必须 HTTPS 且有匹配的本地
// consent 记录才许发;回环 Collector 免披露问答。TLS hostname 校验不提供
// 关闭口(cpr 默认校验,本件不碰 verify 开关)。
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/spool.hpp"

namespace lubancode::telemetry {

// ---------------------------------------------------------------------------
// 纯函数层
// ---------------------------------------------------------------------------

// endpoint 的 host 是不是回环(127.0.0.0/8、localhost、[::1]、
// [::ffff:127.x])。§8.4 回环 Collector 免公网披露问答。
bool EndpointIsLoopback(const std::string& endpoint);
// scheme 是不是 https(§19.4 公网默认 HTTPS)。
bool EndpointIsHttps(const std::string& endpoint);
// 状态输出用的 endpoint 展示形(§24.3):去 query、去 userinfo,留
// scheme://host:port。解析不动返回原串。
std::string SanitizeEndpointForDisplay(const std::string& endpoint);
// endpoint 配置校验:scheme 只认 http/https、不带 userinfo/query/fragment
// (§19.4 禁 token 进 URL)、host 非空。空串(未配)由调用方另行判。
std::optional<std::string> ValidateEndpoint(const std::string& endpoint);

// Retry-After(§19.2):秒数或 HTTP-date(IMF-fixdate)。HTTP-date 折成
// "距 now_ms 的毫秒"(过去的日期 = 0)。解析不动回 nullopt。
std::optional<std::int64_t> ParseRetryAfterMs(const std::string& header_value,
                                              std::int64_t now_ms);

// 重试策略(§19.2 双限:每批最大尝试次数与最大年龄;本地退避帽)。
struct RetryPolicy {
    std::int64_t base_ms = 500;                    // 首次退避
    std::int64_t max_ms = 30 * 1000;               // 本地退避帽(Retry-After 也受它管)
    int max_attempts = 0;                          // 0 = 不限次数,只受年龄帽
    std::int64_t max_batch_age_ms = 24 * 3600 * 1000;  // 出口年龄帽(按段 created_at)
};

// 指数退避 + jitter(§19.2):有 Retry-After 尊重之,仍受本地帽;没有则
// base 翻倍(封顶 64 倍,防溢出),同样受帽。jitter_unit ∈ [0,1000) 折
// -10%~+10%,由调用方注入(测试钉确定性)。
std::int64_t BackoffDelayMs(int attempt, std::optional<std::int64_t> retry_after_ms,
                            const RetryPolicy& policy, std::uint32_t jitter_unit);

// HTTP 状态分型(§19.2):408/429/502/503/504 与其余 5xx = 可重试;
// 400/401/403/404 等其余 4xx = 永久(停该 endpoint generation);2xx 归
// Accept(由 Export() 细分 Partial)。
enum class ExportOutcomeKind {
    Accepted,    // 2xx 全收
    Partial,     // 2xx + partialSuccess 且有点被收(§19.3)
    Retryable,   // 408/429/502/503/504、其余 5xx、传输临时错
    Permanent,   // 其余 4xx:停该 endpoint generation,报 doctor
    Cancelled,   // 取消(关停/迁移),不算失败
};
bool HttpStatusRetryable(int status);
bool HttpStatusPermanent(int status);

// ---------------------------------------------------------------------------
// 一次出口尝试的收账
// ---------------------------------------------------------------------------
struct ExportAttempt {
    ExportOutcomeKind kind = ExportOutcomeKind::Retryable;
    int http_status = 0;                    // 0 = 没到 HTTP 层(传输/取消)
    std::int64_t retry_after_ms = -1;       // 服务端 Retry-After;-1 = 没给
    std::int64_t rejected_points = 0;       // partialSuccess 拒收点数
    std::int64_t accepted_points = -1;      // 同上;-1 = 服务端没说
    std::uint64_t body_bytes = 0;           // 实发 body 字节(账目用)
    std::string error_code;                 // telemetry.export.* 稳定码
    std::string detail;                     // 人话;服务端消息截帽,不带值
};

// exporter 静态配置(§24.1 telemetry.exporter{})。
struct OtlpExporterOptions {
    std::string endpoint;                // base,如 http://127.0.0.1:4318;空 = 不出网
    std::string secret_ref;              // 环境变量名(§15.4 配置只存引用);空 = 匿名
    std::string compression = "none";    // T2 只认 none(构建未开 zlib,gzip 明拒)
    std::int64_t timeout_ms = 10000;     // 单次请求总帽(非流式,直 Timeout)
    RetryPolicy retry;
    // 凭证 seam:装配层适配 runtime::SecretResolver;每次出口现解析。
    // 返回的串只用于拼 Authorization 头,即取即弃,不落任何账面。
    std::function<std::optional<std::string>()> token_source;

    bool configured() const { return !endpoint.empty(); }
};

// 单次 OTLP/HTTP JSON POST。无共享可变态,可多线程用(出口线程单飞)。
// record.payload 是 T0 编码器产好的 ExportTraceServiceRequest /
// ExportMetricsServiceRequest JSON;record.signal 决定 /v1/<signal>。
// cancel 非空且置位:ProgressCallback 掐流,回 Cancelled(§26.4 正在传输
// 的请求发取消)。
class OtlpHttpExporter {
public:
    explicit OtlpHttpExporter(OtlpExporterOptions options);

    ExportAttempt Export(const SpoolBatchRecord& record, const std::atomic<bool>* cancel);

    // §24.2 "/doctor telemetry --probe":对明配 endpoint 发一只无业务数据
    // 的探针(空 resourceSpans 的 traces 请求),不碰 spool、不记账。
    ExportAttempt Probe(const std::atomic<bool>* cancel);

    const OtlpExporterOptions& options() const { return options_; }

private:
    ExportAttempt Post(const std::string& path, const nlohmann::json& body,
                       const std::atomic<bool>* cancel,
                       const std::map<std::string, std::string>& extra_headers);

    OtlpExporterOptions options_;
};

// ---------------------------------------------------------------------------
// consent(§8.4 公网确认)
// ---------------------------------------------------------------------------
// <telemetry-root>/consent.json 一枚:最新授权。endpoint、数据档、脱敏
// 版本任一变了,记录即失效,须重新授权(§8.4)。回环 endpoint 不需要它。
struct ConsentRecord {
    std::string endpoint;             // 授权时的 base endpoint
    std::string data_class;           // 授权时的数据档名
    std::string redaction_version;    // 授权时的脱敏规则版本
    std::int64_t granted_at_ms = 0;

    nlohmann::json ToJson() const;
    static std::optional<ConsentRecord> FromJson(const nlohmann::json& json);
};

class ConsentStore {
public:
    explicit ConsentStore(std::filesystem::path path);

    std::optional<ConsentRecord> Load() const;   // 不在/坏 = nullopt(不猜)
    bool Save(const ConsentRecord& record);      // 原子写
    bool Remove();                               // revoke(文件不在也算成)

    // 记录与现行配置是否匹配(§8.4 变更须重确认)。
    static bool Matches(const ConsentRecord& record, const std::string& endpoint,
                        const std::string& data_class, const std::string& redaction_version);

private:
    std::filesystem::path path_;
};

// 出口门的一次裁决(Start 时与 consent 变更时各判一次)。
// reason 空 = 放行。稳定码:telemetry.endpoint_not_https /
// telemetry.consent_required。
std::string EvaluateExportGate(const std::string& endpoint, DataClass data_class,
                               const std::optional<ConsentRecord>& consent);

// 状态面的出口账(§24.3 收窄到 T2:无 Connector/policy 节)。
struct ExportStatusFace {
    bool configured = false;          // endpoint 配了
    bool active = false;              // 出口线程在跑
    bool paused = false;              // pause 命令停出口
    std::string gate_reason;          // 非空 = 门关(endpoint_not_https/consent_required/永久错停代)
    std::string endpoint_display;     // 去 query/userinfo(§24.3)
    bool endpoint_loopback = false;
    std::string consent_state;        // granted | not_required | required
    std::int64_t last_success_at_ms = 0;
    std::int64_t last_error_at_ms = 0;
    std::string last_error_code;
    int last_error_status = 0;
    std::uint64_t exported_batches_total = 0;
    std::uint64_t exported_bytes_total = 0;
    std::uint64_t retried_batches_total = 0;   // 尝试未成的批次数
    std::uint64_t abandoned_batches_total = 0; // 双限弃置(有 tombstone,不再发)
    std::uint64_t partial_rejected_points_total = 0;  // §19.3 拒收账
    std::uint64_t in_flight = 0;               // 0/1(单飞)

    nlohmann::json ToJson() const;
};

}  // namespace lubancode::telemetry
