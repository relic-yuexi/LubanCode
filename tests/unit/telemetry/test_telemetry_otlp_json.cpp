// 离线 OTLP/HTTP JSON encoder 测试(端云协同可观测单 §3.2/§19.1/
// §29.1"OTLP/HTTP JSON 对照官方 proto JSON 规则",T0"产本地 OTLP JSON
// fixture,不联网"):
//   - proto JSON 映射:int64 走十进制字符串、时间纳秒字符串、枚举数值、
//     attribute value 单键 typed;
//   - golden fixture:projector 吃 trajectory 金夹具 Journal,编码结果与
//     committed fixture 逐字节相同(离线产物,不连任何端点);
//   - ValidateOtlpTracesJson 自校验。
// 重生成 fixture:LUBANCODE_TEST_REGEN_FIXTURE=1 跑本册(验收留痕用)。
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "telemetry/otlp_json.hpp"
#include "telemetry/projector.hpp"

using namespace lubancode::telemetry;

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "."
#endif

namespace {

ProjectorOptions TestOptions() {
    ProjectorOptions options;
    options.projection_key = "test-projection-key-v1";  // 测试假钥匙,非真密
    options.resource.service_version = "0.26.0-test";
    options.resource.service_instance_id = "proc-test-0001";
    options.resource.os_type = "windows";
    options.resource.host_arch = "amd64";
    options.resource.device_instance_id = "device-test-0001";
    options.resource.workspace_key = "ws-test-000000000000";
    options.resource.frontend = "terminal";
    options.resource.trajectory_schema_version = 1;
    return options;
}

std::filesystem::path GoldenJournal() {
    return std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "trajectory" / "v1" /
           "golden_main.jsonl";
}

std::filesystem::path FixtureDir() {
    return std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "telemetry" / "v1";
}

std::optional<std::string> ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void WriteFileText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << text;
    file.close();
    REQUIRE(file.good());
}

}  // namespace

TEST_CASE("proto JSON 映射: int64 字符串/时间纳秒/枚举数值/typed value") {
    // 一枚手造 span(合同校验过的形状)直接喂 encoder。
    ProjectionReport report = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(report.ok);
    REQUIRE_FALSE(report.spans.empty());
    const nlohmann::json request =
        EncodeTracesRequest(report.resource_attributes, report.spans);

    REQUIRE(request.contains("resourceSpans"));
    const nlohmann::json& resource_spans = request.at("resourceSpans");
    REQUIRE(resource_spans.is_array());
    REQUIRE(resource_spans.size() == 1);
    CHECK(resource_spans.at(0).at("resource").at("attributes").at(0).at("key") ==
          "deployment.environment.name");  // 字典序首键
    // attribute value 单键 typed。
    const nlohmann::json& first_value =
        resource_spans.at(0).at("resource").at("attributes").at(0).at("value");
    CHECK(first_value.size() == 1);
    CHECK(first_value.contains("stringValue"));

    const nlohmann::json& scope_spans =
        resource_spans.at(0).at("scopeSpans").at(0);
    CHECK(scope_spans.at("scope").at("name") == "lubancode.telemetry");
    CHECK(scope_spans.at("scope").at("version") == "telemetry-projector-v1");

    const nlohmann::json& span = scope_spans.at("spans").at(0);
    CHECK(span.at("traceId").is_string());
    CHECK(span.at("spanId").is_string());
    // 时间:十进制字符串纳秒(proto JSON 的 uint64 映射)。
    CHECK(span.at("startTimeUnixNano").is_string());
    CHECK(span.at("startTimeUnixNano").get<std::string>() == "1759000000000000000");
    CHECK(span.at("endTimeUnixNano").is_string());
    // kind 与 status code:proto 枚举数值。
    CHECK(span.at("kind") == 1);  // INTERNAL
    CHECK(span.at("status").at("code") == 1);  // OK
    // int 属性走 intValue 字符串(64 位整数不落 JSON number);扫全部 span。
    bool saw_int_value_string = false;
    for (const nlohmann::json& all_scopes : resource_spans) {
        for (const nlohmann::json& one_scope : all_scopes.at("scopeSpans")) {
            for (const nlohmann::json& any_span : one_scope.at("spans")) {
                for (const nlohmann::json& attribute : any_span.at("attributes")) {
                    if (attribute.at("value").contains("intValue")) {
                        CHECK(attribute.at("value").at("intValue").is_string());
                        saw_int_value_string = true;
                    }
                }
            }
        }
    }
    CHECK(saw_int_value_string);
    CHECK(ValidateOtlpTracesJson(request) == std::nullopt);

    // POST body 是纯 JSON 文本。
    const std::string body = EncodeOtlpBody(request);
    CHECK(body.front() == '{');
    CHECK(nlohmann::json::parse(body, nullptr, false) != nlohmann::json());
}

TEST_CASE("metrics 编码: CUMULATIVE 单调累加,asInt 字符串") {
    ProjectionReport report = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(report.ok);
    REQUIRE_FALSE(report.metrics.empty());
    const nlohmann::json request =
        EncodeMetricsRequest(report.resource_attributes, report.metrics);
    const nlohmann::json& metric =
        request.at("resourceMetrics").at(0).at("scopeMetrics").at(0).at("metrics").at(0);
    CHECK(metric.at("name").is_string());
    const nlohmann::json& sum = metric.at("sum");
    CHECK(sum.at("aggregationTemporality") == 2);
    CHECK(sum.at("isMonotonic") == true);
    CHECK(sum.at("dataPoints").at(0).at("asInt").is_string());
}

TEST_CASE("golden fixture: 金夹具 Journal 的离线 OTLP 产物逐字节稳定") {
    ProjectionReport report = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(report.ok);
    const std::string traces_body =
        EncodeOtlpBody(EncodeTracesRequest(report.resource_attributes, report.spans));
    const std::string metrics_body =
        EncodeOtlpBody(EncodeMetricsRequest(report.resource_attributes, report.metrics));

    // 自校验先行:产物得先过合同再谈 fixture。
    CHECK(ValidateOtlpTracesJson(
              nlohmann::json::parse(traces_body, nullptr, false)) == std::nullopt);

    const char* regen = std::getenv("LUBANCODE_TEST_REGEN_FIXTURE");
    if (regen != nullptr && std::string(regen) == "1") {
        WriteFileText(FixtureDir() / "golden_traces.json", traces_body + "\n");
        WriteFileText(FixtureDir() / "golden_metrics.json", metrics_body + "\n");
        return;
    }
    const std::optional<std::string> expected_traces =
        ReadFileText(FixtureDir() / "golden_traces.json");
    REQUIRE_MESSAGE(expected_traces.has_value(), "golden_traces.json 缺失;重生成跑 "
                                                 "LUBANCODE_TEST_REGEN_FIXTURE=1");
    CHECK(traces_body + "\n" == *expected_traces);
    const std::optional<std::string> expected_metrics =
        ReadFileText(FixtureDir() / "golden_metrics.json");
    REQUIRE(expected_metrics.has_value());
    CHECK(metrics_body + "\n" == *expected_metrics);

    // D1 验收线:fixture 无正文与路径(§31 T0)。
    const std::string fixture_text = *expected_traces + *expected_metrics;
    CHECK(fixture_text.find("README") == std::string::npos);
    CHECK(fixture_text.find(".cpp") == std::string::npos);
    CHECK(fixture_text.find("C:\\") == std::string::npos);
    CHECK(fixture_text.find("/home/") == std::string::npos);
    CHECK(fixture_text.find("sk-") == std::string::npos);
}

TEST_CASE("ValidateOtlpTracesJson: 坏形状给稳定码") {
    // 逐层拼(深嵌套统一字面量在 g++ 的 brace 初始化里啃不动)。
    const auto make_request = [](nlohmann::json span) {
        nlohmann::json spans = nlohmann::json::array();
        spans.push_back(std::move(span));
        nlohmann::json scope_spans = nlohmann::json{{"spans", std::move(spans)}};
        nlohmann::json scope_array = nlohmann::json::array();
        scope_array.push_back(std::move(scope_spans));
        nlohmann::json resource_spans = nlohmann::json{{"scopeSpans", std::move(scope_array)}};
        nlohmann::json resource_array = nlohmann::json::array();
        resource_array.push_back(std::move(resource_spans));
        return nlohmann::json{{"resourceSpans", std::move(resource_array)}};
    };

    SUBCASE("空 resourceSpans") {
        const nlohmann::json request = nlohmann::json{{"resourceSpans", nlohmann::json::array()}};
        CHECK(ValidateOtlpTracesJson(request)->code == "telemetry.otlp.shape");
    }
    SUBCASE("时间不是字符串") {
        nlohmann::json span;
        span["traceId"] = "0123456789abcdef0123456789abcdef";
        span["spanId"] = "0123456789abcdef";
        span["startTimeUnixNano"] = 123;  // 坏:数字不是十进制字符串
        span["endTimeUnixNano"] = "456";
        const auto violation = ValidateOtlpTracesJson(make_request(std::move(span)));
        REQUIRE(violation.has_value());
        CHECK(violation->code == "telemetry.otlp.span_time");
    }
    SUBCASE("attribute value 不带类型键") {
        nlohmann::json span;
        span["traceId"] = "0123456789abcdef0123456789abcdef";
        span["spanId"] = "0123456789abcdef";
        span["startTimeUnixNano"] = "1";
        span["endTimeUnixNano"] = "2";
        nlohmann::json attribute;
        attribute["key"] = "k";
        attribute["value"] = "裸字符串";  // 坏:没走 typed 单键
        nlohmann::json attributes = nlohmann::json::array();
        attributes.push_back(std::move(attribute));
        span["attributes"] = std::move(attributes);
        const auto violation = ValidateOtlpTracesJson(make_request(std::move(span)));
        REQUIRE(violation.has_value());
        CHECK(violation->code == "telemetry.otlp.attribute");
    }
}
