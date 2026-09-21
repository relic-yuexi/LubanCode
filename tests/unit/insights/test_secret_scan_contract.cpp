// FD-06 合同测试:把 ScanSecrets/RedactSecrets 的现行行为钉死在语料上
// ——算法自 insights/redaction 下沉 privacy/secret_scan 时,同一语料必须
// 逐字节同输出(验收门:迁移前后同一 secret 语料逐字节输出相同)。
//
// 期望值全部写死,不现算:
//   - ScanSecrets 钉命中枚数、kind 次序,并用 offset/length 圈回原文片段
//     比对(片段对得上,下标与长度自然对得上);
//   - RedactSecrets 钉整串输出。
//
// 语料覆盖:header 整行锚点、环境变量赋值、前缀令牌(尾长门槛/大小写/
// 词边界)、私钥块头、连接串(行内 @ 才报)、重叠取先到、无命中透传。
// 全是假串,不涉任何真凭据。
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "insights/redaction.hpp"

using namespace lubancode::insights;

namespace {

// 圈回命中片段——比手数下标稳:片段对了,offset/length 自然对。
std::string HitText(std::string_view text, const SecretHit& hit) {
    return std::string(text.substr(hit.offset, hit.length));
}

}  // namespace

TEST_CASE("合同:header 锚点整行盖,大小写不敏感") {
    const std::string text = "AUTHORIZATION: Bearer t0k\ncookie: a=b\nContext-Token: zig";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].kind == SecretKind::AuthorizationHeader);
    CHECK(hits[1].kind == SecretKind::CookieHeader);
    CHECK(hits[2].kind == SecretKind::ContextToken);
    CHECK(HitText(text, hits[0]) == "AUTHORIZATION: Bearer t0k");
    CHECK(HitText(text, hits[1]) == "cookie: a=b");
    CHECK(HitText(text, hits[2]) == "Context-Token: zig");
    CHECK(RedactSecrets(text) ==
          "[REDACTED:authorization_header]\n[REDACTED:cookie_header]\n[REDACTED:context_token]");
}

TEST_CASE("合同:环境变量赋值——尾巴门槛量整串,命中盖整行") {
    const std::string text = "API_KEY=zzz12345\nAUTH_TOKEN=qqq\nsecret=\npassword=x";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].kind == SecretKind::EnvAssignment);
    CHECK(hits[1].kind == SecretKind::EnvAssignment);
    CHECK(hits[2].kind == SecretKind::EnvAssignment);
    CHECK(HitText(text, hits[0]) == "API_KEY=zzz12345");
    CHECK(HitText(text, hits[1]) == "AUTH_TOKEN=qqq");
    CHECK(HitText(text, hits[2]) == "secret=");
    // 现行行为照钉:尾巴门槛量的是锚点后的整串长度,不是本行——"secret="
    // 行内没尾巴,但串里后面还有字,照样报(命中圈到行尾,长度即锚点本身);
    // "password=" 后只剩一个字符,过不了门槛,不报。
    CHECK(RedactSecrets(text) ==
          "[REDACTED:env_assignment]\n[REDACTED:env_assignment]\n[REDACTED:env_assignment]\n"
          "password=x");
}

TEST_CASE("合同:前缀令牌——尾长门槛、大小写、词边界") {
    const std::string text = "key sk-abc123def456ghi789 done";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].kind == SecretKind::ApiKey);
    CHECK(HitText(text, hits[0]) == "sk-abc123def456ghi789");
    // 尾巴不足八个词字符:不报。
    CHECK(ScanSecrets("see sk-abc here").empty());
    // 前缀匹配大小写敏感:SK- 不认。
    CHECK(ScanSecrets("SK-12345678abc").empty());
    // 词边界:逗号断尾,命中只圈到边界。
    const std::string bounded = "x sk-abc12345678,done";
    const auto bounded_hits = ScanSecrets(bounded);
    REQUIRE(bounded_hits.size() == 1);
    CHECK(HitText(bounded, bounded_hits[0]) == "sk-abc12345678");
    // AKIA 档尾门更高(12)。
    const std::string aws = "aws AKIA1234567890ABCD end";
    const auto aws_hits = ScanSecrets(aws);
    REQUIRE(aws_hits.size() == 1);
    CHECK(aws_hits[0].kind == SecretKind::ApiKey);
    CHECK(HitText(aws, aws_hits[0]) == "AKIA1234567890ABCD");
    CHECK(ScanSecrets("AKIA123").empty());
    // ghp_/xoxb_/r8_/sk-ant- 同表;sk-ant- 与 sk- 同位重叠,只报一枚。
    CHECK(RedactSecrets("token ghp_12345678abc tail") == "token [REDACTED:api_key] tail");
    CHECK(RedactSecrets("xoxb-12345678abc") == "[REDACTED:api_key]");
    CHECK(RedactSecrets("r8_abcdefgh1234") == "[REDACTED:api_key]");
    CHECK(RedactSecrets("sk-ant-12345678ab") == "[REDACTED:api_key]");
}

TEST_CASE("合同:私钥块头") {
    const std::string text = "-----BEGIN RSA PRIVATE KEY-----";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].kind == SecretKind::PrivateKey);
    CHECK(HitText(text, hits[0]) == text);
    CHECK(RedactSecrets(text) == "[REDACTED:private_key]");
    CHECK(RedactSecrets("-----BEGIN PRIVATE KEY-----") == "[REDACTED:private_key]");
    // 块头后没字:不报。
    CHECK(ScanSecrets("-----begin").empty());
}

TEST_CASE("合同:连接串——行内见 @ 才报,盖整行") {
    CHECK(RedactSecrets("postgres://u:p@h/d") == "[REDACTED:connection_string]");
    // 没 @:裸 URL 不报。
    CHECK(ScanSecrets("postgres://h/d").empty());
    // @ 在行外:不报。
    CHECK(ScanSecrets("postgres://h/d\nuser@host").empty());
    // https 不在 scheme 表上——URL 凭据归 telemetry 的 URL 道闸管,这里不报。
    CHECK(ScanSecrets("https://u:p@example.com/x").empty());
    // 其余 scheme 同表,命中圈到行尾。
    const std::string text = "redis://u:p@h\nmysql://u:p@h/x\nftp://u:p@h";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 3);
    CHECK(hits[0].kind == SecretKind::ConnectionString);
    CHECK(hits[1].kind == SecretKind::ConnectionString);
    CHECK(hits[2].kind == SecretKind::ConnectionString);
    CHECK(HitText(text, hits[0]) == "redis://u:p@h");
    CHECK(HitText(text, hits[1]) == "mysql://u:p@h/x");
    CHECK(HitText(text, hits[2]) == "ftp://u:p@h");
    CHECK(RedactSecrets(text) ==
          "[REDACTED:connection_string]\n[REDACTED:connection_string]\n[REDACTED:connection_string]");
}

TEST_CASE("合同:重叠取先到,替换不吞并行正文") {
    // authorization 整行盖在前,sk- 前缀命中重叠被丢弃:只报一枚。
    const std::string text = "Authorization: Bearer sk-live12345678";
    const auto hits = ScanSecrets(text);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].kind == SecretKind::AuthorizationHeader);
    CHECK(HitText(text, hits[0]) == text);
    CHECK(RedactSecrets(text + "\ndone") == "[REDACTED:authorization_header]\ndone");
    // 同一行两类命中:各替各的,中间正文原样保留。
    CHECK(RedactSecrets("token=sk-FAKE12345678 and postgres://u:secret@h/d\ndone") ==
          "token=[REDACTED:api_key] and [REDACTED:connection_string]\ndone");
}

TEST_CASE("合同:空串与无命中透传") {
    CHECK(ScanSecrets("").empty());
    CHECK(RedactSecrets("") == "");
    CHECK(RedactSecrets("nothing here") == "nothing here");
    CHECK(ScanSecrets("see https://example.com/docs for details").empty());
}
