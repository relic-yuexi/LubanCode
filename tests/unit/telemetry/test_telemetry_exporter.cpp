// telemetry exporter 纯函数层测试(端云协同可观测单 §19/§8.4,分期 T2):
//   - 回环/HTTPS/展示脱敏/endpoint 校验(URL 面);
//   - Retry-After 秒数与 HTTP-date;
//   - 指数退避 + jitter + 本地帽(Retry-After 也受帽);
//   - HTTP 状态分型(408/429/502/503/504/5xx 可重试;其余 4xx 永久);
//   - consent 记录的存/读/匹配/失效(§8.4 变更须重确认);
//   - 出口门:回环免披露;公网须 HTTPS;须匹配 consent。
// 网络面(真 POST)在 tests/integration/telemetry 的回环假 collector 册里。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/exporter.hpp"

using namespace lubancode::telemetry;

namespace {

std::filesystem::path TempRoot(const char* tag) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("lubancode-tel-exp-" + std::string(tag));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

}  // namespace

TEST_CASE("endpoint 分界:回环判定") {
    CHECK(EndpointIsLoopback("http://127.0.0.1:4318"));
    CHECK(EndpointIsLoopback("http://127.0.0.2:4318"));       // 127.0.0.0/8 整段
    CHECK(EndpointIsLoopback("http://localhost:4318"));
    CHECK(EndpointIsLoopback("http://LOCALHOST:4318"));
    CHECK(EndpointIsLoopback("http://[::1]:4318"));
    CHECK_FALSE(EndpointIsLoopback("http://192.168.1.4:4318"));
    CHECK_FALSE(EndpointIsLoopback("http://collector.example.com:4318"));
    CHECK_FALSE(EndpointIsLoopback("http://10.0.0.1:4318"));
    CHECK_FALSE(EndpointIsLoopback(""));  // 未配不算回环(也不算公网)
}

TEST_CASE("endpoint 分界:HTTPS 判定") {
    CHECK(EndpointIsHttps("https://collector.example.com"));
    CHECK(EndpointIsHttps("HTTPS://collector.example.com"));
    CHECK_FALSE(EndpointIsHttps("http://collector.example.com"));
    CHECK_FALSE(EndpointIsHttps("collector.example.com"));
}

TEST_CASE("endpoint 展示脱敏:去 query/userinfo,留 path(§24.3)") {
    CHECK(SanitizeEndpointForDisplay("http://127.0.0.1:4318") == "http://127.0.0.1:4318");
    CHECK(SanitizeEndpointForDisplay("https://k@collector.example.com:4318/v1?token=x#f") ==
          "https://collector.example.com:4318/v1");
    CHECK(SanitizeEndpointForDisplay("not a url") == "not a url");
}

TEST_CASE("endpoint 配置校验:scheme/userinfo/query/host(§19.4 禁 token 进 URL)") {
    CHECK_FALSE(ValidateEndpoint("http://127.0.0.1:4318").has_value());
    CHECK_FALSE(ValidateEndpoint("https://collector.example.com").has_value());
    CHECK_FALSE(ValidateEndpoint("").has_value());  // 未配是合法态
    REQUIRE(ValidateEndpoint("ftp://127.0.0.1:4318").has_value());
    CHECK(ValidateEndpoint("ftp://127.0.0.1:4318")->find("scheme") != std::string::npos);
    REQUIRE(ValidateEndpoint("http://user:pw@127.0.0.1:4318").has_value());
    CHECK(ValidateEndpoint("http://user:pw@127.0.0.1:4318")->find("userinfo") != std::string::npos);
    REQUIRE(ValidateEndpoint("http://127.0.0.1:4318/?token=secret").has_value());
    CHECK(ValidateEndpoint("http://127.0.0.1:4318/?token=secret")->find("query") !=
          std::string::npos);
    REQUIRE(ValidateEndpoint("http://").has_value());
    CHECK(ValidateEndpoint("http://")->find("host") != std::string::npos);
    REQUIRE(ValidateEndpoint("127.0.0.1:4318").has_value());
    CHECK(ValidateEndpoint("127.0.0.1:4318")->find("://") != std::string::npos);
}

TEST_CASE("Retry-After:秒数与 HTTP-date") {
    const std::int64_t now = 1759000000000LL;
    CHECK(ParseRetryAfterMs("3", now) == std::optional<std::int64_t>(3000));
    CHECK(ParseRetryAfterMs(" 7 ", now) == std::optional<std::int64_t>(7000));
    CHECK(ParseRetryAfterMs("0", now) == std::optional<std::int64_t>(0));
    // 2026-09-27 00:53:20 GMT = 1790470400s(days_from_civil 手算与 date -u 对过)
    CHECK(ParseRetryAfterMs("Sun, 27 Sep 2026 00:53:20 GMT", 1790470390000LL) ==
          std::optional<std::int64_t>(10000));
    CHECK(ParseRetryAfterMs("Sun, 27 Sep 2026 00:53:20 GMT", 1790470405000LL) ==
          std::optional<std::int64_t>(0));  // 已过期的日期 = 立即可重试
    CHECK_FALSE(ParseRetryAfterMs("soon", now).has_value());
    CHECK_FALSE(ParseRetryAfterMs("", now).has_value());
}

TEST_CASE("退避:指数 + jitter ±10% + 本地帽;Retry-After 尊重但仍受帽") {
    const RetryPolicy policy{.base_ms = 100, .max_ms = 1000, .max_attempts = 0,
                             .max_batch_age_ms = 1000};
    // 无 jitter(单位 500 = 0%):1 次 = base,2 次 = 2x,3 次 = 4x…
    CHECK(BackoffDelayMs(1, std::nullopt, policy, 500) == 100);
    CHECK(BackoffDelayMs(2, std::nullopt, policy, 500) == 200);
    CHECK(BackoffDelayMs(3, std::nullopt, policy, 500) == 400);
    CHECK(BackoffDelayMs(4, std::nullopt, policy, 500) == 800);
    CHECK(BackoffDelayMs(5, std::nullopt, policy, 500) == 1000);  // 帽
    CHECK(BackoffDelayMs(50, std::nullopt, policy, 500) == 1000); // 翻倍封顶也不破帽
    // Retry-After 大于指数:尊重之;超帽:压回帽。
    CHECK(BackoffDelayMs(1, std::optional<std::int64_t>(3000), policy, 500) == 1000);
    CHECK(BackoffDelayMs(4, std::optional<std::int64_t>(900), policy, 500) == 900);
    // jitter:0 -> -10%;999 -> +9.98% 取整 109(整数除法向下)。
    CHECK(BackoffDelayMs(1, std::nullopt, policy, 0) == 90);
    CHECK(BackoffDelayMs(1, std::nullopt, policy, 999) == 109);
    CHECK(BackoffDelayMs(1, std::optional<std::int64_t>(0), policy, 500) == 100);
}

TEST_CASE("HTTP 状态分型(§19.2)") {
    CHECK(HttpStatusRetryable(408));
    CHECK(HttpStatusRetryable(429));
    CHECK(HttpStatusRetryable(500));
    CHECK(HttpStatusRetryable(502));
    CHECK(HttpStatusRetryable(503));
    CHECK(HttpStatusRetryable(504));
    CHECK(HttpStatusPermanent(400));
    CHECK(HttpStatusPermanent(401));
    CHECK(HttpStatusPermanent(403));
    CHECK(HttpStatusPermanent(404));
    CHECK_FALSE(HttpStatusRetryable(400));
    CHECK_FALSE(HttpStatusPermanent(429));
    CHECK_FALSE(HttpStatusPermanent(500));
    CHECK_FALSE(HttpStatusPermanent(200));
}

TEST_CASE("consent:存/读/匹配/坏文件不猜(§8.4)") {
    const std::filesystem::path root = TempRoot("consent");
    ConsentStore store(root / "consent.json");
    CHECK_FALSE(store.Load().has_value());  // 还没授权

    ConsentRecord record;
    record.endpoint = "https://collector.example.com";
    record.data_class = "metadata";
    record.redaction_version = std::string(kRedactionPolicyVersion);
    record.granted_at_ms = 1759000000000LL;
    REQUIRE(store.Save(record));

    const auto loaded = store.Load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->endpoint == record.endpoint);
    CHECK(loaded->data_class == "metadata");
    CHECK(loaded->granted_at_ms == record.granted_at_ms);

    // 匹配:全等才放行;endpoint/数据档/脱敏版本任一变 = 须重确认。
    CHECK(ConsentStore::Matches(*loaded, record.endpoint, "metadata",
                                std::string(kRedactionPolicyVersion)));
    CHECK_FALSE(ConsentStore::Matches(*loaded, "https://other.example.com", "metadata",
                                      std::string(kRedactionPolicyVersion)));
    CHECK_FALSE(ConsentStore::Matches(*loaded, record.endpoint, "diagnostic",
                                      std::string(kRedactionPolicyVersion)));
    CHECK_FALSE(ConsentStore::Matches(*loaded, record.endpoint, "metadata", "redact-v2"));

    // revoke:文件删掉,再读 = 无。
    REQUIRE(store.Remove());
    CHECK_FALSE(store.Load().has_value());

    // 坏文件:不猜,当未授权。
    std::filesystem::path bad = root / "consent.json";
    {
        std::ofstream file(bad, std::ios::binary | std::ios::trunc);
        file << "{ not json";
    }
    CHECK_FALSE(store.Load().has_value());
}

TEST_CASE("出口门:回环免披露;公网须 HTTPS + 匹配 consent") {
    CHECK(EvaluateExportGate("http://127.0.0.1:4318", DataClass::Metadata,
                             std::nullopt) == "");  // 回环:无需 consent
    CHECK(EvaluateExportGate("http://localhost:4318", DataClass::Diagnostic,
                             std::nullopt) == "");
    CHECK(EvaluateExportGate("", DataClass::Metadata, std::nullopt) == "");  // 没配不出网

    // 公网 http:门关。
    CHECK(EvaluateExportGate("http://collector.example.com", DataClass::Metadata, std::nullopt) ==
          "telemetry.endpoint_not_https");

    // 公网 https 无 consent:门关。
    CHECK(EvaluateExportGate("https://collector.example.com", DataClass::Metadata,
                             std::nullopt) == "telemetry.consent_required");

    // 公网 https + 匹配 consent:放行。
    ConsentRecord record;
    record.endpoint = "https://collector.example.com";
    record.data_class = "metadata";
    record.redaction_version = std::string(kRedactionPolicyVersion);
    CHECK(EvaluateExportGate("https://collector.example.com", DataClass::Metadata, record) == "");
    // 数据档变了:须重确认。
    CHECK(EvaluateExportGate("https://collector.example.com", DataClass::Diagnostic, record) ==
          "telemetry.consent_required");
}

TEST_CASE("ExportStatusFace ToJson:出口账齐全(§24.3)") {
    ExportStatusFace face;
    face.configured = true;
    face.endpoint_display = "http://127.0.0.1:4318";
    face.exported_batches_total = 3;
    const nlohmann::json json = face.ToJson();
    CHECK(json.at("configured").get<bool>());
    CHECK(json.at("endpoint").get<std::string>() == "http://127.0.0.1:4318");
    CHECK(json.at("exported_batches_total").get<std::uint64_t>() == 3);
    CHECK(json.contains("last_error_code"));
    CHECK(json.contains("partial_rejected_points_total"));
}
