// UsageSample 与 purpose 合同测试(Token 账本单 §15.1 A0)。
#include <doctest/doctest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "accounting/usage_sample.hpp"

using namespace lubancode;
using namespace lubancode::accounting;

namespace {

UsageSample MakeSample() {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260830-000001-FX001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.turn_id = "turn-0007";
    sample.request_id = "req-0012";
    sample.provider_response_id = "resp_x7";
    sample.attempt = 1;
    sample.purpose = RequestPurpose::MainTurn;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = UsageSource::ProviderReported;
    api::Usage usage;
    usage.input_tokens = 1200;
    usage.cache_read_tokens = 48000;
    usage.cache_creation_tokens = 0;
    usage.output_tokens = 1800;
    usage.output_reasoning_tokens = 900;
    sample.usage = usage;
    sample.total_input_tokens = api::TotalInputTokens(usage);
    sample.total_billed_shape_tokens = sample.total_input_tokens + usage.output_tokens;
    sample.cache_epoch = 3;
    sample.prefix_append_only = true;
    sample.cost.status = CostStatus::NotPriced;
    sample.source_event = SourceEventRef{"main-0001", "main-0001:evt-00000129",
                                         std::string(64, 'a')};
    sample.request_outcome = "completed";
    return sample;
}

}  // namespace

TEST_CASE("purpose 枚举齐全且严格") {
    CHECK(AllPurposes().size() == 12);
    CHECK(PurposeName(RequestPurpose::MainTurn) == std::string("main_turn"));
    CHECK(PurposeName(RequestPurpose::InsightsModelReview) == std::string("insights_model_review"));
    CHECK(PurposeName(RequestPurpose::OtherHostRequest) == std::string("other_host_request"));
    for (const auto purpose : AllPurposes()) {
        const auto back = PurposeFromName(PurposeName(purpose));
        REQUIRE(back.has_value());
        CHECK(*back == purpose);
    }
    CHECK(!PurposeFromName("main").has_value());
    CHECK(!PurposeFromName("MAIN_TURN").has_value());
    CHECK(!PurposeFromName("").has_value());
}

TEST_CASE("UsageSample round-trip") {
    const UsageSample sample = MakeSample();
    const nlohmann::json json = sample.ToJson();
    std::string error;
    const auto parsed = UsageSample::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    INFO(error.c_str());
    CHECK(parsed->request_id == sample.request_id);
    CHECK(parsed->provider_response_id == sample.provider_response_id);
    CHECK(parsed->purpose == sample.purpose);
    CHECK(parsed->usage_source == UsageSource::ProviderReported);
    REQUIRE(parsed->usage.has_value());
    CHECK(parsed->usage->output_reasoning_tokens == 900);
    CHECK(parsed->total_input_tokens == 49200);
    CHECK(parsed->total_billed_shape_tokens == 51000);
    CHECK(parsed->source_event.has_value());
    CHECK(parsed->request_outcome == "completed");
    // 再来一遍:ToJson 字节稳定。
    CHECK(parsed->ToJson() == json);
}

TEST_CASE("unknown 与 0 分开:没报的 sample 不带 token 字段") {
    UsageSample sample = MakeSample();
    sample.usage.reset();
    sample.usage_source = UsageSource::Unknown;
    sample.total_input_tokens = 0;
    sample.total_billed_shape_tokens = 0;
    const nlohmann::json json = sample.ToJson();
    CHECK(json.at("usage").is_null());
    CHECK(!json.contains("total_input_tokens"));
    std::string error;
    const auto parsed = UsageSample::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    CHECK(!parsed->usage.has_value());
    CHECK(parsed->usage_source == UsageSource::Unknown);
    // provider 明报全零:usage 在,五项全 0,与 unknown 分得开。
    UsageSample zero = MakeSample();
    zero.usage = api::Usage{};
    zero.usage_source = UsageSource::ProviderReported;
    zero.total_input_tokens = 0;
    zero.total_billed_shape_tokens = 0;
    const auto zero_parsed = UsageSample::FromJsonStrict(zero.ToJson(), &error);
    REQUIRE(zero_parsed.has_value());
    REQUIRE(zero_parsed->usage.has_value());
    CHECK(zero_parsed->usage->input_tokens == 0);
}

TEST_CASE("非法 sample 拒收") {
    std::string error;
    // reasoning > output。
    UsageSample bad = MakeSample();
    bad.usage->output_reasoning_tokens = bad.usage->output_tokens + 1;
    CHECK(!UsageSample::FromJsonStrict(bad.ToJson(), &error).has_value());
    // total_input 与五项不合口径。
    UsageSample mismatch = MakeSample();
    nlohmann::json json = mismatch.ToJson();
    json["total_input_tokens"] = 1;
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
    // unknown 却带 usage。
    json = mismatch.ToJson();
    json["usage_source"] = "unknown";
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
    // purpose 认不得。
    json = mismatch.ToJson();
    json["purpose"] = "yolo";
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
    // usage_source 认不得。
    json = mismatch.ToJson();
    json["usage_source"] = "guessed";
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
    // 未知键。
    json = mismatch.ToJson();
    json["leak"] = "正文不许进";
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
    // schema 名错。
    json = mismatch.ToJson();
    json["schema"] = "lubancode.usage.samp";
    CHECK(!UsageSample::FromJsonStrict(json, &error).has_value());
}

TEST_CASE("CostEstimate 四态与整数 micros") {
    CHECK(std::string(CostStatusName(CostStatus::Estimated)) == "estimated");
    CHECK(std::string(CostStatusName(CostStatus::ProviderReported)) == "provider_reported");
    CHECK(std::string(CostStatusName(CostStatus::NotPriced)) == "not_priced");
    CHECK(std::string(CostStatusName(CostStatus::NotApplicable)) == "not_applicable");
    for (const auto status : {CostStatus::Estimated, CostStatus::ProviderReported,
                              CostStatus::NotPriced, CostStatus::NotApplicable}) {
        REQUIRE(CostStatusFromName(CostStatusName(status)).has_value());
        CHECK(*CostStatusFromName(CostStatusName(status)) == status);
    }
    CHECK(!CostStatusFromName("cheap").has_value());

    CostEstimate cost;
    cost.status = CostStatus::Estimated;
    cost.currency = "USD";
    cost.micros = 284000;
    cost.price_table_id = "user-list-price-2026-08-30";
    std::string error;
    const auto parsed = CostEstimate::FromJsonStrict(cost.ToJson(), &error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->micros == 284000);
    CHECK(parsed->price_table_id == cost.price_table_id);
}
