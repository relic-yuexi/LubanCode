// features.telemetry 激活合同测试(端云协同可观测单 §8.2 激活矩阵/§8.3
// 环境变量/§29.2 默认关闭,T0"冻结 Activation"):
//   - 四态矩阵:Disabled/Active/RequiresTrajectory/DisabledByKillSwitch;
//   - LUBANCODE_TELEMETRY 显式压配置一头;坏值不当意见;
//   - 总闸 LUBANCODE_DISABLE_TELEMETRY 压一切;
//   - 不暗开 trajectory(不变量 5):RequiresTrajectory 不改传入配置。
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "telemetry/activation.hpp"

using namespace lubancode::telemetry;

namespace {

void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    REQUIRE(_putenv_s(name, value) == 0);
#else
    REQUIRE(setenv(name, value, 1) == 0);
#endif
}

void ClearEnv(const char* name) {
#ifdef _WIN32
    REQUIRE(_putenv_s(name, "") == 0);
#else
    REQUIRE(unsetenv(name) == 0);
#endif
}

}  // namespace

TEST_CASE("激活矩阵: 默认关闭与四态") {
    ClearEnv("LUBANCODE_TELEMETRY");
    ClearEnv("LUBANCODE_DISABLE_TELEMETRY");

    // 旧路:两只都关。
    const TelemetryActivation both_off = ResolveTelemetryActivation(false, false);
    REQUIRE(both_off.status == TelemetryActivationStatus::Disabled);
    CHECK_FALSE(both_off.enabled());
    CHECK(both_off.reason_code == "telemetry.off");

    // 只开 trajectory:仍是 Disabled(不开投影)。
    const TelemetryActivation traj_only = ResolveTelemetryActivation(false, true);
    REQUIRE(traj_only.status == TelemetryActivationStatus::Disabled);

    // telemetry=true && trajectory=false:明确拒绝,不暗开(§8.2 首版收窄)。
    const TelemetryActivation requires_traj = ResolveTelemetryActivation(true, false);
    REQUIRE(requires_traj.status == TelemetryActivationStatus::RequiresTrajectory);
    CHECK(requires_traj.reason_code == "telemetry.requires_trajectory");
    CHECK_FALSE(requires_traj.enabled());
    // 传入配置原样带回:Activation 不改字段(总闸只关采集不改配置,§8.3)。
    CHECK(requires_traj.config_telemetry);
    CHECK_FALSE(requires_traj.config_trajectory);

    // 完整路:唯一 Active。
    const TelemetryActivation active = ResolveTelemetryActivation(true, true);
    REQUIRE(active.status == TelemetryActivationStatus::Active);
    CHECK(active.enabled());
    CHECK(active.reason_code == "telemetry.active");
}

TEST_CASE("环境变量 LUBANCODE_TELEMETRY: 显式压一头,坏值不当意见") {
    ClearEnv("LUBANCODE_DISABLE_TELEMETRY");

    SUBCASE("=1 压配置关") {
        SetEnv("LUBANCODE_TELEMETRY", "1");
        const TelemetryActivation activation = ResolveTelemetryActivation(false, true);
        REQUIRE(activation.status == TelemetryActivationStatus::Active);
        CHECK(activation.env_opinion.has_value());
        CHECK(*activation.env_opinion);
    }
    SUBCASE("true 同义") {
        SetEnv("LUBANCODE_TELEMETRY", "true");
        CHECK(ResolveTelemetryActivation(false, true).enabled());
    }
    SUBCASE("=0 压配置开") {
        SetEnv("LUBANCODE_TELEMETRY", "0");
        const TelemetryActivation activation = ResolveTelemetryActivation(true, true);
        REQUIRE(activation.status == TelemetryActivationStatus::Disabled);
        CHECK(activation.reason_code == "telemetry.env_off");
    }
    SUBCASE("坏值听配置") {
        SetEnv("LUBANCODE_TELEMETRY", "maybe");
        const TelemetryActivation activation = ResolveTelemetryActivation(true, true);
        REQUIRE(activation.status == TelemetryActivationStatus::Active);
        CHECK_FALSE(activation.env_opinion.has_value());
    }
    SUBCASE("空串听配置") {
        SetEnv("LUBANCODE_TELEMETRY", "");
        CHECK(ResolveTelemetryActivation(false, true).status ==
              TelemetryActivationStatus::Disabled);
    }
    ClearEnv("LUBANCODE_TELEMETRY");
}

TEST_CASE("总闸 LUBANCODE_DISABLE_TELEMETRY: 压一切,只关不改") {
    SetEnv("LUBANCODE_DISABLE_TELEMETRY", "1");
    // 完整配置路也被压下。
    const TelemetryActivation activation = ResolveTelemetryActivation(true, true);
    REQUIRE(activation.status == TelemetryActivationStatus::DisabledByKillSwitch);
    CHECK(activation.reason_code == "telemetry.kill_switch");
    CHECK_FALSE(activation.enabled());
    // 只关采集发送:配置字段原样,拉总闸不等于改配置(§8.3)。
    CHECK(activation.config_telemetry);
    CHECK(activation.config_trajectory);
    // 总闸压过 env=1。
    SetEnv("LUBANCODE_TELEMETRY", "1");
    CHECK(ResolveTelemetryActivation(false, true).status ==
          TelemetryActivationStatus::DisabledByKillSwitch);
    // 非 1/true 不拉闸。
    SetEnv("LUBANCODE_DISABLE_TELEMETRY", "0");
    CHECK(ResolveTelemetryActivation(true, true).enabled());

    ClearEnv("LUBANCODE_TELEMETRY");
    ClearEnv("LUBANCODE_DISABLE_TELEMETRY");
}
