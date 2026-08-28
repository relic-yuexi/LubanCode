// 自进化闭环阶段 1 的核:观察的序列化/解析、指纹归一(口径)、稳定 id、
// 脱敏窄口。纯函数,零 IO。

#include <doctest/doctest.h>

#include <string>

#include "evolution/observation.hpp"

namespace {

const char* kFakeToken = "sk-FAKE1234567890abcdef";
const char* kFakeCookie = "FAKECOOKIE987654321";

// 一条观察全文(行 + details + 摘要),断言"查无密钥"用这份。
std::string FullText(const lubancode::evolution::EvolutionObservation& observation) {
    return lubancode::evolution::SerializeObservation(observation);
}

}  // namespace

TEST_CASE("观察:序列化与解析往返") {
    lubancode::evolution::EvolutionObservation observation;
    observation.id = "obs-abcdef0123456789";
    observation.source = lubancode::evolution::ObservationSource::Recording;
    observation.source_id = "20260828-120000-demo";
    observation.source_ref = "C:/users/x/.lubancode/recordings/20260828-120000-demo";
    observation.summary = "录制 demo:排查绑定;工具 3 步;已验证";
    observation.outcome = lubancode::evolution::ObservationOutcome::Success;
    observation.fingerprint = "fp-1122334455667788";
    observation.details["tools"] = std::vector<std::string>{"read_file", "run_command"};
    observation.details["tool_call_count"] = 3;
    observation.evidence.push_back({".../events.jsonl", "录制事件流"});
    observation.created_at = "2026-08-28 12:00:00";

    const std::string line = lubancode::evolution::SerializeObservation(observation);
    const auto parsed = lubancode::evolution::ParseObservation(line);
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == observation.id);
    CHECK(parsed->source == lubancode::evolution::ObservationSource::Recording);
    CHECK(parsed->source_id == observation.source_id);
    CHECK(parsed->source_ref == observation.source_ref);
    CHECK(parsed->summary == observation.summary);
    CHECK(parsed->outcome == lubancode::evolution::ObservationOutcome::Success);
    CHECK(parsed->fingerprint == observation.fingerprint);
    CHECK(parsed->details.at("tool_call_count").get<int>() == 3);
    REQUIRE(parsed->evidence.size() == 1);
    CHECK(parsed->evidence[0].ref == ".../events.jsonl");
    CHECK(parsed->created_at == observation.created_at);

    // 坏行/半截行/缺关键字段:一律 nullopt,调用方跳过。
    CHECK_FALSE(lubancode::evolution::ParseObservation("{\"schema\":1").has_value());
    CHECK_FALSE(lubancode::evolution::ParseObservation("not json at all").has_value());
    CHECK_FALSE(lubancode::evolution::ParseObservation(
                    R"({"schema":2,"id":"obs-x","source":"run","source_id":"r1"})")
                    .has_value());
    CHECK_FALSE(lubancode::evolution::ParseObservation(
                    R"({"schema":1,"id":"","source":"run","source_id":"r1"})")
                    .has_value());
    CHECK_FALSE(lubancode::evolution::ParseObservation(
                    R"({"schema":1,"id":"obs-x","source":"nope","source_id":"r1"})")
                    .has_value());
}

TEST_CASE("观察:来源与结局枚举的线上串") {
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::Run) == "run");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::Goal) == "goal");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::Recording) ==
          "recording");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::ToolTrace) ==
          "tooltrace");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::Memory) ==
          "memory");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationSource::UserFeedback) ==
          "user_feedback");
    lubancode::evolution::ObservationSource parsed = lubancode::evolution::ObservationSource::Run;
    CHECK(lubancode::evolution::ParseObservationSource("tooltrace", parsed));
    CHECK(parsed == lubancode::evolution::ObservationSource::ToolTrace);
    CHECK_FALSE(lubancode::evolution::ParseObservationSource("nope", parsed));

    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationOutcome::Success) ==
          "success");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationOutcome::Failure) ==
          "failure");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationOutcome::Partial) ==
          "partial");
    CHECK(lubancode::evolution::ToString(lubancode::evolution::ObservationOutcome::Unknown) ==
          "unknown");
}

TEST_CASE("指纹:日期/URL/绝对路径归一,偶发值不进指纹") {
    using lubancode::evolution::NormalizeShapeText;
    // 同一桩活,两次口述只差日期/路径/大小写 → 归一后同形。
    const std::string first =
        "排查 provider 绑定,日志在 C:\\work\\proj\\a.log,2026-08-28 完成";
    const std::string second =
        "排查 provider 绑定,日志在 /home/u/proj/b.log,2026-08-29 完成";
    CHECK(NormalizeShapeText(first) == NormalizeShapeText(second));

    // URL 归一。
    CHECK(NormalizeShapeText("看 https://example.com/x 和 http://a.b/c") ==
          NormalizeShapeText("看 https://other.example.org/y 和 http://z.w/v"));
    // 8 位纯数字日期同归 <date>。
    CHECK(NormalizeShapeText("批次 20260828") == NormalizeShapeText("批次 20260901"));
    // 相对路径/普通词不动;数字保留。
    CHECK(NormalizeShapeText("重试 3 次") == "重试 3 次");
    CHECK(NormalizeShapeText("src/app/main.cpp") == "src/app/main.cpp");
    // 大小写折叠。
    CHECK(NormalizeShapeText("Fix Provider Binding") == "fix provider binding");
    // 空串安全。
    CHECK(NormalizeShapeText("").empty());
}

TEST_CASE("指纹:口径——进的是形状,不进的是哪一次") {
    using lubancode::evolution::ComputeFingerprint;
    // 同形(归一后):同指纹。
    CHECK(ComputeFingerprint(lubancode::evolution::ObservationSource::Goal,
                             lubancode::evolution::NormalizeShapeText("让测试全绿")) ==
          ComputeFingerprint(lubancode::evolution::ObservationSource::Goal,
                             lubancode::evolution::NormalizeShapeText("让测试全绿")));
    // 来源不同:不同指纹(哪怕形状同)。
    CHECK(ComputeFingerprint(lubancode::evolution::ObservationSource::Goal, "x") !=
          ComputeFingerprint(lubancode::evolution::ObservationSource::Run, "x"));
    // 形状不同:不同指纹。
    CHECK(ComputeFingerprint(lubancode::evolution::ObservationSource::Goal, "x") !=
          ComputeFingerprint(lubancode::evolution::ObservationSource::Goal, "y"));
    // 口径版本在指纹里:v1 前缀写死,改口径换 v2(此处钉住形状,防漂移)。
    CHECK(ComputeFingerprint(lubancode::evolution::ObservationSource::Memory, "feedback|用 uv")
              .rfind("fp-", 0) == 0);
}

TEST_CASE("稳定 id:同来源同 ID 必同,异来源必异") {
    using lubancode::evolution::MakeObservationId;
    const auto a = MakeObservationId(lubancode::evolution::ObservationSource::Run, "run-7");
    const auto b = MakeObservationId(lubancode::evolution::ObservationSource::Run, "run-7");
    const auto c = MakeObservationId(lubancode::evolution::ObservationSource::Run, "run-8");
    const auto d = MakeObservationId(lubancode::evolution::ObservationSource::Goal, "run-7");
    CHECK(a == b);  // 重采同一条账 id 不变 → 账去重靠它
    CHECK(a != c);
    CHECK(a != d);
    CHECK(a.rfind("obs-", 0) == 0);
}

TEST_CASE("脱敏:文本与 JSON 都过既有打码器") {
    using lubancode::evolution::SanitizeObservationText;
    using lubancode::evolution::SanitizeObservationJson;
    // 文本:sk- 形态 key、bearer、cookie= 赋值都打码(打码器认键形态)。
    const std::string dirty = "key " + std::string(kFakeToken) +
                              " Authorization: Bearer abc.def.ghi cookie=" +
                              std::string(kFakeCookie);
    const std::string clean = SanitizeObservationText(dirty);
    CHECK(clean.find(kFakeToken) == std::string::npos);
    CHECK(clean.find("abc.def.ghi") == std::string::npos);
    CHECK(clean.find(kFakeCookie) == std::string::npos);
    CHECK(clean.find("[已打码]") != std::string::npos);

    // JSON:敏感键的值整体打码(SanitizeToolInput 同规矩)。
    nlohmann::json input;
    input["api_key"] = kFakeToken;
    input["path"] = "src/main.cpp";
    const nlohmann::json sanitized = SanitizeObservationJson(input);
    CHECK(sanitized.at("api_key").get<std::string>() == "[已打码]");
    CHECK(sanitized.at("path").get<std::string>() == "src/main.cpp");

    // 截长在 UTF-8 边界上(中文不劈半个)。
    const std::string long_text(500, 'x');
    CHECK(SanitizeObservationText(long_text, 100).size() == 100);
    const std::string chinese(200, '\xe4');  // 模拟多字节流,截断不得落在续字节中间
    const std::string cut = SanitizeObservationText(chinese, 7);
    CHECK(cut.size() <= 7);
}

TEST_CASE("时间:epoch 毫秒格式化,非正值给空") {
    using lubancode::evolution::FormatEpochMsLocal;
    const std::string stamp = FormatEpochMsLocal(1750000000000LL);
    REQUIRE(stamp.size() == 19);
    CHECK(stamp[4] == '-');
    CHECK(stamp[7] == '-');
    CHECK(stamp[10] == ' ');
    CHECK(FormatEpochMsLocal(0).empty());
    CHECK(FormatEpochMsLocal(-5).empty());
}

TEST_CASE("脱敏防漏:观察全文查无密钥(构造侧自证)") {
    lubancode::evolution::EvolutionObservation observation;
    observation.id = lubancode::evolution::MakeObservationId(
        lubancode::evolution::ObservationSource::Memory, "fact-1");
    observation.source = lubancode::evolution::ObservationSource::Memory;
    observation.source_id = "fact-1";
    observation.summary = lubancode::evolution::SanitizeObservationText(
        "部署密钥 " + std::string(kFakeToken) + " 不要入库");
    observation.details["note"] = lubancode::evolution::SanitizeObservationText(
        "authorization: Bearer tok-do-not-leak");
    const std::string full = FullText(observation);
    CHECK(full.find(kFakeToken) == std::string::npos);
    CHECK(full.find("tok-do-not-leak") == std::string::npos);
}
