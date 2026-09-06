// TokenCalibrator(token 估算校准单):真实 usage 反推 byte/token 比率的
// 会话级校准器。两层验收——纯逻辑(护栏:兜底/剔除/漂移重置/分桶/定长
// 收敛)与假后端对账(AgentLoop 走真路,两轮请求后估算收敛到校准比率,
// B 闸真实水位吃到系数;不接线的 loop 行为与从前一字不差)。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/context.hpp"
#include "agent/loop.hpp"
#include "agent/token_calibrator.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"

using namespace lubancode;
using agent::TokenCalibrationSample;
using agent::TokenCalibrator;
using RecordVerdict = agent::TokenCalibrator::RecordVerdict;

namespace {

// 一对合法样本:估算 5000(过小请求护栏),实报可配。
TokenCalibrationSample Sample(std::int64_t reported, std::size_t est = 5000, std::size_t bytes = 20000) {
    TokenCalibrationSample s;
    s.request_bytes = bytes;
    s.estimated_tokens = est;
    s.reported_input_tokens = reported;
    return s;
}

// 实报 = 估算 × ratio 的样本。
TokenCalibrationSample SampleRatio(double ratio, std::size_t est = 5000, std::size_t bytes = 20000) {
    return Sample(static_cast<std::int64_t>(static_cast<double>(est) * ratio), est, bytes);
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯逻辑:兜底、收敛、护栏、分桶
// ---------------------------------------------------------------------------

TEST_CASE("TokenCalibrator: 首两对样本之前用默认系数兜底,状态如实报未校准") {
    TokenCalibrator cal;
    CHECK(cal.Record("p", "m", SampleRatio(1.4)) == RecordVerdict::Accepted);
    // 一对样本:系数仍是默认尺,不校准。
    CHECK(cal.Coefficient("p", "m") == 1.0);
    const auto status = cal.StatusOf("p", "m");
    CHECK_FALSE(status.calibrated);
    CHECK(status.coefficient == 1.0);
    CHECK(status.estimate_deviation_percent == 0);
    CHECK(status.sample_count == 1);
    // 没记过样本的桶:同样兜底。
    CHECK(cal.Coefficient("p", "other") == 1.0);
}

TEST_CASE("TokenCalibrator: 定长对账收敛——固定比率的样本,系数取中位") {
    TokenCalibrator cal;
    CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("p", "m", SampleRatio(1.2)) == RecordVerdict::Accepted);
    // 两对:中位 = (1.0 + 1.2) / 2 = 1.1。
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(1.1));
    const auto status = cal.StatusOf("p", "m");
    CHECK(status.calibrated);
    CHECK(status.sample_count == 2);
    CHECK(status.coefficient == doctest::Approx(1.1));
    // 偏差 = round((1/1.1 - 1) * 100) = -9:默认尺比真实低 9%。
    CHECK(status.estimate_deviation_percent == -9);
    // tokens/byte 中位:ratio 1.0 的样本 5000/20000=0.25,ratio 1.2 的
    // 6000/20000=0.30,中位 0.275。
    CHECK(status.tokens_per_byte == doctest::Approx(0.275));
    // 第三对进来:中位换成三枚的正中。
    CHECK(cal.Record("p", "m", SampleRatio(1.4)) == RecordVerdict::Accepted);
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(1.2));
}

TEST_CASE("TokenCalibrator: 滚动窗口只留最近 8 对") {
    TokenCalibrator cal;
    // 8 对 ratio 1.0 先垫底,再记 1.3(带内,进窗)——最旧两枚 1.0 被挤出,
    // 计数钉在 8;窗口渐渐向新口径滑动,中位跟着走。
    for (int i = 0; i < 8; ++i) {
        CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    }
    for (int i = 0; i < 2; ++i) {
        CHECK(cal.Record("p", "m", SampleRatio(1.3)) == RecordVerdict::Accepted);
    }
    CHECK(cal.StatusOf("p", "m").sample_count == TokenCalibrator::kWindow);
    // [1.0×6, 1.3×2]:中位仍 1.0。
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(1.0));
    // 再挤进四枚 1.3:[1.0×2, 1.3×6],中位翻到 1.3——旧样本滚出窗口。
    for (int i = 0; i < 4; ++i) {
        CHECK(cal.Record("p", "m", SampleRatio(1.3)) == RecordVerdict::Accepted);
    }
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(1.3));
}

TEST_CASE("TokenCalibrator: 坏账与小请求剔除——实报为零/字节为零/估算过小") {
    TokenCalibrator cal;
    // 实报为零(没回 usage 或明报全零):弃。
    CHECK(cal.Record("p", "m", Sample(0)) == RecordVerdict::RejectedInvalid);
    // 负数同理(防御)。
    CHECK(cal.Record("p", "m", Sample(-1)) == RecordVerdict::RejectedInvalid);
    // 字节账为零:弃。
    TokenCalibrationSample no_bytes = SampleRatio(1.0);
    no_bytes.request_bytes = 0;
    CHECK(cal.Record("p", "m", no_bytes) == RecordVerdict::RejectedInvalid);
    // 小请求(协议脚手架占比过高):弃。
    CHECK(cal.Record("p", "m", SampleRatio(1.0, /*est=*/2047)) == RecordVerdict::RejectedInvalid);
    // 硬带外(真分词器不可能偏成这样,八成 usage 残缺):弃。
    CHECK(cal.Record("p", "m", SampleRatio(6.0)) == RecordVerdict::RejectedHardBand);
    CHECK(cal.Record("p", "m", SampleRatio(0.1)) == RecordVerdict::RejectedHardBand);
    // 全被拦下,窗口空着,系数兜底。
    CHECK(cal.Coefficient("p", "m") == 1.0);
    CHECK(cal.StatusOf("p", "m").sample_count == 0);
}

TEST_CASE("TokenCalibrator: 异常样本剔除——偏离窗口中位 ±40% 外不进窗") {
    TokenCalibrator cal;
    CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    // 1.6 落在硬带内、异常带外([0.6, 1.4]):弃。
    CHECK(cal.Record("p", "m", SampleRatio(1.6)) == RecordVerdict::RejectedAnomaly);
    // 再来一枚 1.6:偏离不足漂移线(2 倍),仍只弃不重置。
    CHECK(cal.Record("p", "m", SampleRatio(1.6)) == RecordVerdict::RejectedAnomaly);
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(1.0));
    CHECK(cal.StatusOf("p", "m").sample_count == 2);
}

TEST_CASE("TokenCalibrator: 漂移重置——连续两枚超中位两倍,清窗换新口径") {
    TokenCalibrator cal;
    CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("p", "m", SampleRatio(1.0)) == RecordVerdict::Accepted);
    // 3.0 超中位两倍:第一枚先拦(等第二枚确认,单枚可能是脏样本)。
    CHECK(cal.Record("p", "m", SampleRatio(3.0)) == RecordVerdict::RejectedAnomaly);
    // 第二枚同向超线:窗口重置,本样本独居新窗。
    CHECK(cal.Record("p", "m", SampleRatio(3.0)) == RecordVerdict::WindowReset);
    // 重置后只有一对,回到兜底;再记一对同口径的,系数翻到新比率。
    CHECK_FALSE(cal.StatusOf("p", "m").calibrated);
    CHECK(cal.Coefficient("p", "m") == 1.0);
    CHECK(cal.Record("p", "m", SampleRatio(3.0)) == RecordVerdict::Accepted);
    CHECK(cal.Coefficient("p", "m") == doctest::Approx(3.0));
}

TEST_CASE("TokenCalibrator: (provider,model) 分桶,不跨模型共用比率") {
    TokenCalibrator cal;
    CHECK(cal.Record("p", "m1", SampleRatio(2.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("p", "m1", SampleRatio(2.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("p", "m2", SampleRatio(1.0)) == RecordVerdict::Accepted);
    CHECK(cal.Record("q", "m1", SampleRatio(0.8)) == RecordVerdict::Accepted);
    CHECK(cal.Record("q", "m1", SampleRatio(0.8)) == RecordVerdict::Accepted);
    CHECK(cal.Coefficient("p", "m1") == doctest::Approx(2.0));
    CHECK(cal.Coefficient("p", "m2") == 1.0);  // 单样本,兜底
    CHECK(cal.Coefficient("q", "m1") == doctest::Approx(0.8));
}

TEST_CASE("TokenCalibrator: 估算口系数落整——四舍五入,零不凭空造,非零至少保一") {
    CHECK(agent::ApplyTokenCalibration(0, 0.5) == 0);
    CHECK(agent::ApplyTokenCalibration(100, 1.0) == 100);
    CHECK(agent::ApplyTokenCalibration(100, 1.257) == 126);  // 125.7 → 126
    CHECK(agent::ApplyTokenCalibration(1, 0.3) == 1);        // 0.3 → 非零至少保 1
    CHECK(agent::ApplyTokenCalibration(1000, 0.5) == 500);
}

// ---------------------------------------------------------------------------
// 假后端对账:AgentLoop 走真路,定长 usage 反推收敛
// ---------------------------------------------------------------------------

namespace {

// 假后端:"真分词器" = 默认尺的 real_per_estimate 倍。每份请求按与 loop
// 样本口径同源的默认尺(system + messages + 工具定义)估一遍,实报 input
// = 估算 × 倍数——定长对账,两轮后校准器必然收敛到该倍数。
class CalibratingFakeBackend : public api::Backend {
public:
    double real_per_estimate = 2.0;
    std::vector<api::Request> captured;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured.push_back(request);
        std::size_t est = agent::EstimateUtf8Tokens(request.system) +
                          agent::EstimateHistoryTokens(request.messages);
        for (const auto& tool : request.tools) {
            est += agent::EstimateUtf8Tokens(tool.name) + agent::EstimateUtf8Tokens(tool.description) +
                   agent::EstimateUtf8Tokens(tool.input_schema.dump());
        }
        api::Usage usage;
        // 四舍五入不截断:整数估算 × 整倍数要落得精确,比率才收敛得干净。
        usage.input_tokens =
            static_cast<std::int64_t>(static_cast<double>(est) * real_per_estimate + 0.5);
        usage.output_tokens = 16;
        on_event(api::MessageStart{"msg", "calib-model"});
        on_event(api::TextDelta{"回答"});
        on_event(api::ContentBlockDone{0});
        on_event(api::MessageDone{"end_turn", usage, /*usage_reported=*/true});
        return {};
    }
};

std::unique_ptr<agent::Agent> MakeAgent(api::Backend& backend, tools::ToolRegistry& registry) {
    return std::make_unique<agent::Agent>(
        backend, registry,
        agent::AgentProfile{.provider = "fake",
                            .request{.model = "calib-model"},
                            .runtime{.max_output_tokens = 2048},
                            .system_prompt = "sys"});
}

}  // namespace

TEST_CASE("AgentLoop+TokenCalibrator: 假后端定长对账,两轮后估算收敛到校准比率") {
    agent::TokenCalibrator calibrator;
    CalibratingFakeBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<agent::Agent> loop = MakeAgent(backend, registry);
    loop->SetContextWindowTokens(262144);

    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring agent_wiring;
    agent_wiring.on_context_pressure = [&seen](const agent::ContextPressure& pressure) {
        if (pressure.phase == agent::ContextPressure::Phase::PreRequest) {
            seen.push_back(pressure);
        }
    };
    loop->SetWiring(std::move(agent_wiring));

    agent::TurnWiring wiring;
    wiring.token_calibrator = &calibrator;

    // 24000 个 ASCII:默认尺 6000 token,过小请求护栏。
    const std::string big(24000, 'x');

    // 第一轮:零样本,系数 1.0,B 闸水位 = 默认尺原值。
    REQUIRE(loop->Run("第一问 " + big, wiring).has_value());
    CHECK(calibrator.Coefficient("fake", "calib-model") == 1.0);
    REQUIRE_FALSE(seen.empty());
    const std::size_t first_raw = agent::EstimateHistoryTokens(backend.captured.back().messages);
    CHECK(seen.back().working_view_tokens == first_raw);

    // 第二轮:一对样本在账,本轮估算仍按兜底系数;请求收口后第二对落账,
    // 立刻够上校准线(单子"首两对之前兜底"的两对,一到就翻)。
    REQUIRE(loop->Run("第二问 " + big, wiring).has_value());
    const double coefficient = calibrator.Coefficient("fake", "calib-model");
    CHECK(coefficient == doctest::Approx(2.0));

    // 第三轮:系数 2.0 在手,B 闸真实水位 = 默认尺 × 2.0。
    REQUIRE(loop->Run("第三问 " + big, wiring).has_value());
    CHECK(calibrator.Coefficient("fake", "calib-model") == doctest::Approx(2.0));
    const auto status = calibrator.StatusOf("fake", "calib-model");
    CHECK(status.calibrated);
    CHECK(status.sample_count == 3);
    CHECK(status.coefficient == doctest::Approx(2.0));
    // B 闸吃系数:压力通报的真实水位恰是默认尺(同一副发出的请求)乘系数。
    const std::size_t third_raw = agent::EstimateHistoryTokens(backend.captured.back().messages);
    CHECK(third_raw >= TokenCalibrator::kMinRequestEstimateTokens);
    CHECK(seen.back().working_view_tokens == agent::ApplyTokenCalibration(third_raw, coefficient));
    // projected(A 闸托底尺)同乘:必大于等于日常尺水位。
    CHECK(seen.back().projected_tokens >= seen.back().working_view_tokens);
}

TEST_CASE("AgentLoop 不接校准器:行为与从前一字不差,B 闸水位不吃任何系数") {
    CalibratingFakeBackend backend;
    tools::ToolRegistry registry;
    std::unique_ptr<agent::Agent> loop = MakeAgent(backend, registry);
    loop->SetContextWindowTokens(262144);

    std::vector<agent::ContextPressure> seen;
    agent::AgentWiring agent_wiring;
    agent_wiring.on_context_pressure = [&seen](const agent::ContextPressure& pressure) {
        if (pressure.phase == agent::ContextPressure::Phase::PreRequest) {
            seen.push_back(pressure);
        }
    };
    loop->SetWiring(std::move(agent_wiring));

    const std::string big(24000, 'x');
    // 空 wiring(token_calibrator 默认空):双闸既有单测的形状。
    REQUIRE(loop->Run("第一问 " + big, agent::TurnWiring{}).has_value());
    REQUIRE(loop->Run("第二问 " + big, agent::TurnWiring{}).has_value());
    REQUIRE_FALSE(seen.empty());
    const std::size_t raw = agent::EstimateHistoryTokens(backend.captured.back().messages);
    CHECK(seen.back().working_view_tokens == raw);
    // 进程级默认实例没人碰过:任何桶都是兜底 1.0。
    CHECK(agent::DefaultTokenCalibrator().Coefficient("fake", "calib-model") == 1.0);
}
