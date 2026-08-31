// 条件工具的本轮能力段(动态工具 PromptCache 守恒单 P2·§8.2):
// BuildTurnCapabilitiesSegment 纯函数钉死——两行都隐藏给空串(干净会话
// 不塞空脚手架)、可用带注、不可用给指路、段头明写"以执行门为准"
//(状态段不是安全边界,模型硬叫了执行门照样拦,那半边在 test_loop 的
// turn 闸册里钉)。
#include <doctest/doctest.h>

#include <string>

#include "app/turn_capabilities.hpp"

using lubancode::app::BuildTurnCapabilitiesSegment;
using lubancode::app::TurnCapabilities;

TEST_CASE("能力段: 两行都隐藏给空串,干净会话零注入") {
    TurnCapabilities caps;  // 默认全隐藏(features 没开,定义也不进 tools)
    CHECK(BuildTurnCapabilitiesSegment(caps).empty());
}

TEST_CASE("能力段: 普通轮——两枚都常驻可见但本轮不可用,各给一行指路") {
    TurnCapabilities caps;
    caps.goal_checkpoint.shown = true;
    caps.loop_control.shown = true;
    const std::string segment = BuildTurnCapabilitiesSegment(caps);
    CHECK(segment.find("[turn capabilities]") != std::string::npos);
    CHECK(segment.find("goal_checkpoint: 不可用") != std::string::npos);
    CHECK(segment.find("loop_control: 不可用") != std::string::npos);
    // 段头明写自己不是安全边界:执行门以真实轮次为准(§8.2 的原话)。
    CHECK(segment.find("执行门") != std::string::npos);
}

TEST_CASE("能力段: goal 轮——goal_checkpoint 可用带 goal id 注,loop_control 仍不可用") {
    TurnCapabilities caps;
    caps.goal_checkpoint.shown = true;
    caps.goal_checkpoint.available = true;
    caps.goal_checkpoint.note = "g-7";
    caps.loop_control.shown = true;
    const std::string segment = BuildTurnCapabilitiesSegment(caps);
    CHECK(segment.find("goal_checkpoint: 可用(当前 goal 执行轮:g-7)") != std::string::npos);
    CHECK(segment.find("loop_control: 不可用(当前不在 loop 定时拍") != std::string::npos);
}

TEST_CASE("能力段: loop 拍——loop_control 可用带任务 id 注,goal 行照常报不可用") {
    TurnCapabilities caps;
    caps.goal_checkpoint.shown = true;
    caps.loop_control.shown = true;
    caps.loop_control.available = true;
    caps.loop_control.note = "t-3";
    const std::string segment = BuildTurnCapabilitiesSegment(caps);
    CHECK(segment.find("loop_control: 可用(当前 loop 定时拍:t-3)") != std::string::npos);
    CHECK(segment.find("goal_checkpoint: 不可用(当前不在 goal 执行轮") != std::string::npos);
}

TEST_CASE("能力段: 只开一枚功能,另一枚整行不出") {
    TurnCapabilities caps;
    caps.goal_checkpoint.shown = true;  // 只有 goal 功能开了
    caps.loop_control.shown = false;
    const std::string segment = BuildTurnCapabilitiesSegment(caps);
    CHECK(segment.find("goal_checkpoint") != std::string::npos);
    CHECK(segment.find("loop_control") == std::string::npos);
}
