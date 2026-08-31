// /usage 命令面的纯函数测试(Token 账本单 A2):解析、整数 micros 金额、
// 计价贴账、报告渲染(--by 分账/降级口径/provisional 标注/缺口点名截断)
// 与 JSON 输出的报告面 schema。
#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/purpose.hpp"
#include "accounting/usage_aggregate.hpp"
#include "accounting/usage_sample.hpp"
#include "app/commands/usage_commands.hpp"

using namespace lubancode;
using namespace lubancode::app;
using lubancode::accounting::RequestPurpose;
using lubancode::accounting::UsageSample;

namespace {

UsageSample Sample(const char* request_id, RequestPurpose purpose, std::int64_t input,
                   std::int64_t cache_read, std::int64_t output) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-UC0001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.purpose = purpose;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = lubancode::accounting::UsageSource::ProviderReported;
    api::Usage usage;
    usage.input_tokens = input;
    usage.cache_read_tokens = cache_read;
    usage.output_tokens = output;
    sample.usage = usage;
    sample.total_input_tokens = api::TotalInputTokens(usage);
    sample.total_billed_shape_tokens = sample.total_input_tokens + output;
    sample.request_outcome = "completed";
    return sample;
}

UsageSample UnknownSample(const char* request_id) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-UC0001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.purpose = RequestPurpose::TitleRefine;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = lubancode::accounting::UsageSource::Unknown;
    sample.request_outcome = "completed";
    return sample;
}

std::string Join(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += line + "\n";
    }
    return joined;
}

}  // namespace

TEST_CASE("ParseUsageCommand:裸敲/指定场/分账/json/组合/后续批次/坏词") {
    CHECK(ParseUsageCommand("").scope == ParsedUsageCommand::Scope::ActiveSession);
    CHECK(!ParseUsageCommand("").json);
    CHECK(ParseUsageCommand("").by == ParsedUsageCommand::By::None);

    const auto named = ParseUsageCommand("session 20260830-000001-FX001");
    CHECK(named.scope == ParsedUsageCommand::Scope::NamedSession);
    CHECK(named.session_id == "20260830-000001-FX001");

    const auto combo = ParseUsageCommand("session abc --by purpose --json");
    CHECK(combo.scope == ParsedUsageCommand::Scope::NamedSession);
    CHECK(combo.by == ParsedUsageCommand::By::Purpose);
    CHECK(combo.json);

    CHECK(ParseUsageCommand("--by model").by == ParsedUsageCommand::By::Model);
    CHECK(ParseUsageCommand("--by run").by == ParsedUsageCommand::By::Run);
    CHECK(ParseUsageCommand("--by outcome").by == ParsedUsageCommand::By::Outcome);
    CHECK(ParseUsageCommand("--by color").invalid);

    // day/week/workspace/all:后续批次的口径,不当错也不冒充。
    CHECK(ParseUsageCommand("day").later_scope == "day");
    CHECK(ParseUsageCommand("workspace --json").later_scope == "workspace");
    CHECK(!ParseUsageCommand("week").invalid);

    CHECK(ParseUsageCommand("blah").invalid);
    CHECK(ParseUsageCommand("blah").bad_word == "blah");
    CHECK(ParseUsageCommand("session").invalid);  // session 后没跟 id
    CHECK(ParseUsageCommand("--by").invalid);     // --by 后没跟维度
}

TEST_CASE("FormatMicrosAmount:整数拼装,无 float,负数与空货币") {
    CHECK(FormatMicrosAmount(284000, "$") == "$0.284000");
    CHECK(FormatMicrosAmount(0, "$") == "$0.000000");
    CHECK(FormatMicrosAmount(5, "$") == "$0.000005");
    CHECK(FormatMicrosAmount(2'500'000, "$") == "$2.500000");
    CHECK(FormatMicrosAmount(-1'500'000, "$") == "-$1.500000");
    CHECK(FormatMicrosAmount(284000, "") == "0.284000");
}

TEST_CASE("ApplyCostEstimates:无表 not_priced;有表命中;unknown 不贴价") {
    SUBCASE("无表:整包 not_priced,token 不动") {
        std::vector<UsageSample> samples{Sample("req-1", RequestPurpose::MainTurn, 1000, 0, 100)};
        ApplyCostEstimates(samples, std::nullopt);
        CHECK(samples[0].cost.status == lubancode::accounting::CostStatus::NotPriced);
        CHECK(samples[0].cost.micros == 0);
        CHECK(samples[0].usage.has_value());  // token 照报
    }
    SUBCASE("有表:按单价贴,reasoning 不另乘") {
        lubancode::accounting::PricingTable table;
        table.id = "test-table";
        table.currency = "USD";
        table.effective_from = "2026-08-01";  // session id 头 8 位 20260831 在生效日内
        table.models["ccmoon/gpt-5.6-sol"] = lubancode::accounting::ModelPrice{
            3'000'000, 300'000, 3'750'000, 12'000'000};  // $3/$0.3/$3.75/$12 per M
        std::vector<UsageSample> samples;
        UsageSample a = Sample("req-1", RequestPurpose::MainTurn, 1'000'000, 0, 500'000);
        a.usage->output_reasoning_tokens = 200'000;  // 子集,不许再乘一遍
        samples.push_back(a);
        samples.push_back(UnknownSample("req-2"));  // unknown 不贴价
        ApplyCostEstimates(samples, table);
        REQUIRE(samples[0].cost.status == lubancode::accounting::CostStatus::Estimated);
        // 1M*3 + 0.5M*12 = 3 + 6 = $9 → 9'000'000 micros。
        CHECK(samples[0].cost.micros == 9'000'000);
        CHECK(samples[0].cost.price_table_id == "test-table");
        CHECK(samples[1].cost.status == lubancode::accounting::CostStatus::NotPriced);
    }
    SUBCASE("表里没这只端:not_priced") {
        lubancode::accounting::PricingTable table;
        table.id = "test-table";
        table.models["other/model"] = lubancode::accounting::ModelPrice{1, 1, 1, 1};
        std::vector<UsageSample> samples{Sample("req-1", RequestPurpose::MainTurn, 1000, 0, 100)};
        ApplyCostEstimates(samples, table);
        CHECK(samples[0].cost.status == lubancode::accounting::CostStatus::NotPriced);
    }
}

TEST_CASE("FormatUsageReport:默认画面——coverage、cache、purpose、成色") {
    std::vector<UsageSample> samples;
    UsageSample main1 = Sample("req-1", RequestPurpose::MainTurn, 1200, 48000, 1800);
    main1.usage->output_reasoning_tokens = 900;
    samples.push_back(main1);
    samples.push_back(Sample("req-2", RequestPurpose::CompactMap, 60000, 20000, 1200));
    samples.push_back(UnknownSample("req-3"));  // title_refine,没报

    UsageReportModel model;
    model.session_id = "20260831-000001-UC0001";
    model.workspace_key = "ws-000000000000";
    model.status = "running";
    model.provisional = true;
    model.pricing_note = "未配价格表";
    model.aggregate = lubancode::accounting::AggregateUsage(samples);

    const std::string text = Join(FormatUsageReport(model));
    // 标题带 provisional。
    CHECK(text.find("Usage · 20260831-000001-UC0001(未封口 provisional)") != std::string::npos);
    // coverage:2/3 报了,1 笔 unknown 不折 0。
    CHECK(text.find("2/3 笔有 provider usage") != std::string::npos);
    CHECK(text.find("1 笔 unknown(未报,不折 0)") != std::string::npos);
    // 输入:cache 读 68000 / 合计输入 129200 = 53%(FormatTokenCount 的
    // ".0 省略"规则:68.0k 写作 68k)。
    CHECK(text.find("cache 读 68k(53%)") != std::string::npos);
    CHECK(text.find("合计输入 129.2k") != std::string::npos);
    // 输出:reasoning 是子集,注明。
    CHECK(text.find("推理 900(已含在输出,不另加)") != std::string::npos);
    // 用途分账占比,分母注明。
    CHECK(text.find("(按 input+output token 占比)") != std::string::npos);
    CHECK(text.find("main_turn") != std::string::npos);
    CHECK(text.find("compact_map") != std::string::npos);
    CHECK(text.find("title_refine") != std::string::npos);
    // 没配价格表:token 照报,费用 not_priced。
    CHECK(text.find("not_priced(未配价格表;token 照报,不估不猜)") != std::string::npos);
    // 成色:1 条 run,status=running。
    CHECK(text.find("1 条 run") != std::string::npos);
    CHECK(text.find("session status=running") != std::string::npos);
}

TEST_CASE("FormatUsageReport:空账不猜;--by purpose 分账表;缺口点名截断") {
    SUBCASE("空账") {
        UsageReportModel model;
        model.session_id = "20260831-000009-EMPTY9";
        model.aggregate = lubancode::accounting::AggregateUsage({});
        const auto lines = FormatUsageReport(model);
        REQUIRE(lines.size() == 2);
        CHECK(lines[1].find("还没有模型请求账") != std::string::npos);
    }
    SUBCASE("--by purpose 分账表") {
        std::vector<UsageSample> samples;
        samples.push_back(Sample("req-1", RequestPurpose::MainTurn, 1000, 3000, 100));
        samples.push_back(Sample("req-2", RequestPurpose::SubagentTurn, 500, 0, 200));
        UsageReportModel model;
        model.session_id = "s";
        model.aggregate = lubancode::accounting::AggregateUsage(samples);
        model.by = ParsedUsageCommand::By::Purpose;
        const std::string text = Join(FormatUsageReport(model));
        CHECK(text.find("按 purpose 分账") != std::string::npos);
        CHECK(text.find("main_turn") != std::string::npos);
        CHECK(text.find("subagent_turn") != std::string::npos);
        // 每行有笔数与输入输出。
        CHECK(text.find("2 笔") != std::string::npos);
    }
    SUBCASE("缺口点名最多 5 条,超了计数") {
        std::vector<UsageSample> samples{Sample("req-1", RequestPurpose::MainTurn, 1, 0, 1)};
        UsageReportModel model;
        model.session_id = "s";
        model.aggregate = lubancode::accounting::AggregateUsage(samples);
        for (int i = 0; i < 7; ++i) {
            model.aggregate.warnings.push_back("usage.purpose_missing: req-" + std::to_string(i));
        }
        const std::string text = Join(FormatUsageReport(model));
        CHECK(text.find("缺口点名") != std::string::npos);
        CHECK(text.find("…另有 2 条") != std::string::npos);
    }
}

TEST_CASE("BuildUsageReportJson:报告面 schema 与成色字段") {
    std::vector<UsageSample> samples;
    UsageSample a = Sample("req-1", RequestPurpose::MainTurn, 1000, 0, 100);
    a.cost.status = lubancode::accounting::CostStatus::Estimated;
    a.cost.micros = 3300;
    samples.push_back(a);
    samples.push_back(UnknownSample("req-2"));

    UsageReportModel model;
    model.session_id = "20260831-000001-UC0001";
    model.workspace_key = "ws-000000000000";
    model.status = "running";
    model.provisional = true;
    model.pricing_note = "未配价格表";
    model.aggregate = lubancode::accounting::AggregateUsage(samples);
    model.by = ParsedUsageCommand::By::Purpose;

    const nlohmann::json json = BuildUsageReportJson(model);
    CHECK(json.at("schema") == "lubancode.usage.report");
    CHECK(json.at("schema_version") == 1);
    CHECK(json.at("provisional") == true);
    CHECK(json.at("by") == "purpose");
    CHECK(json.at("pricing").at("note") == "未配价格表");
    CHECK(json.at("aggregate").at("totals").at("requests_total") == 2);
    CHECK(json.at("aggregate").at("totals").at("requests_unknown") == 1);
    CHECK(json.at("aggregate").at("totals").at("cost_micros") == 3300);
    CHECK(json.at("aggregate").at("cache").at("read_ratio_percent") == 0);
    // price 表在时 pricing 换 id/currency 形。
    lubancode::accounting::PricingTable table;
    table.id = "user-list-price";
    table.currency = "USD";
    model.pricing = table;
    const nlohmann::json with_table = BuildUsageReportJson(model);
    CHECK(with_table.at("pricing").at("id") == "user-list-price");
    CHECK(with_table.at("pricing").at("currency") == "USD");
}
