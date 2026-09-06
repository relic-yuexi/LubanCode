// 镜像常量对数册(清理批):同一个数在仓里有几份手抄,各域不许互相
// include(依赖只许单向),只好各写一份——本册把各份钉在一条断言上,
// 谁改漏一处,这里红灯。改值的规矩:几份一起改,一起过本册;只想改
// 一份的,先想清楚那不是改值,是分家(该立单拆成单一出处,别在本册
// 里放行)。
//
// 现状五组:
//   8192  输出上限兜底三份(api/types.hpp / config.hpp / agent/runtime_profile.hpp);
//   4 与 2..8  compact 分区默认与取值域(config.hpp / agent/compact.hpp);
//   2048  协议余量(agent/compact.hpp 的 CompactBudget / agent/context_budget.hpp);
//   1     length 续跑默认次数(config.hpp / agent/runtime_profile.hpp,同文注释);
//   80    自动压缩触发线(agent/context.hpp / cli/context_tracker.hpp)。
// 曾有的 600000 三份已随字节轴拆除批(9f795d74)收走,无残份;往后再有
// 同数多份的镜像,往本册里添条断言,别裸放。
#include <doctest/doctest.h>

#include <cstddef>

#include "agent/compact.hpp"
#include "agent/context.hpp"
#include "agent/context_budget.hpp"
#include "agent/runtime_profile.hpp"
#include "api/types.hpp"
#include "cli/context_tracker.hpp"
#include "config/config.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// 8192:anthropic 系 wire 必填 max_tokens 时,三级声明全缺席的公开兜底。
// 三处各自声明"改一处须三处一起改",本册替它们记账。
// ---------------------------------------------------------------------------
TEST_CASE("镜像常量:8192 输出上限兜底三份相等") {
    CHECK(api::kRequiredMaxOutputTokensFallback == 8192);
    CHECK(config::kDefaultRequiredMaxOutputTokens == 8192);
    CHECK(agent::kUnsetOutputReserveEstimateTokens == 8192);
    CHECK(api::kRequiredMaxOutputTokensFallback == config::kDefaultRequiredMaxOutputTokens);
    CHECK(config::kDefaultRequiredMaxOutputTokens == agent::kUnsetOutputReserveEstimateTokens);
}

// ---------------------------------------------------------------------------
// compact 分区:默认 4、允许 2..8。config 域(int)与 agent 域(size_t)
// 各一份,越界由配置层报错、不静默夹值。
// ---------------------------------------------------------------------------
TEST_CASE("镜像常量:compact 分区默认与取值域两份相等") {
    CHECK(config::kDefaultCompactPartitionCount == 4);
    CHECK(agent::kDefaultCompactPartitionCount == 4u);
    CHECK(config::kMinCompactPartitionCount == 2);
    CHECK(agent::kMinCompactPartitionCount == 2u);
    CHECK(config::kMaxCompactPartitionCount == 8);
    CHECK(agent::kMaxCompactPartitionCount == 8u);
    CHECK(static_cast<std::size_t>(config::kDefaultCompactPartitionCount) ==
          agent::kDefaultCompactPartitionCount);
    CHECK(static_cast<std::size_t>(config::kMinCompactPartitionCount) == agent::kMinCompactPartitionCount);
    CHECK(static_cast<std::size_t>(config::kMaxCompactPartitionCount) == agent::kMaxCompactPartitionCount);
    CHECK(config::kMinCompactPartitionCount < config::kDefaultCompactPartitionCount);
    CHECK(config::kDefaultCompactPartitionCount < config::kMaxCompactPartitionCount);
}

// ---------------------------------------------------------------------------
// 2048:协议与安全余量(system 指令、wire 包装、流式协议开销)。
// CompactBudget(压缩请求自己的预算)与 ContextBudgetInputs(总账)各
// 一份默认值,注释写明"同款"。
// ---------------------------------------------------------------------------
TEST_CASE("镜像常量:2048 协议余量两份相等") {
    const agent::CompactBudget compact{};
    const agent::ContextBudgetInputs inputs{};
    CHECK(compact.protocol_headroom_tokens == 2048);
    CHECK(inputs.protocol_headroom_tokens == 2048);
    CHECK(compact.protocol_headroom_tokens == inputs.protocol_headroom_tokens);
}

// ---------------------------------------------------------------------------
// 1:length 续跑默认次数。config 域与 agent 域两份,注释同文(实测一次
// 足以收束思考)。
// ---------------------------------------------------------------------------
TEST_CASE("镜像常量:length 续跑默认次数两份相等") {
    CHECK(config::kDefaultLengthContinuations == 1);
    CHECK(agent::kDefaultLengthContinuations == 1);
    CHECK(config::kDefaultLengthContinuations == agent::kDefaultLengthContinuations);
}

// ---------------------------------------------------------------------------
// 80:自动压缩触发线。mid-turn 的 projected 参考线(agent 域)与会话级
// 真实水位线(cli 域)同档——两份都动才叫调线,改一份就是两处判定打架。
// ---------------------------------------------------------------------------
TEST_CASE("镜像常量:80 自动压缩触发线两份相等") {
    CHECK(agent::kProjectedOverflowPercent == 80);
    CHECK(cli::kAutoCompactThresholdPercent == 80);
    CHECK(agent::kProjectedOverflowPercent == cli::kAutoCompactThresholdPercent);
}
