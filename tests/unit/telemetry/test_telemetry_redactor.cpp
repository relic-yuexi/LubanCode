// D1 Redactor 泄漏测试(端云协同可观测单 §15.2 顺序/§15.3 manifest/
// §29.4 脱敏矩阵,T0"Redactor D1 allowlist 与泄漏测试"):
//   - allowlist 先行:表外键整键删并计数;
//   - secret:Authorization/Cookie/API key/JWT/PEM/环境变量赋值全打码;
//   - URL 只留 scheme+host(query/userinfo 丢);
//   - 绝对路径(Windows/UNC/POSIX)整段占位;
//   - 长度帽截断记账;D1 无正文无路径;
//   - manifest 形状(§15.3)。
#include <doctest/doctest.h>

#include <string>

#include "telemetry/redactor.hpp"

using namespace lubancode::telemetry;

TEST_CASE("allowlist 先行: 表外键整键删并计数") {
    nlohmann::json attributes;
    attributes.emplace("tool.name", "read_file");              // 在表
    attributes.emplace("prompt.text", "读一下 README 并数行数");  // 表外:正文
    attributes.emplace("workspace.path", "D:/work/repo");       // 表外:路径
    const RedactionResult result = RedactAttributes(attributes, DataClass::Metadata,
                                                   AttributeDomain::Span);
    CHECK(result.attributes.size() == 1);
    CHECK(result.attributes.at("tool.name") == "read_file");
    CHECK(result.manifest.removed_fields == 2);
    CHECK(result.manifest.content_included == false);
    CHECK(result.manifest.data_class == DataClass::Metadata);
}

TEST_CASE("secret 打码: 常见凭证形状不外泄") {
    SUBCASE("Authorization 头") {
        const std::string out = SanitizeText("Authorization: Bearer sk-ant-1234567890", nullptr);
        CHECK(out.find("sk-ant-1234567890") == std::string::npos);
        CHECK(out.find("[REDACTED") != std::string::npos);
    }
    SUBCASE("Cookie 头") {
        const std::string out = SanitizeText("Cookie: session=deadbeefcafe", nullptr);
        CHECK(out.find("deadbeefcafe") == std::string::npos);
    }
    SUBCASE("API key 裸串") {
        CHECK(LooksLikeSecret("sk-proj-abcdefgh1234567890"));
        CHECK(LooksLikeSecret("ghp_16C7e42f292c6912E7710c838347Ae178B4a"));
        CHECK(LooksLikeSecret("AKIAIOSFODNN7EXAMPLE"));
        CHECK_FALSE(LooksLikeSecret("read_file"));
        CHECK_FALSE(LooksLikeSecret("succeeded"));
    }
    SUBCASE("环境变量赋值") {
        const std::string out = SanitizeText("api_key=super-secret-value", nullptr);
        CHECK(out.find("super-secret-value") == std::string::npos);
    }
    SUBCASE("PEM 私钥块头") {
        CHECK(LooksLikeSecret("-----BEGIN RSA PRIVATE KEY-----"));
    }
    SUBCASE("连接串 userinfo") {
        const std::string out =
            SanitizeText("postgres://alice:hunter2@db.internal:5432/prod", nullptr);
        CHECK(out.find("hunter2") == std::string::npos);
        CHECK(out.find("alice") == std::string::npos);
    }
}

TEST_CASE("URL sanitizer: 只留 scheme 与 host") {
    SUBCASE("query 丢") {
        const std::string out =
            SanitizeText("https://api.example.com/v1/traces?token=sekrit&x=1", nullptr);
        CHECK(out == "https://api.example.com");
    }
    SUBCASE("userinfo 丢") {
        const std::string out = SanitizeText("http://bob:pw@collector.local:4318/v1", nullptr);
        CHECK(out == "http://collector.local:4318");
    }
    SUBCASE("非 URL 不动") {
        CHECK(SanitizeText("tool_use", nullptr) == "tool_use");
    }
}

TEST_CASE("路径假名化: D1 无路径") {
    CHECK(SanitizeText("C:\\Users\\moontidef\\repo\\src\\a.cpp", nullptr) == "[REDACTED:path]");
    CHECK(SanitizeText("D:/work/lubancode/src/main.cpp", nullptr) == "[REDACTED:path]");
    CHECK(SanitizeText("/home/alice/project/README.md", nullptr) == "[REDACTED:path]");
    CHECK(SanitizeText("\\\\server\\share\\file.txt", nullptr) == "[REDACTED:path]");
    // 相对文件名不是路径,放行(工具名/文件名是 D1 元数据)。
    CHECK(SanitizeText("README.md", nullptr) == "README.md");
    CHECK(SanitizeText("src/a.cpp", nullptr) == "src/a.cpp");
    // 句中夹的路径一样是泄漏(§28.1),也要替。
    CHECK(SanitizeText("failed: cannot open C:\\Users\\bob\\key.pem now", nullptr) ==
          "failed: cannot open [REDACTED:path] now");
    CHECK(SanitizeText("error at /home/alice/x.txt line 3", nullptr) ==
          "error at [REDACTED:path] line 3");
    // URL scheme 不是盘符路径("https://" 的 s: 不误伤)。
    CHECK(SanitizeText("https://collector.example.com:4318", nullptr) ==
          "https://collector.example.com:4318");
}

TEST_CASE("长度帽: 超长截断记账") {
    const std::string long_text(kD1TextCap + 100, 'x');
    RedactionManifest manifest;
    const std::string out = SanitizeText(long_text, &manifest);
    CHECK(out.size() <= kD1TextCap + std::string("[TRUNCATED]").size());
    CHECK(out.find("[TRUNCATED]") != std::string::npos);
    CHECK(manifest.truncated_fields == 1);
}

TEST_CASE("D1 整包: 塞满正文/密钥/路径的属性包,出口无一处原文") {
    nlohmann::json attributes;
    attributes.emplace("tool.name", "read_file");
    attributes.emplace("tool.outcome",
                       "failed: api_key=sk-live-9f8e7d6c 放在错误里 C:\\Users\\bob\\key.pem");
    attributes.emplace("error.type",
                       "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig");
    const RedactionResult result = RedactAttributes(attributes, DataClass::Metadata,
                                                   AttributeDomain::Span);
    const std::string dump = result.attributes.dump();
    CHECK(dump.find("sk-live-9f8e7d6c") == std::string::npos);
    CHECK(dump.find("moontidef") == std::string::npos);  // 上文用户名不来自这包,兜底
    CHECK(dump.find("eyJhbGciOiJIUzI1NiJ9.payload.sig") == std::string::npos);
    CHECK(dump.find("C:\\Users") == std::string::npos);
    CHECK(dump.find("read_file") != std::string::npos);
    // manifest 形状(§15.3)。
    const nlohmann::json manifest_json = result.manifest.ToJson();
    CHECK(manifest_json.at("policy_version") == "redact-v1");
    CHECK(manifest_json.at("data_class") == "metadata");
    CHECK(manifest_json.at("content_included") == false);
    CHECK(manifest_json.contains("removed_fields"));
    CHECK(manifest_json.contains("truncated_fields"));
    CHECK(manifest_json.at("path_mode") == "none");
}

TEST_CASE("resource 域: 表外 resource 键也删") {
    nlohmann::json attributes;
    attributes.emplace("service.name", "lubancode");
    attributes.emplace("host.name", "DESKTOP-XYZ");  // §10.1 默认不发
    const RedactionResult result = RedactAttributes(attributes, DataClass::Metadata,
                                                   AttributeDomain::Resource);
    CHECK(result.attributes.size() == 1);
    CHECK(result.manifest.removed_fields == 1);
}

TEST_CASE("标量透传: 布尔与整数原样过,数组对象按删计") {
    nlohmann::json attributes;
    attributes.emplace("tool.cancelled", true);
    attributes.emplace("gen_ai.usage.input_tokens", 128);
    attributes.emplace("gen_ai.usage.output_tokens", std::int64_t{64});
    attributes.emplace("tool.output_bytes_bucket", nlohmann::json::array({"a"}));  // 结构不合
    const RedactionResult result = RedactAttributes(attributes, DataClass::Metadata,
                                                   AttributeDomain::Span);
    CHECK(result.attributes.at("tool.cancelled") == true);
    CHECK(result.attributes.at("gen_ai.usage.input_tokens") == 128);
    CHECK(result.attributes.at("gen_ai.usage.output_tokens") == 64);
    CHECK(result.manifest.removed_fields == 1);  // 只有数组那枚
}
