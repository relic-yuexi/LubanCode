// Usage 五层聚合与 session 读侧的合同测试(Token 账本单 §7/§15.3 A2)。
//   - 聚合数学:多请求求和、unknown 不折 0、reasoning 不双计、重试单列、
//     cache 比例的分母规则;
//   - cache 行为观察:epoch 重建/前缀改写/疑似未命中(候选)/无标注点名;
//   - 读侧:真 recorder 写的 Journal 读回对账(含 subagent/workflow stream),
//     坏 stream 点名不出残账,没有的 session 不算账错。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "accounting/purpose.hpp"
#include "accounting/session_usage_reader.hpp"
#include "accounting/usage_aggregate.hpp"
#include "accounting/usage_sample.hpp"

#include "../insights/insights_fixtures.hpp"

using namespace lubancode;
using namespace lubancode::accounting;

namespace {

UsageSample Sample(const char* request_id, RequestPurpose purpose, std::int64_t input,
                   std::int64_t cache_read, std::int64_t output, int attempt = 1) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-AGG001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.attempt = attempt;
    sample.purpose = purpose;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = UsageSource::ProviderReported;
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

// 没报 usage 的 sample(coverage 靠它数出来)。
UsageSample UnknownSample(const char* request_id, int attempt = 1) {
    UsageSample sample;
    sample.workspace_key = "ws-000000000000";
    sample.session_id = "20260831-000001-AGG001";
    sample.run_id = "main-0001";
    sample.run_kind = "main_session";
    sample.request_id = request_id;
    sample.attempt = attempt;
    sample.purpose = RequestPurpose::MainTurn;
    sample.provider = "ccmoon";
    sample.wire = "responses";
    sample.model = "gpt-5.6-sol";
    sample.usage_source = UsageSource::Unknown;
    sample.request_outcome = "completed";
    return sample;
}

const UsageBreakdown* FindRow(const std::vector<UsageBreakdown>& rows, const std::string& label) {
    for (const auto& row : rows) {
        if (row.label == label) {
            return &row;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("聚合数学:求和、unknown 不折 0、reasoning 不双计、重试单列") {
    std::vector<UsageSample> samples;
    UsageSample a = Sample("req-1", RequestPurpose::MainTurn, 1200, 48000, 1800);
    a.usage->output_reasoning_tokens = 900;
    a.cache_epoch = 1;
    samples.push_back(a);
    UsageSample b = Sample("req-2", RequestPurpose::CompactMap, 60000, 20000, 1200);
    b.cache_epoch = 2;
    b.run_id = "main-0001";  // compact 旁路也在 main stream(§11.2)
    samples.push_back(b);
    // 重试:同 request 第 2 次 attempt,失败那笔照记。
    UsageSample retry = Sample("req-3", RequestPurpose::MainTurn, 5000, 0, 60, 2);
    retry.request_outcome = "failed";
    retry.cache_epoch = 2;
    samples.push_back(retry);
    samples.push_back(UnknownSample("req-4"));

    const UsageAggregate aggregate = AggregateUsage(samples);
    CHECK(aggregate.totals.requests_total == 4);
    CHECK(aggregate.totals.requests_with_usage == 3);
    CHECK(aggregate.totals.requests_unknown == 1);
    CHECK(aggregate.totals.requests_retry == 1);
    // 输入合计 = (1200+48000) + (60000+20000) + (5000+0);unknown 不进账。
    CHECK(aggregate.totals.input_tokens == 1200 + 60000 + 5000);
    CHECK(aggregate.totals.cache_read_tokens == 48000 + 20000);
    CHECK(aggregate.totals.output_tokens == 1800 + 1200 + 60);
    CHECK(aggregate.totals.reasoning_tokens == 900);  // 子集,只这一笔记了
    CHECK(aggregate.totals.total_input_tokens ==
          aggregate.totals.input_tokens + aggregate.totals.cache_read_tokens +
              aggregate.totals.cache_creation_tokens);
    // billed shape = total_input + output(不加 reasoning 第二遍)。
    CHECK(aggregate.totals.total_billed_shape_tokens ==
          aggregate.totals.total_input_tokens + aggregate.totals.output_tokens);
    // 终态分布:completed 3 · failed 1。
    REQUIRE(aggregate.totals.by_outcome.contains("completed"));
    REQUIRE(aggregate.totals.by_outcome.contains("failed"));
    CHECK(aggregate.totals.by_outcome.at("completed") == 3);
    CHECK(aggregate.totals.by_outcome.at("failed") == 1);

    // purpose 分账:main_turn 2 笔(compact 1 笔另列);unknown 样本计入
    // main_turn 的 requests_total/requests_unknown,不贡献 token。
    REQUIRE(FindRow(aggregate.by_purpose, "main_turn") != nullptr);
    CHECK(FindRow(aggregate.by_purpose, "main_turn")->totals.requests_total == 3);
    CHECK(FindRow(aggregate.by_purpose, "main_turn")->totals.requests_unknown == 1);
    CHECK(FindRow(aggregate.by_purpose, "main_turn")->totals.input_tokens == 1200 + 5000);
    REQUIRE(FindRow(aggregate.by_purpose, "compact_map") != nullptr);
    CHECK(FindRow(aggregate.by_purpose, "compact_map")->totals.requests_total == 1);

    // model 分账同一只端合成一行。
    REQUIRE(FindRow(aggregate.by_model, "ccmoon/gpt-5.6-sol") != nullptr);
    CHECK(FindRow(aggregate.by_model, "ccmoon/gpt-5.6-sol")->totals.requests_total == 4);

    // cache 比例:分母 = input+read+write(只计实测笔)。
    const auto ratio = aggregate.totals.cache_read_ratio_percent();
    REQUIRE(ratio.has_value());
    const std::int64_t base = (1200 + 48000) + (60000 + 20000) + 5000;
    CHECK(*ratio == static_cast<int>((68000 * 200 + base) / (base * 2)));

    // epoch 1 -> 2:一次预期重建。req-2(命中 20k)之后的 req-3 同 epoch 却
    // 零命中且默认自称 append-only——观察候选 +1(重试也长这模样,只计数)。
    CHECK(aggregate.cache.expected_rebuild_events == 1);
    CHECK(aggregate.cache.unexpected_miss_candidates == 1);
    CHECK(aggregate.run_ids.size() == 1);
}

TEST_CASE("cache epoch 分段:compact 翻页后各段独立求和,未报缓存不伪装 0%") {
    UsageSample first = Sample("req-1", RequestPurpose::MainTurn, 1000, 9000, 10);
    first.cache_epoch = 1;
    first.cache_reported_by_provider = true;
    UsageSample second = Sample("req-2", RequestPurpose::MainTurn, 2000, 8000, 20);
    second.cache_epoch = 2;
    second.cache_reported_by_provider = true;
    UsageSample unknown = Sample("req-3", RequestPurpose::MainTurn, 3000, 0, 30);
    unknown.cache_epoch = 2;
    unknown.cache_reported_by_provider = false;

    const UsageAggregate aggregate = AggregateUsage({first, second, unknown});
    REQUIRE(aggregate.by_cache_epoch.size() == 2);
    CHECK(aggregate.by_cache_epoch[0].cache_epoch == 1);
    CHECK(aggregate.by_cache_epoch[0].totals.cache_read_tokens == 9000);
    CHECK(aggregate.by_cache_epoch[0].totals.total_input_tokens == 10000);
    CHECK(aggregate.by_cache_epoch[0].requests_cache_reported == 1);
    CHECK(aggregate.by_cache_epoch[1].cache_epoch == 2);
    CHECK(aggregate.by_cache_epoch[1].totals.cache_read_tokens == 8000);
    CHECK(aggregate.by_cache_epoch[1].totals.total_input_tokens == 13000);
    CHECK(aggregate.by_cache_epoch[1].requests_cache_reported == 1);
    CHECK(aggregate.by_cache_epoch[1].requests_cache_unknown == 1);

    const nlohmann::json json = aggregate.ToJson();
    REQUIRE(json.at("by_cache_epoch").size() == 2);
    CHECK(json.at("by_cache_epoch")[0].at("read_ratio_percent") == 90);
}

TEST_CASE("全 unknown:比例给 nullopt,不拿 0% 冒充") {
    std::vector<UsageSample> samples;
    samples.push_back(UnknownSample("req-1"));
    samples.push_back(UnknownSample("req-2"));
    const UsageAggregate aggregate = AggregateUsage(samples);
    CHECK(aggregate.totals.requests_total == 2);
    CHECK(aggregate.totals.requests_with_usage == 0);
    CHECK(aggregate.totals.requests_unknown == 2);
    CHECK(aggregate.totals.total_input_tokens == 0);
    CHECK(!aggregate.totals.cache_read_ratio_percent().has_value());
}

TEST_CASE("provider 明报全零:是实测零,不是 unknown") {
    UsageSample zero = Sample("req-1", RequestPurpose::MainTurn, 0, 0, 0);
    const UsageAggregate aggregate = AggregateUsage({zero});
    CHECK(aggregate.totals.requests_with_usage == 1);
    CHECK(aggregate.totals.requests_unknown == 0);
    CHECK(aggregate.totals.requests_total == 1);
    CHECK(aggregate.totals.total_input_tokens == 0);
    // 分母 0:比例 unknown。
    CHECK(!aggregate.totals.cache_read_ratio_percent().has_value());
}

TEST_CASE("cache 观察:疑似未命中按候选计数,epoch 缺席点名") {
    std::vector<UsageSample> samples;
    // 同 epoch 两笔:前笔命中,后笔自称 append-only 却零命中。
    UsageSample hit = Sample("req-1", RequestPurpose::MainTurn, 100, 40000, 50);
    hit.cache_epoch = 1;
    hit.prefix_append_only = true;
    samples.push_back(hit);
    UsageSample miss = Sample("req-2", RequestPurpose::MainTurn, 41000, 0, 80);
    miss.cache_epoch = 1;
    miss.prefix_append_only = true;
    samples.push_back(miss);
    // 前缀改写的一笔:cache 丢是自找的,不算疑似未命中。
    UsageSample churn = Sample("req-3", RequestPurpose::MainTurn, 100, 0, 60);
    churn.cache_epoch = 1;
    churn.prefix_append_only = false;
    samples.push_back(churn);
    // 没有 epoch 标注的一笔。
    samples.push_back(UnknownSample("req-4"));

    const UsageAggregate aggregate = AggregateUsage(samples);
    CHECK(aggregate.cache.unexpected_miss_candidates == 1);
    CHECK(aggregate.cache.append_only_breaks == 1);
    CHECK(aggregate.cache.epoch_unlabeled == 1);
    CHECK(aggregate.cache.expected_rebuild_events == 0);
}

TEST_CASE("费用:只累 estimated 的整数 micros,not_priced 不进") {
    std::vector<UsageSample> samples;
    UsageSample a = Sample("req-1", RequestPurpose::MainTurn, 1000, 0, 100);
    a.cost.status = CostStatus::Estimated;
    a.cost.micros = 284000;
    samples.push_back(a);
    UsageSample b = Sample("req-2", RequestPurpose::MainTurn, 1000, 0, 100);
    b.cost.status = CostStatus::NotPriced;
    samples.push_back(b);
    const UsageAggregate aggregate = AggregateUsage(samples);
    CHECK(aggregate.totals.cost_micros == 284000);
    CHECK(aggregate.totals.requests_priced == 1);

    const nlohmann::json json = aggregate.ToJson();
    CHECK(json.at("totals").at("cost_micros") == 284000);
    CHECK(json.at("totals").at("requests_total") == 2);
    CHECK(json.at("cache").at("read_ratio_percent") == 0);  // 实测分母为 0 输入
}

TEST_CASE("读侧:真 Journal 的 main+subagent+workflow 读回对账") {
    const auto dir = insights_fixtures::PrepareDir(
        std::filesystem::temp_directory_path() / "lubancode-usage-reader");
    // 真目录树布局(§3.1):main.jsonl 在根、subagents/ 与 workflows/<run>/
    // 各自子目录——reader 按这棵树认领 stream。
    std::filesystem::create_directories(dir / "subagents");
    std::filesystem::create_directories(dir / "workflows" / "workflow-0001");
    const insights_fixtures::FixedClock clock;
    const auto open_stream = [&](const std::filesystem::path& stream_path, const std::string& run_id,
                                 insights_fixtures::RunKind kind) {
        return insights_fixtures::FixtureStream(stream_path, dir / "artifacts", "ws-000000000000",
                                                "20260831-000002-RD0001", run_id, kind, 2, clock);
    };
    {
        auto main = open_stream(dir / "main.jsonl", "main-0001",
                                insights_fixtures::RunKind::MainSession);
        main.StartRun();
        main.StartTurn("turn-0001");
        insights_fixtures::UsageSpec main_usage;
        main_usage.input = 900;
        main_usage.cache_read = 1200;
        main_usage.output = 80;
        main.ModelExchange("turn-0001", "req-0001", "main_turn", main_usage);
        insights_fixtures::UsageSpec compact;
        compact.input = 500;
        compact.output = 40;
        main.ModelExchange("turn-0001", "req-0002", "compact_reduce", compact);
        main.EndTurn("turn-0001");
        main.Seal();
    }
    {
        auto sub = open_stream(dir / "subagents" / "subagent-0001.jsonl", "subagent-0001",
                               insights_fixtures::RunKind::Subagent);
        sub.StartRun("subagent_dispatch");
        sub.StartTurn("turn-0001", "peer_agent");
        insights_fixtures::UsageSpec sub_usage;
        sub_usage.input = 4000;
        sub_usage.output = 700;
        sub.ModelExchange("turn-0001", "req-0001", "subagent_turn", sub_usage);
        sub.EndTurn("turn-0001");
        sub.Seal();
    }
    {
        auto node = open_stream(dir / "workflows" / "workflow-0001" / "workflow.jsonl",
                                "workflow-0001", insights_fixtures::RunKind::Workflow);
        node.StartRun("workflow_node");
        node.StartTurn("turn-0001", "scheduled_host");
        insights_fixtures::UsageSpec node_usage;
        node_usage.reported = false;
        node.ModelExchange("turn-0001", "req-0001", "workflow_node", node_usage);
        node.EndTurn("turn-0001");
        node.Seal();
    }

    const SessionUsageRead read = ReadSessionUsage(dir);
    REQUIRE(read.ok);
    CHECK(read.session_id == "20260831-000002-RD0001");
    CHECK(read.workspace_key == "ws-000000000000");
    // 三条 stream · 四笔 sample;workflow 那笔 unknown。
    CHECK(read.samples.size() == 4);
    std::int64_t unknown = 0;
    std::int64_t total_input = 0;
    for (const auto& sample : read.samples) {
        if (!sample.usage.has_value()) {
            unknown += 1;
        } else {
            total_input += sample.total_input_tokens;
        }
    }
    CHECK(unknown == 1);
    CHECK(total_input == (900 + 1200) + 500 + 4000);

    const UsageAggregate aggregate = AggregateUsage(read.samples);
    CHECK(aggregate.run_ids.size() == 3);
    REQUIRE(FindRow(aggregate.by_run, "main_session") != nullptr);
    CHECK(FindRow(aggregate.by_run, "main_session")->totals.requests_total == 2);
    REQUIRE(FindRow(aggregate.by_run, "subagent") != nullptr);
    CHECK(FindRow(aggregate.by_run, "subagent")->totals.requests_total == 1);
    REQUIRE(FindRow(aggregate.by_purpose, "compact_reduce") != nullptr);
    CHECK(FindRow(aggregate.by_purpose, "compact_reduce")->totals.requests_total == 1);
    // session.json 没写(FixtureStream 不建目录树),status=unknown:
    // 封口与否调用方按 provisional 处理,这里如实。
    CHECK(read.status == "unknown");
    CHECK(!read.sealed());
}

TEST_CASE("读侧:坏 stream 点名,好 stream 照读;没有的 session 不算账") {
    const auto dir = insights_fixtures::PrepareDir(
        std::filesystem::temp_directory_path() / "lubancode-usage-reader-bad");
    const insights_fixtures::FixedClock clock;
    {
        insights_fixtures::FixtureStream main(dir / "main.jsonl", dir / "artifacts",
                                              "ws-000000000000", "20260831-000003-RD0002",
                                              "main-0001", insights_fixtures::RunKind::MainSession, 2,
                                              clock);
        main.StartRun();
        main.StartTurn("turn-0001");
        insights_fixtures::UsageSpec usage;
        usage.input = 100;
        usage.output = 20;
        main.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        main.EndTurn("turn-0001");
        main.Seal();
    }
    // 坏 stream:subagents/下一行不是 JSON。
    std::filesystem::create_directories(dir / "subagents");
    { std::ofstream out(dir / "subagents" / "subagent-0001.jsonl", std::ios::binary); out << "not-json\n"; }

    const SessionUsageRead read = ReadSessionUsage(dir);
    REQUIRE(read.ok);
    CHECK(read.samples.size() == 1);
    // 两条点名:坏 stream + session.json 缺(FixtureStream 不建目录树)。
    REQUIRE(read.warnings.size() == 2);
    bool saw_unreadable = false;
    bool saw_manifest_missing = false;
    for (const auto& warning : read.warnings) {
        saw_unreadable = saw_unreadable || warning.find("usage.stream_unreadable") != std::string::npos;
        saw_manifest_missing =
            saw_manifest_missing || warning.find("usage.session_manifest_missing") != std::string::npos;
    }
    CHECK(saw_unreadable);
    CHECK(saw_manifest_missing);

    // 目录不存在:稳定码,不产残账。
    const SessionUsageRead missing = ReadSessionUsage(dir / "no-such-session");
    CHECK(!missing.ok);
    CHECK(missing.error_code == "usage.session_not_found");
    CHECK(missing.samples.empty());
}
