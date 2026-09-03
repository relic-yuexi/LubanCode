// OTLP/HTTP exporter 的实现。合同见 exporter.hpp 文件头。
//
// 网络面只此一处碰 cpr:非流式 POST + 总 Timeout + ProgressCallback 取消
//(§26.4 关停时正在传输的请求发取消)。响应体很小(OTLP export 响应只有
// partialSuccess 一档信息),整体收下再判,不走流式。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // cpr -> curl -> windows.h 的 min/max 宏与 std::min 撞车
#endif
#endif
#include "telemetry/exporter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <utility>

#include <cpr/cpr.h>

#include "platform/atomic_write.hpp"  // 统一原子写(审计 P1)
#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"

namespace lubancode::telemetry {
namespace {

// ---------------------------------------------------------------------------
// URL 窄解析:只拆 scheme://host[:port]/path?query#fragment 这一层。
// ---------------------------------------------------------------------------

struct EndpointParts {
    std::string scheme;
    std::string host_port;  // host[:port] 或 [v6]:port,无 userinfo
    std::string rest;       // path 起(含 ?/#)
};

std::optional<EndpointParts> SplitEndpoint(const std::string& endpoint) {
    const std::size_t sep = endpoint.find("://");
    if (sep == std::string::npos) {
        return std::nullopt;
    }
    EndpointParts out;
    out.scheme = endpoint.substr(0, sep);
    for (char& ch : out.scheme) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    std::string authority = endpoint.substr(sep + 3);
    const std::size_t path = authority.find_first_of("/?#");
    if (path != std::string::npos) {
        out.rest = authority.substr(path);
        authority = authority.substr(0, path);
    }
    // userinfo(§19.4 禁;状态面也要剥掉)。
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    out.host_port = authority;
    return out;
}

std::string EndpointHost(const std::string& endpoint) {
    const auto parts = SplitEndpoint(endpoint);
    if (!parts.has_value() || parts->host_port.empty()) {
        return {};
    }
    std::string host = parts->host_port;
    // [v6]:port
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        return close == std::string::npos ? host : host.substr(1, close - 1);
    }
    const std::size_t colon = host.rfind(':');
    if (colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    for (char& ch : host) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return host;
}

// 十进制点分四段且首段 127 = 回环(127.0.0.0/8 整段都是)。
bool IsLoopbackIpv4(const std::string& host) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : host) {
        if (ch == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    if (parts.size() != 4) {
        return false;
    }
    for (const std::string& part : parts) {
        if (part.empty() || part.size() > 3) {
            return false;
        }
        for (char ch : part) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                return false;
            }
        }
    }
    return parts.front() == "127";
}

// RFC 1123 IMF-fixdate:Wed, 21 Oct 2015 07:28:00 GMT。时区只认 GMT/UTC
//(HTTP-date 规定如此,别的写法按解析失败走)。
std::optional<std::int64_t> ParseHttpDateMs(const std::string& value) {
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    char dow[8] = {0}, mon[8] = {0}, tz[8] = {0};
    if (std::sscanf(value.c_str(), "%7[a-zA-Z], %d %7[a-zA-Z] %d %d:%d:%d %7s",
                    dow, &day, mon, &year, &hour, &minute, &second, tz) != 8) {
        return std::nullopt;
    }
    if (day < 1 || day > 31 || year < 1970 || hour > 23 || minute > 59 || second > 60) {
        return std::nullopt;
    }
    if (std::string(tz) != "GMT" && std::string(tz) != "UTC") {
        return std::nullopt;
    }
    std::string month = mon;
    int month_index = -1;
    for (int i = 0; i < 12; ++i) {
        std::string candidate = kMonths[i];
        if (candidate.size() == month.size()) {
            bool same = true;
            for (std::size_t j = 0; j < month.size(); ++j) {
                if (std::tolower(static_cast<unsigned char>(month[j])) !=
                    std::tolower(static_cast<unsigned char>(candidate[j]))) {
                    same = false;
                    break;
                }
            }
            if (same) {
                month_index = i + 1;
                break;
            }
        }
    }
    if (month_index < 0) {
        return std::nullopt;
    }
    // days_from_civil(Howard Hinnant 的算法):民用日 -> 天数,不依赖 mktime
    //(mktime 吃本地时区,HTTP-date 是 GMT)。
    const long long y = year - (month_index <= 2 ? 1 : 0);
    const unsigned m = static_cast<unsigned>(month_index);
    const unsigned d = static_cast<unsigned>(day);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days = era * 146097 + static_cast<long long>(doe) - 719468;
    const long long seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return seconds * 1000;
}

// partialSuccess 的点数键:proto JSON 里 int64 是十进制字符串,两种都收。
std::int64_t ReadPointCount(const nlohmann::json& node) {
    if (node.is_number_integer() || node.is_number_unsigned()) {
        return node.get<std::int64_t>();
    }
    if (node.is_string()) {
        std::int64_t value = 0;
        const auto result =
            std::from_chars(node.get_ref<const std::string&>().data(),
                            node.get_ref<const std::string&>().data() +
                                node.get_ref<const std::string&>().size(),
                            value);
        if (result.ec == std::errc{}) {
            return value;
        }
    }
    return 0;
}

std::string TruncateDetail(std::string text, std::size_t cap = 160) {
    if (text.size() > cap) {
        text.resize(cap);
        text += "…";
    }
    return text;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& content) {
    // 统一原子写(审计 P1):唯一临时名 + 平台原子替换,替掉本文件原先
    // 自备的固定 .tmp 协议。耐久档与旧实现持平(可见性原子)。
    return platform::AtomicWriteFile(path, content).has_value();
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数层
// ---------------------------------------------------------------------------

bool EndpointIsLoopback(const std::string& endpoint) {
    const std::string host = EndpointHost(endpoint);
    if (host.empty()) {
        return false;
    }
    if (host == "localhost" || host == "localhost.localdomain") {
        return true;
    }
    if (host == "::1" || host == "::ffff:127.0.0.1") {
        return true;
    }
    return IsLoopbackIpv4(host);
}

bool EndpointIsHttps(const std::string& endpoint) {
    const auto parts = SplitEndpoint(endpoint);
    return parts.has_value() && parts->scheme == "https";
}

std::string SanitizeEndpointForDisplay(const std::string& endpoint) {
    const auto parts = SplitEndpoint(endpoint);
    if (!parts.has_value()) {
        return endpoint;
    }
    std::string out = parts->scheme + "://" + parts->host_port;
    // path 保留(回环 collector 常带子路径),query/fragment 剥掉(§24.3)。
    const std::size_t query = parts->rest.find_first_of("?#");
    out += query == std::string::npos ? parts->rest : parts->rest.substr(0, query);
    return out;
}

std::optional<std::string> ValidateEndpoint(const std::string& endpoint) {
    if (endpoint.empty()) {
        return std::nullopt;  // 未配置是合法态(不出网)
    }
    const auto parts = SplitEndpoint(endpoint);
    if (!parts.has_value()) {
        return "缺 \"://\"(须形如 http://127.0.0.1:4318)";
    }
    if (parts->scheme != "http" && parts->scheme != "https") {
        return "scheme 只认 http/https";
    }
    if (parts->host_port.empty()) {
        return "host 不能为空";
    }
    if (endpoint.find('@') != std::string::npos) {
        return "不许带 userinfo(token 只走 Authorization 头,不进 URL)";
    }
    if (endpoint.find('?') != std::string::npos || endpoint.find('#') != std::string::npos) {
        return "不许带 query/fragment(§19.4 禁 token 进 URL)";
    }
    return std::nullopt;
}

std::optional<std::int64_t> ParseRetryAfterMs(const std::string& header_value,
                                              std::int64_t now_ms) {
    std::string value = header_value;
    // 容忍首尾空白。
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    if (value.empty()) {
        return std::nullopt;
    }
    if (std::all_of(value.begin(), value.end(), [](char ch) {
            return std::isdigit(static_cast<unsigned char>(ch));
        })) {
        std::int64_t seconds = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (result.ec != std::errc{}) {
            return std::nullopt;
        }
        return seconds * 1000;
    }
    const auto date_ms = ParseHttpDateMs(value);
    if (!date_ms.has_value()) {
        return std::nullopt;
    }
    return std::max<std::int64_t>(0, *date_ms - now_ms);
}

std::int64_t BackoffDelayMs(int attempt, std::optional<std::int64_t> retry_after_ms,
                            const RetryPolicy& policy, std::uint32_t jitter_unit) {
    std::int64_t delay = policy.base_ms;
    if (attempt > 1) {
        // 指数:base * 2^(attempt-1),翻倍封顶 64 倍(防溢出;反正还有帽)。
        int shift = std::min(attempt - 1, 6);
        delay = policy.base_ms << shift;
    }
    if (retry_after_ms.has_value() && *retry_after_ms > delay) {
        delay = *retry_after_ms;  // §19.2 尊重 Retry-After
    }
    delay = std::min(delay, policy.max_ms);  // …但仍受本地最大退避帽
    // jitter:±10%(jitter_unit ∈ [0,1000) 折 [-100,+100)/1000)。
    const std::int64_t jitter = static_cast<std::int64_t>(jitter_unit % 1000) - 500;
    delay += delay * jitter / 5000;
    return std::max<std::int64_t>(0, delay);
}

bool HttpStatusRetryable(int status) {
    if (status == 408 || status == 429 || status == 502 || status == 503 || status == 504) {
        return true;
    }
    return status >= 500;  // 其余 5xx(500 等)按传输临时错待遇
}

bool HttpStatusPermanent(int status) {
    return status >= 400 && status < 500 && !HttpStatusRetryable(status);
}

// ---------------------------------------------------------------------------
// OtlpHttpExporter
// ---------------------------------------------------------------------------

OtlpHttpExporter::OtlpHttpExporter(OtlpExporterOptions options)
    : options_(std::move(options)) {}

ExportAttempt OtlpHttpExporter::Post(const std::string& path, const nlohmann::json& body,
                                     const std::atomic<bool>* cancel,
                                     const std::map<std::string, std::string>& extra_headers) {
    ExportAttempt attempt;
    std::string url = options_.endpoint;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += path;

    cpr::Header headers;
    headers["Content-Type"] = "application/json";
    headers["Accept"] = "application/json";
    for (const auto& [name, value] : extra_headers) {
        headers[name] = value;
    }
    if (options_.token_source != nullptr) {
        const auto token = options_.token_source();
        if (token.has_value() && !token->empty()) {
            // 即取即用:只进这一行头,不落 detail/日志(§15.4)。
            headers["Authorization"] = "Bearer " + *token;
        }
    }

    const std::string payload = body.dump();
    attempt.body_bytes = payload.size();

    bool cancelled = false;
    cpr::ProgressCallback progress_cb([&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                                          cpr::cpr_pf_arg_t, intptr_t) -> bool {
        if (cancel != nullptr && cancel->load()) {
            cancelled = true;
            return false;
        }
        return true;
    });

    cpr::Response response = cpr::Post(
        cpr::Url{url}, cpr::Header{headers}, cpr::Body{payload},
        cpr::Timeout{static_cast<std::int32_t>(std::max<std::int64_t>(1, options_.timeout_ms))},
        progress_cb);

    if (cancelled || (cancel != nullptr && cancel->load())) {
        attempt.kind = ExportOutcomeKind::Cancelled;
        attempt.error_code = "telemetry.export.cancelled";
        return attempt;
    }
    if (response.error) {
        // 传输错(DNS/连接/超时/代理)按临时错(§19.2);原始 message 进
        // detail 排查用(不含 token——token 只在头里)。
        attempt.kind = ExportOutcomeKind::Retryable;
        attempt.http_status = 0;
        attempt.error_code = "telemetry.export.transport";
        attempt.detail = TruncateDetail(response.error.message);
        return attempt;
    }

    attempt.http_status = static_cast<int>(response.status_code);
    const int status = attempt.http_status;
    if (status >= 200 && status < 300) {
        // partialSuccess(§19.3):三个键名对应 traces/metrics/logs 三族,
        // 都收;proto JSON 里 int64 是字符串。
        const nlohmann::json parsed = nlohmann::json::parse(response.text, nullptr, false);
        std::int64_t rejected = 0;
        std::int64_t accepted = -1;
        std::string message;
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("partialSuccess") &&
            parsed.at("partialSuccess").is_object()) {
            const nlohmann::json& partial = parsed.at("partialSuccess");
            for (const char* key : {"rejectedSpans", "rejectedDataPoints", "rejectedLogRecords"}) {
                if (partial.contains(key)) {
                    rejected += ReadPointCount(partial.at(key));
                }
            }
            if (partial.contains("acceptedSpans")) {
                accepted = ReadPointCount(partial.at("acceptedSpans"));
            } else if (partial.contains("acceptedDataPoints")) {
                accepted = ReadPointCount(partial.at("acceptedDataPoints"));
            } else if (partial.contains("acceptedLogRecords")) {
                accepted = ReadPointCount(partial.at("acceptedLogRecords"));
            }
            if (partial.contains("errorMessage") && partial.at("errorMessage").is_string()) {
                message = partial.at("errorMessage").get<std::string>();
            }
        }
        if (rejected > 0 && (accepted == -1 || accepted == 0)) {
            // 一只点都没收:按可重试走(双限兜底,不盲发)。
            attempt.kind = ExportOutcomeKind::Retryable;
            attempt.error_code = "telemetry.export.partial_all_rejected";
            attempt.rejected_points = rejected;
            attempt.accepted_points = accepted;
            attempt.detail = TruncateDetail(message);
            return attempt;
        }
        attempt.kind = rejected > 0 ? ExportOutcomeKind::Partial : ExportOutcomeKind::Accepted;
        attempt.rejected_points = rejected;
        attempt.accepted_points = accepted;
        if (rejected > 0) {
            attempt.detail = TruncateDetail(message);
        }
        return attempt;
    }
    if (HttpStatusRetryable(status)) {
        attempt.kind = ExportOutcomeKind::Retryable;
        attempt.error_code = "telemetry.export.http_retryable";
    } else {
        attempt.kind = ExportOutcomeKind::Permanent;
        attempt.error_code = "telemetry.export.http_permanent";
    }
    const auto retry_after = response.header.find("Retry-After");
    if (retry_after != response.header.end()) {
        if (const auto parsed_retry = ParseRetryAfterMs(retry_after->second,
                                                        platform::WallClockNowMs())) {
            attempt.retry_after_ms = *parsed_retry;
        }
    }
    attempt.detail = TruncateDetail(response.text.empty() ? ("HTTP " + std::to_string(status))
                                                          : response.text);
    return attempt;
}

ExportAttempt OtlpHttpExporter::Export(const SpoolBatchRecord& record,
                                       const std::atomic<bool>* cancel) {
    const std::string path = record.signal == "metrics" ? "/v1/metrics" : "/v1/traces";
    // 批次识别头(§18.6 at-least-once:对端凭 batch id 去重,重复可识别;
    // 标准 OTLP Collector 不认未知头,不碍事)。id 与投影代随头走,payload
    // 本体是纯 OTLP,不掺私有字段。
    std::map<std::string, std::string> extra;
    extra["x-lubancode-batch-id"] = record.batch_id;
    extra["x-lubancode-projection-generation"] = std::to_string(record.projection_generation);
    extra["x-lubancode-telemetry-schema"] = std::to_string(record.telemetry_schema_version);
    return Post(path, record.payload, cancel, extra);
}

ExportAttempt OtlpHttpExporter::Probe(const std::atomic<bool>* cancel) {
    // 无业务数据(§24.2):一只空 resourceSpans 的 traces 请求。
    return Post("/v1/traces", nlohmann::json{{"resourceSpans", nlohmann::json::array()}}, cancel,
                {});
}

// ---------------------------------------------------------------------------
// consent
// ---------------------------------------------------------------------------

namespace {
inline constexpr std::string_view kConsentSchema = "lubancode.telemetry.consent";
inline constexpr int kConsentVersion = 1;
}  // namespace

nlohmann::json ConsentRecord::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["schema"] = kConsentSchema;
    out["version"] = kConsentVersion;
    out["endpoint"] = endpoint;
    out["data_class"] = data_class;
    out["redaction_version"] = redaction_version;
    out["granted_at_ms"] = granted_at_ms;
    return out;
}

std::optional<ConsentRecord> ConsentRecord::FromJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("schema") || !json.at("schema").is_string() ||
        json.at("schema").get<std::string>() != kConsentSchema) {
        return std::nullopt;
    }
    ConsentRecord out;
    auto read_string = [&json](const char* key, std::string& target) {
        if (json.contains(key) && json.at(key).is_string()) {
            target = json.at(key).get<std::string>();
        }
    };
    read_string("endpoint", out.endpoint);
    read_string("data_class", out.data_class);
    read_string("redaction_version", out.redaction_version);
    if (json.contains("granted_at_ms") && json.at("granted_at_ms").is_number_integer()) {
        out.granted_at_ms = json.at("granted_at_ms").get<std::int64_t>();
    }
    if (out.endpoint.empty()) {
        return std::nullopt;
    }
    return out;
}

ConsentStore::ConsentStore(std::filesystem::path path) : path_(std::move(path)) {}

std::optional<ConsentRecord> ConsentStore::Load() const {
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return ConsentRecord::FromJson(json);
}

bool ConsentStore::Save(const ConsentRecord& record) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) {
        return false;
    }
    return WriteTextFileAtomic(path_, record.ToJson().dump());
}

bool ConsentStore::Remove() {
    std::error_code ec;
    (void)std::filesystem::remove(path_, ec);
    return !ec;  // 文件不在也算成(revoke 的目的已达成)
}

bool ConsentStore::Matches(const ConsentRecord& record, const std::string& endpoint,
                           const std::string& data_class, const std::string& redaction_version) {
    return record.endpoint == endpoint && record.data_class == data_class &&
           record.redaction_version == redaction_version;
}

std::string EvaluateExportGate(const std::string& endpoint, DataClass data_class,
                               const std::optional<ConsentRecord>& consent) {
    if (endpoint.empty() || EndpointIsLoopback(endpoint)) {
        return {};  // §8.4 回环免披露问答
    }
    if (!EndpointIsHttps(endpoint)) {
        return "telemetry.endpoint_not_https";  // §19.4 公网默认 HTTPS
    }
    if (!consent.has_value() ||
        !ConsentStore::Matches(*consent, endpoint, DataClassName(data_class),
                               std::string(kRedactionPolicyVersion))) {
        return "telemetry.consent_required";  // §8.4 endpoint/数据档变了须重确认
    }
    return {};
}

nlohmann::json ExportStatusFace::ToJson() const {
    nlohmann::json out = nlohmann::json::object();
    out["configured"] = configured;
    out["active"] = active;
    out["paused"] = paused;
    out["gate_reason"] = gate_reason;
    out["endpoint"] = endpoint_display;
    out["endpoint_loopback"] = endpoint_loopback;
    out["consent_state"] = consent_state;
    out["last_success_at_ms"] = last_success_at_ms;
    out["last_error_at_ms"] = last_error_at_ms;
    out["last_error_code"] = last_error_code;
    out["last_error_status"] = last_error_status;
    out["exported_batches_total"] = exported_batches_total;
    out["exported_bytes_total"] = exported_bytes_total;
    out["retried_batches_total"] = retried_batches_total;
    out["abandoned_batches_total"] = abandoned_batches_total;
    out["partial_rejected_points_total"] = partial_rejected_points_total;
    out["in_flight"] = in_flight;
    return out;
}

}  // namespace lubancode::telemetry
