#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "platform/text_encoding.hpp"
#include "remote/result_sync.hpp"
#include "runtime/secret_resolver.hpp"

namespace {

using namespace lubancode::remote;
using lubancode::runtime::SecretRedactor;
using lubancode::runtime::SecretValue;

const ResultSyncIdentity kIdentity{"20260927-120000-ABC123", "action-7", "result-7"};

NodeResultSyncPolicy Policy(const nlohmann::json& config = nlohmann::json::object(),
                            std::string version = "node-policy-1") {
    auto parsed = ParseNodeResultSyncPolicy(config, std::move(version));
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

void RegisterSecret(SecretRedactor& redactor, std::string value) {
    const SecretValue secret(std::move(value));
    redactor.Register(secret);
}

SavedToolResult Text(std::string_view text) {
    SavedToolResult result;
    result.text = text;
    return result;
}

nlohmann::json Export(const FrozenToolResult& record, const NodeResultSyncPolicy& policy,
                      const SecretRedactor& secrets) {
    auto exported = record.ForTransmission(policy, secrets);
    REQUIRE(exported.has_value());
    return *exported;
}

}  // namespace

TEST_CASE("remote result sync: node config defaults and strict validation") {
    const auto defaults = Policy();
    CHECK(defaults.mode() == ResultSyncMode::Preview);
    CHECK(defaults.preview_max_bytes() == 4096);
    CHECK(defaults.version() == "node-policy-1");
    CHECK(Policy({{"tool_result_sync", "full"}}).mode() == ResultSyncMode::Full);
    CHECK(Policy({{"preview_max_bytes", kMaxResultPreviewBytes}}).preview_max_bytes() ==
          kMaxResultPreviewBytes);

    for (const nlohmann::json& value : std::vector<nlohmann::json>{
             nullptr, true, 1, "", "Full", " full ", "raw", nlohmann::json::array()}) {
        const auto parsed = ParseNodeResultSyncPolicy({{"tool_result_sync", value}}, "p1");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ResultSyncError::InvalidMode);
    }
    for (const nlohmann::json& value : std::vector<nlohmann::json>{
             nullptr, true, 0, -1, 0.5, "4096", kMaxResultPreviewBytes + 1,
             std::numeric_limits<std::uint64_t>::max()}) {
        const auto parsed = ParseNodeResultSyncPolicy({{"preview_max_bytes", value}}, "p1");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ResultSyncError::InvalidPreviewLimit);
    }
    for (const nlohmann::json& config : std::vector<nlohmann::json>{
             nullptr, "full", nlohmann::json::array(), {{"tool_result_syncc", "full"}},
             {{"full", true}}, {{"offset", 4096}}, {{"tail", true}}}) {
        const auto parsed = ParseNodeResultSyncPolicy(config, "p1");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == ResultSyncError::InvalidConfig);
    }
    CHECK_FALSE(ParseNodeResultSyncPolicy(nlohmann::json::object(), "").has_value());
}

TEST_CASE("remote result sync: empty short and bounded UTF8 preview") {
    SecretRedactor secrets;
    const auto policy = Policy();
    for (const std::string text : {std::string(), std::string("ordinary output")}) {
        auto projected = ProjectSavedToolResult(policy, kIdentity, Text(text), secrets);
        REQUIRE(projected.has_value());
        const auto json = Export(*projected, policy, secrets);
        CHECK(json["text"] == text);
        CHECK(json["truncated"] == false);
        CHECK(json["originalBytes"] == text.size());
        CHECK(json["captureComplete"] == true);
        CHECK(json["mode"] == "preview");
    }

    // 不用源码编码决定字节数：A + 三字节汉字 + 四字节 emoji + B。
    const std::string unicode = "A\xE4\xB8\xAD\xF0\x9F\x98\x80" "B";
    const std::vector<std::pair<std::size_t, std::string>> cases{
        {1, "A"}, {3, "A"}, {4, unicode.substr(0, 4)}, {7, unicode.substr(0, 4)},
        {8, unicode.substr(0, 8)}, {9, unicode},
    };
    for (const auto& [limit, expected] : cases) {
        CAPTURE(limit);
        const auto capped = Policy({{"preview_max_bytes", limit}});
        auto projected = ProjectSavedToolResult(capped, kIdentity, Text(unicode), secrets);
        REQUIRE(projected.has_value());
        const auto json = Export(*projected, capped, secrets);
        CHECK(json["text"] == expected);
        CHECK(json["truncated"] == (limit < unicode.size()));
        CHECK(lubancode::platform::IsValidUtf8(json["text"].get<std::string>()));
        CHECK(json["text"].get<std::string>().size() <= limit);
    }
    const auto tiny = Policy({{"preview_max_bytes", 1}});
    auto projected = ProjectSavedToolResult(tiny, kIdentity, Text(unicode.substr(1, 3)), secrets);
    REQUIRE(projected.has_value());
    CHECK(Export(*projected, tiny, secrets)["text"] == "");
}

TEST_CASE("remote result sync: registered and detected secrets are removed before cutoff") {
    SecretRedactor secrets;
    const std::string known = "FAKE_CORPORATE_CREDENTIAL_0123456789";
    RegisterSecret(secrets, known);
    const auto policy = Policy();
    const std::string raw = std::string(4090, 'a') + known + " PUBLIC-SUFFIX";
    auto projected = ProjectSavedToolResult(policy, kIdentity, Text(raw), secrets);
    REQUIRE(projected.has_value());
    const auto json = Export(*projected, policy, secrets);
    const std::string preview = json["text"];
    CHECK(preview.size() == 4096);
    CHECK(preview == std::string(4090, 'a') + "[REDAC");
    CHECK(preview.find("FAKE_C") == std::string::npos);
    CHECK(json["redacted"] == true);

    const auto short_policy = Policy({{"preview_max_bytes", 16}});
    auto detected = ProjectSavedToolResult(short_policy, kIdentity,
        Text("visible: sk-FAKEabcdefghijklmnopqrst more"), secrets);
    REQUIRE(detected.has_value());
    const auto safe = Export(*detected, short_policy, secrets);
    CHECK(safe["text"] == "visible: [REDACT");
    CHECK(safe.dump().find("sk-FAKE") == std::string::npos);

    // 扫描器只盖 PEM 头行时，仍不得把后续私钥正文送到远端。
    const auto full = Policy({{"tool_result_sync", "full"}});
    auto pem = ProjectSavedToolResult(full, kIdentity,
        Text("-----BEGIN PRIVATE KEY-----\nFAKE_PRIVATE_BODY\n-----END PRIVATE KEY-----"), secrets);
    REQUIRE(pem.has_value());
    const auto pem_json = Export(*pem, full, secrets);
    CHECK(pem_json["text"] == "[REDACTED:private_key]");
    CHECK(pem_json.dump().find("FAKE_PRIVATE_BODY") == std::string::npos);

    // 先替换已知子串会把模式令牌变成 sk-[REDACTED]12345678；
    // 两路规则的重叠不能把原始 API key 的尾巴留下。
    RegisterSecret(secrets, "abcdefgh");
    auto overlap = ProjectSavedToolResult(full, kIdentity,
        Text("key sk-abcdefgh12345678 end"), secrets);
    REQUIRE(overlap.has_value());
    const auto overlap_json = Export(*overlap, full, secrets);
    CHECK(overlap_json["text"] == "[REDACTED]");
    CHECK(overlap_json.dump().find("12345678") == std::string::npos);
}

TEST_CASE("remote result sync: invalid UTF8 or binary bytes cannot pass as text") {
    SecretRedactor secrets;
    const auto policy = Policy();
    for (const std::string raw : {std::string("\xC0\xAF", 2), std::string("\xED\xA0\x80", 3),
                                 std::string("\xF4\x90\x80\x80", 4), std::string("\xF0\x9F", 2)}) {
        const auto projected = ProjectSavedToolResult(policy, kIdentity, Text(raw), secrets);
        REQUIRE_FALSE(projected.has_value());
        CHECK(projected.error() == ResultSyncError::InvalidUtf8);
    }
    for (const std::string raw : {std::string("a\0b", 3), std::string("x\x01", 2),
                                 std::string("\x1B[31m", 5), std::string("\x7f", 1)}) {
        const auto projected = ProjectSavedToolResult(policy, kIdentity, Text(raw), secrets);
        REQUIRE_FALSE(projected.has_value());
        CHECK(projected.error() == ResultSyncError::ResultNotText);
    }
    auto ordinary = ProjectSavedToolResult(policy, kIdentity, Text("a\tb\r\nc"), secrets);
    REQUIRE(ordinary.has_value());
    CHECK(Export(*ordinary, policy, secrets)["text"] == "a\tb\r\nc");

    // 已知秘密是字节值，也可能把原本合法的码点替换坏；输出再验一遍。
    RegisterSecret(secrets, std::string("\xB8", 1));
    const auto split_scalar = ProjectSavedToolResult(policy, kIdentity,
        Text("\xE4\xB8\xAD"), secrets);
    REQUIRE_FALSE(split_scalar.has_value());
    CHECK(split_scalar.error() == ResultSyncError::InvalidUtf8);
}

TEST_CASE("remote result sync: binary preview only exports metadata and full rejects it") {
    SecretRedactor secrets;
    const auto policy = Policy();
    SavedToolResult binary;
    binary.kind = ResultContentKind::Binary;
    binary.original_bytes = 8192;
    // 明确 binary 后连这块借用正文也不检查、更不序列化。
    binary.text = "DO-NOT-EXPORT-IMAGE-OR-MODEL-WEIGHTS";
    auto projected = ProjectSavedToolResult(policy, kIdentity, binary, secrets);
    REQUIRE(projected.has_value());
    const auto json = Export(*projected, policy, secrets);
    CHECK(json["status"] == "metadata_only");
    CHECK(json["contentKind"] == "binary");
    CHECK(json["originalBytes"] == 8192);
    CHECK_FALSE(json.contains("text"));
    CHECK(json.dump().find("DO-NOT-EXPORT") == std::string::npos);
    const auto full = Policy({{"tool_result_sync", "full"}});
    const auto rejected = ProjectSavedToolResult(full, kIdentity, binary, secrets);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == ResultSyncError::ResultNotText);
}

TEST_CASE("remote result sync: missing and incomplete captures are never reported as full") {
    SecretRedactor secrets;
    const auto preview = Policy();
    const auto full = Policy({{"tool_result_sync", "full"}});
    SavedToolResult result = Text("captured prefix");
    result.available = false;
    for (const auto& policy : {preview, full}) {
        const auto rejected = ProjectSavedToolResult(policy, kIdentity, result, secrets);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == ResultSyncError::ResultMissing);
    }
    result.available = true;
    result.capture_complete = false;
    auto projected = ProjectSavedToolResult(preview, kIdentity, result, secrets);
    REQUIRE(projected.has_value());
    const auto json = Export(*projected, preview, secrets);
    CHECK_FALSE(json.contains("text"));
    CHECK(json["status"] == "capture_incomplete");
    CHECK(json["captureComplete"] == false);
    CHECK(json["truncated"] == true);
    CHECK(json["originalBytes"].is_null());
    const auto rejected = ProjectSavedToolResult(full, kIdentity, result, secrets);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error() == ResultSyncError::ResultIncomplete);

    result.capture_complete = true;
    result.original_bytes = 500;
    const auto inconsistent = ProjectSavedToolResult(preview, kIdentity, result, secrets);
    REQUIRE_FALSE(inconsistent.has_value());
    CHECK(inconsistent.error() == ResultSyncError::InvalidSource);
}

TEST_CASE("remote result sync: incomplete capture cannot expose a partial credential") {
    SecretRedactor secrets;
    RegisterSecret(secrets, "abcdefghijklmnop");
    const auto preview = Policy();
    const auto full = Policy({{"tool_result_sync", "full"}});
    for (const std::string partial : {std::string("abcdefghijklmno"), std::string("sk-abc")}) {
        // 一例是已知值的前15/16，一例是还没到模式识别长度的令牌前缀。
        auto saved = Text(partial);
        saved.capture_complete = false;
        auto record = ProjectSavedToolResult(preview, kIdentity, saved, secrets);
        REQUIRE(record.has_value());
        const auto json = Export(*record, preview, secrets);
        CHECK_FALSE(json.contains("text"));
        CHECK(json["status"] == "capture_incomplete");
        CHECK(json["captureComplete"] == false);
        CHECK(json["truncated"] == true);
        CHECK(json.dump().find(partial) == std::string::npos);
        const auto rejected = ProjectSavedToolResult(full, kIdentity, saved, secrets);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error() == ResultSyncError::ResultIncomplete);
    }
}

TEST_CASE("remote result sync: full is opt-in bounded and still redacted") {
    SecretRedactor secrets;
    const std::string known = "FAKE_REGISTERED_LONG_SECRET";
    RegisterSecret(secrets, known);
    const auto preview = Policy();
    const auto full = Policy({{"tool_result_sync", "full"}}, "node-policy-2");
    const std::string raw = std::string(4096, 'x') + known + "\nend";
    auto default_result = ProjectSavedToolResult(preview, kIdentity, Text(raw), secrets);
    auto full_result = ProjectSavedToolResult(full, kIdentity, Text(raw), secrets);
    REQUIRE(default_result.has_value());
    REQUIRE(full_result.has_value());
    CHECK(Export(*default_result, preview, secrets)["text"] == std::string(4096, 'x'));
    const auto all = Export(*full_result, full, secrets);
    CHECK(all["text"] == std::string(4096, 'x') + "[REDACTED]\nend");
    CHECK(all["mode"] == "full");
    CHECK(all["truncated"] == false);
    CHECK(all.dump().find(known) == std::string::npos);

    const std::string at_cap(kMaxFullResultBytes, 'x');
    CHECK(ProjectSavedToolResult(full, kIdentity, Text(at_cap), secrets).has_value());
    const std::string over_cap(kMaxFullResultBytes + 1, 'x');
    const auto over = ProjectSavedToolResult(full, kIdentity, Text(over_cap), secrets);
    REQUIRE_FALSE(over.has_value());
    CHECK(over.error() == ResultSyncError::ResultTooLarge);
    auto capped = ProjectSavedToolResult(preview, kIdentity, Text(over_cap), secrets);
    REQUIRE(capped.has_value());
    CHECK(Export(*capped, preview, secrets)["truncated"] == true);

    const std::string oversized_input(kMaxResultInputBytes + 1, 'x');
    const auto refused = ProjectSavedToolResult(preview, kIdentity, Text(oversized_input), secrets);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ResultSyncError::ResultTooLarge);
}

TEST_CASE("remote result sync: retry reuses frozen prefix despite source and caller changes") {
    SecretRedactor secrets;
    const auto policy = Policy({{"preview_max_bytes", 4}});
    std::string stored = "HEAD-MIDDLE-TAIL";
    auto record = ProjectSavedToolResult(policy, kIdentity, Text(stored), secrets);
    REQUIRE(record.has_value());
    const auto first = Export(*record, policy, secrets);
    REQUIRE(first["text"] == "HEAD");
    stored = "NEXT-LOG-WINDOW";
    auto consumer_copy = Export(*record, policy, secrets);
    consumer_copy["text"] = "MALICIOUS-REPLACEMENT";
    for (int retry = 0; retry < 3; ++retry) {
        CHECK(Export(*record, policy, secrets).dump() == first.dump());
    }
    // 换策略版本也不能把旧 preview 自动补成全文或重写同一条记录。
    const auto upgraded = Policy({{"tool_result_sync", "full"}}, "node-policy-2");
    const auto changed = record->ForTransmission(upgraded, secrets);
    REQUIRE_FALSE(changed.has_value());
    CHECK(changed.error() == ResultSyncError::PolicyChanged);
    // 即使调用方错用旧版本号，当前更小预算也不能被绕过。
    const auto tightened = Policy({{"preview_max_bytes", 2}});
    const auto blocked = record->ForTransmission(tightened, secrets);
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error() == ResultSyncError::PolicyRestricted);
}

TEST_CASE("remote result sync: stricter node policy or new secret blocks pending records") {
    SecretRedactor secrets;
    const auto full = Policy({{"tool_result_sync", "full"}});
    const auto preview = Policy();
    auto full_record = ProjectSavedToolResult(full, kIdentity, Text("safe text"), secrets);
    REQUIRE(full_record.has_value());
    const auto disabled = full_record->ForTransmission(preview, secrets);
    REQUIRE_FALSE(disabled.has_value());
    CHECK(disabled.error() == ResultSyncError::FullSyncDisabled);
    CHECK(ResultSyncErrorCode(disabled.error()) == "full_result_sync_disabled");

    auto record = ProjectSavedToolResult(preview, kIdentity, Text("LATER_KNOWN_CREDENTIAL"), secrets);
    REQUIRE(record.has_value());
    RegisterSecret(secrets, "LATER_KNOWN_CREDENTIAL");
    const auto changed = record->ForTransmission(preview, secrets);
    REQUIRE_FALSE(changed.has_value());
    CHECK(changed.error() == ResultSyncError::RedactionChanged);

    SecretRedactor initial_secrets;
    const auto short_policy = Policy({{"preview_max_bytes", 15}});
    auto prefix = ProjectSavedToolResult(short_policy, kIdentity,
        Text("abcdefghijklmnop"), initial_secrets);
    REQUIRE(prefix.has_value());
    CHECK(Export(*prefix, short_policy, initial_secrets)["text"] == "abcdefghijklmno");
    RegisterSecret(initial_secrets, "abcdefghijklmnop");
    const auto new_redaction_context = Policy({{"preview_max_bytes", 15}}, "node-policy-2");
    const auto stale = prefix->ForTransmission(new_redaction_context, initial_secrets);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error() == ResultSyncError::PolicyChanged);
}

TEST_CASE("remote result sync: DTO has a closed field set and identifiers cannot leak secrets") {
    SecretRedactor secrets;
    const auto policy = Policy();
    auto record = ProjectSavedToolResult(policy, kIdentity, Text("output"), secrets);
    REQUIRE(record.has_value());
    const auto json = Export(*record, policy, secrets);
    std::set<std::string> keys;
    for (auto it = json.begin(); it != json.end(); ++it) {
        keys.insert(it.key());
    }
    const std::set<std::string> expected_keys{"sessionId", "toolCallId", "resultId", "policyVersion",
        "mode", "contentKind", "captureComplete", "originalBytes", "status", "text", "truncated",
        "redacted"};
    CHECK(keys == expected_keys);
    for (const char* forbidden : {"input", "args", "diff", "artifacts", "env", "trace", "raw", "path"}) {
        CHECK_FALSE(json.contains(forbidden));
    }
    ResultSyncIdentity bad = kIdentity;
    bad.result_id = "../private/result.txt";
    const auto path = ProjectSavedToolResult(policy, bad, Text("output"), secrets);
    REQUIRE_FALSE(path.has_value());
    CHECK(path.error() == ResultSyncError::InvalidIdentity);
    bad.result_id = "FAKE_METADATA_CREDENTIAL";
    RegisterSecret(secrets, bad.result_id);
    const auto secret = ProjectSavedToolResult(policy, bad, Text("output"), secrets);
    REQUIRE_FALSE(secret.has_value());
    CHECK(secret.error() == ResultSyncError::SensitiveIdentity);

    bad = kIdentity;
    bad.tool_call_id = "sk-FAKEabcdefghijklmnopqrst";
    const auto detected = ProjectSavedToolResult(policy, bad, Text("output"), secrets);
    REQUIRE_FALSE(detected.has_value());
    CHECK(detected.error() == ResultSyncError::SensitiveIdentity);
}
